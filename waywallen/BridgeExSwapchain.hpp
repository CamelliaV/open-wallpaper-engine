// BridgeExSwapchain — ExSwapchain implementation that delegates DMA-BUF
// allocation, modifier negotiation, and frame_ready emission to the
// waywallen-bridge pool API. Used by waywallen-wescene-renderer; the
// standalone viewers continue to use LocalExSwapchain.
//
// **Render target indirection**:
//   The bridge creates slot VkImages with usage =
//   TRANSFER_DST_BIT | TRANSFER_SRC_BIT — they are *transfer-only*, by
//   design, to keep the DRM-format-modifier sub-layout (DCC) consistent
//   between producer and the TRANSFER_SRC-only consumer (see comment in
//   pool_vulkan.c::alloc_slot). FinPass cannot render directly into
//   them.
//
//   Solution: BridgeExSwapchain owns a per-slot *intermediate*
//   VkImage (COLOR_ATTACHMENT | TRANSFER_SRC, OPTIMAL tiling). FinPass
//   renders into the intermediate; BridgeExSwapchain's submitRendered
//   records a vkCmdCopyImage from intermediate to slot, releases the
//   slot to VK_QUEUE_FAMILY_FOREIGN_EXT, and forwards an exported
//   SYNC_FD to the bridge.
//
// Threading model:
//   - Reader / main threads only call `queueDirective` (stash-only). They
//     never touch VkImage, VkImageView, or the bridge slot pool.
//   - The render thread (VulkanRender::Impl::drawFrameOffscreen) calls
//     `poll`, `acquireRenderTarget`, and `submitRendered`. poll is the
//     only site that ever destroys/recreates resources or invokes
//     `ww_bridge_pool_apply_directive`, making the slot pool
//     single-writer with no quiesce barrier required.
//
// Lifecycle:
//   1) Caller creates the pool (`ww_bridge_pool_create`) and constructs
//      this swapchain with the producer's VkDevice + graphics queue.
//   2) Caller registers `setOnFirstNegotiated(...)` (e.g. `wp.play()`)
//      and calls `ww_bridge_pool_advertise_caps`.
//   3) Reader thread receives WW_REQ_NEGOTIATE_BUFFERS and calls
//      `queueDirective` — stashes the directive without applying it.
//   4) Render thread's next `poll()` drains the pending directive,
//      runs `ww_bridge_pool_apply_directive`, allocates intermediate
//      images, then fires the on-first-negotiated callback.
//   5) Each subsequent frame: `acquireRenderTarget` returns the next
//      slot's intermediate; VulkanRender records FinPass into it +
//      submits + exports producer sync_fd; `submitRendered` records
//      copy + release on the bridge slot, exports a downstream sync_fd,
//      and hands off via `ww_bridge_pool_submit_slot`.

#pragma once

#include "Swapchain/ExSwapchain.hpp"
#include "Vulkan/Parameters.hpp"

#include <waywallen-bridge/bridge.h>
#include <waywallen-bridge/pool.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vulkan/vulkan.h>

namespace ww_wescene
{

class BridgeExSwapchain : public wallpaper::ExSwapchain {
public:
    static constexpr uint32_t kMaxSlots = 8; // matches bridge cap

    // `pool` and `sock` are caller-owned; bridge expects the same
    // (pool, sock) pair to live for the swapchain's lifetime.
    //
    // `physical_device` / `device` / `graphics_queue` /
    // `graphics_queue_family` describe the producer's Vulkan context.
    // BridgeExSwapchain creates its own command pool, semaphores,
    // fence, and per-slot intermediate images on top of these.
    BridgeExSwapchain(ww_pool_t* pool, int sock,
                      VkPhysicalDevice physical_device,
                      VkDevice         device,
                      VkQueue          graphics_queue,
                      uint32_t         graphics_queue_family);
    ~BridgeExSwapchain() override;

    // Stash a directive received from the daemon. Safe to call from any
    // thread — only the in-render-thread `poll()` ever reads the stash.
    // If a directive was already pending, it is dropped (the newer
    // directive supersedes it; the daemon's older negotiation is
    // implicitly abandoned).
    void queueDirective(const ww_pool_directive_t& directive);

    // True iff a queued directive is waiting to be applied. Read-only.
    bool hasPendingDirective() const {
        return m_pending_valid.load(std::memory_order_acquire);
    }

    // One-shot callback fired from the render thread the first time a
    // directive is successfully applied. Use this to start scene
    // playback (`wp.play()`) without racing the slot-pool readiness.
    // Setting this after the first negotiate has already happened is a
    // no-op.
    void setOnFirstNegotiated(std::function<void()> cb) {
        std::lock_guard<std::mutex> lk(m_cb_mu);
        m_on_first_negotiated = std::move(cb);
    }

    // ExSwapchain interface ---------------------------------------------

    // Render-thread-only. Drains any pending negotiate directive so
    // `format()` / `ready()` reflect the new state. Must run before the
    // caller decides whether to acquire a slot — the slot pool can only
    // change here (no other site touches m_intermediates / m_slot_count).
    void poll() override { drainPendingDirective(); }

    // Returns the *intermediate* image for the chosen slot, not the
    // bridge slot itself. FinPass renders here; submitRendered does the
    // copy.
    bool acquireRenderTarget(wallpaper::vulkan::ImageParameters& out) override;

    // Records an internal command buffer that copies intermediate →
    // bridge slot, releases the slot to VK_QUEUE_FAMILY_FOREIGN_EXT,
    // submits on the producer queue (waiting `producer_sync_fd`), and
    // hands the new outgoing SYNC_FD to ww_bridge_pool_submit_slot.
    // Takes ownership of `producer_sync_fd`.
    void submitRendered(int producer_sync_fd) override;

    unsigned width() const override  { return m_width; }
    unsigned height() const override { return m_height; }
    // Negotiated VkFormat — set by applyDirective from the picked
    // fourcc. Returns VK_FORMAT_UNDEFINED until the first directive is
    // applied. The render layer (VulkanRender::Impl::drawFrameOffscreen)
    // adapts FinPass's renderpass+pipeline whenever this changes.
    VkFormat format() const override { return m_export_format; }

    // FinPass renders into intermediate; layout after renderpass should
    // be ready for vkCmdCopyImage as a transfer source.
    VkImageLayout producerOutputLayout() const override {
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    // Ready iff we have at least one slot and a resolved export format.
    bool ready() const override {
        return m_slot_count > 0 && m_export_format != VK_FORMAT_UNDEFINED;
    }

    // Callback fired from the render thread inside drainPendingDirective
    // after a directive successfully applies. Coexists with
    // setOnFirstNegotiated; both fire in undefined order.
    void setOnReadyChanged(
        std::function<void(const wallpaper::ExSwapchainReadyEvent&)> cb) override {
        std::lock_guard<std::mutex> lk(m_cb_mu);
        m_on_ready_changed = std::move(cb);
    }

private:
    // Per-slot intermediate render target (producer-owned).
    struct Intermediate {
        VkImage        image  { VK_NULL_HANDLE };
        VkDeviceMemory memory { VK_NULL_HANDLE };
        VkImageView    view   { VK_NULL_HANDLE };
    };

    // Render-thread-only. If a directive is pending, take it and run
    // applyDirective. After this returns, `m_intermediates` /
    // `m_slot_count` are owned by the render thread for the rest of
    // the frame.
    void drainPendingDirective();

    // Render-thread-only. Tears down old intermediates, runs the
    // bridge's apply_directive, allocates new intermediates. Returns:
    //   0   success — m_slot_count == directive.count
    //  <0   dry-run failure (bridge already emitted bind_failed); caller
    //       leaves m_slot_count = 0 and waits for the next directive
    //  >0   hard system error (logged); m_slot_count = 0
    int applyDirective(const ww_pool_directive_t& directive);

    // Helpers split out of applyDirective.
    bool createIntermediate(uint32_t slot, uint32_t w, uint32_t h, VkFormat fmt);
    void destroyIntermediates();
    bool createCopyResources(); // cmd pool/buffer + fence + semaphores
    void destroyCopyResources();
    uint32_t pickMemoryType(uint32_t type_bits, VkMemoryPropertyFlags want) const;

    // Caller-owned bridge handles.
    ww_pool_t*       m_pool { nullptr };
    int              m_sock { -1 };

    // Producer Vulkan context (caller-owned; no destroy on shutdown).
    VkPhysicalDevice m_physical_device { VK_NULL_HANDLE };
    VkDevice         m_device { VK_NULL_HANDLE };
    VkQueue          m_queue { VK_NULL_HANDLE };
    uint32_t         m_queue_family { 0 };

    // Owned by this swapchain.
    VkCommandPool   m_cmd_pool { VK_NULL_HANDLE };
    VkCommandBuffer m_cmd { VK_NULL_HANDLE };
    VkFence         m_copy_fence { VK_NULL_HANDLE };
    bool            m_copy_fence_inflight { false };
    VkSemaphore     m_wait_sem { VK_NULL_HANDLE };   // import producer sync_fd
    VkSemaphore     m_signal_sem { VK_NULL_HANDLE }; // export downstream sync_fd
    bool            m_resources_ready { false };

    // Extension entry points loaded via vkGetDeviceProcAddr — these are
    // KHR functions, not exported by the Vulkan loader's static symbol
    // table.
    PFN_vkImportSemaphoreFdKHR m_pfn_import_sem { nullptr };
    PFN_vkGetSemaphoreFdKHR    m_pfn_get_sem_fd { nullptr };

    VkFormat m_export_format { VK_FORMAT_UNDEFINED };

    // Pending directive stash. `m_pending_valid` is the publish flag —
    // set last on push, cleared first on take, paired with
    // `m_pending_mu` for the directive payload itself.
    std::atomic<bool>   m_pending_valid { false };
    std::mutex          m_pending_mu;
    ww_pool_directive_t m_pending_directive {};

    // Callback slots — both fire from drainPendingDirective on the
    // render thread. m_cb_mu guards both setter races; the actual
    // invocation copies the std::function out under the lock and calls
    // unlocked.
    std::mutex            m_cb_mu;
    std::function<void()> m_on_first_negotiated;
    bool                  m_first_negotiated_done { false };
    std::function<void(const wallpaper::ExSwapchainReadyEvent&)>
                          m_on_ready_changed;

    // Render-thread-only state (no mutex). Written by acquireRenderTarget
    // / submitRendered / drainPendingDirective, all on the render thread.
    uint32_t m_slot_count { 0 };
    uint32_t m_next_slot { 0 };
    uint32_t m_pending_slot { 0 }; // slot returned by last acquireRenderTarget
    bool     m_have_pending { false };
    uint32_t m_width { 0 };
    uint32_t m_height { 0 };
    uint32_t m_fourcc { 0 };

    std::array<Intermediate, kMaxSlots> m_intermediates {};
};

} // namespace ww_wescene
