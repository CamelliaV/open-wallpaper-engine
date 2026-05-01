#pragma once
#include "SceneWallpaper.hpp"
#include "Swapchain/ExSwapchain.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>

namespace wallpaper
{
using ReDrawCB = std::function<void()>;

struct VulkanSurfaceInfo {
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> createSurfaceOp;
    std::vector<std::string>                           instanceExts;
};

struct RenderInitInfo {
    bool enable_valid_layer { false };
    bool offscreen { false };

    std::span<const std::uint8_t> uuid;
    TexTiling                     offscreen_tiling { TexTiling::OPTIMAL };
    /* When true, allocate the offscreen ExSwapchain images out of
     * HOST_VISIBLE && !DEVICE_LOCAL (true GTT) so the exported dmabuf
     * fds are importable by a foreign GPU (cross-GPU PRIME). Ignored
     * when offscreen == false. */
    bool                          offscreen_host_visible { false };
    VulkanSurfaceInfo             surface_info;

    uint16_t width { 1920 };
    uint16_t height { 1080 };
    ReDrawCB redraw_callback;

    /* When set AND `offscreen == true`, VulkanRender invokes this factory
     * after picking the GPU and creating the VkDevice, and adopts the
     * returned swapchain instead of allocating its own LocalExSwapchain.
     * Used by the waywallen-wescene host to construct a BridgeExSwapchain
     * around a ww_bridge_pool created with the just-picked device.
     * Ignored on the on-screen path. */
    struct ExSwapchainHandles {
        VkInstance       instance;
        VkPhysicalDevice physical_device;
        VkDevice         device;
        VkQueue          graphics_queue;
        uint32_t         graphics_queue_family;
    };
    std::function<std::unique_ptr<ExSwapchain>(const ExSwapchainHandles&)>
        ex_swapchain_factory;
};

} // namespace wallpaper
