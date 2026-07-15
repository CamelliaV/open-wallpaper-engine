export module wescene.resource:shader_registry;
import rstd;
import wescene.render;
import wescene.vulkan;

using namespace rstd::prelude;

export namespace owe::resource
{

class ShaderRegistry {
public:
    ShaderRegistry()                                 = default;
    ShaderRegistry(const ShaderRegistry&)            = delete;
    ShaderRegistry& operator=(const ShaderRegistry&) = delete;

    auto Register(vvk::ShaderModule resource) -> render::ShaderHandle {
        auto handle = NextHandle();
        (void)m_resources.insert(handle, rstd::move(resource));
        return handle;
    }

    auto Resolve(render::ShaderHandle handle) const noexcept -> const vvk::ShaderModule* {
        auto resource = m_resources.get(handle);
        return resource.is_some() ? resource->as_raw_ptr() : nullptr;
    }

    auto Remove(render::ShaderHandle handle) -> Option<vvk::ShaderModule> {
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
    auto NextHandle() -> render::ShaderHandle {
        return {
            .index      = m_next_index++,
            .generation = m_generation,
        };
    }

    using ResourceMap =
        rstd::collections::HashMap<render::ShaderHandle, vvk::ShaderModule,
                                   render::ResourceHandleHasher<render::ShaderHandle>>;

    u64         m_generation { 1 };
    u64         m_next_index { 0 };
    ResourceMap m_resources;
};

} // namespace owe::resource

namespace rstd
{

template<>
struct Impl<owe::render::ShaderRegistry, owe::resource::ShaderRegistry>
    : ImplBase<owe::resource::ShaderRegistry> {
    auto Resolve(owe::render::ShaderHandle handle) const -> const vvk::ShaderModule* {
        return this->self().Resolve(handle);
    }
};

} // namespace rstd
