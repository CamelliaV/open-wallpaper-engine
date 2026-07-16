export module wescene.resource:catalog;
import rstd;
import wescene.types;
import :error;
import :texture;
import :buffer;
import :shader;

export namespace owe::resource
{

using namespace rstd::prelude;

struct TextureCatalog {
    using Trait                  = TextureCatalog;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureCatalog;

        auto ResolveTexture(TextureDefinitionId id) const -> Option<TextureRequest> {
            return rstd::trait_call<0>(this, id);
        }

        auto FindTexture(ref<str> name) const -> Option<TextureRequest> {
            return rstd::trait_call<1>(this, name);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::ResolveTexture, &T::FindTexture>;
};

struct TextureContentProvider {
    using Trait                  = TextureContentProvider;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureContentProvider;

        auto LoadTexture(const TextureRequest& request) -> Result<mut_ref<Image>, ResourceError> {
            return rstd::trait_call<0>(this, request);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::LoadTexture>;
};

struct BufferCatalog {
    using Trait                  = BufferCatalog;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = BufferCatalog;

        auto ResolveBuffer(BufferDefinitionId id) const -> Option<BufferRequest> {
            return rstd::trait_call<0>(this, id);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::ResolveBuffer>;
};

struct BufferContentProvider {
    using Trait                  = BufferContentProvider;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = BufferContentProvider;

        auto LoadBuffer(const BufferRequest& request) -> Result<rstd::vec::Vec<u8>, ResourceError> {
            return rstd::trait_call<0>(this, request);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::LoadBuffer>;
};

struct BufferContentWriter {
    using Trait                  = BufferContentWriter;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = BufferContentWriter;

        auto UpdateBuffer(BufferUseHandle use, slice<u8> content, u64 content_version)
            -> Result<empty, ResourceError> {
            return rstd::trait_call<0>(this, use, content, content_version);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::UpdateBuffer>;
};

struct ShaderCatalog {
    using Trait                  = ShaderCatalog;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ShaderCatalog;

        auto ResolveShader(ShaderDefinitionId id) const -> Option<ShaderRequest> {
            return rstd::trait_call<0>(this, id);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::ResolveShader>;
};

struct ShaderArtifactProvider {
    using Trait                  = ShaderArtifactProvider;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ShaderArtifactProvider;

        auto LoadShader(const ShaderRequest& request) -> Result<ShaderArtifact, ResourceError> {
            return rstd::trait_call<0>(this, request);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::LoadShader>;
};

struct TextureLogicalState {
    TextureHandle handle;
    u64           definition_version { 0 };
    u64           content_version { 0 };
};

struct TextureLogicalRegistryView {
    using Trait                  = TextureLogicalRegistryView;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = TextureLogicalRegistryView;

        auto ResolveTextureState(TextureHandle handle) const -> Option<TextureLogicalState> {
            return rstd::trait_call<0>(this, handle);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::ResolveTextureState>;
};

} // namespace owe::resource
