#pragma once
#include "SceneWallpaper.hpp"

#include <functional>
#include <string_view>
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
};

} // namespace wallpaper
