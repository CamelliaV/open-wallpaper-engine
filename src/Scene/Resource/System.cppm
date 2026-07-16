export module wescene.resource_registry:system;
import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.vulkan;

import :owner;
import :prepared;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

struct PipelinePreparation {
    resource::PipelineHandle   resource;
    resource::RenderPassHandle render_pass;
    vulkan::PipelineCacheKey   cache_key;
    vulkan::RenderPassCacheKey render_pass_key;
    bool                       cache_hit { false };
    u64                        cache_observed_count { 0 };
    bool                       render_pass_cache_hit { false };
    u64                        render_pass_cache_observed_count { 0 };
};

struct FramebufferPreparation {
    resource::FramebufferHandle resource;
    vulkan::FramebufferCacheKey cache_key;
    bool                        cache_hit { false };
    u64                         cache_observed_count { 0 };
};

struct RenderPassPreparation {
    resource::RenderPassHandle resource;
    vulkan::RenderPassCacheKey cache_key;
    bool                       cache_hit { false };
    u64                        cache_observed_count { 0 };
};

struct GraphicsResourcePreparer {
    using Trait                  = GraphicsResourcePreparer;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = GraphicsResourcePreparer;

        auto PreparePipeline(resource::PipelineUseHandle   pipeline_use,
                             resource::RenderPassUseHandle render_pass_use,
                             const vulkan::Device& device, vulkan::PipelineResourceRequest request)
            -> Result<PipelinePreparation, resource::ResourceError> {
            return rstd::trait_call<0>(
                this, pipeline_use, render_pass_use, device, rstd::move(request));
        }

        auto PrepareFramebuffer(resource::FramebufferUseHandle                 framebuffer_use,
                                resource::RenderPassUseHandle                  render_pass_use,
                                const vulkan::Device&                          device,
                                std::vector<vulkan::FramebufferAttachmentDesc> attachments,
                                VkExtent2D                                     extent)
            -> Result<FramebufferPreparation, resource::ResourceError> {
            return rstd::trait_call<1>(
                this, framebuffer_use, render_pass_use, device, rstd::move(attachments), extent);
        }

        auto PrepareRenderPass(resource::RenderPassUseHandle use, const vulkan::Device& device,
                               const vulkan::RenderPassResourceDesc& desc)
            -> Result<RenderPassPreparation, resource::ResourceError> {
            return rstd::trait_call<2>(this, use, device, desc);
        }

        auto PrepareDescriptor(const vulkan::Device&           device,
                               resource::PipelineUseHandle     pipeline_use,
                               slice<DescriptorImageBinding>   images,
                               Option<DescriptorBufferBinding> buffer)
            -> Result<resource::DescriptorBindingHandle, resource::ResourceError> {
            return rstd::trait_call<3>(this, device, pipeline_use, images, rstd::move(buffer));
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::PreparePipeline, &T::PrepareFramebuffer, &T::PrepareRenderPass,
                             &T::PrepareDescriptor>;
};

struct ExternalResourcePreparer {
    using Trait                  = ExternalResourcePreparer;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ExternalResourcePreparer;

        auto PrepareExternal(resource::ExternalUseHandle       external_use,
                             resource::TextureUseHandle        source_use,
                             const vulkan::DeviceCapabilities& capabilities,
                             FrameSurfaceLease lease, u32 graphics_queue_family)
            -> Result<empty, resource::ResourceError> {
            return rstd::trait_call<0>(this,
                                       external_use,
                                       source_use,
                                       capabilities,
                                       rstd::move(lease),
                                       graphics_queue_family);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::PrepareExternal>;
};

class RenderResourceSystem {
public:
    bool Initialize(const vulkan::Device& device) { return m_registries.Initialize(device); }

    void Reset() {
        m_prepared = PreparedResourceTable {};
        m_registries.Reset();
    }

    bool Ready() const noexcept { return m_registries.Ready(); }

    auto PreparePlan(const resource::ResourcePlan& plan, ResourceContentProviders providers)
        -> Result<empty, resource::ResourceError> {
        auto image_backend = dyn<vulkan::ImagePrepareBackend>::from_ref(m_registries.Textures());
        auto buffer_backend =
            dyn<vulkan::BufferUploadBackend>::from_ref(m_registries.BufferUploads());
        ResourcePrepareService service(m_registries.TextureEntries(),
                                       Some(image_backend.as_mut_ref()),
                                       m_registries.Buffers(),
                                       buffer_backend.as_mut_ref(),
                                       m_registries.Shaders());
        auto                   prepared = service.Prepare(plan, rstd::move(providers));
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        auto next = rstd::move(prepared).unwrap_unchecked();
        if (! m_registries.States().Compile(plan, next)) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("compile resource state plan failed"),
            });
        }
        m_prepared = rstd::move(next);
        return Ok(empty {});
    }

    auto Prepared() const -> const PreparedResourceTable& { return m_prepared; }
    auto States() -> ResourceStateTracker& { return m_registries.States(); }

    auto PreparePipeline(resource::PipelineUseHandle   pipeline_use,
                         resource::RenderPassUseHandle render_pass_use,
                         const vulkan::Device& device, vulkan::PipelineResourceRequest request)
        -> Result<PipelinePreparation, resource::ResourceError> {
        PipelineResourceSystem system(device,
                                      m_registries.DescriptorLayouts(),
                                      m_registries.PipelineCache(),
                                      m_registries.RenderPassCache());
        auto                   result = system.CreateGraphicsPipeline(std::move(request));
        if (result.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("prepare graphics pipeline failed"),
            });
        }
        if (! m_prepared.Insert(PreparedRenderPass {
                .use       = render_pass_use,
                .resource  = result->render_pass,
                .cache_key = result->render_pass_key,
                .physical  = result->render_pass_physical.clone(),
            }) ||
            ! m_prepared.Insert(PreparedPipeline {
                .use         = pipeline_use,
                .resource    = result->handle,
                .render_pass = result->render_pass,
                .physical    = result->pipeline.clone(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate prepared graphics use"),
            });
        }
        return Ok(PipelinePreparation {
            .resource                         = result->handle,
            .render_pass                      = result->render_pass,
            .cache_key                        = rstd::move(result->cache_key),
            .render_pass_key                  = rstd::move(result->render_pass_key),
            .cache_hit                        = result->cache_hit,
            .cache_observed_count             = result->cache_observed_count,
            .render_pass_cache_hit            = result->render_pass_cache_hit,
            .render_pass_cache_observed_count = result->render_pass_cache_observed_count,
        });
    }

    auto PrepareFramebuffer(resource::FramebufferUseHandle                 framebuffer_use,
                            resource::RenderPassUseHandle                  render_pass_use,
                            const vulkan::Device&                          device,
                            std::vector<vulkan::FramebufferAttachmentDesc> attachments,
                            VkExtent2D                                     extent)
        -> Result<FramebufferPreparation, resource::ResourceError> {
        auto render_pass = m_prepared.Resolve(render_pass_use);
        if (render_pass.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("prepared render pass unavailable"),
            });
        }
        FramebufferResourceSystem system(
            device, m_registries.FramebufferCache(), m_registries.FramebufferDiagnostics());
        auto result = system.CreateFramebuffer(vulkan::FramebufferResourceRequest {
            .render_pass     = **(**render_pass).physical,
            .render_pass_key = (**render_pass).cache_key,
            .attachments     = rstd::move(attachments),
            .extent          = extent,
        });
        if (result.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("prepare framebuffer failed"),
            });
        }
        if (! m_prepared.Insert(PreparedFramebuffer {
                .use      = framebuffer_use,
                .resource = result->handle,
                .physical = result->framebuffer.clone(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate prepared framebuffer use"),
            });
        }
        return Ok(FramebufferPreparation {
            .resource             = result->handle,
            .cache_key            = rstd::move(result->cache_key),
            .cache_hit            = result->cache_hit,
            .cache_observed_count = result->cache_observed_count,
        });
    }

    auto PrepareRenderPass(resource::RenderPassUseHandle use, const vulkan::Device& device,
                           const vulkan::RenderPassResourceDesc& desc)
        -> Result<RenderPassPreparation, resource::ResourceError> {
        auto result = m_registries.RenderPasses().Ensure(device, desc);
        if (result.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("prepare render pass failed"),
            });
        }
        if (! m_prepared.Insert(PreparedRenderPass {
                .use       = use,
                .resource  = result->handle,
                .cache_key = result->cache_key,
                .physical  = result->render_pass.clone(),
            })) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate prepared render pass use"),
            });
        }
        return Ok(RenderPassPreparation {
            .resource             = result->handle,
            .cache_key            = rstd::move(result->cache_key),
            .cache_hit            = result->cache_hit,
            .cache_observed_count = result->cache_observed_count,
        });
    }

    auto PrepareDescriptor(const vulkan::Device& device, resource::PipelineUseHandle pipeline_use,
                           slice<DescriptorImageBinding>   images,
                           Option<DescriptorBufferBinding> buffer)
        -> Result<resource::DescriptorBindingHandle, resource::ResourceError> {
        auto pipeline = m_prepared.Resolve(pipeline_use);
        if (pipeline.is_none() || (**pipeline).physical->descriptor_layouts.is_empty()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("pipeline descriptor layout unavailable"),
            });
        }
        auto layout =
            m_registries.DescriptorLayouts().Resolve((**pipeline).physical->descriptor_layouts[0]);
        if (layout.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("descriptor layout unavailable"),
            });
        }
        auto prepared =
            m_registries.Descriptors().Prepare(device, **layout, images, rstd::move(buffer));
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        auto binding = rstd::move(prepared).unwrap_unchecked();
        auto handle  = binding.handle;
        if (! m_prepared.Insert(rstd::move(binding))) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::BackendFailure,
                .message = rstd::format("duplicate descriptor binding {}", handle.index),
            });
        }
        return Ok(handle);
    }

    bool PreparePendingUploads() { return m_registries.BufferUploads().preparePendingUploads(); }

    bool RecordPendingUploads(vvk::CommandBuffer& command) {
        return m_registries.BufferUploads().recordPendingUploads(command);
    }

    auto UpdateBuffer(resource::BufferUseHandle use, slice<u8> content, u64 content_version)
        -> Result<empty, resource::ResourceError> {
        auto prepared = m_prepared.Resolve(use);
        if (prepared.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("prepared buffer use {} is unavailable", use.index),
            });
        }
        auto backend = dyn<vulkan::BufferUploadBackend>::from_ref(m_registries.BufferUploads());
        return m_registries.Buffers().Update(
            (**prepared).buffer.resource, content, content_version, backend.as_mut_ref());
    }

    auto ReserveUpload() -> resource::ReadyToken { return m_registries.Uploads().Reserve(); }
    bool MarkUploadSubmitted(resource::ReadyToken token) {
        return m_registries.Uploads().MarkSubmitted(token, m_prepared);
    }
    auto PendingUpload() -> Option<resource::ReadyToken> {
        return m_registries.Uploads().Pending();
    }
    void CompleteUploadsThrough(u64 value) { m_registries.Uploads().CompleteThrough(value); }

    auto BeginSubmission() -> resource::CompletionToken {
        return m_registries.Submissions().Begin(m_prepared);
    }
    auto CompleteSubmission(resource::CompletionToken token) -> Option<SubmissionLease> {
        return m_registries.Submissions().Complete(token);
    }

    auto PrepareExternal(resource::ExternalUseHandle       external_use,
                         resource::TextureUseHandle        source_use,
                         const vulkan::DeviceCapabilities& capabilities, FrameSurfaceLease lease,
                         u32 graphics_queue_family) -> Result<empty, resource::ResourceError> {
        auto source = m_prepared.Resolve(source_use);
        if (source.is_none()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("prepared external source unavailable"),
            });
        }
        auto prepared =
            m_registries.External().Prepare(capabilities, rstd::move(lease), graphics_queue_family);
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        m_prepared.Insert(PreparedExternalUse {
            .use   = external_use,
            .frame = rstd::move(prepared).unwrap_unchecked(),
        });
        return Ok(empty {});
    }

    void Collect(mut_ref<dyn<vulkan::MemoryBudgetSource>> memory) {
        m_registries.PipelineCache().PruneExpired();
        m_registries.RenderPassCache().PruneExpired();
        m_registries.FramebufferCache().PruneExpired();
        m_registries.Memory().Refresh(memory.as_ref());
        if (! m_registries.Memory().ShouldEvictTransient()) return;
        m_registries.TextureEntries().EvictUnused();
        m_registries.Buffers().EvictUnused();
    }

    void PumpVideoTextures(double seconds) { m_registries.Textures().PumpVideoTextures(seconds); }
    void SetVideoDecodeOptions(vulkan::TextureCache::VideoDecodeOptions options) {
        m_registries.Textures().SetVideoDecodeOptions(std::move(options));
    }
    bool UploadFontAtlasRegion(const std::string& key, const std::uint8_t* atlas,
                               std::uint32_t atlas_width, std::uint32_t x, std::uint32_t y,
                               std::uint32_t width, std::uint32_t height) {
        auto handle = m_registries.TextureEntries().Find(resource::TextureRequestKind::Imported,
                                                         rstd::cppstd::as_str(key));
        if (handle.is_none()) return false;
        auto physical = m_registries.TextureEntries().ResolveCurrent(*handle);
        if (physical.is_none()) return false;
        return m_registries.Textures().UploadFontAtlasRegion(
            (**physical).allocation.deref(), atlas, atlas_width, x, y, width, height);
    }
    auto CreateLocalSwapchain(const vulkan::Device& device, unsigned width, unsigned height,
                              VkImageTiling tiling) -> std::shared_ptr<vulkan::LocalExSwapchain> {
        return vulkan::CreateLocalExSwapchain(
            device, m_registries.Textures(), width, height, tiling);
    }
    void ClearTextures() {
        m_prepared = PreparedResourceTable {};
        m_registries.TextureEntries().Reset();
        m_registries.Textures().Clear();
    }
    void ClearTransientTextures() { m_registries.TextureEntries().ClearGraphResources(); }
    void EvictUnusedBuffers() { m_registries.Buffers().EvictUnused(); }

    auto PipelineDiagnostics() -> PipelineCacheDiagnostics& {
        return m_registries.PipelineDiagnostics();
    }
    auto FramebufferDiagnostics() -> FramebufferCacheDiagnostics& {
        return m_registries.FramebufferDiagnostics();
    }

private:
    ResourceRegistries    m_registries;
    PreparedResourceTable m_prepared;
};

} // namespace owe::resource_registry

export namespace rstd
{

template<>
struct Impl<owe::resource_registry::GraphicsResourcePreparer,
            owe::resource_registry::RenderResourceSystem>
    : ImplBase<owe::resource_registry::RenderResourceSystem> {
    auto PreparePipeline(owe::resource::PipelineUseHandle     pipeline_use,
                         owe::resource::RenderPassUseHandle   render_pass_use,
                         const owe::vulkan::Device&           device,
                         owe::vulkan::PipelineResourceRequest request)
        -> Result<owe::resource_registry::PipelinePreparation, owe::resource::ResourceError> {
        return this->self().PreparePipeline(
            pipeline_use, render_pass_use, device, rstd::move(request));
    }

    auto PrepareFramebuffer(owe::resource::FramebufferUseHandle                 framebuffer_use,
                            owe::resource::RenderPassUseHandle                  render_pass_use,
                            const owe::vulkan::Device&                          device,
                            std::vector<owe::vulkan::FramebufferAttachmentDesc> attachments,
                            VkExtent2D                                          extent)
        -> Result<owe::resource_registry::FramebufferPreparation, owe::resource::ResourceError> {
        return this->self().PrepareFramebuffer(
            framebuffer_use, render_pass_use, device, rstd::move(attachments), extent);
    }

    auto PrepareRenderPass(owe::resource::RenderPassUseHandle         use,
                           const owe::vulkan::Device&                 device,
                           const owe::vulkan::RenderPassResourceDesc& desc)
        -> Result<owe::resource_registry::RenderPassPreparation, owe::resource::ResourceError> {
        return this->self().PrepareRenderPass(use, device, desc);
    }

    auto PrepareDescriptor(const owe::vulkan::Device&                              device,
                           owe::resource::PipelineUseHandle                        pipeline_use,
                           slice<owe::resource_registry::DescriptorImageBinding>   images,
                           Option<owe::resource_registry::DescriptorBufferBinding> buffer)
        -> Result<owe::resource::DescriptorBindingHandle, owe::resource::ResourceError> {
        return this->self().PrepareDescriptor(device, pipeline_use, images, rstd::move(buffer));
    }
};

template<>
struct Impl<owe::resource_registry::ExternalResourcePreparer,
            owe::resource_registry::RenderResourceSystem>
    : ImplBase<owe::resource_registry::RenderResourceSystem> {
    auto PrepareExternal(owe::resource::ExternalUseHandle       external_use,
                         owe::resource::TextureUseHandle        source_use,
                         const owe::vulkan::DeviceCapabilities& capabilities,
                         owe::FrameSurfaceLease lease, u32 graphics_queue_family)
        -> Result<empty, owe::resource::ResourceError> {
        return this->self().PrepareExternal(
            external_use, source_use, capabilities, rstd::move(lease), graphics_queue_family);
    }
};

template<>
struct Impl<owe::resource::BufferContentWriter, owe::resource_registry::RenderResourceSystem>
    : ImplBase<owe::resource_registry::RenderResourceSystem> {
    auto UpdateBuffer(owe::resource::BufferUseHandle use, slice<u8> content, u64 content_version)
        -> Result<empty, owe::resource::ResourceError> {
        return this->self().UpdateBuffer(use, content, content_version);
    }
};

} // namespace rstd
