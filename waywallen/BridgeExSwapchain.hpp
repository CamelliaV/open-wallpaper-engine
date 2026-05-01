// BridgeExSwapchain — ExSwapchain implementation that delegates DMA-BUF
// allocation, modifier negotiation, and frame_ready emission to the
// waywallen-bridge pool API. Used by waywallen-wescene-renderer; the
// standalone viewers continue to use LocalExSwapchain.
//
// FinPass writes directly into the bridge slot (vkCmdBlitImage). The
// bridge slot is created with TRANSFER_DST_BIT | TRANSFER_SRC_BIT, so
// blit-as-dst works. Producer-side queue ownership is released to
// VK_QUEUE_FAMILY_FOREIGN_EXT inside FinPass's exit barrier, which
// flushes GPU caches before the consumer reads the buffer.
//
// Threading model:
//   - Reader / main threads only call `queueDirective` (stash-only). They
//     never touch the bridge slot pool or any per-slot state.
//   - The render thread (VulkanRender::Impl::drawFrameOffscreen) calls
//     `poll`, `acquireRenderTarget`, and `submitRendered`. poll is the
//     only site that ever invokes `ww_bridge_pool_apply_directive`,
//     making the slot pool single-writer with no quiesce barrier.
//
// Lifecycle:
//   1) Caller creates the pool (`ww_bridge_pool_create`) and constructs
//      this swapchain.
//   2) Caller registers `setOnFirstNegotiated(...)` (e.g. `wp.play()`)
//      and calls `ww_bridge_pool_advertise_caps`.
//   3) Reader thread receives WW_REQ_NEGOTIATE_BUFFERS and calls
//      `queueDirective` — stashes the directive without applying it.
//   4) Render thread's next `poll()` drains the pending directive,
//      runs `ww_bridge_pool_apply_directive`, then fires the
//      on-first-negotiated callback.
//   5) Each subsequent frame: `acquireRenderTarget` returns the next
//      slot's VkImage; VulkanRender records FinPass's blit + release
//      barrier, submits, exports a SYNC_FD; `submitRendered` forwards
//      the fd to `ww_bridge_pool_submit_slot`.

#pragma once

#include "Swapchain/ExSwapchain.hpp"
#include "Vulkan/Parameters.hpp"

#include <waywallen-bridge/bridge.h>
#include <waywallen-bridge/pool.h>

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
    BridgeExSwapchain(ww_pool_t* pool, int sock);
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
    // caller decides whether to acquire a slot.
    void poll() override { drainPendingDirective(); }

    // Returns the bridge slot's VkImage. No image view / sampler is set
    // — FinPass uses vkCmdBlitImage which only needs the handle.
    bool acquireRenderTarget(wallpaper::vulkan::ImageParameters& out) override;

    // Forwards `producer_sync_fd` to ww_bridge_pool_submit_slot. Bridge
    // takes ownership of the fd and closes it.
    void submitRendered(int producer_sync_fd) override;

    unsigned width() const override  { return m_width; }
    unsigned height() const override { return m_height; }
    // Negotiated VkFormat — set by applyDirective from the picked
    // fourcc. Returns VK_FORMAT_UNDEFINED until the first directive is
    // applied.
    VkFormat format() const override { return m_export_format; }

    // FinPass's exit barrier transitions the slot to GENERAL after
    // blitting; consumers (KMS / display through DMA-BUF) read from
    // there.
    VkImageLayout producerOutputLayout() const override {
        return VK_IMAGE_LAYOUT_GENERAL;
    }

    // DMA-BUF hand-off: consumer is non-Vulkan.
    uint32_t releaseTargetQueueFamily() const override {
        return VK_QUEUE_FAMILY_FOREIGN_EXT;
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
    // Render-thread-only. If a directive is pending, take it and run
    // applyDirective.
    void drainPendingDirective();

    // Render-thread-only. Runs the bridge's apply_directive, publishes
    // new geometry/format on success. Returns:
    //   0   success — m_slot_count == directive.count
    //  <0   dry-run failure (bridge already emitted bind_failed); caller
    //       leaves m_slot_count = 0 and waits for the next directive
    //  >0   hard system error (logged); m_slot_count = 0
    int applyDirective(const ww_pool_directive_t& directive);

    // Caller-owned bridge handles.
    ww_pool_t*       m_pool { nullptr };
    int              m_sock { -1 };

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

    // Render-thread-only state (no mutex).
    uint32_t m_slot_count { 0 };
    uint32_t m_next_slot { 0 };
    uint32_t m_pending_slot { 0 }; // slot returned by last acquireRenderTarget
    bool     m_have_pending { false };
    uint32_t m_width { 0 };
    uint32_t m_height { 0 };
    uint32_t m_fourcc { 0 };
};

} // namespace ww_wescene
