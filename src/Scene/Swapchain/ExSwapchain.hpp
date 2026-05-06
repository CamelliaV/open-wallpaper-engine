#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vulkan/vulkan.h>

namespace owe
{

// Stays classic-attached because (a) ExSwapchain (classic virtual) takes
// a reference to it in `acquireRenderTarget`, and (b) the `wescene.vulkan`
// module-attached LocalExSwapchain / BridgeExSwapchain override that
// virtual — both sides must see the *same* type identity, so it can't
// be in module purview.
namespace vulkan {

struct ImageParameters {
    VkImage     handle {};
    VkImageView view {};
    VkSampler   sampler {};
    VkExtent3D  extent {};
    uint32_t    mipmap_level { 1 };

    ImageParameters()  = default;
    ~ImageParameters() = default;

    // Conversion helpers live as free functions in `wescene.vulkan` —
    // they can't be inline class members here because they read members
    // of the module-attached Vma/Ex types.
};

} // namespace vulkan

// Snapshot of the negotiated buffer set the producer should render into.
// Fired by ExSwapchain implementations whenever readiness flips or the
// negotiated extent/format changes.
struct ExSwapchainReadyEvent {
    bool     ready;
    unsigned width;
    unsigned height;
    VkFormat format;
};

enum class TexTiling
{
    OPTIMAL,
    LINEAR
};

// Per-slot DMA-BUF descriptor surfaced to the IPC layer. Local backend
// fills these from its self-allocated `ExImageParameters`; bridge backend
// keeps them empty (the bridge already published the metadata via
// `bind_buffers` itself).
struct ExHandle {
    int         fd { -1 };
    int32_t     width { 0 };
    int32_t     height { 0 };
    std::size_t size { 0 };

    uint32_t drm_fourcc { 0 };
    uint64_t drm_modifier { 0 };
    uint64_t plane0_offset { 0 };
    uint32_t plane0_stride { 0 };

    ExHandle() = default;
    ExHandle(int id): m_id(id) {};

    int32_t id() const { return m_id; }

private:
    int32_t m_id { 0 };
};

// Producer-side abstraction over the offscreen swapchain. Two
// implementations exist:
//   - LocalExSwapchain: self-allocates 3 DMA-BUF-backed VkImages via the
//     Vulkan TextureCache; the standalone viewers and any in-process host
//     drive bind_buffers / frame_ready themselves and consume `eatFrame()`
//     / `snapshot_all_slots()`.
//   - BridgeExSwapchain: wraps a `ww_pool_t`; bridge owns the slot images
//     and emits bind_buffers / frame_ready itself when the host calls
//     `submitRendered`. `eatFrame()` / `snapshot_all_slots()` return null
//     in this mode.
class ExSwapchain {
public:
    virtual ~ExSwapchain() = default;

    ExSwapchain(const ExSwapchain&)            = delete;
    ExSwapchain& operator=(const ExSwapchain&) = delete;
    ExSwapchain(ExSwapchain&&)                 = delete;
    ExSwapchain& operator=(ExSwapchain&&)      = delete;

    // Render-thread-only. Apply any pending IPC state (e.g. bridge
    // negotiate directives) so `format()` / `ready()` reflect the latest
    // resolved values *before* the caller commits to acquiring a slot.
    // Default: no-op (LocalExSwapchain's state is fully resolved at
    // construction; only the bridge backend needs this).
    //
    // Callers must invoke `poll()` before reading `format()` for the
    // purpose of deciding to rebuild downstream pipelines, and before
    // `acquireRenderTarget` if they want to bail out without leaking a
    // slot on a format-mismatch path.
    virtual void poll() {}

    // Pull the next slot to render into. Returns false when the swapchain
    // is not ready (e.g. bridge backend is between negotiations); the
    // render loop should skip this frame in that case.
    virtual bool acquireRenderTarget(vulkan::ImageParameters& out) = 0;

    // Hand the just-rendered slot back. Takes ownership of `acquire_sync_fd`;
    // pass -1 if export failed. The implementation closes the fd.
    virtual void submitRendered(int acquire_sync_fd) = 0;

    // Local-only: transfer ownership of the most recent rendered frame's
    // exported sync_file fd. Bridge backend always returns -1 (it consumes
    // the fd itself in submitRendered).
    virtual int takeLastFrameSyncFd() { return -1; }

    // Local-only producer-consumer ring helpers. Bridge backend returns
    // null / empty.
    virtual ExHandle* eatFrame() { return nullptr; }
    virtual std::array<ExHandle*, 3> snapshot_all_slots() {
        return { nullptr, nullptr, nullptr };
    }

    virtual unsigned width() const  = 0;
    virtual unsigned height() const = 0;
    virtual VkFormat format() const = 0;

    // Layout the slot ends in after FinPass finishes. FinPass blits into
    // the slot in TRANSFER_DST_OPTIMAL, then transitions to this layout
    // as part of its release barrier so the consumer reads coherent
    // pixels. Both backends pick GENERAL: bridge slot's foreign consumer
    // (KMS / display server) ignores Vulkan layout but expects a defined
    // transition; local in-process consumer mmaps the dma-buf — also
    // layout-agnostic.
    //
    // VulkanRender wires this to FinPass via setPresentLayout. Surface
    // mode does not consult this method — its layout is dictated by
    // VkSwapchainKHR (PRESENT_SRC_KHR).
    virtual VkImageLayout producerOutputLayout() const = 0;

    // Queue family to release the rendered slot to in FinPass's exit
    // barrier. Returns VK_QUEUE_FAMILY_FOREIGN_EXT for bridge (DMA-BUF
    // hand-off to a non-Vulkan consumer needs a release-to-FOREIGN to
    // flush GPU caches). Returns VK_QUEUE_FAMILY_IGNORED when no
    // transfer is needed (LocalExSwapchain's consumer is in-process).
    // VulkanRender translates IGNORED to graphics_queue.family_index
    // before handing off to FinPass, so FinPass's barrier-emitting
    // branch (`present_queue_index != graphics_queue.family_index`)
    // simply doesn't fire.
    virtual uint32_t releaseTargetQueueFamily() const = 0;

    // True iff the swapchain is ready to hand out render targets. Local
    // backend is always ready post-construction; bridge backend goes
    // ready once the first NEGOTIATE directive succeeds (slot count > 0
    // and format != VK_FORMAT_UNDEFINED) and may flip back to not-ready
    // briefly mid-renegotiation.
    virtual bool ready() const = 0;

    // Register a callback fired whenever readiness or the negotiated
    // extent/format changes. Threading model:
    //   - LocalExSwapchain: fired once synchronously inside
    //     setOnReadyChanged with the current snapshot, then never again.
    //   - BridgeExSwapchain: fired from the render thread inside
    //     drainPendingDirective whenever applyDirective succeeds.
    // The callback must NOT call back into the swapchain (avoids
    // re-entry); it should just post a message to its own looper.
    // Replacing the callback with a different one drops the previous
    // one; passing `{}` clears the registration.
    virtual void
    setOnReadyChanged(std::function<void(const ExSwapchainReadyEvent&)>) = 0;

protected:
    ExSwapchain() = default;
};

} // namespace owe
