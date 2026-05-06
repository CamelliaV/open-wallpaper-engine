module;

#include <rstd/macro.hpp>
#include "Swapchain/ExSwapchain.hpp"
module wescene.vulkan_render;
import rstd.log;
import rstd.cppstd;
import cppstd;
import wescene.vulkan;
import wescene.scene;

using namespace wallpaper::vulkan;

FinPass::FinPass(const Desc&) {}
FinPass::~FinPass() {}

void FinPass::setPresent(ImageParameters img)        { m_desc.vk_present = img; }
void FinPass::setPresentLayout(VkImageLayout layout) { m_desc.present_layout = layout; }
void FinPass::setPresentQueueIndex(uint32_t i)       { m_desc.present_queue_index = i; }

void FinPass::prepare(Scene& scene, const Device& device, RenderingResources& /*rr*/) {
    auto tex_name = std::string(m_desc.result);
    if (scene.renderTargets.count(tex_name) == 0) {
        rstd_error("FinPass: scene render target \"{}\" not found", tex_name);
        return;
    }
    auto& rt  = scene.renderTargets.at(tex_name);
    auto  opt = device.tex_cache().Query(tex_name, ToTexKey(rt), !rt.allowReuse);
    if (! opt.has_value()) {
        rstd_error("FinPass: TextureCache::Query(\"{}\") failed", tex_name);
        return;
    }
    m_desc.vk_result = opt.value();
    setPrepared();
}

void FinPass::execute(const Device& device, RenderingResources& rr) {
    auto& cmd      = rr.command;
    uint32_t gqf   = device.graphics_queue().family_index;

    VkImageSubresourceRange sub {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    {
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = m_desc.vk_result.handle,
            .subresourceRange    = sub,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            b);
    }

    {
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = 0,
            .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = m_desc.vk_present.handle,
            .subresourceRange    = sub,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            b);
    }

    {
        VkImageBlit region {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets     = {
                VkOffset3D { 0, 0, 0 },
                VkOffset3D { (int32_t)m_desc.vk_result.extent.width,
                             (int32_t)m_desc.vk_result.extent.height, 1 },
            },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets     = {
                VkOffset3D { 0, 0, 0 },
                VkOffset3D { (int32_t)m_desc.vk_present.extent.width,
                             (int32_t)m_desc.vk_present.extent.height, 1 },
            },
        };
        cmd.BlitImage(m_desc.vk_result.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      m_desc.vk_present.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      region,
                      VK_FILTER_LINEAR);
    }

    {
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = m_desc.vk_result.handle,
            .subresourceRange    = sub,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            b);
    }

    {
        bool xfer = (m_desc.present_queue_index != gqf);
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask       = 0,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = m_desc.present_layout,
            .srcQueueFamilyIndex = xfer ? gqf : VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = xfer ? m_desc.present_queue_index
                                        : VK_QUEUE_FAMILY_IGNORED,
            .image               = m_desc.vk_present.handle,
            .subresourceRange    = sub,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            b);
    }
}

void FinPass::destory(const Device&, RenderingResources&) {
    setPrepared(false);
    clearReleaseTexs();
}
