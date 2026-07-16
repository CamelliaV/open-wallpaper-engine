export module wescene.vulkan_render:vulkan_pass;
import rstd;
import rstd.cppstd;
import wescene.rgraph;
import wescene.vulkan;
import wescene.scene;

import :resource;

export namespace owe
{

namespace vulkan
{

using PassInvalidationFlags = uint32_t;

enum class PassInvalidation : PassInvalidationFlags
{
    Resources   = 1u << 0u,
    Pipeline    = 1u << 1u,
    Framebuffer = 1u << 2u,
};

inline constexpr PassInvalidationFlags PassInvalidationNone { 0u };
inline constexpr PassInvalidationFlags PassInvalidationAll {
    static_cast<PassInvalidationFlags>(PassInvalidation::Resources) |
        static_cast<PassInvalidationFlags>(PassInvalidation::Pipeline) |
        static_cast<PassInvalidationFlags>(PassInvalidation::Framebuffer),
};

inline constexpr PassInvalidationFlags ToPassInvalidationFlags(PassInvalidation invalidation) {
    return static_cast<PassInvalidationFlags>(invalidation);
}

struct MaterialTextureBindingRefresh {
    PassInvalidationFlags invalidation_flags { PassInvalidationNone };
    bool                  requires_graph_rebuild { false };
};

struct PassTextureRequestDiagnostic {
    std::string                  role;
    uint32_t                     slot { 0 };
    std::string                  name;
    rstd::Option<TextureRequest> request;
};

struct PassRecordContext {
    rstd::mut_ref<vvk::CommandBuffer>                   command;
    rstd::ref<resource_registry::PreparedResourceTable> prepared_resources;
    rstd::Option<rstd::mut_ref<StagingBuffer>>          dynamic_buffer;
};

class VulkanPass : public rg::Pass {
public:
    VulkanPass()                                                                      = default;
    virtual ~VulkanPass()                                                             = default;
    virtual void                  prepare(Scene&, const Device&, RenderingResources&) = 0;
    virtual void                  record(PassRecordContext&)                          = 0;
    virtual void                  destory(const Device&, RenderingResources&)         = 0;
    virtual PassInvalidationFlags finalizeResourceRequests(Scene&) { return PassInvalidationNone; }
    virtual bool prepareResourceStates(resource_registry::ResourceStateTracker&) { return true; }
    virtual std::optional<RenderItemId>        renderItemId() const { return std::nullopt; }
    virtual std::optional<PipelineCacheKey>    pipelineCacheKey() const { return std::nullopt; }
    virtual bool                               pipelineCacheHit() const { return false; }
    virtual uint64_t                           pipelineCacheObservedCount() const { return 0; }
    virtual std::optional<RenderPassCacheKey>  renderPassCacheKey() const { return std::nullopt; }
    virtual bool                               renderPassCacheHit() const { return false; }
    virtual uint64_t                           renderPassCacheObservedCount() const { return 0; }
    virtual std::optional<FramebufferCacheKey> framebufferCacheKey() const { return std::nullopt; }
    virtual bool                               framebufferCacheHit() const { return false; }
    virtual uint64_t                           framebufferCacheObservedCount() const { return 0; }
    virtual std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const {
        return {};
    }
    virtual MaterialTextureBindingRefresh
    refreshMaterialTextureBindings(const RenderSceneSnapshot&) {
        return {};
    }
    virtual bool setTextureBinding(uint32_t, TextureBindingRequest) { return false; }
    virtual bool supportsRenderScope() const { return false; }
    virtual bool canJoinRenderScopeAfter(const VulkanPass&) const { return false; }
    virtual void prepareRenderScopeDraw(PassRecordContext&) {}
    virtual void beginRenderScope(PassRecordContext&) {}
    virtual void recordRenderScopeDraw(PassRecordContext&) {}
    virtual void endRenderScope(PassRecordContext&) {}

    bool prepared() const { return m_prepared; }
    void resetPrepared() { setPrepared(false); }

protected:
    void setPrepared(bool v = true) { m_prepared = v; }

private:
    bool m_prepared { false };
};

} // namespace vulkan
} // namespace owe
