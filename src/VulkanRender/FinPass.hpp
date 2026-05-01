#pragma once
#include "VulkanPass.hpp"
#include <string>

#include "Vulkan/Device.hpp"

#include "Scene/Scene.h"
#include "SpecTexs.hpp"

namespace wallpaper
{
namespace vulkan
{

// Final pass: blit the scene render target into the present buffer
// (offscreen ExSwapchain slot or surface-mode swapchain image), then
// emit the appropriate barrier so the consumer reads coherent pixels.
//
// Implemented as a pure transfer pass (vkCmdBlitImage) — no
// renderpass, pipeline, shader, or descriptor. Per-frame state is just
// the chosen present image, set via setPresent before each execute().
//
// The blit handles cross-format channel reordering (R8G8B8A8 ↔
// B8G8R8A8) automatically because vkCmdBlitImage maps logical RGBA
// channels rather than raw bytes. So no rebuild is needed when the
// bridge renegotiates between ABGR/ARGB families.
class FinPass : public VulkanPass {
public:
    struct Desc {
        // in
        const std::string_view result { SpecTex_Default }; // scene RT key

        // resolved in prepare()
        ImageParameters vk_result;

        // set per-frame via setPresent()
        ImageParameters vk_present;

        // configured once at init by VulkanRender (from
        // ExSwapchain::producerOutputLayout / releaseTargetQueueFamily)
        VkImageLayout present_layout    { VK_IMAGE_LAYOUT_UNDEFINED };
        uint32_t      present_queue_index { 0 };
    };

    FinPass(const Desc&);
    virtual ~FinPass();

    void setPresent(ImageParameters);
    void setPresentLayout(VkImageLayout);
    void setPresentQueueIndex(uint32_t);

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    Desc m_desc;
};

} // namespace vulkan
} // namespace wallpaper
