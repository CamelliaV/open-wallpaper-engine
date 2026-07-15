export module wescene.resource:pipeline_registry;
import rstd;
import wescene.render;
import wescene.vulkan;

using namespace rstd::prelude;

export namespace owe::resource
{

class PipelineRegistry {
public:
    PipelineRegistry()                                   = default;
    PipelineRegistry(const PipelineRegistry&)            = delete;
    PipelineRegistry& operator=(const PipelineRegistry&) = delete;

    auto Register(vulkan::PipelineParameters resource) -> render::PipelineHandle {
        auto handle = NextHandle();
        (void)m_resources.insert(handle, rstd::move(resource));
        return handle;
    }

    auto Resolve(render::PipelineHandle handle) const noexcept
        -> const vulkan::PipelineParameters* {
        auto resource = m_resources.get(handle);
        return resource.is_some() ? resource->as_raw_ptr() : nullptr;
    }

    auto Remove(render::PipelineHandle handle) -> Option<vulkan::PipelineParameters> {
        return m_resources.remove(handle);
    }

    void Reset() {
        m_resources.clear();
        m_next_index = 0;
        ++m_generation;
        if (m_generation == 0) ++m_generation;
    }

    auto Generation() const noexcept -> u64 { return m_generation; }
    auto Size() const noexcept -> usize { return m_resources.len(); }

private:
    auto NextHandle() -> render::PipelineHandle {
        return {
            .index      = m_next_index++,
            .generation = m_generation,
        };
    }

    using ResourceMap =
        rstd::collections::HashMap<render::PipelineHandle, vulkan::PipelineParameters,
                                   render::ResourceHandleHasher<render::PipelineHandle>>;

    u64         m_generation { 1 };
    u64         m_next_index { 0 };
    ResourceMap m_resources;
};

} // namespace owe::resource

namespace rstd
{

template<>
struct Impl<owe::render::PipelineRegistry, owe::resource::PipelineRegistry>
    : ImplBase<owe::resource::PipelineRegistry> {
    auto Resolve(owe::render::PipelineHandle handle) const
        -> const owe::vulkan::PipelineParameters* {
        return this->self().Resolve(handle);
    }
};

} // namespace rstd
