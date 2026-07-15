export module wescene.render;
import rstd;
import wescene.vulkan;

export namespace owe::render
{

using namespace rstd::prelude;

template<typename Tag>
struct ResourceHandle {
    u64 index { numeric_limits<u64>::max() };
    u64 generation { 0 };

    bool Valid() const noexcept { return index != numeric_limits<u64>::max() && generation != 0; }

    friend bool operator==(const ResourceHandle&, const ResourceHandle&) = default;
};

struct TextureHandleTag;
struct PipelineHandleTag;
struct ShaderHandleTag;

using TextureHandle  = ResourceHandle<TextureHandleTag>;
using PipelineHandle = ResourceHandle<PipelineHandleTag>;
using ShaderHandle   = ResourceHandle<ShaderHandleTag>;

template<typename Handle>
struct ResourceHandleHasher {
    rstd::hash::RandomState state;

    auto operator()(const Handle& handle) const noexcept -> u64 {
        auto seed = state(handle.index);
        seed ^= state(handle.generation) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

struct TextureRegistry {
    using Trait                  = TextureRegistry;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureRegistry;
        auto Resolve(TextureHandle) const -> const vulkan::ImageSlotsRef*;
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Resolve>;
};

struct PipelineRegistry {
    using Trait                  = PipelineRegistry;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = PipelineRegistry;
        auto Resolve(PipelineHandle) const -> const vulkan::PipelineParameters*;
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Resolve>;
};

struct ShaderRegistry {
    using Trait                  = ShaderRegistry;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ShaderRegistry;
        auto Resolve(ShaderHandle) const -> const vvk::ShaderModule*;
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Resolve>;
};

struct ResourceRegistries {
    ResourceRegistries(ref<dyn<TextureRegistry>> textures, ref<dyn<PipelineRegistry>> pipelines,
                       ref<dyn<ShaderRegistry>> shaders)
        : textures(textures), pipelines(pipelines), shaders(shaders) {}

    ref<dyn<TextureRegistry>>  textures;
    ref<dyn<PipelineRegistry>> pipelines;
    ref<dyn<ShaderRegistry>>   shaders;
};

} // namespace owe::render
