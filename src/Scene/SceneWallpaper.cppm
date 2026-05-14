module;
#include <vulkan/vulkan.h>

export module wescene.scene_wallpaper;
import wescene.core;
import rstd.cppstd;

export import wescene.vulkan_render;
export import wescene.vulkan;

export namespace owe
{

using FirstFrameCallback = std::function<void()>;

// Fired once per loaded scene with the parsed `general.clearcolor`.
// The host forwards the value to the daemon via
// `ww_bridge_send_report_state_clear_color` so display-side letterbox
// bars match the scene's intended background. Components are 0..=1
// sRGB. Alpha is fixed at 1.0 by the host (the rendered DMA-BUF is
// always opaque).
using ClearColorCallback = std::function<void(float r, float g, float b)>;

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
    // button: 0=left, 1=right, 2=middle (GLFW numbering). down=true on
    // press, false on release.
    void mouseButton(int button, bool down);
    void mouseEnter(bool in_window);

    void setPropertyBool(std::string_view, bool);
    void setPropertyInt32(std::string_view, int32_t);
    void setPropertyFloat(std::string_view, float);
    void setPropertyString(std::string_view, std::string);
    void setPropertyObject(std::string_view, std::shared_ptr<void>);

    // Install (or clear, with `nullptr`) a callback invoked on the
    // main thread after each scene is parsed, carrying the scene's
    // `general.clearcolor`. Set once before initVulkan.
    void setOnClearColor(ClearColorCallback);

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

} // namespace owe
