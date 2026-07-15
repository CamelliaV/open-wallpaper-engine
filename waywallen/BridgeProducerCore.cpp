module;

#include <cerrno>
#include <cstdio>
#include <unistd.h>

module waywallen.bridge_producer_core;

import rstd.cppstd;
import vulkan;
import waywallen.bridge;
import waywallen.bridge_session;

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
// byte is "ignored" rather than meaningful alpha. The producer's blit
// preserves logical RGBA channels regardless, and a wallpaper renders
// alpha=1.0 anyway, so X is folded onto the A variant.
//
// Mirrors waywallen/bridge/src/pool_vulkan.c::s_vk_fourcc_table and
// waywallen-display/src/backend_vulkan.c::s_vk_fourcc_table — the
// three sides must agree on the same advertised set since the
// daemon negotiates by exact fourcc match.
VkFormat fourcc_to_vk_format(uint32_t fourcc) {
    switch (fourcc) {
    case WW_DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_XBGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_XRGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_RGBA8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_BGRA8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case WW_DRM_FORMAT_RGBX8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case WW_DRM_FORMAT_BGRX8888: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

} // namespace

BridgeProducerCore::BridgeProducerCore(std::shared_ptr<BridgeSession> session)
    : m_session(std::move(session)) {}

BridgeProducerCore::~BridgeProducerCore() = default;

void BridgeProducerCore::queueDirective(const ww_pool_directive_t& directive) {
    {
        std::lock_guard<std::mutex> lk(m_pending_mu);
        m_pending_directive = directive;
    }
    m_pending_valid.store(true, std::memory_order_release);
}

void BridgeProducerCore::drainPendingDirective() {
    if (! m_pending_valid.load(std::memory_order_acquire)) return;

    ww_pool_directive_t d {};
    {
        std::lock_guard<std::mutex> lk(m_pending_mu);
        d = m_pending_directive;
        m_pending_valid.store(false, std::memory_order_release);
    }

    int rc = applyDirective(d);
    if (rc != 0) return; // applyDirective already logged; m_slot_count = 0

    // Snapshot callbacks under the lock and invoke unlocked so the
    // handler can re-enter the core (read width/height) without
    // blocking — only the producer thread ever invokes them anyway.
    std::function<void()>                        first_cb;
    std::function<void(const BridgeReadyEvent&)> ready_cb;
    {
        std::lock_guard<std::mutex> lk(m_cb_mu);
        if (! m_first_negotiated_done && m_on_first_negotiated) {
            first_cb                = m_on_first_negotiated;
            m_first_negotiated_done = true;
        }
        ready_cb = m_on_ready_changed;
    }
    if (first_cb) first_cb();
    if (ready_cb) {
        BridgeReadyEvent e {
            .ready  = true,
            .width  = m_width,
            .height = m_height,
            .format = m_export_format,
        };
        ready_cb(e);
    }
}

int BridgeProducerCore::applyDirective(const ww_pool_directive_t& directive) {
    VkFormat picked = fourcc_to_vk_format(directive.fourcc);
    if (picked == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr,
                     "BridgeProducerCore: fourcc 0x%08x has no VkFormat mapping\n",
                     directive.fourcc);
        m_session->sendBindFailed(directive.fourcc,
                                  directive.modifier,
                                  /*reason*/ 1,
                                  "fourcc unsupported by producer");
        return -EINVAL;
    }
    if (directive.count == 0 || directive.count > kMaxSlots) {
        std::fprintf(stderr,
                     "BridgeProducerCore: invalid slot count %u (cap=%u)\n",
                     directive.count,
                     kMaxSlots);
        return 1;
    }

    // Bridge's apply_directive tears down old slots and allocates new
    // ones. Producer-side has nothing to wind down — the blit dst is
    // the bridge slot directly, no cached views or intermediates.
    m_slot_count       = 0;
    m_next_slot        = 0;
    m_have_pending     = false;
    m_pending_identity = {};
    // Don't publish new geometry yet — wait for apply_directive to
    // succeed. If we publish early and apply fails, the producer sees
    // format()/ready() reporting state that has no slots behind it.

    int rc = m_session->applyDirective(directive);
    if (rc < 0) {
        std::fprintf(stderr, "BridgeProducerCore: apply_directive dry-run failed: %d\n", rc);
        return rc;
    }
    if (rc > 0) {
        std::fprintf(stderr, "BridgeProducerCore: apply_directive system error: %d\n", rc);
        return rc;
    }

    // The directive carries no extent (NegotiateBuffers wire dropped
    // extent_w/h since the renderer is the authority on render extent).
    // Read the actual slot dimensions back from the bridge — they were
    // sized from the `probe_width/height` we passed into advertise_caps.
    ww_pool_slot_t s0 {};
    if (int srx = m_session->acquireSlot(0, s0); srx != 0) {
        std::fprintf(stderr, "BridgeProducerCore: acquire_slot(0) post-apply failed: %d\n", srx);
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

BridgeSlotAcquireResult BridgeProducerCore::acquireSlot(uint32_t timeout_ms) {
    BridgeSlotAcquireResult result;
    if (m_slot_count == 0) return result;
    if (m_have_pending) {
        result.status     = BridgeSlotAcquireStatus::Error;
        result.error_code = -EBUSY;
        return result;
    }

    uint32_t idx = m_next_slot;
    m_next_slot  = (m_next_slot + 1) % m_slot_count;

    ww_pool_slot_acquire_result_t acquired {};
    if (int rc = m_session->acquireSlotForRender(idx, timeout_ms, acquired); rc != 0) {
        result.status     = BridgeSlotAcquireStatus::Error;
        result.error_code = rc;
        return result;
    }

    switch (acquired.status) {
    case WW_POOL_SLOT_ACQUIRE_READY_UNUSED:
        result.status = BridgeSlotAcquireStatus::ReadyUnused;
        break;
    case WW_POOL_SLOT_ACQUIRE_READY_RELEASED:
        result.status = BridgeSlotAcquireStatus::ReadyReleased;
        break;
    case WW_POOL_SLOT_ACQUIRE_BUSY: result.status = BridgeSlotAcquireStatus::Busy; return result;
    case WW_POOL_SLOT_ACQUIRE_FORCED_RELEASE:
        result.status = BridgeSlotAcquireStatus::ForcedRelease;
        return result;
    case WW_POOL_SLOT_ACQUIRE_SESSION_LOST:
        result.status     = BridgeSlotAcquireStatus::SessionLost;
        result.error_code = acquired.error_code;
        return result;
    case WW_POOL_SLOT_ACQUIRE_ERROR:
    default:
        result.status     = BridgeSlotAcquireStatus::Error;
        result.error_code = acquired.error_code;
        return result;
    }

    uint64_t acquire_serial = m_next_acquire_serial++;
    if (acquire_serial == 0) {
        result.status     = BridgeSlotAcquireStatus::Error;
        result.error_code = -EOVERFLOW;
        return result;
    }

    result.identity = BridgeSlotIdentity {
        .bind_generation        = acquired.bind_generation,
        .slot_index             = acquired.slot.index,
        .previous_release_point = acquired.previous_release_point,
        .acquire_serial         = acquire_serial,
    };
    result.image  = static_cast<VkImage>(acquired.slot.vk_image);
    result.width  = acquired.slot.width;
    result.height = acquired.slot.height;
    if (! result.acquired()) {
        result.status     = BridgeSlotAcquireStatus::Error;
        result.error_code = -EINVAL;
        return result;
    }

    m_pending_slot     = idx;
    m_pending_identity = result.identity;
    m_have_pending     = true;
    return result;
}

bool BridgeProducerCore::acquireSlot(VkImage* out_image, uint32_t* out_width,
                                     uint32_t* out_height) {
    auto result = acquireSlot(16);
    if (! result.acquired()) return false;

    if (out_image) *out_image = result.image;
    if (out_width) *out_width = result.width;
    if (out_height) *out_height = result.height;
    return true;
}

BridgeSlotCompletionResult BridgeProducerCore::submitSlot(const BridgeSlotIdentity& identity,
                                                          int producer_sync_fd) {
    if (! m_have_pending) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return BridgeSlotCompletionResult {
            .status   = BridgeSlotCompletionStatus::NotPending,
            .identity = identity,
        };
    }
    if (identity != m_pending_identity) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return BridgeSlotCompletionResult {
            .status   = BridgeSlotCompletionStatus::StaleIdentity,
            .identity = identity,
        };
    }

    uint32_t slot      = m_pending_slot;
    m_have_pending     = false;
    m_pending_identity = {};

    ww_pool_slot_submit_result_t submitted {};
    int rc = m_session->submitSlotForRender(slot, producer_sync_fd, submitted);
    if (rc != 0) {
        std::fprintf(stderr, "BridgeProducerCore: submit_slot(%u) rc=%d\n", slot, rc);
        // Bridge contract: bridge always closes the fd. We don't dup-close.
        return BridgeSlotCompletionResult {
            .status     = BridgeSlotCompletionStatus::ProtocolError,
            .identity   = identity,
            .error_code = rc,
        };
    }
    if (submitted.status == WW_POOL_SLOT_SUBMIT_SESSION_LOST) {
        return BridgeSlotCompletionResult {
            .status     = BridgeSlotCompletionStatus::SessionLost,
            .identity   = identity,
            .error_code = submitted.error_code,
        };
    }
    if (submitted.status != WW_POOL_SLOT_SUBMIT_SUBMITTED) {
        return BridgeSlotCompletionResult {
            .status     = BridgeSlotCompletionStatus::ProtocolError,
            .identity   = identity,
            .error_code = submitted.error_code,
        };
    }
    return BridgeSlotCompletionResult {
        .status   = BridgeSlotCompletionStatus::Submitted,
        .identity = identity,
    };
}

BridgeSlotCompletionResult BridgeProducerCore::abortSlot(const BridgeSlotIdentity& identity) {
    if (! m_have_pending) {
        return BridgeSlotCompletionResult {
            .status   = BridgeSlotCompletionStatus::NotPending,
            .identity = identity,
        };
    }
    if (identity != m_pending_identity) {
        return BridgeSlotCompletionResult {
            .status   = BridgeSlotCompletionStatus::StaleIdentity,
            .identity = identity,
        };
    }

    m_have_pending     = false;
    m_pending_identity = {};
    return BridgeSlotCompletionResult {
        .status   = BridgeSlotCompletionStatus::Aborted,
        .identity = identity,
    };
}

void BridgeProducerCore::submitSlot(int producer_sync_fd) {
    if (! m_have_pending) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return;
    }
    (void)submitSlot(m_pending_identity, producer_sync_fd);
}

} // namespace ww_wescene
