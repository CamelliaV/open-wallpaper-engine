module;

#include <unistd.h>
#include <cerrno>

module waywallen.bridge_ex_swapchain;

import rstd.cppstd;
import wescene.vulkan;
import waywallen.bridge_producer_core;

namespace ww_wescene
{

BridgeExSwapchain::BridgeExSwapchain(std::shared_ptr<BridgeSession> session)
    : m_core(std::move(session)) {}

BridgeExSwapchain::~BridgeExSwapchain() = default;

namespace
{

owe::FrameSurfaceIdentity ToFrameIdentity(const BridgeSlotIdentity& identity) {
    return { .owner_generation = identity.bind_generation,
             .image_index      = identity.slot_index,
             .acquire_serial   = identity.acquire_serial };
}

owe::FrameSurfaceCompletionStatus ToFrameCompletionStatus(BridgeSlotCompletionStatus status) {
    switch (status) {
    case BridgeSlotCompletionStatus::Submitted: return owe::FrameSurfaceCompletionStatus::Submitted;
    case BridgeSlotCompletionStatus::Aborted: return owe::FrameSurfaceCompletionStatus::Aborted;
    case BridgeSlotCompletionStatus::NotPending:
        return owe::FrameSurfaceCompletionStatus::NotPending;
    case BridgeSlotCompletionStatus::StaleIdentity:
        return owe::FrameSurfaceCompletionStatus::StaleIdentity;
    case BridgeSlotCompletionStatus::SessionLost:
        return owe::FrameSurfaceCompletionStatus::SessionLost;
    case BridgeSlotCompletionStatus::ProtocolError:
        return owe::FrameSurfaceCompletionStatus::ProtocolError;
    }
    return owe::FrameSurfaceCompletionStatus::ProtocolError;
}

} // namespace

owe::FrameSurfaceAcquireResult BridgeExSwapchain::acquireRenderTarget() {
    auto acquired = m_core.acquireSlot();
    if (! acquired.acquired()) {
        switch (acquired.status) {
        case BridgeSlotAcquireStatus::Busy:
            return { .status = owe::FrameSurfaceAcquireStatus::Busy };
        case BridgeSlotAcquireStatus::NotReady:
            return { .status = owe::FrameSurfaceAcquireStatus::NotReady };
        case BridgeSlotAcquireStatus::SessionLost:
            return { .status     = owe::FrameSurfaceAcquireStatus::SessionLost,
                     .error_code = acquired.error_code };
        case BridgeSlotAcquireStatus::Error:
            return { .status     = owe::FrameSurfaceAcquireStatus::ProtocolError,
                     .error_code = acquired.error_code };
        case BridgeSlotAcquireStatus::ReadyUnused:
        case BridgeSlotAcquireStatus::ReadyReleased: break;
        }
        return { .status = owe::FrameSurfaceAcquireStatus::ProtocolError, .error_code = -EINVAL };
    }

    owe::vulkan::ImageParameters target;
    target.handle       = acquired.image;
    target.view         = VK_NULL_HANDLE;
    target.sampler      = VK_NULL_HANDLE;
    target.extent       = { acquired.width, acquired.height, 1 };
    target.mipmap_level = 1;
    owe::FrameSurfaceLease lease {
        .identity             = ToFrameIdentity(acquired.identity),
        .reuse                = { .kind          = acquired.status == BridgeSlotAcquireStatus::ReadyUnused
                                                       ? owe::FrameSurfaceReuseKind::NeverSubmitted
                                                       : owe::FrameSurfaceReuseKind::ConsumerReleased,
                                  .release_point = acquired.identity.previous_release_point },
        .image                = target,
        .format               = m_core.format(),
        .initial_layout       = VK_IMAGE_LAYOUT_GENERAL,
        .initial_queue_family = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .acquire              = { .kind = owe::FrameSurfaceAcquireKind::ExternalProtocol },
        .final_layout         = VK_IMAGE_LAYOUT_GENERAL,
        .final_queue_family   = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .discard_content      = true,
    };
    if (! lease.valid()) {
        (void)m_core.abortSlot(acquired.identity);
        return { .status = owe::FrameSurfaceAcquireStatus::ProtocolError, .error_code = -EINVAL };
    }

    m_pending_identity = acquired.identity;
    auto completion    = MakeCompletionCapability(lease.identity);
    return { .status     = owe::FrameSurfaceAcquireStatus::Acquired,
             .lease      = std::move(lease),
             .completion = std::move(completion) };
}

owe::FrameSurfaceCompletionResult
BridgeExSwapchain::CompleteRendered(owe::FrameSurfaceIdentity identity, int producer_sync_fd) {
    if (! m_pending_identity.has_value()) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return { .status = owe::FrameSurfaceCompletionStatus::NotPending, .identity = identity };
    }
    if (ToFrameIdentity(*m_pending_identity) != identity) {
        if (producer_sync_fd >= 0) ::close(producer_sync_fd);
        return { .status = owe::FrameSurfaceCompletionStatus::StaleIdentity, .identity = identity };
    }

    auto core_identity = *m_pending_identity;
    m_pending_identity.reset();
    auto result = m_core.submitSlot(core_identity, producer_sync_fd);
    return { .status     = ToFrameCompletionStatus(result.status),
             .identity   = identity,
             .error_code = result.error_code };
}

owe::FrameSurfaceCompletionResult
BridgeExSwapchain::AbortRenderTarget(owe::FrameSurfaceIdentity identity) {
    if (! m_pending_identity.has_value()) {
        return { .status = owe::FrameSurfaceCompletionStatus::NotPending, .identity = identity };
    }
    if (ToFrameIdentity(*m_pending_identity) != identity) {
        return { .status = owe::FrameSurfaceCompletionStatus::StaleIdentity, .identity = identity };
    }

    auto core_identity = *m_pending_identity;
    m_pending_identity.reset();
    auto result = m_core.abortSlot(core_identity);
    return { .status     = ToFrameCompletionStatus(result.status),
             .identity   = identity,
             .error_code = result.error_code };
}

} // namespace ww_wescene
