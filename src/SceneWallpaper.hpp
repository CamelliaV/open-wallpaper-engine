#pragma once
#include <cstdint>
#include <memory>
#include <string_view>
#include <functional>
#include <vulkan/vulkan.h>
#include "Type.hpp"
#include "Swapchain/ExSwapchain.hpp"

namespace wallpaper
{

using FirstFrameCallback = std::function<void()>;

constexpr std::string_view PROPERTY_SOURCE               = "source";
constexpr std::string_view PROPERTY_ASSETS               = "assets";
constexpr std::string_view PROPERTY_FPS                  = "fps";
constexpr std::string_view PROPERTY_FILLMODE             = "fillmode";
constexpr std::string_view PROPERTY_SPEED                = "speed";
constexpr std::string_view PROPERTY_GRAPHIVZ             = "graphivz";
constexpr std::string_view PROPERTY_VOLUME               = "volume";
constexpr std::string_view PROPERTY_MUTED                = "muted";
constexpr std::string_view PROPERTY_CACHE_PATH           = "cache_path";
constexpr std::string_view PROPERTY_FIRST_FRAME_CALLBACK = "first_frame_callback";

#include "Core/NoCopyMove.hpp"
class MainHandler;
struct RenderInitInfo;

class SceneWallpaper : NoCopy {
public:
    SceneWallpaper();
    ~SceneWallpaper();
    bool init();
    bool inited() const;

    void initVulkan(RenderInitInfo);

    void play();
    void pause();
    void mouseInput(double x, double y);

    void setPropertyBool(std::string_view, bool);
    void setPropertyInt32(std::string_view, int32_t);
    void setPropertyFloat(std::string_view, float);
    void setPropertyString(std::string_view, std::string);
    void setPropertyObject(std::string_view, std::shared_ptr<void>);

    ExSwapchain* exSwapchain() const;

    // Ownership-transfer getter for the dma_fence sync_file fd that
    // was exported after the most recent completed offscreen frame.
    // Returns -1 if no frame has completed since the last call (or
    // export failed). The caller MUST close() the returned fd.
    int takeLastFrameSyncFd();

    // Read the DRM render-node major/minor of the picked
    // VkPhysicalDevice. Returns true and writes `*out_major` /
    // `*out_minor` on success; returns false when the driver lacks
    // `VK_EXT_physical_device_drm` or initVulkan hasn't run yet. The
    // waywallen-renderer host forwards the result on the IPC `Ready`
    // event so the daemon can match the renderer's GPU against each
    // display's GPU.
    bool getDrmRenderNode(uint32_t& out_major, uint32_t& out_minor) const;

    // Block until VulkanRender::init has finished, with a wall-clock
    // timeout. Returns true if init completed; false on timeout. Used
    // by the waywallen-wescene host to gate `vkInstance()` etc. on the
    // looper's INIT_VULKAN handler.
    bool waitVulkanInited(uint32_t timeout_ms);

    // Raw Vulkan handles for IPC backends that need to share the device
    // with another library (e.g. waywallen-bridge pool). Valid after
    // `waitVulkanInited()` returns true; nullptr / 0 otherwise.
    VkInstance       vkInstance() const;
    VkPhysicalDevice vkPhysicalDevice() const;
    VkDevice         vkDevice() const;
    VkQueue          vkGraphicsQueue() const;
    uint32_t         vkGraphicsQueueFamily() const;

    // 16-byte UUIDs from VkPhysicalDeviceIDProperties; zero-filled if
    // the extension is unavailable.
    void deviceUuid(uint8_t out[16]) const;
    void driverUuid(uint8_t out[16]) const;

private:
    bool m_inited { false };

private:
    friend class MainHandler;

    bool                         m_offscreen { false };
    std::shared_ptr<MainHandler> m_main_handler;
};
} // namespace wallpaper
