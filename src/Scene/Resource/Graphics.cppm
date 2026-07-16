export module wescene.resource_registry:graphics;
import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.vulkan;

import :descriptor;
import :resource_key;

export namespace owe::resource_registry
{

using namespace owe::vulkan;
using namespace rstd::prelude;

struct PipelineResourceEntry {
    PipelineParameters                               pipeline;
    rstd::vec::Vec<resource::DescriptorLayoutHandle> descriptor_layouts;
};

struct PipelineResourceResult {
    std::shared_ptr<PipelineResourceEntry> pipeline;
    resource::PipelineHandle               handle;
    resource::RenderPassHandle             render_pass;
    PipelineCacheKey                       cache_key;
    RenderPassCacheKey                     render_pass_key;
    bool                                   cache_hit { false };
    uint64_t                               cache_observed_count { 0 };
    bool                                   render_pass_cache_hit { false };
    uint64_t                               render_pass_cache_observed_count { 0 };
};

struct FramebufferResourceResult {
    std::shared_ptr<vvk::Framebuffer> framebuffer;
    resource::FramebufferHandle       handle;
    FramebufferCacheKey               cache_key;
    bool                              cache_hit { false };
    uint64_t                          cache_observed_count { 0 };
};

class PipelineCacheDiagnostics {
public:
    PipelineCacheProbe Record(PipelineCacheKey key) {
        auto& count = m_seen[key];
        bool  hit   = count > 0;
        ++count;
        return PipelineCacheProbe {
            .key            = std::move(key),
            .hit            = hit,
            .observed_count = count,
        };
    }

    void Reset() { m_seen.clear(); }

private:
    std::unordered_map<PipelineCacheKey, uint64_t, CanonicalCacheKeyHash, PipelineCacheKeyEqual>
        m_seen;
};

class FramebufferCacheDiagnostics {
public:
    struct Probe {
        FramebufferCacheKey key;
        bool                hit { false };
        uint64_t            observed_count { 0 };
    };

    Probe Record(FramebufferCacheKey key) {
        auto& count = m_seen[key];
        bool  hit   = count > 0;
        ++count;
        return Probe {
            .key            = std::move(key),
            .hit            = hit,
            .observed_count = count,
        };
    }

    void Reset() { m_seen.clear(); }

private:
    std::unordered_map<FramebufferCacheKey, uint64_t, CanonicalCacheKeyHash,
                       FramebufferCacheKeyEqual>
        m_seen;
};

class FramebufferRegistry {
public:
    std::optional<FramebufferResourceResult> Ensure(const Device&                     device,
                                                    const FramebufferResourceRequest& request) {
        if (request.render_pass == VK_NULL_HANDLE || request.attachments.empty()) {
            return std::nullopt;
        }

        auto  desc = MakeFramebufferResourceDesc(request);
        auto  key  = MakeFramebufferCacheKey(desc);
        auto& slot = m_entries[key];
        if (slot.framebuffer) {
            ++slot.observed_count;
            return FramebufferResourceResult {
                .framebuffer          = slot.framebuffer,
                .handle               = slot.handle,
                .cache_key            = key,
                .cache_hit            = true,
                .cache_observed_count = slot.observed_count,
            };
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
            return std::nullopt;
        }
        auto shared      = std::make_shared<vvk::Framebuffer>(std::move(framebuffer));
        slot.framebuffer = shared;
        slot.handle      = NextHandle();
        (void)m_handles.insert(slot.handle, std::weak_ptr<vvk::Framebuffer>(shared));
        ++slot.observed_count;
        return FramebufferResourceResult {
            .framebuffer          = std::move(shared),
            .handle               = slot.handle,
            .cache_key            = key,
            .cache_hit            = false,
            .cache_observed_count = slot.observed_count,
        };
    }

    void PruneExpired() {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (! it->second.framebuffer || it->second.framebuffer.use_count() == 1) {
                (void)m_handles.remove(it->second.handle);
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    void Reset() {
        m_entries.clear();
        m_handles.clear();
        m_next_index = 0;
        ++m_generation;
        if (m_generation == 0) ++m_generation;
    }

    auto Resolve(resource::FramebufferHandle handle) const
        -> std::optional<std::shared_ptr<vvk::Framebuffer>> {
        auto entry = m_handles.get(handle);
        if (entry.is_none()) return std::nullopt;
        auto resource = (**entry).lock();
        if (! resource) return std::nullopt;
        return resource;
    }

    std::size_t entryCount() const { return m_entries.size(); }

private:
    struct Entry {
        std::shared_ptr<vvk::Framebuffer> framebuffer;
        resource::FramebufferHandle       handle;
        uint64_t                          observed_count { 0 };
    };

    auto NextHandle() -> resource::FramebufferHandle {
        return { .index = m_next_index++, .generation = m_generation };
    }

    using HandleMap =
        rstd::collections::HashMap<resource::FramebufferHandle, std::weak_ptr<vvk::Framebuffer>,
                                   resource::ResourceHandleHasher<resource::FramebufferHandle>>;

    std::unordered_map<FramebufferCacheKey, Entry, CanonicalCacheKeyHash, FramebufferCacheKeyEqual>
              m_entries;
    u64       m_generation { 1 };
    u64       m_next_index { 0 };
    HandleMap m_handles;
};

inline bool HasPipelineResources(const PipelineParameters& pipeline) {
    return static_cast<bool>(pipeline.handle) || static_cast<bool>(pipeline.layout) ||
           static_cast<bool>(pipeline.pass);
}

inline bool HasPipelineResources(const PipelineResourceEntry& entry) {
    return HasPipelineResources(entry.pipeline);
}

struct RenderPassResourceResult {
    std::shared_ptr<vvk::RenderPass> render_pass;
    resource::RenderPassHandle       handle;
    RenderPassCacheKey               cache_key;
    bool                             cache_hit { false };
    uint64_t                         cache_observed_count { 0 };
};

class RenderPassRegistry {
public:
    std::optional<RenderPassResourceResult> Ensure(const Device&                 device,
                                                   const RenderPassResourceDesc& desc) {
        auto  key  = MakeRenderPassCacheKey(desc);
        auto& slot = m_entries[key];
        if (slot.render_pass) {
            ++slot.observed_count;
            return RenderPassResourceResult {
                .render_pass          = slot.render_pass,
                .handle               = slot.handle,
                .cache_key            = key,
                .cache_hit            = true,
                .cache_observed_count = slot.observed_count,
            };
        }

        auto created = CreateRenderPass(device, desc);
        if (! created.has_value()) return std::nullopt;
        auto shared      = std::make_shared<vvk::RenderPass>(std::move(*created));
        slot.render_pass = shared;
        slot.handle      = NextHandle();
        (void)m_handles.insert(slot.handle, std::weak_ptr<vvk::RenderPass>(shared));
        ++slot.observed_count;
        return RenderPassResourceResult {
            .render_pass          = std::move(shared),
            .handle               = slot.handle,
            .cache_key            = key,
            .cache_hit            = false,
            .cache_observed_count = slot.observed_count,
        };
    }

    std::optional<RenderPassResourceResult> Ensure(const Device&                  device,
                                                   const PipelineResourceRequest& request) {
        return Ensure(device, MakeRenderPassResourceDesc(request));
    }

    void PruneExpired() {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (! it->second.render_pass || it->second.render_pass.use_count() == 1) {
                (void)m_handles.remove(it->second.handle);
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    void Reset() {
        m_entries.clear();
        m_handles.clear();
        m_next_index = 0;
        ++m_generation;
        if (m_generation == 0) ++m_generation;
    }

    auto Resolve(resource::RenderPassHandle handle) const
        -> std::optional<std::shared_ptr<vvk::RenderPass>> {
        auto entry = m_handles.get(handle);
        if (entry.is_none()) return std::nullopt;
        auto resource = (**entry).lock();
        if (! resource) return std::nullopt;
        return resource;
    }

    std::size_t entryCount() const { return m_entries.size(); }

private:
    struct Entry {
        std::shared_ptr<vvk::RenderPass> render_pass;
        resource::RenderPassHandle       handle;
        uint64_t                         observed_count { 0 };
    };

    auto NextHandle() -> resource::RenderPassHandle {
        return { .index = m_next_index++, .generation = m_generation };
    }

    using HandleMap =
        rstd::collections::HashMap<resource::RenderPassHandle, std::weak_ptr<vvk::RenderPass>,
                                   resource::ResourceHandleHasher<resource::RenderPassHandle>>;

    static std::optional<vvk::RenderPass> CreateRenderPass(const Device&                 device,
                                                           const RenderPassResourceDesc& desc) {
        const bool              has_resolve = desc.samples != VK_SAMPLE_COUNT_1_BIT;
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
        if (device.handle().CreateRenderPass(create, pass) != VK_SUCCESS) return std::nullopt;
        return pass;
    }

    std::unordered_map<RenderPassCacheKey, Entry, CanonicalCacheKeyHash, RenderPassCacheKeyEqual>
              m_entries;
    u64       m_generation { 1 };
    u64       m_next_index { 0 };
    HandleMap m_handles;
};

class PipelineRegistry {
public:
    std::optional<PipelineResourceResult>
    Ensure(const Device& device, PipelineResourceRequest request,
           RenderPassRegistry&                          render_pass_cache,
           resource_registry::DescriptorLayoutRegistry& descriptor_layouts) {
        auto  desc = MakePipelineResourceDesc(request);
        auto  key  = MakePipelineCacheKey(desc);
        auto& slot = m_entries[key];
        if (slot.pipeline) {
            ++slot.observed_count;
            return PipelineResourceResult {
                .pipeline                         = slot.pipeline,
                .handle                           = slot.handle,
                .render_pass                      = slot.render_pass,
                .cache_key                        = key,
                .render_pass_key                  = slot.render_pass_key,
                .cache_hit                        = true,
                .cache_observed_count             = slot.observed_count,
                .render_pass_cache_hit            = true,
                .render_pass_cache_observed_count = slot.observed_count,
            };
        }

        auto render_pass = render_pass_cache.Ensure(device, desc.render_pass);
        if (! render_pass.has_value() || ! render_pass->render_pass) return std::nullopt;

        auto entry = std::make_shared<PipelineResourceEntry>();
        auto vk_descriptor_layouts =
            rstd::vec::Vec<VkDescriptorSetLayout>::with_capacity(desc.descriptor_sets.size());
        entry->descriptor_layouts.reserve(desc.descriptor_sets.size());
        for (const auto& descriptor_set : desc.descriptor_sets) {
            auto layout = descriptor_layouts.Ensure(device, descriptor_set);
            if (layout.is_err()) return std::nullopt;
            auto handle   = rstd::move(layout).unwrap_unchecked();
            auto resolved = descriptor_layouts.Resolve(handle);
            if (resolved.is_none()) return std::nullopt;
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
                vk_descriptor_layouts.data(), vk_descriptor_layouts.len()));
        for (auto& spv : desc.shader_stages) {
            pipeline.addStage(std::make_unique<ShaderSpv>(std::move(spv)));
        }
        if (! pipeline.create(device, **render_pass->render_pass, entry->pipeline)) {
            return std::nullopt;
        }
        entry->pipeline.pass = render_pass->render_pass;
        slot.pipeline        = entry;
        slot.handle          = NextHandle();
        (void)m_handles.insert(slot.handle, std::weak_ptr<PipelineResourceEntry>(entry));
        slot.render_pass_key = render_pass->cache_key;
        slot.render_pass     = render_pass->handle;
        ++slot.observed_count;
        return PipelineResourceResult {
            .pipeline                         = std::move(entry),
            .handle                           = slot.handle,
            .render_pass                      = render_pass->handle,
            .cache_key                        = key,
            .render_pass_key                  = render_pass->cache_key,
            .cache_hit                        = false,
            .cache_observed_count             = slot.observed_count,
            .render_pass_cache_hit            = render_pass->cache_hit,
            .render_pass_cache_observed_count = render_pass->cache_observed_count,
        };
    }

    void PruneExpired() {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (! it->second.pipeline || it->second.pipeline.use_count() == 1) {
                (void)m_handles.remove(it->second.handle);
                it = m_entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    void Reset() {
        m_entries.clear();
        m_handles.clear();
        m_next_index = 0;
        ++m_generation;
        if (m_generation == 0) ++m_generation;
    }

    auto Resolve(resource::PipelineHandle handle) const
        -> std::optional<std::shared_ptr<PipelineResourceEntry>> {
        auto entry = m_handles.get(handle);
        if (entry.is_none()) return std::nullopt;
        auto resource = (**entry).lock();
        if (! resource) return std::nullopt;
        return resource;
    }

    std::size_t entryCount() const { return m_entries.size(); }

private:
    struct Entry {
        std::shared_ptr<PipelineResourceEntry> pipeline;
        resource::PipelineHandle               handle;
        resource::RenderPassHandle             render_pass;
        RenderPassCacheKey                     render_pass_key;
        uint64_t                               observed_count { 0 };
    };

    auto NextHandle() -> resource::PipelineHandle {
        return { .index = m_next_index++, .generation = m_generation };
    }

    using HandleMap =
        rstd::collections::HashMap<resource::PipelineHandle, std::weak_ptr<PipelineResourceEntry>,
                                   resource::ResourceHandleHasher<resource::PipelineHandle>>;

    std::unordered_map<PipelineCacheKey, Entry, CanonicalCacheKeyHash, PipelineCacheKeyEqual>
              m_entries;
    u64       m_generation { 1 };
    u64       m_next_index { 0 };
    HandleMap m_handles;
};

using PipelineResourceCache    = PipelineRegistry;
using RenderPassResourceCache  = RenderPassRegistry;
using FramebufferResourceCache = FramebufferRegistry;

class PipelineRetireQueue {
public:
    void Retire(PipelineParameters&& pipeline) {
        if (! HasPipelineResources(pipeline)) return;
        m_retired.push_back(std::move(pipeline));
    }

    void Retire(std::shared_ptr<PipelineResourceEntry> pipeline) {
        if (! pipeline || ! HasPipelineResources(*pipeline)) return;
        m_pipeline_entries.push_back(std::move(pipeline));
    }

    void Retire(vvk::Framebuffer&& framebuffer) {
        if (! framebuffer) return;
        m_framebuffers.push_back(std::make_shared<vvk::Framebuffer>(std::move(framebuffer)));
    }

    void Retire(std::shared_ptr<vvk::Framebuffer> framebuffer) {
        if (! framebuffer || ! *framebuffer) return;
        m_framebuffers.push_back(std::move(framebuffer));
    }

    void ReleaseReady() {
        m_pipeline_entries.clear();
        m_retired.clear();
    }

    void ReleaseFramebuffersReady() { m_framebuffers.clear(); }

    void ReleaseAllReady() {
        ReleaseFramebuffersReady();
        ReleaseReady();
    }

    std::size_t pending() const {
        return m_pipeline_entries.size() + m_retired.size() + m_framebuffers.size();
    }

private:
    std::vector<std::shared_ptr<PipelineResourceEntry>> m_pipeline_entries;
    std::vector<PipelineParameters>                     m_retired;
    std::vector<std::shared_ptr<vvk::Framebuffer>>      m_framebuffers;
};

class FramebufferResourceSystem {
public:
    explicit FramebufferResourceSystem(const Device&                device,
                                       FramebufferResourceCache*    cache       = nullptr,
                                       FramebufferCacheDiagnostics* diagnostics = nullptr)
        : m_device(&device), m_cache(cache), m_diagnostics(diagnostics) {}

    std::optional<FramebufferResourceResult>
    CreateFramebuffer(const FramebufferResourceRequest& request) const {
        if (request.render_pass == VK_NULL_HANDLE || request.attachments.empty()) {
            return std::nullopt;
        }
        if (m_diagnostics != nullptr) m_diagnostics->Record(MakeFramebufferCacheKey(request));
        FramebufferRegistry local_cache;
        auto&               cache = m_cache != nullptr ? *m_cache : local_cache;
        return cache.Ensure(*m_device, request);
    }

private:
    const Device*                m_device { nullptr };
    FramebufferResourceCache*    m_cache { nullptr };
    FramebufferCacheDiagnostics* m_diagnostics { nullptr };
};

class PipelineResourceSystem {
public:
    explicit PipelineResourceSystem(const Device&                                device,
                                    resource_registry::DescriptorLayoutRegistry& descriptor_layouts,
                                    PipelineResourceCache*   pipeline_cache    = nullptr,
                                    RenderPassResourceCache* render_pass_cache = nullptr)
        : m_device(&device),
          m_descriptor_layouts(
              rstd::mut_ref<resource_registry::DescriptorLayoutRegistry>::from_raw_parts(
                  rstd::addressof(descriptor_layouts))),
          m_pipeline_cache(pipeline_cache),
          m_render_pass_cache(render_pass_cache) {}

    std::optional<PipelineResourceResult>
    CreateGraphicsPipeline(PipelineResourceRequest request) const {
        PipelineRegistry   local_pipeline_cache;
        RenderPassRegistry local_render_pass_cache;
        auto&              pipeline_cache =
            m_pipeline_cache != nullptr ? *m_pipeline_cache : local_pipeline_cache;
        auto& render_pass_cache =
            m_render_pass_cache != nullptr ? *m_render_pass_cache : local_render_pass_cache;
        return pipeline_cache.Ensure(
            *m_device, std::move(request), render_pass_cache, *m_descriptor_layouts.as_mut_ptr());
    }

private:
    const Device*                                              m_device { nullptr };
    rstd::mut_ref<resource_registry::DescriptorLayoutRegistry> m_descriptor_layouts;
    PipelineResourceCache*                                     m_pipeline_cache { nullptr };
    RenderPassResourceCache*                                   m_render_pass_cache { nullptr };
};

} // namespace owe::resource_registry
