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
// byte is "ignored" rather than meaningful alpha. FinPass's vkCmdBlitImage
// preserves logical RGBA channels regardless, and the wallpaper scene
// renders alpha=1.0 anyway, so X is folded onto the A variant.
VkFormat fourcc_to_vk_format(uint32_t fourcc) {
    // Mirrors waywallen/bridge/src/pool_vulkan.c::s_vk_fourcc_table and
    // waywallen-display/src/backend_vulkan.c::s_vk_fourcc_table — the
    // three sides must agree on the same advertised set since the
    // daemon negotiates by exact fourcc match.
    switch (fourcc) {
    case WW_DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_XBGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_XRGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_RGBA8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_BGRA8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_RGBX8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_BGRX8888: return VK_FORMAT_B8G8R8A8_UNORM;
    default:                     return VK_FORMAT_UNDEFINED;
    }
}

} // namespace


BridgeExSwapchain::BridgeExSwapchain(ww_pool_t* pool, int sock)
    : m_pool(pool), m_sock(sock) {}

BridgeExSwapchain::~BridgeExSwapchain() = default;

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

    // Bridge's apply_directive tears down old slots and allocates new
    // ones. Producer-side has nothing to wind down — FinPass's blit dst
    // is the bridge slot directly, no cached views or intermediates.
    m_slot_count   = 0;
    m_next_slot    = 0;
    m_have_pending = false;
    // Don't publish new geometry yet — wait for apply_directive to
    // succeed. If we publish early and apply fails, the render thread
    // sees format()/ready() reporting state that has no slots behind it.

    int rc = ww_bridge_pool_apply_directive(m_pool, m_sock, &directive);
    if (rc < 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: apply_directive dry-run failed: %d\n", rc);
        return rc;
    }
    if (rc > 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: apply_directive system error: %d\n", rc);
        return rc;
    }

    // The directive carries no extent (NegotiateBuffers wire dropped
    // extent_w/h since the renderer is the authority on render extent).
    // Read the actual slot dimensions back from the bridge — they were
    // sized from the `probe_width/height` we passed into advertise_caps.
    ww_pool_slot_t s0 {};
    if (int srx = ww_bridge_pool_acquire_slot(m_pool, 0, &s0); srx != 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: acquire_slot(0) post-apply failed: %d\n",
                     srx);
        return srx;
    }

    // Publish atomically only after apply_directive succeeded.
    m_width         = s0.width;
    m_height        = s0.height;
    m_fourcc        = directive.fourcc;
    m_export_format = picked;
    m_slot_count    = directive.count;
    return 0;
}

bool BridgeExSwapchain::acquireRenderTarget(wallpaper::vulkan::ImageParameters& out) {
    // Caller must invoke `poll()` first.
    if (m_slot_count == 0) return false;

    uint32_t idx = m_next_slot;
    m_next_slot  = (m_next_slot + 1) % m_slot_count;

    // Producer back-pressure on the bridge slot. Bridge contract
    // (pool.h wait_slot_release): non-zero return means "consumer still
    // using; render anyway". Producer-runs-ahead is documented.
    (void)ww_bridge_pool_wait_slot_release(m_pool, idx, /*timeout_ms*/ 16);

    ww_pool_slot_t s {};
    if (int rc = ww_bridge_pool_acquire_slot(m_pool, idx, &s); rc != 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: acquire_slot(%u) failed: %d\n", idx, rc);
        return false;
    }

    out.handle       = static_cast<VkImage>(s.vk_image);
    out.view         = VK_NULL_HANDLE; // FinPass blit needs only the handle.
    out.sampler      = VK_NULL_HANDLE;
    out.extent       = { s.width, s.height, 1 };
    out.mipmap_level = 1;

    m_pending_slot = idx;
    m_have_pending = true;
    return true;
}

void BridgeExSwapchain::submitRendered(int producer_sync_fd) {
    if (!m_have_pending) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return;
    }
    uint32_t slot  = m_pending_slot;
    m_have_pending = false;

    int rc = ww_bridge_pool_submit_slot(m_pool, m_sock, slot, producer_sync_fd);
    if (rc != 0) {
        std::fprintf(stderr,
                     "BridgeExSwapchain: submit_slot(%u) rc=%d\n", slot, rc);
        // Bridge contract: bridge always closes the fd. We don't dup-close.
    }
}

} // namespace ww_wescene
