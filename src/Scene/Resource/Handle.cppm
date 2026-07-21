export module wescene.resource:handle;
import rstd;

export namespace owe::resource
{

using namespace rstd::prelude;

template<typename Tag>
struct ResourceHandle {
    u64 index { u64::MAX };
    u64 generation {};

    bool Valid() const noexcept { return index != u64::MAX && generation != u64(); }

    friend bool operator==(const ResourceHandle&, const ResourceHandle&) = default;
};

struct TextureHandleTag;
struct BufferHandleTag;
struct PipelineHandleTag;
struct ShaderHandleTag;
struct RenderPassHandleTag;
struct FramebufferHandleTag;
struct ResourceUseHandleTag;
struct TextureUseHandleTag;
struct BufferUseHandleTag;
struct ShaderUseHandleTag;
struct PipelineUseHandleTag;
struct RenderPassUseHandleTag;
struct FramebufferUseHandleTag;
struct ExternalUseHandleTag;
struct DescriptorLayoutHandleTag;
struct DescriptorBindingHandleTag;

using TextureHandle           = ResourceHandle<TextureHandleTag>;
using BufferHandle            = ResourceHandle<BufferHandleTag>;
using PipelineHandle          = ResourceHandle<PipelineHandleTag>;
using ShaderHandle            = ResourceHandle<ShaderHandleTag>;
using RenderPassHandle        = ResourceHandle<RenderPassHandleTag>;
using FramebufferHandle       = ResourceHandle<FramebufferHandleTag>;
using ResourceUseHandle       = ResourceHandle<ResourceUseHandleTag>;
using TextureUseHandle        = ResourceHandle<TextureUseHandleTag>;
using BufferUseHandle         = ResourceHandle<BufferUseHandleTag>;
using ShaderUseHandle         = ResourceHandle<ShaderUseHandleTag>;
using PipelineUseHandle       = ResourceHandle<PipelineUseHandleTag>;
using RenderPassUseHandle     = ResourceHandle<RenderPassUseHandleTag>;
using FramebufferUseHandle    = ResourceHandle<FramebufferUseHandleTag>;
using ExternalUseHandle       = ResourceHandle<ExternalUseHandleTag>;
using DescriptorLayoutHandle  = ResourceHandle<DescriptorLayoutHandleTag>;
using DescriptorBindingHandle = ResourceHandle<DescriptorBindingHandleTag>;

template<typename Handle>
struct ResourceHandleHasher {
    rstd::hash::RandomState state;

    auto operator()(const Handle& handle) const noexcept -> u64 {
        auto seed = state(handle.index);
        seed ^= state(handle.generation)
                    .wrapping_add(u64(0x9e3779b97f4a7c15ULL))
                    .wrapping_add(seed.wrapping_shl(u64(6)))
                    .wrapping_add(seed >> u64(2));
        return seed;
    }
};

} // namespace owe::resource
