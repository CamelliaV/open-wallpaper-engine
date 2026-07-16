export module wescene.resource_registry:state;
import rstd;
import wescene.resource;
import wescene.vulkan;

import :prepared;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

enum class TextureStateKind
{
    Undefined,
    Sampled,
    TransferSource,
    TransferDestination,
    ColorAttachment,
    DepthAttachment,
};

struct TextureSubresourceRange {
    VkImageAspectFlags aspect { VK_IMAGE_ASPECT_COLOR_BIT };
    u32                base_mip_level { 0 };
    u32                level_count { VK_REMAINING_MIP_LEVELS };
    u32                base_array_layer { 0 };
    u32                layer_count { VK_REMAINING_ARRAY_LAYERS };
};

struct PreparedImageBarrier {
    VkPipelineStageFlags src_stage { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };
    VkPipelineStageFlags dst_stage { VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT };
    VkDependencyFlags    dependency { VK_DEPENDENCY_BY_REGION_BIT };
    VkImageMemoryBarrier barrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    };

    void Record(vvk::CommandBuffer& command) const {
        command.PipelineBarrier(src_stage, dst_stage, dependency, barrier);
    }
};

class PreparedBarrierBatch {
public:
    void Add(PreparedImageBarrier barrier) { m_barriers.push(rstd::move(barrier)); }

    void Clear() { m_barriers.clear(); }

    void Record(vvk::CommandBuffer& command) const {
        for (const auto& barrier : m_barriers) barrier.Record(command);
    }

    auto Size() const noexcept -> usize { return m_barriers.len(); }
    bool Empty() const noexcept { return m_barriers.is_empty(); }

private:
    rstd::vec::Vec<PreparedImageBarrier> m_barriers;
};

class ResourceStateTracker {
public:
    bool Compile(const resource::ResourcePlan& plan, const PreparedResourceTable& resources) {
        m_textures.clear();
        if (plan.generation != resources.Generation()) return false;
        for (const auto& entry : plan.textures) {
            auto prepared = resources.Resolve(entry.handle);
            if (prepared.is_none()) return false;
            auto initial = entry.request.kind == resource::TextureRequestKind::Imported
                               ? TextureStateKind::Sampled
                               : TextureStateKind::Undefined;
            if (m_textures
                    .insert(entry.handle,
                            TrackedTexture {
                                .image = (**prepared).image.getActive(),
                                .state = initial,
                            })
                    .is_some()) {
                return false;
            }
        }
        return true;
    }

    auto Prepare(resource::TextureUseHandle use, TextureStateKind target,
                 TextureSubresourceRange range = {}, bool discard = false)
        -> Option<PreparedImageBarrier> {
        auto tracked = m_textures.get_mut(use);
        if (tracked.is_none()) return None();

        auto source      = State(discard ? TextureStateKind::Undefined : (**tracked).state);
        auto destination = State(target);
        PreparedImageBarrier prepared {
            .src_stage = source.stage,
            .dst_stage = destination.stage,
            .barrier =
                VkImageMemoryBarrier {
                    .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext               = nullptr,
                    .srcAccessMask       = source.access,
                    .dstAccessMask       = destination.access,
                    .oldLayout           = source.layout,
                    .newLayout           = destination.layout,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image               = (**tracked).image.handle,
                    .subresourceRange =
                        VkImageSubresourceRange {
                            .aspectMask     = range.aspect,
                            .baseMipLevel   = range.base_mip_level,
                            .levelCount     = range.level_count,
                            .baseArrayLayer = range.base_array_layer,
                            .layerCount     = range.layer_count,
                        },
                },
        };
        (**tracked).state = target;
        return Some(rstd::move(prepared));
    }

    bool Set(resource::TextureUseHandle use, TextureStateKind state) {
        auto tracked = m_textures.get_mut(use);
        if (tracked.is_none()) return false;
        (**tracked).state = state;
        return true;
    }

    void Reset() { m_textures.clear(); }

    auto Size() const noexcept -> usize { return m_textures.len(); }

private:
    struct StateInfo {
        VkPipelineStageFlags stage;
        VkAccessFlags        access;
        VkImageLayout        layout;
    };

    struct TrackedTexture {
        vulkan::ImageParameters image;
        TextureStateKind        state { TextureStateKind::Undefined };
    };

    static auto State(TextureStateKind state) -> StateInfo {
        switch (state) {
        case TextureStateKind::Sampled:
            return {
                .stage  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                .access = VK_ACCESS_SHADER_READ_BIT,
                .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
        case TextureStateKind::TransferSource:
            return {
                .stage  = VK_PIPELINE_STAGE_TRANSFER_BIT,
                .access = VK_ACCESS_TRANSFER_READ_BIT,
                .layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            };
        case TextureStateKind::TransferDestination:
            return {
                .stage  = VK_PIPELINE_STAGE_TRANSFER_BIT,
                .access = VK_ACCESS_TRANSFER_WRITE_BIT,
                .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            };
        case TextureStateKind::ColorAttachment:
            return {
                .stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                .access =
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            };
        case TextureStateKind::DepthAttachment:
            return {
                .stage  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                .access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            };
        case TextureStateKind::Undefined:
        default:
            return {
                .stage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                .access = 0,
                .layout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
        }
    }

    using TextureMap =
        rstd::collections::HashMap<resource::TextureUseHandle, TrackedTexture,
                                   resource::ResourceHandleHasher<resource::TextureUseHandle>>;

    TextureMap m_textures;
};

} // namespace owe::resource_registry
