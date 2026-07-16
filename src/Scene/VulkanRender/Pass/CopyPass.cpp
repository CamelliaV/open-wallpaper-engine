module;

#include <rstd/macro.hpp>
#include "Utils/AutoDeletor.hpp"
#include "vvk/macros.hpp"

module wescene.vulkan_render;
import wescene.spec_names;
import wescene.core;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

using namespace owe::vulkan;

CopyPass::CopyPass(Desc&& desc): m_desc(std::move(desc)) {}

CopyPass::~CopyPass() {};

PassInvalidationFlags CopyPass::finalizeResourceRequests(Scene& scene) {
    PassInvalidationFlags flags = PassInvalidationNone;
    auto refresh                = [&scene](std::string_view name) -> rstd::Option<TextureRequest> {
        if (name.empty() || ! IsSpecTex(name)) return rstd::None();
        auto it = scene.renderTargets.find(std::string(name));
        if (it == scene.renderTargets.end()) return rstd::None();
        return rstd::Some(MakeRenderTargetTextureRequest(name, it->second));
    };

    if (auto request = refresh(m_desc.src);
        request.is_some() && SetTextureRequestIfChanged(m_desc.src_request, std::move(request))) {
        flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
    }
    if (auto request = refresh(m_desc.dst);
        request.is_some() && SetTextureRequestIfChanged(m_desc.dst_request, std::move(request))) {
        flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
    }
    return flags;
}

std::vector<PassTextureRequestDiagnostic> CopyPass::textureRequestDiagnostics() const {
    std::vector<PassTextureRequestDiagnostic> out;
    out.reserve(2);
    out.push_back(PassTextureRequestDiagnostic {
        .role    = "copy-src",
        .name    = m_desc.src,
        .request = m_desc.src_request.is_some() ? rstd::Some(m_desc.src_request->clone())
                                                : rstd::None<TextureRequest>(),
    });
    out.push_back(PassTextureRequestDiagnostic {
        .role    = "copy-dst",
        .name    = m_desc.dst,
        .request = m_desc.dst_request.is_some() ? rstd::Some(m_desc.dst_request->clone())
                                                : rstd::None<TextureRequest>(),
    });
    return out;
}

bool CopyPass::prepareResourceStates(resource_registry::ResourceStateTracker& states) {
    m_desc.before_barriers.Clear();
    m_desc.after_barriers.Clear();
    if (m_desc.src_use.is_none() || m_desc.dst_use.is_none()) return true;

    auto range = resource_registry::TextureSubresourceRange {
        .level_count = 1,
        .layer_count = 1,
    };
    auto src_before =
        states.Prepare(*m_desc.src_use, resource_registry::TextureStateKind::TransferSource, range);
    auto dst_before = states.Prepare(
        *m_desc.dst_use, resource_registry::TextureStateKind::TransferDestination, range, true);
    auto src_after =
        states.Prepare(*m_desc.src_use, resource_registry::TextureStateKind::Sampled, range);
    auto dst_after =
        states.Prepare(*m_desc.dst_use, resource_registry::TextureStateKind::Sampled, range);
    if (src_before.is_none() || dst_before.is_none() || src_after.is_none() ||
        dst_after.is_none()) {
        return false;
    }
    m_desc.before_barriers.Add(rstd::move(src_before).unwrap_unchecked());
    m_desc.before_barriers.Add(rstd::move(dst_before).unwrap_unchecked());
    m_desc.after_barriers.Add(rstd::move(src_after).unwrap_unchecked());
    m_desc.after_barriers.Add(rstd::move(dst_after).unwrap_unchecked());
    return true;
}

void CopyPass::prepare(Scene&, const Device&, RenderingResources& rr) {
    std::array<std::string, 2>      textures    = { m_desc.src, m_desc.dst };
    std::array<ImageParameters*, 2> vk_textures = { &m_desc.vk_src, &m_desc.vk_dst };
    std::array<rstd::Option<resource::TextureUseHandle>*, 2> texture_uses = {
        &m_desc.src_use,
        &m_desc.dst_use,
    };
    for (usize i = 0; i < textures.size(); i++) {
        auto& tex_name = textures[i];
        if (tex_name.empty()) continue;

        if (texture_uses[i]->is_none()) {
            rstd_error("copy texture {} has no resource use", tex_name);
            return;
        }
        auto prepared = rr.prepared_resources.Resolve(**texture_uses[i]);
        if (prepared.is_none()) {
            rstd_error("prepared copy texture {} not found", tex_name);
            return;
        }
        *vk_textures[i] = (**prepared).image.getActive();
    }

    setPrepared();
};
void CopyPass::record(PassRecordContext& context) {
    auto& cmd = *context.command;
    auto& src = m_desc.vk_src;
    auto& dst = m_desc.vk_dst;

    if (! (src.handle && dst.handle)) {
        rstd_assert(src.handle && dst.handle);
        return;
    }

    VkImageSubresourceRange srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,

    };
    VkImageCopy copy {
        .srcSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = srang.aspectMask,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .dstSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = srang.aspectMask,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .extent = { src.extent.width, src.extent.height, 1 },
    };
    if (! m_desc.before_barriers.Empty()) {
        m_desc.before_barriers.Record(cmd);
    } else {
        VkImageMemoryBarrier in_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image            = src.handle,
            .subresourceRange = srang,
        };
        VkImageMemoryBarrier out_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image            = dst.handle,
            .subresourceRange = srang,
        };

        auto barriers = std::array { in_bar, out_bar };
        cmd.PipelineBarrier(
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_DEPENDENCY_BY_REGION_BIT,
            {},
            {},
            rstd::slice<VkImageMemoryBarrier>::from_raw_parts(barriers.data(), barriers.size()));
    }
    cmd.CopyImage(src.handle,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  dst.handle,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  copy);
    if (! m_desc.after_barriers.Empty()) {
        m_desc.after_barriers.Record(cmd);
    } else {
        VkImageMemoryBarrier in_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = src.handle,
            .subresourceRange = srang,
        };
        VkImageMemoryBarrier out_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = dst.handle,
            .subresourceRange = srang,
        };

        auto barriers = std::array { in_bar, out_bar };
        cmd.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_DEPENDENCY_BY_REGION_BIT,
            {},
            {},
            rstd::slice<VkImageMemoryBarrier>::from_raw_parts(barriers.data(), barriers.size()));
    }

    if (dst.mipmap_level > 1) {
        RecordGenerateMipmaps(cmd, dst);
    }
};
void CopyPass::destory(const Device&, RenderingResources&) {}
