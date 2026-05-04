module;
#include <vulkan/vulkan.h>

#include "Core/NoCopyMove.hpp"
#include "Swapchain/ExSwapchain.hpp"

export module wescene.scene_wallpaper;
import cppstd;

export import wescene.vulkan_render;

export namespace wallpaper
{

using FirstFrameCallback = std::function<void()>;

inline constexpr std::string_view PROPERTY_SOURCE               = "source";
inline constexpr std::string_view PROPERTY_ASSETS               = "assets";
inline constexpr std::string_view PROPERTY_FPS                  = "fps";
inline constexpr std::string_view PROPERTY_FILLMODE             = "fillmode";
inline constexpr std::string_view PROPERTY_SPEED                = "speed";
inline constexpr std::string_view PROPERTY_GRAPHIVZ             = "graphivz";
inline constexpr std::string_view PROPERTY_VOLUME               = "volume";
inline constexpr std::string_view PROPERTY_MUTED                = "muted";
inline constexpr std::string_view PROPERTY_CACHE_PATH           = "cache_path";
inline constexpr std::string_view PROPERTY_FIRST_FRAME_CALLBACK = "first_frame_callback";

class MainHandler;

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

    int takeLastFrameSyncFd();

    bool getDrmRenderNode(uint32_t& out_major, uint32_t& out_minor) const;

    bool waitVulkanInited(uint32_t timeout_ms);

    VkInstance       vkInstance() const;
    VkPhysicalDevice vkPhysicalDevice() const;
    VkDevice         vkDevice() const;
    VkQueue          vkGraphicsQueue() const;
    uint32_t         vkGraphicsQueueFamily() const;

    void deviceUuid(uint8_t out[16]) const;
    void driverUuid(uint8_t out[16]) const;

private:
    bool m_inited { false };

private:
    friend class MainHandler;

    bool                         m_offscreen { false };
    std::unique_ptr<MainHandler> m_main_handler;
};

// `TexTiling` is declared in the classic header `Swapchain/ExSwapchain.hpp`
// which is still consumed by ExSwapchain backends. Re-export it from the
// module purview so a single `import wescene.scene_wallpaper;` lets
// consumers reach `wallpaper::TexTiling::OPTIMAL`.
using ::wallpaper::TexTiling;

} // namespace wallpaper
