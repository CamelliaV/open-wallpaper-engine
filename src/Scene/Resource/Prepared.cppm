export module wescene.resource_registry:prepared;
import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.vulkan;

import :texture_registry;
import :buffer_registry;
import :shader_registry;
import :descriptor;
import :graphics;
import :external;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

struct PreparedTexture {
    resource::TextureUseHandle                 use;
    resource::TextureHandle                    resource;
    resource::TextureRequest                   request;
    rstd::sync::Arc<vulkan::TextureAllocation> physical;
    vulkan::ImageSlotsRef                      image;
    u64                                        physical_generation { 0 };
    resource::ReadyToken                       ready;
};

struct PreparedTextureLease {
    resource::TextureHandle                    resource;
    rstd::sync::Arc<vulkan::TextureAllocation> physical;
    u64                                        physical_generation { 0 };
};

struct PreparedBufferUse {
    resource::BufferUseHandle use;
    PreparedBuffer            buffer;
};

struct PreparedShaderUse {
    resource::ShaderUseHandle use;
    PreparedShader            shader;
};

struct PreparedPipeline {
    resource::PipelineUseHandle            use;
    resource::PipelineHandle               resource;
    resource::RenderPassHandle             render_pass;
    rstd::sync::Arc<PipelineResourceEntry> physical;
};

struct PreparedRenderPass {
    resource::RenderPassUseHandle    use;
    resource::RenderPassHandle       resource;
    vulkan::RenderPassCacheKey       cache_key;
    rstd::sync::Arc<vvk::RenderPass> physical;
};

struct PreparedFramebuffer {
    resource::FramebufferUseHandle    use;
    resource::FramebufferHandle       resource;
    rstd::sync::Arc<vvk::Framebuffer> physical;
};

struct PreparedExternalUse {
    resource::ExternalUseHandle use;
    PreparedExternalFrame       frame;
};

struct PreparedBufferLease {
    resource::BufferHandle          resource;
    rstd::sync::Arc<BufferPhysical> physical;
};

struct PreparedShaderLease {
    resource::ShaderHandle          resource;
    rstd::sync::Arc<ShaderPhysical> physical;
};

struct PreparedPipelineLease {
    resource::PipelineHandle               resource;
    rstd::sync::Arc<PipelineResourceEntry> physical;
};

struct PreparedRenderPassLease {
    resource::RenderPassHandle       resource;
    rstd::sync::Arc<vvk::RenderPass> physical;
};

struct PreparedFramebufferLease {
    resource::FramebufferHandle       resource;
    rstd::sync::Arc<vvk::Framebuffer> physical;
};

struct PreparedDescriptorLease {
    resource::DescriptorBindingHandle handle;
    Option<vvk::DescriptorSetLease>   set;
};

struct PreparedExternalLease {
    resource::ExternalUseHandle use;
    FrameSurfaceLease           lease;
};

struct PreparedResourceLeases {
    rstd::vec::Vec<PreparedTextureLease>     textures;
    rstd::vec::Vec<PreparedBufferLease>      buffers;
    rstd::vec::Vec<PreparedShaderLease>      shaders;
    rstd::vec::Vec<PreparedPipelineLease>    pipelines;
    rstd::vec::Vec<PreparedRenderPassLease>  render_passes;
    rstd::vec::Vec<PreparedFramebufferLease> framebuffers;
    rstd::vec::Vec<PreparedDescriptorLease>  descriptors;
    rstd::vec::Vec<PreparedExternalLease>    externals;
};

class PreparedResourceTable {
public:
    explicit PreparedResourceTable(u64 generation = u64()): m_generation(generation) {}

    bool Insert(PreparedTexture texture) {
        return m_textures.insert(texture.use, rstd::move(texture)).is_none();
    }

    auto Resolve(resource::TextureUseHandle use) const -> Option<ref<PreparedTexture>> {
        return m_textures.get(use);
    }

    bool Insert(PreparedBufferUse buffer) {
        return m_buffers.insert(buffer.use, rstd::move(buffer)).is_none();
    }

    auto Resolve(resource::BufferUseHandle use) const -> Option<ref<PreparedBufferUse>> {
        return m_buffers.get(use);
    }

    bool Insert(PreparedShaderUse shader) {
        return m_shaders.insert(shader.use, rstd::move(shader)).is_none();
    }

    auto Resolve(resource::ShaderUseHandle use) const -> Option<ref<PreparedShaderUse>> {
        return m_shaders.get(use);
    }

    bool Insert(PreparedPipeline pipeline) {
        return m_pipelines.insert(pipeline.use, rstd::move(pipeline)).is_none();
    }

    auto Resolve(resource::PipelineUseHandle use) const -> Option<ref<PreparedPipeline>> {
        return m_pipelines.get(use);
    }

    bool Insert(PreparedRenderPass render_pass) {
        return m_render_passes.insert(render_pass.use, rstd::move(render_pass)).is_none();
    }

    auto Resolve(resource::RenderPassUseHandle use) const -> Option<ref<PreparedRenderPass>> {
        return m_render_passes.get(use);
    }

    bool Insert(PreparedFramebuffer framebuffer) {
        return m_framebuffers.insert(framebuffer.use, rstd::move(framebuffer)).is_none();
    }

    auto Resolve(resource::FramebufferUseHandle use) const -> Option<ref<PreparedFramebuffer>> {
        return m_framebuffers.get(use);
    }

    bool Insert(PreparedDescriptorBinding binding) {
        return m_descriptors.insert(binding.handle, rstd::move(binding)).is_none();
    }

    auto Resolve(resource::DescriptorBindingHandle handle) const
        -> Option<ref<PreparedDescriptorBinding>> {
        return m_descriptors.get(handle);
    }

    auto ResolveMut(resource::DescriptorBindingHandle handle)
        -> Option<mut_ref<PreparedDescriptorBinding>> {
        return m_descriptors.get_mut(handle);
    }

    void Insert(PreparedExternalUse external) {
        (void)m_externals.insert(external.use, rstd::move(external));
    }

    auto Resolve(resource::ExternalUseHandle use) const -> Option<ref<PreparedExternalUse>> {
        return m_externals.get(use);
    }

    auto Generation() const noexcept -> u64 { return m_generation; }
    auto TextureCount() const noexcept -> usize { return m_textures.len(); }
    auto BufferCount() const noexcept -> usize { return m_buffers.len(); }
    auto ShaderCount() const noexcept -> usize { return m_shaders.len(); }
    auto PipelineCount() const noexcept -> usize { return m_pipelines.len(); }
    auto RenderPassCount() const noexcept -> usize { return m_render_passes.len(); }
    auto FramebufferCount() const noexcept -> usize { return m_framebuffers.len(); }
    auto DescriptorCount() const noexcept -> usize { return m_descriptors.len(); }
    auto ExternalCount() const noexcept -> usize { return m_externals.len(); }

    auto Leases() const -> PreparedResourceLeases {
        auto texture_leases = rstd::vec::Vec<PreparedTextureLease>::with_capacity(m_textures.len());
        auto textures       = m_textures.values();
        for (auto texture = textures.next(); texture.is_some(); texture = textures.next()) {
            texture_leases.push(PreparedTextureLease {
                .resource            = (**texture).resource,
                .physical            = (**texture).physical.clone(),
                .physical_generation = (**texture).physical_generation,
            });
        }

        auto descriptor_leases =
            rstd::vec::Vec<PreparedDescriptorLease>::with_capacity(m_descriptors.len());
        auto descriptors = m_descriptors.values();
        for (auto descriptor = descriptors.next(); descriptor.is_some();
             descriptor      = descriptors.next()) {
            descriptor_leases.push(PreparedDescriptorLease {
                .handle = (**descriptor).handle,
                .set    = (**descriptor).set.is_some() ? Some((**descriptor).set->clone())
                                                       : None<vvk::DescriptorSetLease>(),
            });
        }

        auto buffer_leases = rstd::vec::Vec<PreparedBufferLease>::with_capacity(m_buffers.len());
        auto buffers       = m_buffers.values();
        for (auto buffer = buffers.next(); buffer.is_some(); buffer = buffers.next()) {
            buffer_leases.push(PreparedBufferLease {
                .resource = (**buffer).buffer.resource,
                .physical = (**buffer).buffer.physical.clone(),
            });
        }

        auto shader_leases = rstd::vec::Vec<PreparedShaderLease>::with_capacity(m_shaders.len());
        auto shaders       = m_shaders.values();
        for (auto shader = shaders.next(); shader.is_some(); shader = shaders.next()) {
            shader_leases.push(PreparedShaderLease {
                .resource = (**shader).shader.resource,
                .physical = (**shader).shader.physical.clone(),
            });
        }

        auto pipeline_leases =
            rstd::vec::Vec<PreparedPipelineLease>::with_capacity(m_pipelines.len());
        auto pipelines = m_pipelines.values();
        for (auto pipeline = pipelines.next(); pipeline.is_some(); pipeline = pipelines.next()) {
            pipeline_leases.push(PreparedPipelineLease {
                .resource = (**pipeline).resource,
                .physical = (**pipeline).physical.clone(),
            });
        }

        auto render_pass_leases =
            rstd::vec::Vec<PreparedRenderPassLease>::with_capacity(m_render_passes.len());
        auto render_passes = m_render_passes.values();
        for (auto render_pass = render_passes.next(); render_pass.is_some();
             render_pass      = render_passes.next()) {
            render_pass_leases.push(PreparedRenderPassLease {
                .resource = (**render_pass).resource,
                .physical = (**render_pass).physical.clone(),
            });
        }

        auto framebuffer_leases =
            rstd::vec::Vec<PreparedFramebufferLease>::with_capacity(m_framebuffers.len());
        auto framebuffers = m_framebuffers.values();
        for (auto framebuffer = framebuffers.next(); framebuffer.is_some();
             framebuffer      = framebuffers.next()) {
            framebuffer_leases.push(PreparedFramebufferLease {
                .resource = (**framebuffer).resource,
                .physical = (**framebuffer).physical.clone(),
            });
        }
        auto external_leases =
            rstd::vec::Vec<PreparedExternalLease>::with_capacity(m_externals.len());
        auto externals = m_externals.values();
        for (auto external = externals.next(); external.is_some(); external = externals.next()) {
            external_leases.push(PreparedExternalLease {
                .use   = (**external).use,
                .lease = (**external).frame.lease,
            });
        }
        return PreparedResourceLeases {
            .textures      = rstd::move(texture_leases),
            .buffers       = rstd::move(buffer_leases),
            .shaders       = rstd::move(shader_leases),
            .pipelines     = rstd::move(pipeline_leases),
            .render_passes = rstd::move(render_pass_leases),
            .framebuffers  = rstd::move(framebuffer_leases),
            .descriptors   = rstd::move(descriptor_leases),
            .externals     = rstd::move(external_leases),
        };
    }

    void CarryForward(PreparedResourceTable&&        previous,
                      resource::ResourcePlanSections prepared_sections) {
        if (! resource::ResourcePlanIncludes(prepared_sections, resource::ResourcePlanTextures)) {
            m_textures = rstd::move(previous.m_textures);
        }
        if (! resource::ResourcePlanIncludes(prepared_sections, resource::ResourcePlanBuffers)) {
            m_buffers = rstd::move(previous.m_buffers);
        }
        if (! resource::ResourcePlanIncludes(prepared_sections, resource::ResourcePlanShaders)) {
            m_shaders = rstd::move(previous.m_shaders);
        }
        m_pipelines     = rstd::move(previous.m_pipelines);
        m_render_passes = rstd::move(previous.m_render_passes);
        m_framebuffers  = rstd::move(previous.m_framebuffers);
        m_descriptors   = rstd::move(previous.m_descriptors);
        m_externals     = rstd::move(previous.m_externals);
    }

    void ClearPreparedState() {
        m_pipelines.clear();
        m_render_passes.clear();
        m_framebuffers.clear();
        m_descriptors.clear();
        m_externals.clear();
    }

    void Remove(resource::PipelineUseHandle use) { (void)m_pipelines.remove(use); }
    void Remove(resource::RenderPassUseHandle use) { (void)m_render_passes.remove(use); }
    void Remove(resource::FramebufferUseHandle use) { (void)m_framebuffers.remove(use); }
    void Remove(resource::DescriptorBindingHandle use) { (void)m_descriptors.remove(use); }
    void Remove(resource::ExternalUseHandle use) { (void)m_externals.remove(use); }

private:
    using TextureMap = rstd::collections::HashMap<resource::TextureUseHandle, PreparedTexture>;
    using DescriptorMap =
        rstd::collections::HashMap<resource::DescriptorBindingHandle, PreparedDescriptorBinding>;
    using BufferMap   = rstd::collections::HashMap<resource::BufferUseHandle, PreparedBufferUse>;
    using ShaderMap   = rstd::collections::HashMap<resource::ShaderUseHandle, PreparedShaderUse>;
    using PipelineMap = rstd::collections::HashMap<resource::PipelineUseHandle, PreparedPipeline>;
    using RenderPassMap =
        rstd::collections::HashMap<resource::RenderPassUseHandle, PreparedRenderPass>;
    using FramebufferMap =
        rstd::collections::HashMap<resource::FramebufferUseHandle, PreparedFramebuffer>;
    using ExternalMap =
        rstd::collections::HashMap<resource::ExternalUseHandle, PreparedExternalUse>;

    u64            m_generation { 0 };
    TextureMap     m_textures;
    BufferMap      m_buffers;
    ShaderMap      m_shaders;
    PipelineMap    m_pipelines;
    RenderPassMap  m_render_passes;
    FramebufferMap m_framebuffers;
    DescriptorMap  m_descriptors;
    ExternalMap    m_externals;
};

class ResourcePrepareService;

class TexturePrepareTrace {
public:
    enum class Kind
    {
        Plan,
        Upload,
    };

    TexturePrepareTrace(Option<mut_ref<dyn<resource::TexturePrepareObserver>>> observer, Kind kind)
        : m_observer(observer), m_kind(kind) {
        if (m_observer.is_none()) return;
        if (m_kind == Kind::Plan) {
            (*m_observer)->BeginTexturePlan();
        } else {
            (*m_observer)->BeginTextureUpload();
        }
    }

    ~TexturePrepareTrace() { Finish(); }

    TexturePrepareTrace(const TexturePrepareTrace&)                    = delete;
    TexturePrepareTrace(TexturePrepareTrace&&)                         = delete;
    auto operator=(const TexturePrepareTrace&) -> TexturePrepareTrace& = delete;
    auto operator=(TexturePrepareTrace&&) -> TexturePrepareTrace&      = delete;

    void Finish() {
        if (m_observer.is_none()) return;
        if (m_kind == Kind::Plan) {
            (*m_observer)->EndTexturePlan();
        } else {
            (*m_observer)->EndTextureUpload();
        }
        m_observer = None();
    }

private:
    Option<mut_ref<dyn<resource::TexturePrepareObserver>>> m_observer;
    Kind                                                   m_kind;
};

struct ResourceContentProviders {
    Option<mut_ref<dyn<resource::TextureContentProvider>>> texture;
    Option<mut_ref<dyn<resource::BufferContentProvider>>>  buffer;
    Option<mut_ref<dyn<resource::ShaderArtifactProvider>>> shader;
};

class ResourcePlanPrepareVisitor {
public:
    ResourcePlanPrepareVisitor(ResourcePrepareService& service, PreparedResourceTable& table,
                               ResourceContentProviders providers)
        : m_service(service), m_table(table), m_providers(rstd::move(providers)) {}

    auto VisitTexture(const resource::TexturePlanEntry&) -> Result<empty, resource::ResourceError>;
    auto VisitBuffer(const resource::BufferPlanEntry&) -> Result<empty, resource::ResourceError>;
    auto VisitShader(const resource::ShaderPlanEntry&) -> Result<empty, resource::ResourceError>;

private:
    ResourcePrepareService&  m_service;
    PreparedResourceTable&   m_table;
    ResourceContentProviders m_providers;
};

class ResourcePrepareService {
public:
    ResourcePrepareService(resource::TextureRegistry&                        textures,
                           Option<mut_ref<dyn<vulkan::ImagePrepareBackend>>> textures_backend,
                           BufferRegistry&                                   buffers,
                           mut_ref<dyn<vulkan::BufferBackend>>               buffer_backend,
                           ShaderRegistry&                                   shaders)
        : m_textures(textures),
          m_textures_backend(textures_backend),
          m_buffers(buffers),
          m_buffer_backend(buffer_backend),
          m_shaders(shaders) {}

    auto Prepare(const resource::ResourcePlan& plan, ResourceContentProviders providers = {},
                 resource::ResourcePlanSections sections = resource::ResourcePlanAll,
                 Option<mut_ref<dyn<resource::TexturePrepareObserver>>> texture_observer = None())
        -> Result<PreparedResourceTable, resource::ResourceError> {
        PreparedResourceTable    table(plan.generation);
        auto                     texture_provider = rstd::move(providers.texture);
        ResourceContentProviders remaining {
            .buffer = rstd::move(providers.buffer),
            .shader = rstd::move(providers.shader),
        };
        ResourcePlanPrepareVisitor visitor(*this, table, rstd::move(remaining));
        auto object  = rstd::dyn<resource::ResourcePlanVisitor>::from_ref(visitor);
        auto visited = resource::VisitResourcePlan(
            plan,
            object,
            sections & (resource::ResourcePlanBuffers | resource::ResourcePlanShaders));
        if (visited.is_err()) return Err(rstd::move(visited).unwrap_err_unchecked());
        if (resource::ResourcePlanIncludes(sections, resource::ResourcePlanTextures)) {
            auto prepared = PrepareTextures(
                plan.textures.as_slice(), table, rstd::move(texture_provider), texture_observer);
            if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        }
        return Ok(rstd::move(table));
    }

    auto PrepareBuffer(const resource::BufferPlanEntry& entry, PreparedResourceTable& table,
                       Option<mut_ref<dyn<resource::BufferContentProvider>>> content)
        -> Result<empty, resource::ResourceError> {
        if (content.is_none()) {
            return Err(resource::ResourceError {
                .kind = resource::ResourceErrorKind::MissingContent,
                .message =
                    rstd::format("buffer content {} unavailable", entry.request.name.as_str()),
            });
        }
        auto loaded = (*content)->LoadBuffer(entry.request);
        if (loaded.is_err()) return Err(rstd::move(loaded).unwrap_err_unchecked());
        auto prepared =
            m_buffers.Ensure(entry.request.clone(), loaded.unwrap_unchecked(), m_buffer_backend);
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        if (! table.Insert(PreparedBufferUse {
                .use    = entry.handle,
                .buffer = rstd::move(prepared).unwrap_unchecked(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate buffer use {}", entry.handle.index),
            });
        }
        return Ok(empty {});
    }

    auto PrepareShader(const resource::ShaderPlanEntry& entry, PreparedResourceTable& table,
                       Option<mut_ref<dyn<resource::ShaderArtifactProvider>>> provider)
        -> Result<empty, resource::ResourceError> {
        if (provider.is_none()) {
            return Err(resource::ResourceError {
                .kind = resource::ResourceErrorKind::MissingContent,
                .message =
                    rstd::format("shader artifact {} unavailable", entry.request.name.as_str()),
            });
        }
        auto prepared = m_shaders.Prepare(entry.request.clone(), *provider);
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        if (! table.Insert(PreparedShaderUse {
                .use    = entry.handle,
                .shader = rstd::move(prepared).unwrap_unchecked(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate shader use {}", entry.handle.index),
            });
        }
        return Ok(empty {});
    }

    auto PrepareTextures(slice<resource::TexturePlanEntry> entries, PreparedResourceTable& table,
                         Option<mut_ref<dyn<resource::TextureContentProvider>>> content,
                         Option<mut_ref<dyn<resource::TexturePrepareObserver>>> observer)
        -> Result<empty, resource::ResourceError> {
        struct PendingUse {
            resource::TextureUseHandle use;
            resource::TextureHandle    resource;
            resource::TextureRequest   request;
        };
        struct PendingContent {
            String                                  key;
            Vec<PendingUse>                         uses;
            Option<vulkan::PreparedImageAllocation> prepared;
        };

        auto                pending       = Vec<PendingContent>::make();
        auto                pending_index = rstd::collections::HashMap<String, usize>::make();
        TexturePrepareTrace plan_trace(observer, TexturePrepareTrace::Kind::Plan);

        for (usize entry_index {}; entry_index < entries.len(); ++entry_index) {
            const auto& entry  = entries[entry_index];
            auto        handle = m_textures.Register(entry.request.clone());
            if (! handle.Valid()) {
                return Err(resource::ResourceError {
                    .kind = resource::ResourceErrorKind::MissingDefinition,
                    .message =
                        rstd::format("invalid texture request {}", entry.request.name.as_str()),
                });
            }

            auto physical = m_textures.ResolveCurrent(handle);
            if (physical.is_some()) {
                auto inserted =
                    InsertPrepared(entry.handle, handle, entry.request.clone(), **physical, table);
                if (inserted.is_err()) return inserted;
                continue;
            }

            if (entry.request.kind != resource::TextureRequestKind::Imported) {
                auto allocated = AllocateTexture(entry.request);
                if (allocated.is_err()) {
                    return Err(rstd::move(allocated).unwrap_err_unchecked());
                }
                auto published = PublishPrepared(entry.handle,
                                                 handle,
                                                 entry.request.clone(),
                                                 rstd::move(allocated).unwrap_unchecked(),
                                                 None<vulkan::ImageUploadTicket>(),
                                                 table);
                if (published.is_err()) return published;
                continue;
            }

            if (content.is_none()) {
                return Err(resource::ResourceError {
                    .kind = resource::ResourceErrorKind::MissingContent,
                    .message =
                        rstd::format("texture content {} unavailable", entry.request.name.as_str()),
                });
            }

            auto resolved = (*content)->ResolveTextureKey(entry.request);
            if (resolved.is_err()) return Err(rstd::move(resolved).unwrap_err_unchecked());
            auto  key   = rstd::move(resolved).unwrap_unchecked();
            auto  found = pending_index.get(key.as_str());
            usize index;
            if (found.is_some()) {
                index = **found;
            } else {
                index = pending.len();
                (void)pending_index.insert(key.clone(), index);
                pending.push(PendingContent {
                    .key      = rstd::move(key),
                    .uses     = Vec<PendingUse>::make(),
                    .prepared = None<vulkan::PreparedImageAllocation>(),
                });
            }
            pending[index].uses.push(PendingUse {
                .use      = entry.handle,
                .resource = handle,
                .request  = entry.request.clone(),
            });
        }
        plan_trace.Finish();

        constexpr usize batch_size { 4 };
        for (usize offset {}; offset < pending.len(); offset += batch_size) {
            const auto count = rstd::min(batch_size, pending.len() - offset);
            auto       keys  = Vec<String>::with_capacity(count);
            for (usize index {}; index < count; ++index) {
                keys.push(pending[offset + index].key.clone());
            }

            auto loaded = (*content)->LoadTextures(keys.as_slice());
            if (loaded.len() != count) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::BackendFailure,
                    .message = rstd::format(
                        "texture content batch returned {} of {} results", loaded.len(), count),
                });
            }

            TexturePrepareTrace upload_trace(observer, TexturePrepareTrace::Kind::Upload);
            for (usize index {}; index < count; ++index) {
                auto& item = loaded[index];
                if (item.is_err()) return Err(rstd::move(item).unwrap_err_unchecked());
                if (m_textures_backend.is_none()) {
                    return Err(resource::ResourceError {
                        .kind    = resource::ResourceErrorKind::BackendFailure,
                        .message = rstd::format("texture backend unavailable"),
                    });
                }
                auto image   = rstd::move(item).unwrap_unchecked();
                auto created = (*m_textures_backend)->CreateImportedTexture(image.deref());
                if (created.is_none()) {
                    return Err(resource::ResourceError {
                        .kind    = resource::ResourceErrorKind::BackendFailure,
                        .message = rstd::format("create imported texture {} failed",
                                                pending[offset + index].key.as_str()),
                    });
                }

                pending[offset + index].prepared = Some(rstd::move(*created));
            }
        }

        for (auto& content_item : pending) {
            if (content_item.prepared.is_none()) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::BackendFailure,
                    .message = rstd::format("texture content {} was not prepared",
                                            content_item.key.as_str()),
                });
            }
            auto prepared   = rstd::move(content_item.prepared).unwrap_unchecked();
            auto allocation = rstd::move(prepared.allocation);
            for (auto& use : content_item.uses) {
                auto published = PublishPrepared(use.use,
                                                 use.resource,
                                                 rstd::move(use.request),
                                                 allocation.clone(),
                                                 prepared.upload,
                                                 table);
                if (published.is_err()) return published;
            }
        }
        return Ok(empty {});
    }

private:
    auto AllocateTexture(const resource::TextureRequest& request)
        -> Result<rstd::sync::Arc<vulkan::TextureAllocation>, resource::ResourceError> {
        if (m_textures_backend.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("texture backend unavailable"),
            });
        }
        if (request.definition.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("texture definition {} unavailable", request.name.as_str()),
            });
        }
        auto image = (*m_textures_backend)->AllocateTexture(ToTextureKey(*request.definition));
        if (image.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("create texture {} failed", request.name.as_str()),
            });
        }
        return Ok(rstd::move(*image));
    }

    auto PublishPrepared(resource::TextureUseHandle use, resource::TextureHandle handle,
                         resource::TextureRequest                   request,
                         rstd::sync::Arc<vulkan::TextureAllocation> allocation,
                         Option<vulkan::ImageUploadTicket> upload, PreparedResourceTable& table)
        -> Result<empty, resource::ResourceError> {
        auto physical = m_textures.ResolveCurrent(handle);
        if (physical.is_none()) {
            auto prepared_allocation = allocation.clone();
            auto view                = allocation->View();
            auto ready               = resource::ReadyToken {};
            if (upload.is_none()) {
                ready.value = view.slots.empty() ? u64(1) : u64(view.getActive().generation);
            }
            auto published = m_textures.Publish(handle, rstd::move(allocation), ready, upload);
            if (published.is_none()) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::BackendFailure,
                    .message = rstd::format("publish texture {} failed", request.name.as_str()),
                });
            }
            if (upload.is_some()) {
                return InsertPrepared(use,
                                      handle,
                                      rstd::move(request),
                                      rstd::move(prepared_allocation),
                                      *published,
                                      ready,
                                      table);
            }
            physical = m_textures.ResolveCurrent(handle);
        }
        if (physical.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("resolve texture {} failed", request.name.as_str()),
            });
        }
        return InsertPrepared(use, handle, rstd::move(request), **physical, table);
    }

    static auto InsertPrepared(resource::TextureUseHandle use, resource::TextureHandle handle,
                               resource::TextureRequest         request,
                               const resource::TexturePhysical& physical,
                               PreparedResourceTable&           table)
        -> Result<empty, resource::ResourceError> {
        return InsertPrepared(use,
                              handle,
                              rstd::move(request),
                              physical.allocation.clone(),
                              physical.generation,
                              physical.ready,
                              table);
    }

    static auto InsertPrepared(resource::TextureUseHandle use, resource::TextureHandle handle,
                               resource::TextureRequest                   request,
                               rstd::sync::Arc<vulkan::TextureAllocation> allocation,
                               u64 physical_generation, resource::ReadyToken ready,
                               PreparedResourceTable& table)
        -> Result<empty, resource::ResourceError> {
        auto image = allocation->View();
        if (! table.Insert(PreparedTexture {
                .use                 = use,
                .resource            = handle,
                .request             = rstd::move(request),
                .physical            = rstd::move(allocation),
                .image               = rstd::move(image),
                .physical_generation = physical_generation,
                .ready               = ready,
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate texture use {}", use.index),
            });
        }
        return Ok(empty {});
    }

    static auto ToTextureKey(const resource::TextureDefinition& definition) -> vulkan::TextureKey {
        return vulkan::TextureKey {
            .width  = definition.width,
            .height = definition.height,
            .usage  = definition.usage == resource::TextureUsage::Depth ? vulkan::TexUsage::DEPTH
                                                                        : vulkan::TexUsage::COLOR,
            .format = definition.format,
            .sample = definition.sample,
            .mipmap_level = definition.mip_levels.to_primitive(),
            .samples      = TextureSampleCount(definition.samples),
        };
    }

    static auto TextureSampleCount(u32 sample_count) -> VkSampleCountFlagBits {
        switch (sample_count.to_primitive()) {
        case 2: return VK_SAMPLE_COUNT_2_BIT;
        case 4: return VK_SAMPLE_COUNT_4_BIT;
        case 8: return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        case 32: return VK_SAMPLE_COUNT_32_BIT;
        case 64: return VK_SAMPLE_COUNT_64_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
        }
    }

    resource::TextureRegistry&                        m_textures;
    Option<mut_ref<dyn<vulkan::ImagePrepareBackend>>> m_textures_backend;
    BufferRegistry&                                   m_buffers;
    mut_ref<dyn<vulkan::BufferBackend>>               m_buffer_backend;
    ShaderRegistry&                                   m_shaders;
};

inline auto ResourcePlanPrepareVisitor::VisitTexture(const resource::TexturePlanEntry& entry)
    -> Result<empty, resource::ResourceError> {
    (void)entry;
    return Ok(empty {});
}

inline auto ResourcePlanPrepareVisitor::VisitBuffer(const resource::BufferPlanEntry& entry)
    -> Result<empty, resource::ResourceError> {
    return m_service.PrepareBuffer(entry, m_table, m_providers.buffer);
}

inline auto ResourcePlanPrepareVisitor::VisitShader(const resource::ShaderPlanEntry& entry)
    -> Result<empty, resource::ResourceError> {
    return m_service.PrepareShader(entry, m_table, m_providers.shader);
}

} // namespace owe::resource_registry
