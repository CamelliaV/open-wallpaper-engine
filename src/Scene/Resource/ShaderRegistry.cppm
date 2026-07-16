export module wescene.resource_registry:shader_registry;
import rstd;
import wescene.resource;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

struct ShaderEntry {
    resource::ShaderHandle   handle;
    resource::ShaderRequest  request;
    resource::ShaderArtifact artifact;
    u64                      physical_generation { 1 };
};

class ShaderRegistry {
public:
    auto Ensure(resource::ShaderRequest                        request,
                mut_ref<dyn<resource::ShaderArtifactProvider>> provider)
        -> Result<resource::ShaderHandle, resource::ResourceError> {
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
                return Ok((**entry).handle);
            }
            auto artifact = provider->LoadShader(request);
            if (artifact.is_err()) return Err(rstd::move(artifact).unwrap_err_unchecked());
            (**entry).request  = rstd::move(request);
            (**entry).artifact = rstd::move(artifact).unwrap_unchecked();
            ++(**entry).physical_generation;
            return Ok((**entry).handle);
        }

        auto artifact = provider->LoadShader(request);
        if (artifact.is_err()) return Err(rstd::move(artifact).unwrap_err_unchecked());
        auto handle = NextHandle();
        auto name   = request.name.clone();
        (void)m_entries.insert(handle,
                               ShaderEntry {
                                   .handle   = handle,
                                   .request  = rstd::move(request),
                                   .artifact = rstd::move(artifact).unwrap_unchecked(),
                               });
        (void)m_names.insert(rstd::move(name), handle);
        return Ok(handle);
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
