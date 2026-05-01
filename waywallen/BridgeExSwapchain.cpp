#include "BridgeExSwapchain.hpp"

#include <waywallen-bridge/drm_fourcc.h>

#include <cstdio>
#include <unistd.h>
#include <utility>

namespace ww_wescene
{

namespace
{

// fourcc → VkFormat for the producer-side image view.
//
// DRM fourcc names channels MSB→LSB in a little-endian uint32; VkFormat
// names channels in memory byte order. Hence the byte-for-byte equiv:
//   DRM_FORMAT_ABGR8888  (mem: R G B A) ↔ VK_FORMAT_R8G8B8A8_UNORM
//   DRM_FORMAT_ARGB8888  (mem: B G R A) ↔ VK_FORMAT_B8G8R8A8_UNORM
//
// X variants share the byte layout of their A counterpart — the 4th
// byte is "ignored" rather than meaningful alpha. Vulkan render targets
// cannot use non-identity component swizzle to force A=ONE on the view,
// so X is folded onto the A variant here and FinPass's fragment shader
// writes alpha=1.0 unconditionally. That keeps producer output valid
// for both X (consumer ignores alpha) and A (consumer sees fully opaque)
// without needing to rebuild FinPass on X↔A re-negotiation.
VkFormat fourcc_to_vk_format(uint32_t fourcc) {
    switch (fourcc) {
    case WW_DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_XBGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_XRGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    default:                     return VK_FORMAT_UNDEFINED;
    }
}

} // namespace


BridgeExSwapchain::BridgeExSwapchain(ww_pool_t* pool, int sock,
                                     VkDevice device,
                                     PFN_vkCreateImageView pfn_create_view,
                                     PFN_vkDestroyImageView pfn_destroy_view)
    : m_pool(pool),
      m_sock(sock),
      m_device(device),
      m_pfn_create_view(pfn_create_view),
      m_pfn_destroy_view(pfn_destroy_view) {}

BridgeExSwapchain::~BridgeExSwapchain() {
    destroyViews();
}

void BridgeExSwapchain::destroyViews() {
    if (!m_pfn_destroy_view || m_device == VK_NULL_HANDLE) return;
    for (auto& v : m_views) {
        if (v != VK_NULL_HANDLE) {
            m_pfn_destroy_view(m_device, v, nullptr);
            v = VK_NULL_HANDLE;
        }
    }
}

void BridgeExSwapchain::queueDirective(const ww_pool_directive_t& directive) {
    {
        std::lock_guard<std::mutex> lk(m_pending_mu);
        m_pending_directive = directive;
    }
    m_pending_valid.store(true, std::memory_order_release);
}

void BridgeExSwapchain::drainPendingDirective() {
    if (!m_pending_valid.load(std::memory_order_acquire)) return;

    ww_pool_directive_t d {};
    {
        std::lock_guard<std::mutex> lk(m_pending_mu);
        d = m_pending_directive;
        m_pending_valid.store(false, std::memory_order_release);
    }

    int rc = applyDirective(d);
    if (rc != 0) return; // applyDirective already logged; m_slot_count = 0

    // Snapshot callbacks under the lock and invoke unlocked so the
    // handler can re-enter the swapchain (read width/height) without
    // blocking — only the render thread ever invokes them anyway.
    std::function<void()>                                            first_cb;
    std::function<void(const wallpaper::ExSwapchainReadyEvent&)>     ready_cb;
    {
        std::lock_guard<std::mutex> lk(m_cb_mu);
        if (!m_first_negotiated_done && m_on_first_negotiated) {
            first_cb                = m_on_first_negotiated;
            m_first_negotiated_done = true;
        }
        ready_cb = m_on_ready_changed;
    }
    if (first_cb) first_cb();
    if (ready_cb) {
        wallpaper::ExSwapchainReadyEvent e {
            .ready  = true,
            .width  = m_width,
            .height = m_height,
            .format = m_export_format,
        };
        ready_cb(e);
    }
}

int BridgeExSwapchain::applyDirective(const ww_pool_directive_t& directive) {
    VkFormat picked = fourcc_to_vk_format(directive.fourcc);
    if (picked == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: fourcc 0x%08x has no VkFormat mapping\n",
                     directive.fourcc);
        ww_bridge_send_bind_failed(m_sock,
                                   directive.fourcc, directive.modifier,
                                   /*reason*/ 1,
                                   "fourcc unsupported by producer");
        return -EINVAL;
    }
    if (directive.count == 0 || directive.count > kMaxSlots) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: invalid slot count %u (cap=%u)\n",
                     directive.count, kMaxSlots);
        return 1;
    }

    // Bridge contract (pool.h apply_directive step 2): the call itself
    // tears down the previous slots, so old `vk_image` handles are
    // invalidated synchronously. We must destroy our VkImageViews bound
    // to those VkImages *before* invoking apply_directive — atomic
    // replacement is not possible.
    //
    // GPU work for the previous frame has already fenced inside
    // VulkanRender::Impl::drawFrameOffscreen (rr.fence_frame.Wait), so
    // tearing down here cannot UAF a live submit.
    destroyViews();
    m_slot_count   = 0;
    m_next_slot    = 0;
    m_have_pending = false;
    // Don't publish the new format/extent yet — wait for apply_directive
    // to succeed. If we publish early and apply_directive fails, the
    // render thread sees format() = picked with no slots available, and
    // drawFrameOffscreen wastes a rebuildPresent on a format we'll
    // never actually render to.

    int rc = ww_bridge_pool_apply_directive(m_pool, m_sock, &directive);
    if (rc < 0) {
        // Dry-run failure (bridge already sent bind_failed). Daemon will
        // re-pick; we stay slot-less until the next directive.
        std::fprintf(stderr,
                     "BridgeExSwapchain: apply_directive dry-run failed: %d\n", rc);
        // m_export_format intentionally untouched — keeps last good state.
        return rc;
    }
    if (rc > 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: apply_directive system error: %d\n", rc);
        return rc;
    }

    for (uint32_t i = 0; i < directive.count; ++i) {
        ww_pool_slot_t s {};
        if (int arc = ww_bridge_pool_acquire_slot(m_pool, i, &s); arc != 0) {
            std::fprintf(stderr,
                         "BridgeExSwapchain: acquire_slot(%u) after directive failed: %d\n",
                         i, arc);
            destroyViews();
            return 1;
        }

        VkImageViewCreateInfo vci {};
        vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image    = static_cast<VkImage>(s.vk_image);
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = picked;
        vci.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        };
        vci.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
        };
        VkImageView view = VK_NULL_HANDLE;
        VkResult vr = m_pfn_create_view(m_device, &vci, nullptr, &view);
        if (vr != VK_SUCCESS) {
            std::fprintf(stderr,
                         "BridgeExSwapchain: vkCreateImageView slot %u: %d\n", i, vr);
            destroyViews();
            return 1;
        }
        m_views[i] = view;
    }

    // Publish atomically only after every slot view succeeded.
    m_width         = directive.width;
    m_height        = directive.height;
    m_fourcc        = directive.fourcc;
    m_export_format = picked;
    m_slot_count    = directive.count;
    return 0;
}

bool BridgeExSwapchain::acquireRenderTarget(wallpaper::vulkan::ImageParameters& out) {
    // Caller must invoke `poll()` first to drain any pending directive.
    // Splitting drain out of acquire lets the caller inspect format()
    // and rebuild downstream pipelines *before* committing to a slot —
    // a post-acquire failure would otherwise leak the slot (no
    // `cancelRenderTarget` exists; bridge only releases on submit).
    if (m_slot_count == 0) return false;

    uint32_t idx = m_next_slot;
    m_next_slot  = (m_next_slot + 1) % m_slot_count;

    // Producer back-pressure — bounded wait. Bridge contract (pool.h
    // wait_slot_release): non-zero return means "consumer still using
    // the buffer; render anyway". So this is purely a politeness wait,
    // not a synchronization point. Cap at one ~60Hz frame so a stuck
    // consumer can't pin the render thread for 250ms (15 frames at
    // 60Hz). Producer-runs-ahead is the documented behaviour.
    (void)ww_bridge_pool_wait_slot_release(m_pool, idx, /*timeout_ms*/ 16);

    ww_pool_slot_t s {};
    if (int rc = ww_bridge_pool_acquire_slot(m_pool, idx, &s); rc != 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: acquire_slot(%u) failed: %d\n", idx, rc);
        return false;
    }

    out.handle       = static_cast<VkImage>(s.vk_image);
    out.view         = m_views[idx];
    out.sampler      = VK_NULL_HANDLE;
    out.extent       = { s.width, s.height, 1 };
    out.mipmap_level = 1;

    m_pending_slot = idx;
    m_have_pending = true;
    return true;
}

void BridgeExSwapchain::submitRendered(int acquire_sync_fd) {
    // Render-thread-only; no lock needed. m_have_pending / m_pending_slot
    // were last written by the same thread inside acquireRenderTarget.
    if (!m_have_pending) {
        if (acquire_sync_fd >= 0) ::close(acquire_sync_fd);
        return;
    }
    uint32_t slot = m_pending_slot;
    m_have_pending = false;

    int rc = ww_bridge_pool_submit_slot(m_pool, m_sock, slot, acquire_sync_fd);
    if (rc != 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: submit_slot(%u) rc=%d\n", slot, rc);
        // Bridge contract: bridge always closes the fd. We don't dup-close.
    }
}

} // namespace ww_wescene
