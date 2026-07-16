export module wescene.resource_registry:owner;
import rstd;
import wescene.vulkan;

import :texture_registry;
import :buffer_registry;
import :shader_registry;
import :descriptor;
import :lifetime;
import :state;
import :policy;
import :external;
import :graphics;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

class ResourceRegistries {
public:
    ResourceRegistries()                                     = default;
    ResourceRegistries(const ResourceRegistries&)            = delete;
    ResourceRegistries& operator=(const ResourceRegistries&) = delete;
    ResourceRegistries(ResourceRegistries&&)                 = delete;
    ResourceRegistries& operator=(ResourceRegistries&&)      = delete;

    bool Initialize(const vulkan::Device& device) {
        if (Ready()) return true;

        auto meshes = Box<vulkan::MeshCache>::make(device);
        if (! meshes.get()->init()) return false;

        m_textures = Some(Box<vulkan::TextureCache>::make(device));
        m_meshes   = Some(rstd::move(meshes));
        return true;
    }

    void Reset() {
        if (m_meshes.is_some()) m_meshes->get()->destroy();
        m_meshes   = None();
        m_textures = None();
        m_texture_entries.Reset();
        m_buffer_entries.Reset();
        m_shader_entries.Reset();
        m_descriptor_layouts.Reset();
        m_descriptor_system.Reset();
        m_uploads.Reset();
        m_submissions.Reset(m_descriptor_arena);
        m_states.Reset();
        m_memory.Reset();
        m_pipeline_cache.Reset();
        m_render_pass_cache.Reset();
        m_framebuffer_cache.Reset();
        m_pipeline_diagnostics.Reset();
        m_framebuffer_diagnostics.Reset();
        m_retirement.ReleaseAllReady();
    }

    bool Ready() const noexcept { return m_textures.is_some() && m_meshes.is_some(); }

    auto Textures() -> vulkan::TextureCache& { return *m_textures->get(); }
    auto Textures() const -> const vulkan::TextureCache& {
        return *m_textures->as_ptr().as_raw_ptr();
    }
    auto Meshes() -> vulkan::MeshCache& { return *m_meshes->get(); }
    auto Meshes() const -> const vulkan::MeshCache& { return *m_meshes->as_ptr().as_raw_ptr(); }

    auto TextureEntries() -> resource::TextureRegistry& { return m_texture_entries; }
    auto Buffers() -> BufferRegistry& { return m_buffer_entries; }
    auto Shaders() -> ShaderRegistry& { return m_shader_entries; }
    auto DescriptorLayouts() -> DescriptorLayoutRegistry& { return m_descriptor_layouts; }
    auto Descriptors() -> DescriptorSystem& { return m_descriptor_system; }
    auto DescriptorAllocations() -> DescriptorArena& { return m_descriptor_arena; }
    auto Uploads() -> UploadScheduler& { return m_uploads; }
    auto Submissions() -> SubmissionTracker& { return m_submissions; }
    auto States() -> ResourceStateTracker& { return m_states; }
    auto Memory() -> MemoryBudgetPolicy& { return m_memory; }
    auto External() -> ExternalResourceBridge& { return m_external; }
    auto Pipelines() -> PipelineRegistry& { return m_pipeline_cache; }
    auto PipelineCache() -> PipelineRegistry& { return m_pipeline_cache; }
    auto RenderPasses() -> RenderPassRegistry& { return m_render_pass_cache; }
    auto RenderPassCache() -> RenderPassRegistry& { return m_render_pass_cache; }
    auto Framebuffers() -> FramebufferRegistry& { return m_framebuffer_cache; }
    auto FramebufferCache() -> FramebufferRegistry& { return m_framebuffer_cache; }
    auto PipelineDiagnostics() -> PipelineCacheDiagnostics& { return m_pipeline_diagnostics; }
    auto FramebufferDiagnostics() -> FramebufferCacheDiagnostics& {
        return m_framebuffer_diagnostics;
    }
    auto Retirement() -> PipelineRetireQueue& { return m_retirement; }

private:
    Option<Box<vulkan::TextureCache>> m_textures;
    Option<Box<vulkan::MeshCache>>    m_meshes;
    resource::TextureRegistry         m_texture_entries;
    BufferRegistry                    m_buffer_entries;
    ShaderRegistry                    m_shader_entries;
    DescriptorLayoutRegistry          m_descriptor_layouts;
    DescriptorSystem                  m_descriptor_system;
    DescriptorArena                   m_descriptor_arena;
    UploadScheduler                   m_uploads;
    SubmissionTracker                 m_submissions;
    ResourceStateTracker              m_states;
    MemoryBudgetPolicy                m_memory;
    ExternalResourceBridge            m_external;
    PipelineRegistry                  m_pipeline_cache;
    RenderPassRegistry                m_render_pass_cache;
    FramebufferRegistry               m_framebuffer_cache;
    PipelineCacheDiagnostics          m_pipeline_diagnostics;
    FramebufferCacheDiagnostics       m_framebuffer_diagnostics;
    PipelineRetireQueue               m_retirement;
};

} // namespace owe::resource_registry
