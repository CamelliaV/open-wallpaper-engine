module;

#include <cstdint>
#include <span>

export module wescene.resource_registry:buffer_registry;
import rstd;
import wescene.resource;
import wescene.vulkan;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

struct BufferEntry {
    resource::BufferHandle  handle;
    resource::BufferRequest request;
    u64                     definition_version { 1 };
    u64                     content_version { 1 };
};

struct BufferPhysical {
    vulkan::BufferAllocation buffer;
    u64                      generation { 1 };
    u64                      source_generation { 0 };
    resource::ReadyToken     ready;

    BufferPhysical(vulkan::BufferAllocation value, u64 physical_generation, u64 source,
                   resource::ReadyToken ready_token)
        : buffer(rstd::move(value)),
          generation(physical_generation),
          source_generation(source),
          ready(ready_token) {}
};

struct PreparedBuffer {
    resource::BufferHandle          resource;
    rstd::sync::Arc<BufferPhysical> physical;

    auto clone() const -> PreparedBuffer {
        return PreparedBuffer {
            .resource = resource,
            .physical = physical.clone(),
        };
    }
};

class BufferRegistry {
public:
    auto Declare(resource::BufferRequest request) -> resource::BufferHandle {
        return Register(rstd::move(request));
    }

    auto Ensure(resource::BufferRequest request, slice<u8> content,
                mut_ref<dyn<vulkan::BufferUploadBackend>> backend)
        -> Result<PreparedBuffer, resource::ResourceError> {
        auto handle = Register(request.clone());
        if (! handle.Valid()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("invalid buffer request"),
            });
        }

        auto existing = m_resources.get(handle);
        if (existing.is_some() && (**existing)->source_generation == request.content_version) {
            return Ok(PreparedBuffer {
                .resource = handle,
                .physical = (**existing).clone(),
            });
        }

        auto uploaded = backend->UploadBuffer(
            content,
            vulkan::BufferUploadRequest {
                .size      = static_cast<VkDeviceSize>(request.definition.size.to_primitive()),
                .alignment = static_cast<VkDeviceSize>(request.definition.alignment.to_primitive()),
                .usage     = UploadUsage(request.definition.usage),
            });
        if (uploaded.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("upload buffer {} failed", request.name.as_str()),
            });
        }

        u64  physical_generation = existing.is_some() ? (**existing)->generation + u64(1) : u64(1);
        auto physical            = rstd::sync::Arc<BufferPhysical>::make(
            rstd::move(*uploaded),
            physical_generation,
            request.content_version,
            resource::ReadyToken { .value = request.content_version });
        (void)m_resources.insert(handle, physical.clone());
        return Ok(PreparedBuffer {
            .resource = handle,
            .physical = rstd::move(physical),
        });
    }

    auto Resolve(resource::BufferHandle handle) const -> Option<rstd::sync::Arc<BufferPhysical>> {
        auto physical = m_resources.get(handle);
        if (physical.is_none()) return None();
        return Some((**physical).clone());
    }

    auto ResolveBuffer(resource::BufferHandle handle) const -> Option<ref<BufferEntry>> {
        return m_entries.get(handle);
    }

    auto Update(resource::BufferHandle handle, slice<u8> content, u64 content_version,
                mut_ref<dyn<vulkan::BufferUploadBackend>> backend)
        -> Result<empty, resource::ResourceError> {
        auto entry    = m_entries.get_mut(handle);
        auto physical = m_resources.get_mut(handle);
        if (entry.is_none() || physical.is_none() ||
            (**entry).request.lifetime != resource::BufferLifetimeClass::Dynamic) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("dynamic buffer is unavailable"),
            });
        }
        auto allocation = mut_ref<vulkan::BufferAllocation>::from_raw_parts(
            rstd::addressof((**physical)->buffer));
        if (! backend->UpdateBuffer(allocation, content)) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("update buffer {} failed", (**entry).request.name.as_str()),
            });
        }
        if ((**entry).request.content_version != content_version) ++(**entry).content_version;
        (**entry).request.content_version = content_version;
        (**physical)->source_generation   = content_version;
        return Ok(empty {});
    }

    void EvictUnused() {
        m_resources.retain(
            [](const resource::BufferHandle&, rstd::sync::Arc<BufferPhysical>& value) {
                return value.strong_count() > usize(1);
            });
    }

    void Reset() {
        m_resources.clear();
        m_entries.clear();
        m_names.clear();
        m_next_index = u64();
        ++m_generation;
        if (m_generation == u64()) ++m_generation;
    }

    auto Size() const noexcept -> usize { return m_entries.len(); }

private:
    static auto UploadUsage(resource::BufferUsage usage) -> vulkan::BufferUploadClass {
        switch (usage) {
        case resource::BufferUsage::Vertex: return vulkan::BufferUploadClass::Vertex;
        case resource::BufferUsage::Index: return vulkan::BufferUploadClass::Index;
        case resource::BufferUsage::Uniform: return vulkan::BufferUploadClass::Uniform;
        case resource::BufferUsage::Storage: return vulkan::BufferUploadClass::Storage;
        case resource::BufferUsage::Transfer: return vulkan::BufferUploadClass::Transfer;
        }
        return vulkan::BufferUploadClass::Vertex;
    }

    auto Register(resource::BufferRequest request) -> resource::BufferHandle {
        if (request.name.is_empty() || request.definition.size == usize()) return {};
        auto existing = m_names.get(request.name);
        if (existing.is_some()) {
            auto entry = m_entries.get_mut(**existing);
            if (entry.is_none()) return {};
            if ((**entry).request.definition != request.definition) {
                ++(**entry).definition_version;
            }
            if ((**entry).request.content_version != request.content_version) {
                ++(**entry).content_version;
            }
            (**entry).request = rstd::move(request);
            return (**entry).handle;
        }

        auto handle = resource::BufferHandle {
            .index      = m_next_index++,
            .generation = m_generation,
        };
        auto name = request.name.clone();
        (void)m_entries.insert(handle,
                               BufferEntry {
                                   .handle  = handle,
                                   .request = rstd::move(request),
                               });
        (void)m_names.insert(rstd::move(name), handle);
        return handle;
    }

    template<typename Value>
    using HandleMap =
        rstd::collections::HashMap<resource::BufferHandle, Value,
                                   resource::ResourceHandleHasher<resource::BufferHandle>>;

    u64                                                        m_generation { 1 };
    u64                                                        m_next_index { 0 };
    HandleMap<BufferEntry>                                     m_entries;
    HandleMap<rstd::sync::Arc<BufferPhysical>>                 m_resources;
    rstd::collections::HashMap<String, resource::BufferHandle> m_names;
};

} // namespace owe::resource_registry
