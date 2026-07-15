module;

#include <rstd/macro.hpp>
module wescene.vulkan_render;
import rstd.log;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

using namespace owe::vulkan;

FinPass::FinPass(const Desc& desc) {
    m_desc.result         = desc.result;
    m_desc.result_request = desc.result_request;
}
FinPass::~FinPass() {}

bool FinPass::setFrameSurface(owe::FrameSurfaceLease lease) {
    if (! lease.valid()) return false;
    m_desc.frame_surface = std::move(lease);
    return true;
}
bool FinPass::setResultRequest(std::optional<TextureRequest> request) {
    return SetTextureRequestIfChanged(m_desc.result_request, std::move(request));
}

std::vector<PassTextureRequestDiagnostic> FinPass::textureRequestDiagnostics() const {
    return {
        PassTextureRequestDiagnostic {
            .role    = "frame-result",
            .name    = std::string(m_desc.result),
            .request = m_desc.result_request,
        },
    };
}

void FinPass::prepare(Scene& scene, const Device& device, RenderingResources& /*rr*/) {
    RenderResourceSystem resources(device);

    auto tex_name = std::string(m_desc.result);
    if (scene.renderTargets.count(tex_name) == 0) {
        rstd_error("FinPass: scene render target \"{}\" not found", tex_name);
        return;
    }
    auto& rt      = scene.renderTargets.at(tex_name);
    auto  request = m_desc.result_request.value_or(MakeRenderTargetTextureRequest(tex_name, rt));
    auto  opt     = resources.EnsureTexture(request);
    if (! opt.has_value()) {
        rstd_error("FinPass: TextureCache::Query(\"{}\") failed", tex_name);
        return;
    }
    m_desc.vk_result = opt.value();
    setPrepared();
}

void FinPass::execute(const Device& device, RenderingResources& rr) {
    if (! m_desc.frame_surface.has_value() || ! m_desc.frame_surface->valid()) return;
    const auto& frame_surface = *m_desc.frame_surface;
    const auto& present       = frame_surface.image;
    auto&       cmd           = rr.command;
    uint32_t    gqf           = device.graphics_queue().family_index;

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
            .oldLayout           = frame_surface.discard_content ? VK_IMAGE_LAYOUT_UNDEFINED
                                                                 : frame_surface.initial_layout,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = frame_surface.initial_queue_family == gqf
                                       ? VK_QUEUE_FAMILY_IGNORED
                                       : frame_surface.initial_queue_family,
            .dstQueueFamilyIndex =
                frame_surface.initial_queue_family == gqf ? VK_QUEUE_FAMILY_IGNORED : gqf,
            .image            = present.handle,
            .subresourceRange = sub,
        };
        // srcStage = TRANSFER (not TOP_OF_PIPE) so the layout transition
        // observes the swapchain acquire-semaphore wait, which the submit
        // sets at the same TRANSFER stage. TOP_OF_PIPE would let the
        // transition race the presentation engine's still-in-flight read
        // (sync-validation WRITE_AFTER_READ).
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            b);
    }

    {
        // Result is always R8G8B8A8_UNORM (screen RT). We can copy when
        // present matches that and dimensions are identical; otherwise
        // fall back to blit which handles size/format mismatch.
        const bool can_copy = m_desc.vk_result.extent.width == present.extent.width &&
                              m_desc.vk_result.extent.height == present.extent.height &&
                              frame_surface.format == VK_FORMAT_R8G8B8A8_UNORM;

        if (! m_path_logged) {
            rstd_info("FinPass: {}", can_copy ? "copy" : "blit");
            m_path_logged = true;
        }

        if (can_copy) {
            VkImageCopy region {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffset      = { 0, 0, 0 },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffset      = { 0, 0, 0 },
                .extent = { m_desc.vk_result.extent.width, m_desc.vk_result.extent.height, 1 },
            };
            cmd.CopyImage(m_desc.vk_result.handle,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          present.handle,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          region);
        } else {
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
                    VkOffset3D { (int32_t)present.extent.width,
                                 (int32_t)present.extent.height,
                                 1 },
                },
            };
            cmd.BlitImage(m_desc.vk_result.handle,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          present.handle,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          region,
                          VK_FILTER_LINEAR);
        }
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
        bool                 xfer = frame_surface.final_queue_family != gqf;
        VkImageMemoryBarrier b {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask       = 0,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = frame_surface.final_layout,
            .srcQueueFamilyIndex = xfer ? gqf : VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex =
                xfer ? frame_surface.final_queue_family : VK_QUEUE_FAMILY_IGNORED,
            .image            = present.handle,
            .subresourceRange = sub,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            b);
    }
}

void FinPass::destory(const Device&, RenderingResources&) {
    m_desc.vk_result = {};
    m_desc.frame_surface.reset();
    setPrepared(false);
    clearReleaseTexs();
}
