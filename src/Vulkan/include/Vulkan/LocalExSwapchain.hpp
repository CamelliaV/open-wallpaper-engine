#pragma once

#include "Swapchain/ExSwapchain.hpp"
#include "Swapchain/TripleSwapchain.hpp"
#include "Device.hpp"
#include "Parameters.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <unistd.h>

namespace wallpaper
{
namespace vulkan
{

struct LocalExHandle : NoCopy {
    ExHandle          handle;
    ExImageParameters image;

    LocalExHandle()  = default;
    ~LocalExHandle() = default;
    LocalExHandle(LocalExHandle&& o) noexcept: handle(o.handle), image(std::move(o.image)) {}
    LocalExHandle& operator=(LocalExHandle&& o) noexcept {
        handle = o.handle;
        image  = std::move(o.image);
        return *this;
    }
};

// Self-allocated DMA-BUF triple buffer. Used by standalone viewers and
// any in-process host that drives the IPC layer itself. The IPC layer
// polls `eatFrame()` / `snapshot_all_slots()` to learn slot identity and
// reads `takeLastFrameSyncFd()` after each frame to ship to consumers.
class LocalExSwapchain final : public ExSwapchain, private TripleSwapchain<ExHandle> {
public:
    LocalExSwapchain(std::array<LocalExHandle, 3> handles, VkExtent2D ext)
        : m_handles(std::move(handles)), m_extent(ext) {
        int index = 0;
        for (auto& h : m_handles) {
            auto& handle         = h.handle;
            handle               = ExHandle(index++);
            handle.width         = (i32)h.image.extent.width;
            handle.height        = (i32)h.image.extent.height;
            handle.fd            = h.image.fd;
            handle.size          = h.image.mem_reqs.size;
            handle.drm_fourcc    = h.image.drm_fourcc;
            handle.drm_modifier  = h.image.drm_modifier;
            handle.plane0_offset = h.image.plane0_offset;
            handle.plane0_stride = h.image.plane0_stride;
        }
        m_presented  = &m_handles[0].handle;
        m_ready      = &m_handles[1].handle;
        m_inprogress = &m_handles[2].handle;
    }

    ~LocalExSwapchain() override {
        int fd = m_last_sync_fd.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) ::close(fd);
    }

    // ExSwapchain interface ---------------------------------------------

    bool acquireRenderTarget(ImageParameters& out) override {
        out = m_handles.at((std::size_t)(*this->inprogress()).id()).image;
        return true;
    }

    void submitRendered(int acquire_sync_fd) override {
        if (acquire_sync_fd >= 0) {
            int old = m_last_sync_fd.exchange(acquire_sync_fd, std::memory_order_acq_rel);
            if (old >= 0) ::close(old);
        }
        TripleSwapchain<ExHandle>::renderFrame();
    }

    int takeLastFrameSyncFd() override {
        return m_last_sync_fd.exchange(-1, std::memory_order_acq_rel);
    }

    ExHandle* eatFrame() override { return TripleSwapchain<ExHandle>::eatFrame(); }
    std::array<ExHandle*, 3> snapshot_all_slots() override {
        return TripleSwapchain<ExHandle>::snapshot_all_slots();
    }

    unsigned width() const override  { return m_extent.width; }
    unsigned height() const override { return m_extent.height; }
    VkFormat format() const override { return VK_FORMAT_R8G8B8A8_UNORM; }

    // LocalExSwapchain's images are GENERAL throughout — see CreateExTex
    // which transitions to GENERAL once after allocation; the consumer
    // (host process mmap) doesn't care about Vulkan layout.
    VkImageLayout producerOutputLayout() const override {
        return VK_IMAGE_LAYOUT_GENERAL;
    }

    // Local backend is ready as soon as the constructor returns —
    // images are pre-allocated and the format is fixed.
    bool ready() const override { return true; }

    // Synchronously fire the callback once with the current snapshot.
    // The local backend never changes state, so subsequent reads of
    // width/height/format are stable.
    void setOnReadyChanged(
        std::function<void(const ExSwapchainReadyEvent&)> cb) override {
        if (cb) {
            ExSwapchainReadyEvent e {
                .ready  = true,
                .width  = m_extent.width,
                .height = m_extent.height,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
            };
            cb(e);
        }
    }

protected:
    std::atomic<ExHandle*>& presented()  override { return m_presented; }
    std::atomic<ExHandle*>& ready()      override { return m_ready; }
    std::atomic<ExHandle*>& inprogress() override { return m_inprogress; }

private:
    std::array<LocalExHandle, 3> m_handles;
    std::atomic<ExHandle*>       m_presented { nullptr };
    std::atomic<ExHandle*>       m_ready { nullptr };
    std::atomic<ExHandle*>       m_inprogress { nullptr };
    VkExtent2D                   m_extent;
    std::atomic<int>             m_last_sync_fd { -1 };
};

inline std::unique_ptr<LocalExSwapchain> CreateLocalExSwapchain(const Device& device,
                                                                unsigned w, unsigned h,
                                                                VkImageTiling tiling) {
    std::array<LocalExHandle, 3> handles;
    for (auto& handle : handles) {
        if (auto rv = device.tex_cache().CreateExTex(w, h, VK_FORMAT_R8G8B8A8_UNORM, tiling);
            rv.has_value())
            handle.image = std::move(rv.value());
        else
            return nullptr;
    }
    return std::make_unique<LocalExSwapchain>(std::move(handles), VkExtent2D { w, h });
}

} // namespace vulkan
} // namespace wallpaper
