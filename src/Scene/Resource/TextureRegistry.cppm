export module wescene.resource_registry:texture_registry;
import rstd;
import wescene.resource;
import wescene.vulkan;

using namespace rstd::prelude;

export namespace owe::resource
{

struct Texture {
    resource::TextureHandle id;
    TextureRequest          request;
    u64                     definition_version { 1 };
    u64                     content_version { 1 };
};

struct TexturePhysical {
    vulkan::ImageSlotsRef image;
    u64                   generation { 1 };
    u64                   source_generation { 0 };
    ReadyToken            ready;
};

class TextureRegistry {
public:
    TextureRegistry()                                  = default;
    TextureRegistry(const TextureRegistry&)            = delete;
    TextureRegistry& operator=(const TextureRegistry&) = delete;

    auto Register(TextureRequest request) -> resource::TextureHandle {
        if (request.name.is_empty()) return {};

        Identity identity {
            .kind = request.kind,
            .key  = request.name.clone(),
        };
        auto existing = m_handles.get(identity);
        if (existing.is_some()) {
            auto handle  = **existing;
            auto texture = m_textures.get_mut(handle);
            if (texture.is_none()) return {};
            if (! SameTextureRequest((**texture).request, request)) {
                if ((**texture).request.definition != request.definition) {
                    ++(**texture).definition_version;
                }
                if ((**texture).request.source != request.source) ++(**texture).content_version;
                (**texture).request = rstd::move(request);
            }
            return handle;
        }

        resource::TextureHandle handle {
            .index      = m_next_index++,
            .generation = m_generation,
        };
        (void)m_textures.insert(handle,
                                Texture {
                                    .id      = handle,
                                    .request = rstd::move(request),
                                });
        (void)m_handles.insert(rstd::move(identity), handle);
        return handle;
    }

    auto Publish(resource::TextureHandle handle, vulkan::ImageSlotsRef image, ReadyToken ready)
        -> Option<u64> {
        if (ResolveTexture(handle).is_none() || image.slots.empty()) return None();

        auto source_generation = image.getActive().generation;
        auto physical          = m_resources.get_mut(handle);
        if (physical.is_some()) {
            if ((**physical).source_generation != source_generation) ++(**physical).generation;
            (**physical).image             = rstd::move(image);
            (**physical).source_generation = source_generation;
            (**physical).ready             = ready;
            return Some((**physical).generation);
        }

        (void)m_resources.insert(handle,
                                 TexturePhysical {
                                     .image             = rstd::move(image),
                                     .source_generation = source_generation,
                                     .ready             = ready,
                                 });
        return Some(u64(1));
    }

    auto ResolveTexture(resource::TextureHandle handle) const noexcept -> Option<ref<Texture>> {
        return m_textures.get(handle);
    }

    auto ResolveTextureState(resource::TextureHandle handle) const noexcept
        -> Option<TextureLogicalState> {
        auto texture = ResolveTexture(handle);
        if (texture.is_none()) return None();
        return Some(TextureLogicalState {
            .handle             = handle,
            .definition_version = (**texture).definition_version,
            .content_version    = (**texture).content_version,
        });
    }

    auto Resolve(resource::TextureHandle handle) const noexcept -> Option<ref<TexturePhysical>> {
        return m_resources.get(handle);
    }

    auto Find(TextureRequestKind kind, ref<str> key) const -> Option<resource::TextureHandle> {
        auto handle = m_handles.get(Identity {
            .kind = kind,
            .key  = rstd::string::String::make(key),
        });
        if (handle.is_none()) return None();
        auto value = **handle;
        return Some(rstd::move(value));
    }

    bool Unbind(resource::TextureHandle handle) { return m_resources.remove(handle).is_some(); }

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
        TextureRequestKind   kind { TextureRequestKind::Imported };
        rstd::string::String key;

        friend bool operator==(const Identity& lhs, const Identity& rhs) {
            return lhs.kind == rhs.kind && lhs.key == rhs.key.as_str();
        }
    };

    struct IdentityHasher {
        rstd::hash::RandomState state;

        auto operator()(const Identity& identity) const noexcept -> u64 {
            auto seed = state(static_cast<u32>(identity.kind));
            seed ^= state(identity.key) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
            return seed;
        }
    };

    template<typename Value>
    using HandleMap =
        rstd::collections::HashMap<resource::TextureHandle, Value,
                                   resource::ResourceHandleHasher<resource::TextureHandle>>;

    u64                        m_generation { 1 };
    u64                        m_next_index { 0 };
    HandleMap<Texture>         m_textures;
    HandleMap<TexturePhysical> m_resources;
    rstd::collections::HashMap<Identity, resource::TextureHandle, IdentityHasher> m_handles;
};

} // namespace owe::resource

export namespace rstd
{

template<>
struct Impl<owe::resource::TextureLogicalRegistryView, owe::resource::TextureRegistry>
    : ImplBase<owe::resource::TextureRegistry> {
    auto ResolveTextureState(owe::resource::TextureHandle handle) const
        -> Option<owe::resource::TextureLogicalState> {
        return this->self().ResolveTextureState(handle);
    }
};

} // namespace rstd
