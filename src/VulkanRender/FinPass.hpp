#pragma once
#include "VulkanPass.hpp"
#include <string>
#include <vector>

#include "Vulkan/Device.hpp"
#include "Vulkan/StagingBuffer.hpp"
#include "Vulkan/GraphicsPipeline.hpp"

#include "Scene/Scene.h"
#include "SpecTexs.hpp"

namespace wallpaper
{
namespace vulkan
{

class FinPass : public VulkanPass {
public:
    struct Desc {
        // in
        const std::string_view result { SpecTex_Default };
        VkFormat               present_format { VK_FORMAT_UNDEFINED };
        VkImageLayout          present_layout;
        uint32_t               present_queue_index;

        // prepared
        ImageParameters vk_result;
        ImageParameters vk_present;
        VkImageLayout   render_layout;
        VkClearValue    clear_value;

        StagingBufferRef   vertex_buf;
        vvk::Framebuffer   fb;
        PipelineParameters pipeline;
    };

    FinPass(const Desc&);
    virtual ~FinPass();

    void setPresent(ImageParameters);
    void setPresentLayout(VkImageLayout);
    void setPresentFormat(VkFormat);
    void setPresentQueueIndex(uint32_t);

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

    // Rebuild the renderpass + graphics pipeline from the current
    // present_format/present_layout. Call after setPresentFormat with a
    // new format. The caller is responsible for ensuring no in-flight
    // command buffer references the old pipeline (drawFrameOffscreen
    // already fences the previous frame before reaching this).
    //
    // Requires prepare() to have run at least once so shaders / vertex
    // buffer / descriptor info are cached. Returns true on success;
    // on failure prepared() reverts to false.
    bool rebuildPresent(const Device&);

private:
    // Build / rebuild the renderpass + graphics pipeline using cached
    // shader bytecode (`m_vert_spv` / `m_frag_spv`) and the current
    // present_format/present_layout. Owns no glslang state.
    bool buildPresentPipeline(const Device&);

    Desc m_desc;

    // Cached compile artifacts populated on first prepare(). Reused on
    // every rebuildPresent — avoids re-running glslang outside the
    // VulkanRender::compileRenderGraph init/finalize scope.
    bool                                           m_resources_ready { false };
    std::vector<unsigned int>                      m_vert_spv;
    std::vector<unsigned int>                      m_frag_spv;
    VkVertexInputBindingDescription                m_bind_description {};
    std::vector<VkVertexInputAttributeDescription> m_attr_descriptions;
    DescriptorSetInfo                              m_descriptor_info;
};

} // namespace vulkan
} // namespace wallpaper
