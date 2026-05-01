#include "FinPass.hpp"
#include "Resource.hpp"
#include "PassCommon.hpp"
#include "Utils/Logging.h"

using namespace wallpaper::vulkan;

FinPass::FinPass(const Desc&) {}
FinPass::~FinPass() {}

void FinPass::setPresent(ImageParameters img)        { m_desc.vk_present = img; }
void FinPass::setPresentLayout(VkImageLayout layout) { m_desc.present_layout = layout; }
void FinPass::setPresentQueueIndex(uint32_t i)       { m_desc.present_queue_index = i; }

void FinPass::prepare(Scene& scene, const Device& device, RenderingResources& /*rr*/) {
    auto tex_name = std::string(m_desc.result);
    if (scene.renderTargets.count(tex_name) == 0) {
        LOG_ERROR("FinPass: scene render target \"%s\" not found", tex_name.c_str());
        return;
    }
    auto& rt  = scene.renderTargets.at(tex_name);
    auto  opt = device.tex_cache().Query(tex_name, ToTexKey(rt), !rt.allowReuse);
    if (! opt.has_value()) {
        LOG_ERROR("FinPass: TextureCache::Query(\"%s\") failed", tex_name.c_str());
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

    // 1. Scene RT: SHADER_READ_ONLY_OPTIMAL → TRANSFER_SRC_OPTIMAL.
    //    All RenderGraph passes leave their outputs in
    //    SHADER_READ_ONLY_OPTIMAL (PrePass + CustomShaderPass enforce
    //    this). We need TRANSFER_SRC for vkCmdBlitImage's source.
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

    // 2. Present buffer: UNDEFINED → TRANSFER_DST_OPTIMAL.
    //    UNDEFINED forgets prior contents AND prior queue family
    //    ownership (spec) — the free implicit "acquire from FOREIGN"
    //    when the slot was previously released to the consumer.
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

    // 3. Blit scene RT → present. vkCmdBlitImage handles cross-format
    //    channel mapping (R8G8B8A8 ↔ B8G8R8A8 → correct logical RGBA).
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

    // 4. Scene RT: TRANSFER_SRC_OPTIMAL → SHADER_READ_ONLY_OPTIMAL.
    //    Restores the inter-frame invariant the RenderGraph relies on
    //    (next frame's first pass writing this RT will start from
    //    SHADER_READ_ONLY_OPTIMAL or run a renderpass with
    //    finalLayout=SHADER_READ_ONLY_OPTIMAL).
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

    // 5. Present: TRANSFER_DST_OPTIMAL → present_layout, with optional
    //    queue family release. Bridge path uses
    //    VK_QUEUE_FAMILY_FOREIGN_EXT to flush GPU caches before the
    //    consumer reads via DMA-BUF. Local path (and surface-single-
    //    family path) sets present_queue_index == graphics, so the
    //    barrier is a pure layout transition with no ownership transfer.
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
