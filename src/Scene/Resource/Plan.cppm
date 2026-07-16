export module wescene.resource:plan;
import rstd;
import :handle;
import :error;
import :texture;
import :buffer;
import :shader;

export namespace owe::resource
{

using namespace rstd::prelude;

enum class ResourceAccess
{
    Read,
    Write,
    ReadWrite,
};

struct TexturePlanEntry {
    TextureUseHandle handle;
    TextureRequest   request;
    ResourceAccess   access { ResourceAccess::Read };
    u32              version { 0 };
};

struct BufferPlanEntry {
    BufferUseHandle handle;
    BufferRequest   request;
    ResourceAccess  access { ResourceAccess::Read };
    u32             version { 0 };
};

struct ShaderPlanEntry {
    ShaderUseHandle handle;
    ShaderRequest request;
    u32             version { 0 };
};

struct ReadyToken {
    u64 value { 0 };

    bool        Valid() const noexcept { return value != 0; }
    friend bool operator==(const ReadyToken&, const ReadyToken&) = default;
};

struct CompletionToken {
    u64 value { 0 };

    bool        Valid() const noexcept { return value != 0; }
    friend bool operator==(const CompletionToken&, const CompletionToken&) = default;
};

struct ResourcePlan {
    u64                              generation { 0 };
    rstd::vec::Vec<TexturePlanEntry> textures;
    rstd::vec::Vec<BufferPlanEntry>  buffers;
    rstd::vec::Vec<ShaderPlanEntry>  shaders;
};

struct ResourcePlanVisitor {
    using Trait                  = ResourcePlanVisitor;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ResourcePlanVisitor;

        auto VisitTexture(const TexturePlanEntry& entry) -> Result<empty, ResourceError> {
            return rstd::trait_call<0>(this, entry);
        }

        auto VisitBuffer(const BufferPlanEntry& entry) -> Result<empty, ResourceError> {
            return rstd::trait_call<1>(this, entry);
        }

        auto VisitShader(const ShaderPlanEntry& entry) -> Result<empty, ResourceError> {
            return rstd::trait_call<2>(this, entry);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::VisitTexture, &T::VisitBuffer, &T::VisitShader>;
};

inline auto VisitResourcePlan(const ResourcePlan& plan, mut_ref<dyn<ResourcePlanVisitor>> visitor)
    -> Result<empty, ResourceError> {
    for (const auto& entry : plan.textures) {
        auto result = visitor->VisitTexture(entry);
        if (result.is_err()) return result;
    }
    for (const auto& entry : plan.buffers) {
        auto result = visitor->VisitBuffer(entry);
        if (result.is_err()) return result;
    }
    for (const auto& entry : plan.shaders) {
        auto result = visitor->VisitShader(entry);
        if (result.is_err()) return result;
    }
    return Ok(empty {});
}

} // namespace owe::resource
