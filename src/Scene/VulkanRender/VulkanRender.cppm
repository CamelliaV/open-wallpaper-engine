module;


export module wescene.vulkan_render;
import wescene.types;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

import wescene.rgraph;

export import :vulkan_pass;
export import :resource;
export import :pass_common;
export import :copy_pass;
export import :custom_shader_pass;
export import :fin_pass;
export import :pre_pass;

export namespace owe
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
    // MSAA samples for the screen RT only. 1 disables. Clamped down to
    // device's framebufferColorSampleCounts at init.
    uint32_t msaa_samples { 1 };
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

std::unique_ptr<rg::RenderGraph> sceneToRenderGraph(Scene&);

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
    void UpdateCameraFillMode(Scene&, owe::FillMode);

    bool onSwapchainReady(unsigned width, unsigned height);

    ExSwapchain* exSwapchain() const;
    bool         inited() const;

    int takeLastFrameSyncFd();

    bool getDrmRenderNode(uint32_t& out_major, uint32_t& out_minor) const;

    /* Tick all registered video-tex decoders. No-op when no scene
     * texture has been recognised as a VIDEO container. Invoked from
     * SceneWallpaper's per-frame RenderDraw handler. */
    void pumpVideoTextures(double dt_seconds);

    VkInstance       vkInstance() const;
    VkPhysicalDevice vkPhysicalDevice() const;
    VkDevice         vkDevice() const;
    VkQueue          vkGraphicsQueue() const;
    uint32_t         vkGraphicsQueueFamily() const;

    void deviceUuid(uint8_t out[16]) const;
    void driverUuid(uint8_t out[16]) const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace vulkan
} // namespace owe
