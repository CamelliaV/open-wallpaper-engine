// BridgeExSwapchain — ExSwapchain implementation that delegates DMA-BUF
// allocation, modifier negotiation, and frame_ready emission to the
// waywallen-bridge pool API. Used by waywallen-wescene-renderer; the
// standalone viewers continue to use LocalExSwapchain.
//
// Threading model:
//   - Reader / main threads only call `queueDirective` (stash-only). They
//     never touch VkImageView, VkImage, or the bridge slot pool.
//   - The render thread (VulkanRender::Impl::drawFrameOffscreen) calls
//     `acquireRenderTarget` and `submitRendered`. acquireRenderTarget
//     drains any pending directive at its head — between two frames,
//     after the previous frame's GPU work has fenced — and is the only
//     site that ever destroys/recreates VkImageViews or invokes
//     `ww_bridge_pool_apply_directive`. This makes the slot pool
//     single-writer; no quiesce barrier is needed.
//
// Lifecycle:
//   1) Caller creates the pool (`ww_bridge_pool_create`) and constructs
//      this swapchain.
//   2) Caller registers `setOnFirstNegotiated(...)` (e.g. `wp.play()`)
//      and calls `ww_bridge_pool_advertise_caps`.
//   3) Reader thread receives WW_REQ_NEGOTIATE_BUFFERS and calls
//      `queueDirective` — stashes the directive without applying it.
//   4) Render thread's next `acquireRenderTarget` drains the pending
//      directive, runs `ww_bridge_pool_apply_directive`, recreates
//      VkImageViews, then returns the chosen slot. On the first
//      successful application it fires the on-first-negotiated callback.
//   5) Each subsequent frame: `acquireRenderTarget` round-robins a slot,
//      VulkanRender records + submits + waits + exports sync_fd, then
//      `submitRendered` hands off to `ww_bridge_pool_submit_slot`.

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
    BridgeExSwapchain(ww_pool_t* pool, int sock,
                      VkDevice device, PFN_vkCreateImageView pfn_create_view,
                      PFN_vkDestroyImageView pfn_destroy_view);
    ~BridgeExSwapchain() override;

    // Stash a directive received from the daemon. Safe to call from any
    // thread — only the in-render-thread `acquireRenderTarget` ever reads
    // the stash. If a directive was already pending, it is dropped (the
    // newer directive supersedes it; the daemon's older negotiation is
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
    // change here (no other site touches m_views / m_slot_count).
    void poll() override { drainPendingDirective(); }

    bool acquireRenderTarget(wallpaper::vulkan::ImageParameters& out) override;
    void submitRendered(int acquire_sync_fd) override;

    unsigned width() const override  { return m_width; }
    unsigned height() const override { return m_height; }
    // Negotiated VkFormat — set by applyDirective from the picked
    // fourcc. Returns VK_FORMAT_UNDEFINED until the first directive is
    // applied. The render layer (VulkanRender::Impl::drawFrameOffscreen)
    // adapts FinPass's renderpass+pipeline whenever this changes.
    VkFormat format() const override { return m_export_format; }

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
    // applyDirective. After this returns, `m_views` / `m_slot_count` are
    // owned by the render thread for the rest of the frame.
    void drainPendingDirective();

    // Render-thread-only. Tears down old slot views, runs the bridge's
    // apply_directive, builds new VkImageViews. Returns:
    //   0   success — m_slot_count == directive.count
    //  <0   dry-run failure (bridge already emitted bind_failed); caller
    //       leaves m_slot_count = 0 and waits for the next directive
    //  >0   hard system error (logged); m_slot_count = 0
    int applyDirective(const ww_pool_directive_t& directive);

    void destroyViews();

    ww_pool_t*             m_pool { nullptr };
    int                    m_sock { -1 };
    VkDevice               m_device { VK_NULL_HANDLE };
    PFN_vkCreateImageView  m_pfn_create_view { nullptr };
    PFN_vkDestroyImageView m_pfn_destroy_view { nullptr };
    VkFormat               m_export_format { VK_FORMAT_UNDEFINED };

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

    std::array<VkImageView, kMaxSlots> m_views { VK_NULL_HANDLE };
};

} // namespace ww_wescene
