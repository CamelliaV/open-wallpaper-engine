export module wescene.resource:texture_registry;
import rstd;
import wescene.render;
import wescene.vulkan;

using namespace rstd::prelude;

export namespace owe::resource
{

enum class TextureDomain
{
    Asset,
    RenderTarget,
    Dynamic,
    External,
};

enum class TextureLifetimeClass
{
    FrameLocal,
    Retained,
    Dedicated,
    ExternalOwned,
};

using TextureContentFlags = u32;

enum class TextureContent : TextureContentFlags
{
    SourceDefined        = 1u << 0u,
    DiscardOnAcquire     = 1u << 1u,
    ClearOnFirstWrite    = 1u << 2u,
    ClearOnEveryWrite    = 1u << 3u,
    PreserveWithinGraph  = 1u << 4u,
    PreserveAcrossFrames = 1u << 5u,
    ProducerUpdated      = 1u << 6u,
    ExternalCurrentFrame = 1u << 7u,
};

inline constexpr TextureContentFlags TextureContentFlag(TextureContent content) {
    return static_cast<TextureContentFlags>(content);
}

enum class TextureContractSource
{
    Explicit,
    LegacyAdapter,
};

struct TextureResourceContract {
    TextureDomain         domain { TextureDomain::Asset };
    TextureLifetimeClass  lifetime { TextureLifetimeClass::Retained };
    TextureContentFlags   content { TextureContentFlag(TextureContent::SourceDefined) };
    TextureContractSource source { TextureContractSource::Explicit };

    friend bool operator==(const TextureResourceContract&,
                           const TextureResourceContract&) = default;
};

struct TextureResourceIntent {
    TextureResourceContract contract;

    friend bool operator==(const TextureResourceIntent&, const TextureResourceIntent&) = default;
};

struct Texture {
    render::TextureHandle id;
    rstd::string::String  key;
    TextureResourceIntent intent;
};

class TextureRegistry {
public:
    TextureRegistry()                                  = default;
    TextureRegistry(const TextureRegistry&)            = delete;
    TextureRegistry& operator=(const TextureRegistry&) = delete;

    auto Register(rstd::string::String key, TextureResourceIntent intent) -> render::TextureHandle {
        if (key.is_empty()) return {};

        Identity identity {
            .domain = intent.contract.domain,
            .key    = key.clone(),
        };
        auto existing = m_handles.get(identity);
        if (existing.is_some()) {
            auto        handle  = **existing;
            const auto* texture = ResolveTexture(handle);
            return texture != nullptr && texture->intent == intent ? handle
                                                                   : render::TextureHandle {};
        }

        render::TextureHandle handle {
            .index      = m_next_index++,
            .generation = m_generation,
        };
        (void)m_textures.insert(handle,
                                Texture {
                                    .id     = handle,
                                    .key    = rstd::move(key),
                                    .intent = rstd::move(intent),
                                });
        (void)m_handles.insert(rstd::move(identity), handle);
        return handle;
    }

    bool Bind(render::TextureHandle handle, vulkan::ImageSlotsRef resource) {
        if (ResolveTexture(handle) == nullptr) return false;
        (void)m_resources.insert(handle, rstd::move(resource));
        return true;
    }

    auto ResolveTexture(render::TextureHandle handle) const noexcept -> const Texture* {
        auto texture = m_textures.get(handle);
        return texture.is_some() ? texture->as_raw_ptr() : nullptr;
    }

    auto Resolve(render::TextureHandle handle) const noexcept -> const vulkan::ImageSlotsRef* {
        auto resource = m_resources.get(handle);
        return resource.is_some() ? resource->as_raw_ptr() : nullptr;
    }

    auto Find(TextureDomain domain, ref<str> key) const -> Option<render::TextureHandle> {
        auto handle = m_handles.get(Identity {
            .domain = domain,
            .key    = rstd::string::String::make(key),
        });
        if (handle.is_none()) return None();
        auto value = **handle;
        return Some(rstd::move(value));
    }

    bool Unbind(render::TextureHandle handle) { return m_resources.remove(handle).is_some(); }

    void Reset() {
        m_resources.clear();
        m_textures.clear();
        m_handles.clear();
        m_next_index = 0;
        ++m_generation;
        if (m_generation == 0) ++m_generation;
    }

    auto Generation() const noexcept -> u64 { return m_generation; }
    auto Size() const noexcept -> usize { return m_textures.len(); }

private:
    struct Identity {
        TextureDomain        domain { TextureDomain::Asset };
        rstd::string::String key;

        friend bool operator==(const Identity& lhs, const Identity& rhs) {
            return lhs.domain == rhs.domain && lhs.key == rhs.key.as_str();
        }
    };

    struct IdentityHasher {
        rstd::hash::RandomState state;

        auto operator()(const Identity& identity) const noexcept -> u64 {
            auto seed = state(static_cast<u32>(identity.domain));
            seed ^= state(identity.key) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
            return seed;
        }
    };

    template<typename Value>
    using HandleMap =
        rstd::collections::HashMap<render::TextureHandle, Value,
                                   render::ResourceHandleHasher<render::TextureHandle>>;

    u64                                                                         m_generation { 1 };
    u64                                                                         m_next_index { 0 };
    HandleMap<Texture>                                                          m_textures;
    HandleMap<vulkan::ImageSlotsRef>                                            m_resources;
    rstd::collections::HashMap<Identity, render::TextureHandle, IdentityHasher> m_handles;
};

} // namespace owe::resource

namespace rstd
{

template<>
struct Impl<owe::render::TextureRegistry, owe::resource::TextureRegistry>
    : ImplBase<owe::resource::TextureRegistry> {
    auto Resolve(owe::render::TextureHandle handle) const -> const owe::vulkan::ImageSlotsRef* {
        return this->self().Resolve(handle);
    }
};

} // namespace rstd
