#pragma once

#include "RenderGraph/RenderGraph.hpp"
#include "SceneWallpaperSurface.hpp"
#include "Swapchain/ExSwapchain.hpp"
#include "Type.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vulkan/vulkan.h>

namespace wallpaper
{
class Scene;

namespace vulkan
{
class FinPass;

class VulkanRender {
public:
    VulkanRender();
    ~VulkanRender();

    bool init(RenderInitInfo);

    void destroy();

    void drawFrame(Scene&);

    void clearLastRenderGraph();
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void UpdateCameraFillMode(Scene&, wallpaper::FillMode);

    // Inform VulkanRender that the offscreen ExSwapchain has resolved or
    // changed its negotiated extent. Returns true iff the extent
    // changed — the caller (RenderHandler) should then
    // clearLastRenderGraph + compileRenderGraph + UpdateCameraFillMode
    // so scene render targets get re-sized.
    //
    // Format changes are out of scope here: drawFrameOffscreen's head
    // check picks them up via ExSwapchain::format() and rebuilds
    // FinPass. Internally: on extent change, WaitIdle +
    // Device::set_out_extent.
    //
    // Safe to call before any scene is loaded — m_pass_loaded gating
    // applies in drawFrame.
    bool onSwapchainReady(unsigned width, unsigned height);

    ExSwapchain* exSwapchain() const;
    bool inited() const;

    // Transfer ownership of the most recent frame's exported dma_fence
    // sync_file fd. Returns -1 if no frame has been rendered since the
    // last call (or export failed). Caller owns the returned fd and
    // must close() it. Thread-safe.
    int takeLastFrameSyncFd();

    // Look up the DRM render-node major/minor of the picked
    // VkPhysicalDevice via `VK_EXT_physical_device_drm`. Returns true
    // and writes `*out_major` / `*out_minor` on success; returns false
    // when the extension wasn't enabled by the driver or the device
    // doesn't expose a render node. Safe to call after `init()` has
    // returned successfully; before that or after `destroy()` the
    // result is `false`. Used by the waywallen-renderer host to fill
    // the `Ready` event so the daemon can match the renderer's GPU
    // against each display's GPU.
    bool getDrmRenderNode(uint32_t& out_major, uint32_t& out_minor) const;

    // Raw Vulkan handles for IPC backends that need to share the
    // VkDevice with another library (e.g. waywallen-bridge pool). Valid
    // after `init()` succeeded; nullptr / 0 otherwise.
    VkInstance       vkInstance() const;
    VkPhysicalDevice vkPhysicalDevice() const;
    VkDevice         vkDevice() const;
    VkQueue          vkGraphicsQueue() const;
    uint32_t         vkGraphicsQueueFamily() const;

    // Vulkan ID properties from VkPhysicalDeviceIDProperties. `out`
    // is a 16-byte buffer; on failure (extension missing) zeroes it.
    void deviceUuid(uint8_t out[16]) const;
    void driverUuid(uint8_t out[16]) const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
} // namespace vulkan
} // namespace wallpaper