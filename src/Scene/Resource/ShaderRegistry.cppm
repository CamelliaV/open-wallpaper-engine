export module wescene.resource_registry:shader_registry;
import rstd;
import wescene.resource;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

struct ShaderPhysical {
    resource::ShaderArtifact artifact;
    u64                      physical_generation { 1 };

    ShaderPhysical(resource::ShaderArtifact value, u64 generation)
        : artifact(rstd::move(value)), physical_generation(generation) {}
};

struct ShaderEntry {
    resource::ShaderHandle          handle;
    resource::ShaderRequest         request;
    rstd::sync::Arc<ShaderPhysical> physical;
};

struct PreparedShader {
    resource::ShaderHandle          resource;
    rstd::sync::Arc<ShaderPhysical> physical;

    auto clone() const -> PreparedShader {
        return PreparedShader {
            .resource = resource,
            .physical = physical.clone(),
        };
    }
};

class ShaderRegistry {
public:
    auto Prepare(resource::ShaderRequest                        request,
                 mut_ref<dyn<resource::ShaderArtifactProvider>> provider)
        -> Result<PreparedShader, resource::ResourceError> {
        auto existing = m_names.get(request.name);
        if (existing.is_some()) {
            auto entry = m_entries.get_mut(**existing);
            if (entry.is_none()) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::BackendFailure,
                    .message = rstd::format("shader registry entry is unavailable"),
                });
            }
            if ((**entry).request.content_version == request.content_version &&
                (**entry).request.source == request.source) {
                return Ok(PreparedShader {
                    .resource = (**entry).handle,
                    .physical = (**entry).physical.clone(),
                });
            }
            auto artifact = provider->LoadShader(request);
            if (artifact.is_err()) return Err(rstd::move(artifact).unwrap_err_unchecked());
            auto generation    = (**entry).physical->physical_generation + 1;
            (**entry).request  = rstd::move(request);
            (**entry).physical = rstd::sync::Arc<ShaderPhysical>::make(
                rstd::move(artifact).unwrap_unchecked(), generation);
            return Ok(PreparedShader {
                .resource = (**entry).handle,
                .physical = (**entry).physical.clone(),
            });
        }

        auto artifact = provider->LoadShader(request);
        if (artifact.is_err()) return Err(rstd::move(artifact).unwrap_err_unchecked());
        auto handle = NextHandle();
        auto name   = request.name.clone();
        auto physical =
            rstd::sync::Arc<ShaderPhysical>::make(rstd::move(artifact).unwrap_unchecked(), u64(1));
        (void)m_entries.insert(handle,
                               ShaderEntry {
                                   .handle   = handle,
                                   .request  = rstd::move(request),
                                   .physical = physical.clone(),
                               });
        (void)m_names.insert(rstd::move(name), handle);
        return Ok(PreparedShader {
            .resource = handle,
            .physical = rstd::move(physical),
        });
    }

    auto Ensure(resource::ShaderRequest                        request,
                mut_ref<dyn<resource::ShaderArtifactProvider>> provider)
        -> Result<resource::ShaderHandle, resource::ResourceError> {
        auto prepared = Prepare(rstd::move(request), provider);
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        return Ok(rstd::move(prepared).unwrap_unchecked().resource);
    }

    auto Resolve(resource::ShaderHandle handle) const noexcept -> Option<ref<ShaderEntry>> {
        return m_entries.get(handle);
    }

    void Reset() {
        m_entries.clear();
        m_names.clear();
        m_next_index = 0;
        ++m_generation;
        if (m_generation == 0) ++m_generation;
    }

    auto Generation() const noexcept -> u64 { return m_generation; }
    auto Size() const noexcept -> usize { return m_entries.len(); }

private:
    auto NextHandle() -> resource::ShaderHandle {
        return { .index = m_next_index++, .generation = m_generation };
    }

    using EntryMap =
        rstd::collections::HashMap<resource::ShaderHandle, ShaderEntry,
                                   resource::ResourceHandleHasher<resource::ShaderHandle>>;

    u64                                                        m_generation { 1 };
    u64                                                        m_next_index { 0 };
    EntryMap                                                   m_entries;
    rstd::collections::HashMap<String, resource::ShaderHandle> m_names;
};

} // namespace owe::resource_registry
