export module wescene.resource_registry:graphics;
import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.vulkan;

import :descriptor;
import :resource_key;

using namespace rstd::prelude;

export namespace owe::resource_registry
{

using namespace owe::vulkan;

struct PipelineResourceEntry {
    PipelineParameters                               pipeline;
    rstd::vec::Vec<resource::DescriptorLayoutHandle> descriptor_layouts;
};

struct PipelineResourceResult {
    rstd::sync::Arc<PipelineResourceEntry> pipeline;
    resource::PipelineHandle               handle;
    resource::RenderPassHandle             render_pass;
    rstd::sync::Arc<vvk::RenderPass>       render_pass_physical;
    PipelineCacheKey                       cache_key;
    RenderPassCacheKey                     render_pass_key;
    bool                                   cache_hit { false };
    u64                                    cache_observed_count { 0 };
    bool                                   render_pass_cache_hit { false };
    u64                                    render_pass_cache_observed_count { 0 };
};

struct FramebufferResourceResult {
    rstd::sync::Arc<vvk::Framebuffer> framebuffer;
    resource::FramebufferHandle       handle;
    FramebufferCacheKey               cache_key;
    bool                              cache_hit { false };
    u64                               cache_observed_count { 0 };
};

class PipelineCacheDiagnostics {
public:
    PipelineCacheProbe Record(PipelineCacheKey key) {
        auto count = m_seen.get_mut(key);
        bool hit   = count.is_some();
        u64  observed_count { 1 };
        if (count.is_some()) {
            observed_count = ++**count;
        } else {
            (void)m_seen.insert(key, observed_count);
        }
        return PipelineCacheProbe {
            .key            = std::move(key),
            .hit            = hit,
            .observed_count = observed_count,
        };
    }

    void Reset() { m_seen.clear(); }

private:
    rstd::collections::HashMap<PipelineCacheKey, u64, CanonicalCacheKeyHash, PipelineCacheKeyEqual>
        m_seen;
};

class FramebufferCacheDiagnostics {
public:
    struct Probe {
        FramebufferCacheKey key;
        bool                hit { false };
        u64                 observed_count { 0 };
    };

    Probe Record(FramebufferCacheKey key) {
        auto count = m_seen.get_mut(key);
        bool hit   = count.is_some();
        u64  observed_count { 1 };
        if (count.is_some()) {
            observed_count = ++**count;
        } else {
            (void)m_seen.insert(key, observed_count);
        }
        return Probe {
            .key            = std::move(key),
            .hit            = hit,
            .observed_count = observed_count,
        };
    }

    void Reset() { m_seen.clear(); }

private:
    rstd::collections::HashMap<FramebufferCacheKey, u64, CanonicalCacheKeyHash,
                               FramebufferCacheKeyEqual>
        m_seen;
};

class FramebufferRegistry {
public:
    auto Ensure(const Device& device, const FramebufferResourceRequest& request)
        -> Option<FramebufferResourceResult> {
        if (request.render_pass == VK_NULL_HANDLE || request.attachments.empty()) {
            return None();
        }

        auto desc = MakeFramebufferResourceDesc(request);
        auto key  = MakeFramebufferCacheKey(desc);
        auto slot = m_entries.get_mut(key);
        if (slot.is_some()) {
            ++(**slot).observed_count;
            return Some(FramebufferResourceResult {
                .framebuffer          = (**slot).framebuffer.clone(),
                .handle               = (**slot).handle,
                .cache_key            = key,
                .cache_hit            = true,
                .cache_observed_count = (**slot).observed_count,
            });
        }

        std::vector<VkImageView> attachment_views;
        attachment_views.reserve(desc.attachments.size());
        for (const auto& attachment : desc.attachments) attachment_views.push_back(attachment.view);

        VkFramebufferCreateInfo info {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext           = nullptr,
            .renderPass      = desc.render_pass,
            .attachmentCount = static_cast<uint32_t>(attachment_views.size()),
            .pAttachments    = attachment_views.data(),
            .width           = desc.extent.width,
            .height          = desc.extent.height,
            .layers          = desc.layers,
        };
        vvk::Framebuffer framebuffer;
        if (device.handle().CreateFramebuffer(info, framebuffer) != VK_SUCCESS) {
            return None();
        }
        auto shared = rstd::sync::Arc<vvk::Framebuffer>::make(std::move(framebuffer));
        auto handle = NextHandle();
        (void)m_handles.insert(handle, shared.downgrade());
        (void)m_entries.insert(key,
                               Entry {
                                   .framebuffer    = shared.clone(),
                                   .handle         = handle,
                                   .observed_count = u64(1),
                               });
        return Some(FramebufferResourceResult {
            .framebuffer          = rstd::move(shared),
            .handle               = handle,
            .cache_key            = key,
            .cache_hit            = false,
            .cache_observed_count = u64(1),
        });
    }

    void PruneExpired() {
        m_entries.retain([&](const FramebufferCacheKey&, Entry& entry) {
            if (entry.framebuffer.strong_count() > usize(1)) return true;
            (void)m_handles.remove(entry.handle);
            return false;
        });
    }

    void Reset() {
        m_entries.clear();
        m_handles.clear();
        m_next_index = u64();
        ++m_generation;
        if (m_generation == u64()) ++m_generation;
    }

    auto Resolve(resource::FramebufferHandle handle) const
        -> Option<rstd::sync::Arc<vvk::Framebuffer>> {
        auto entry = m_handles.get(handle);
        if (entry.is_none()) return None();
        auto resource = (**entry).upgrade();
        if (! resource) return None();
        return Some(rstd::move(resource));
    }

    usize entryCount() const { return m_entries.len(); }

private:
    struct Entry {
        rstd::sync::Arc<vvk::Framebuffer> framebuffer;
        resource::FramebufferHandle       handle;
        u64                               observed_count { 0 };
    };

    auto NextHandle() -> resource::FramebufferHandle {
        return { .index = m_next_index++, .generation = m_generation };
    }

    using HandleMap =
        rstd::collections::HashMap<resource::FramebufferHandle, rstd::sync::Weak<vvk::Framebuffer>,
                                   resource::ResourceHandleHasher<resource::FramebufferHandle>>;

    rstd::collections::HashMap<FramebufferCacheKey, Entry, CanonicalCacheKeyHash,
                               FramebufferCacheKeyEqual>
              m_entries;
    u64       m_generation { 1 };
    u64       m_next_index { 0 };
    HandleMap m_handles;
};

inline bool HasPipelineResources(const PipelineParameters& pipeline) {
    return static_cast<bool>(pipeline.handle) || static_cast<bool>(pipeline.layout);
}

inline bool HasPipelineResources(const PipelineResourceEntry& entry) {
    return HasPipelineResources(entry.pipeline);
}

struct RenderPassResourceResult {
    rstd::sync::Arc<vvk::RenderPass> render_pass;
    resource::RenderPassHandle       handle;
    RenderPassCacheKey               cache_key;
    bool                             cache_hit { false };
    u64                              cache_observed_count { 0 };
};

class RenderPassRegistry {
public:
    auto Ensure(const Device& device, const RenderPassResourceDesc& desc)
        -> Option<RenderPassResourceResult> {
        auto key  = MakeRenderPassCacheKey(desc);
        auto slot = m_entries.get_mut(key);
        if (slot.is_some()) {
            ++(**slot).observed_count;
            return Some(RenderPassResourceResult {
                .render_pass          = (**slot).render_pass.clone(),
                .handle               = (**slot).handle,
                .cache_key            = key,
                .cache_hit            = true,
                .cache_observed_count = (**slot).observed_count,
            });
        }

        auto created = CreateRenderPass(device, desc);
        if (created.is_none()) return None();
        auto shared = rstd::sync::Arc<vvk::RenderPass>::make(rstd::move(*created));
        auto handle = NextHandle();
        (void)m_handles.insert(handle, shared.downgrade());
        (void)m_entries.insert(key,
                               Entry {
                                   .render_pass    = shared.clone(),
                                   .handle         = handle,
                                   .observed_count = u64(1),
                               });
        return Some(RenderPassResourceResult {
            .render_pass          = rstd::move(shared),
            .handle               = handle,
            .cache_key            = key,
            .cache_hit            = false,
            .cache_observed_count = u64(1),
        });
    }

    auto Ensure(const Device& device, const PipelineResourceRequest& request)
        -> Option<RenderPassResourceResult> {
        return Ensure(device, MakeRenderPassResourceDesc(request));
    }

    void PruneExpired() {
        m_entries.retain([&](const RenderPassCacheKey&, Entry& entry) {
            if (entry.render_pass.strong_count() > usize(1)) return true;
            (void)m_handles.remove(entry.handle);
            return false;
        });
    }

    void Reset() {
        m_entries.clear();
        m_handles.clear();
        m_next_index = u64();
        ++m_generation;
        if (m_generation == u64()) ++m_generation;
    }

    auto Resolve(resource::RenderPassHandle handle) const
        -> Option<rstd::sync::Arc<vvk::RenderPass>> {
        auto entry = m_handles.get(handle);
        if (entry.is_none()) return None();
        auto resource = (**entry).upgrade();
        if (! resource) return None();
        return Some(rstd::move(resource));
    }

    usize entryCount() const { return m_entries.len(); }

private:
    struct Entry {
        rstd::sync::Arc<vvk::RenderPass> render_pass;
        resource::RenderPassHandle       handle;
        u64                              observed_count { 0 };
    };

    auto NextHandle() -> resource::RenderPassHandle {
        return { .index = m_next_index++, .generation = m_generation };
    }

    using HandleMap =
        rstd::collections::HashMap<resource::RenderPassHandle, rstd::sync::Weak<vvk::RenderPass>,
                                   resource::ResourceHandleHasher<resource::RenderPassHandle>>;

    static auto CreateRenderPass(const Device& device, const RenderPassResourceDesc& desc)
        -> Option<vvk::RenderPass> {
        const bool              has_resolve = desc.has_resolve_attachment;
        VkAttachmentDescription color {
            .format         = desc.color_format,
            .samples        = desc.samples,
            .loadOp         = desc.color_load_op,
            .storeOp        = desc.color_store_op,
            .stencilLoadOp  = desc.color_stencil_load_op,
            .stencilStoreOp = desc.color_stencil_store_op,
            .initialLayout  = desc.color_initial_layout,
            .finalLayout    = has_resolve ? desc.color_attachment_layout : desc.color_final_layout,
        };

        VkAttachmentDescription resolve {
            .format         = desc.color_format,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = desc.resolve_load_op,
            .storeOp        = desc.resolve_store_op,
            .stencilLoadOp  = desc.resolve_stencil_load_op,
            .stencilStoreOp = desc.resolve_stencil_store_op,
            .initialLayout  = desc.resolve_initial_layout,
            .finalLayout    = desc.resolve_final_layout,
        };

        VkAttachmentDescription depth {
            .format         = desc.depth_format,
            .samples        = desc.samples,
            .loadOp         = desc.depth_load_op,
            .storeOp        = desc.depth_store_op,
            .stencilLoadOp  = desc.depth_stencil_load_op,
            .stencilStoreOp = desc.depth_stencil_store_op,
            .initialLayout  = desc.depth_initial_layout,
            .finalLayout    = desc.depth_final_layout,
        };

        VkAttachmentReference color_ref {
            .attachment = 0,
            .layout     = desc.color_attachment_layout,
        };
        VkAttachmentReference resolve_ref {
            .attachment = 1,
            .layout     = desc.resolve_attachment_layout,
        };
        VkAttachmentReference depth_ref {
            .attachment = has_resolve ? 2u : 1u,
            .layout     = desc.depth_attachment_layout,
        };

        std::vector<VkAttachmentDescription> attachments;
        attachments.reserve(3);
        attachments.push_back(color);
        if (has_resolve) attachments.push_back(resolve);
        if (desc.has_depth_attachment) attachments.push_back(depth);

        VkSubpassDescription subpass {
            .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount    = 1,
            .pColorAttachments       = &color_ref,
            .pResolveAttachments     = has_resolve ? &resolve_ref : nullptr,
            .pDepthStencilAttachment = desc.has_depth_attachment ? &depth_ref : nullptr,
        };

        VkSubpassDependency dependency {
            .srcSubpass   = VK_SUBPASS_EXTERNAL,
            .dstSubpass   = 0,
            .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        };

        VkRenderPassCreateInfo create {
            .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = static_cast<uint32_t>(attachments.size()),
            .pAttachments    = attachments.data(),
            .subpassCount    = 1,
            .pSubpasses      = &subpass,
            .dependencyCount = 1,
            .pDependencies   = &dependency,
        };
        vvk::RenderPass pass;
        if (device.handle().CreateRenderPass(create, pass) != VK_SUCCESS) return None();
        return Some(rstd::move(pass));
    }

    rstd::collections::HashMap<RenderPassCacheKey, Entry, CanonicalCacheKeyHash,
                               RenderPassCacheKeyEqual>
              m_entries;
    u64       m_generation { 1 };
    u64       m_next_index { 0 };
    HandleMap m_handles;
};

class PipelineRegistry {
public:
    auto Ensure(const Device& device, PipelineResourceRequest request,
                RenderPassRegistry&                          render_pass_cache,
                resource_registry::DescriptorLayoutRegistry& descriptor_layouts)
        -> Option<PipelineResourceResult> {
        auto desc = MakePipelineResourceDesc(request);
        auto key  = MakePipelineCacheKey(desc);
        auto slot = m_entries.get_mut(key);
        if (slot.is_some()) {
            ++(**slot).observed_count;
            return Some(PipelineResourceResult {
                .pipeline                         = (**slot).pipeline.clone(),
                .handle                           = (**slot).handle,
                .render_pass                      = (**slot).render_pass,
                .render_pass_physical             = (**slot).render_pass_physical.clone(),
                .cache_key                        = key,
                .render_pass_key                  = (**slot).render_pass_key,
                .cache_hit                        = true,
                .cache_observed_count             = (**slot).observed_count,
                .render_pass_cache_hit            = true,
                .render_pass_cache_observed_count = (**slot).observed_count,
            });
        }

        auto render_pass = render_pass_cache.Ensure(device, desc.render_pass);
        if (render_pass.is_none()) return None();

        auto entry                 = rstd::sync::Arc<PipelineResourceEntry>::make();
        auto vk_descriptor_layouts = rstd::vec::Vec<VkDescriptorSetLayout>::with_capacity(
            usize(desc.descriptor_sets.size()));
        entry->descriptor_layouts.reserve(usize(desc.descriptor_sets.size()));
        for (const auto& descriptor_set : desc.descriptor_sets) {
            auto layout = descriptor_layouts.Ensure(device, descriptor_set);
            if (layout.is_err()) return None();
            auto handle   = rstd::move(layout).unwrap_unchecked();
            auto resolved = descriptor_layouts.Resolve(handle);
            if (resolved.is_none()) return None();
            entry->descriptor_layouts.push(resource::DescriptorLayoutHandle {
                .index      = handle.index,
                .generation = handle.generation,
            });
            auto vk_layout = *(**resolved).layout;
            vk_descriptor_layouts.push(rstd::move(vk_layout));
        }
        GraphicsPipeline pipeline;
        pipeline.toDefault();
        pipeline.depth       = desc.depth;
        pipeline.raster      = desc.raster;
        pipeline.multisample = desc.multisample;
        pipeline
            .setColorBlendStates(
                std::span<const VkPipelineColorBlendAttachmentState>(&desc.color_blend, 1))
            .setCreateInfoOptions(desc.create_flags, desc.subpass)
            .setColorBlendOptions(desc.color_blend_flags, desc.blend_constants)
            .setLogicOp(desc.logic_op_enable, desc.logic_op)
            .setTopology(desc.topology)
            .setPrimitiveRestartEnable(desc.primitive_restart_enable)
            .setViewportScissorCount(desc.viewport_count, desc.scissor_count)
            .setDynamicStates(desc.dynamic_states)
            .addInputBindingDescription(desc.vertex_bindings)
            .addInputAttributeDescription(desc.vertex_attrs)
            .setDescriptorSetLayouts(std::span<const VkDescriptorSetLayout>(
                vk_descriptor_layouts.data(), vk_descriptor_layouts.len().to_primitive()));
        for (auto& spv : desc.shader_stages) {
            pipeline.addStage(Box<ShaderSpv>::make(std::move(spv)));
        }
        if (! pipeline.create(device, **render_pass->render_pass, entry->pipeline)) {
            return None();
        }
        auto handle = NextHandle();
        (void)m_handles.insert(handle, entry.downgrade());
        (void)m_entries.insert(key,
                               Entry {
                                   .pipeline             = entry.clone(),
                                   .handle               = handle,
                                   .render_pass          = render_pass->handle,
                                   .render_pass_physical = render_pass->render_pass.clone(),
                                   .render_pass_key      = render_pass->cache_key,
                                   .observed_count       = u64(1),
                               });
        return Some(PipelineResourceResult {
            .pipeline                         = rstd::move(entry),
            .handle                           = handle,
            .render_pass                      = render_pass->handle,
            .render_pass_physical             = render_pass->render_pass.clone(),
            .cache_key                        = key,
            .render_pass_key                  = render_pass->cache_key,
            .cache_hit                        = false,
            .cache_observed_count             = u64(1),
            .render_pass_cache_hit            = render_pass->cache_hit,
            .render_pass_cache_observed_count = render_pass->cache_observed_count,
        });
    }

    void PruneExpired() {
        m_entries.retain([&](const PipelineCacheKey&, Entry& entry) {
            if (entry.pipeline.strong_count() > usize(1)) return true;
            (void)m_handles.remove(entry.handle);
            return false;
        });
    }

    void Reset() {
        m_entries.clear();
        m_handles.clear();
        m_next_index = u64();
        ++m_generation;
        if (m_generation == u64()) ++m_generation;
    }

    auto Resolve(resource::PipelineHandle handle) const
        -> Option<rstd::sync::Arc<PipelineResourceEntry>> {
        auto entry = m_handles.get(handle);
        if (entry.is_none()) return None();
        auto resource = (**entry).upgrade();
        if (! resource) return None();
        return Some(rstd::move(resource));
    }

    usize entryCount() const { return m_entries.len(); }

private:
    struct Entry {
        rstd::sync::Arc<PipelineResourceEntry> pipeline;
        resource::PipelineHandle               handle;
        resource::RenderPassHandle             render_pass;
        rstd::sync::Arc<vvk::RenderPass>       render_pass_physical;
        RenderPassCacheKey                     render_pass_key;
        u64                                    observed_count { 0 };
    };

    auto NextHandle() -> resource::PipelineHandle {
        return { .index = m_next_index++, .generation = m_generation };
    }

    using HandleMap =
        rstd::collections::HashMap<resource::PipelineHandle,
                                   rstd::sync::Weak<PipelineResourceEntry>,
                                   resource::ResourceHandleHasher<resource::PipelineHandle>>;

    rstd::collections::HashMap<PipelineCacheKey, Entry, CanonicalCacheKeyHash,
                               PipelineCacheKeyEqual>
              m_entries;
    u64       m_generation { 1 };
    u64       m_next_index { 0 };
    HandleMap m_handles;
};

using PipelineResourceCache    = PipelineRegistry;
using RenderPassResourceCache  = RenderPassRegistry;
using FramebufferResourceCache = FramebufferRegistry;

class FramebufferResourceSystem {
public:
    explicit FramebufferResourceSystem(const Device& device, FramebufferResourceCache& cache,
                                       FramebufferCacheDiagnostics& diagnostics)
        : m_device(ref<Device>::from_raw_parts(rstd::addressof(device))),
          m_cache(mut_ref<FramebufferResourceCache>::from_raw_parts(rstd::addressof(cache))),
          m_diagnostics(
              mut_ref<FramebufferCacheDiagnostics>::from_raw_parts(rstd::addressof(diagnostics))) {}

    auto CreateFramebuffer(const FramebufferResourceRequest& request)
        -> Option<FramebufferResourceResult> {
        if (request.render_pass == VK_NULL_HANDLE || request.attachments.empty()) {
            return None();
        }
        m_diagnostics->Record(MakeFramebufferCacheKey(request));
        return m_cache->Ensure(*m_device, request);
    }

private:
    ref<Device>                          m_device;
    mut_ref<FramebufferResourceCache>    m_cache;
    mut_ref<FramebufferCacheDiagnostics> m_diagnostics;
};

class PipelineResourceSystem {
public:
    explicit PipelineResourceSystem(const Device&                                device,
                                    resource_registry::DescriptorLayoutRegistry& descriptor_layouts,
                                    PipelineResourceCache&                       pipeline_cache,
                                    RenderPassResourceCache&                     render_pass_cache)
        : m_device(ref<Device>::from_raw_parts(rstd::addressof(device))),
          m_descriptor_layouts(
              rstd::mut_ref<resource_registry::DescriptorLayoutRegistry>::from_raw_parts(
                  rstd::addressof(descriptor_layouts))),
          m_pipeline_cache(
              mut_ref<PipelineResourceCache>::from_raw_parts(rstd::addressof(pipeline_cache))),
          m_render_pass_cache(mut_ref<RenderPassResourceCache>::from_raw_parts(
              rstd::addressof(render_pass_cache))) {}

    auto CreateGraphicsPipeline(PipelineResourceRequest request) -> Option<PipelineResourceResult> {
        return m_pipeline_cache->Ensure(
            *m_device, std::move(request), *m_render_pass_cache, *m_descriptor_layouts);
    }

private:
    ref<Device>                                                m_device;
    rstd::mut_ref<resource_registry::DescriptorLayoutRegistry> m_descriptor_layouts;
    mut_ref<PipelineResourceCache>                             m_pipeline_cache;
    mut_ref<RenderPassResourceCache>                           m_render_pass_cache;
};

} // namespace owe::resource_registry
