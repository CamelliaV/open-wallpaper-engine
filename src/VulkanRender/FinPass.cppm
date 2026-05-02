module;

#include <vulkan/vulkan.h>

#include "SpecTexs.hpp"

#include "Swapchain/ExSwapchain.hpp"

export module wescene.vulkan_render:fin_pass;
import cppstd;
import wescene.vulkan;
import wescene.scene;

import :vulkan_pass;
import :resource;

export namespace wallpaper::vulkan
{

// Final pass: blit the scene render target into the present buffer
// (offscreen ExSwapchain slot or surface-mode swapchain image), then
// emit the appropriate barrier so the consumer reads coherent pixels.
class FinPass : public VulkanPass {
public:
    struct Desc {
        // in
        const std::string_view result { SpecTex_Default }; // scene RT key

        // resolved in prepare()
        ImageParameters vk_result;

        // set per-frame via setPresent()
        ImageParameters vk_present;

        // configured once at init by VulkanRender
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

} // namespace wallpaper::vulkan
