module;

export module wescene.vulkan_render:fin_pass;
import wescene.spec_names;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

import :vulkan_pass;
import :resource;

export namespace owe::vulkan
{

// Final pass: blit the scene render target into the present buffer
// (offscreen ExSwapchain slot or surface-mode swapchain image), then
// emit the appropriate barrier so the consumer reads coherent pixels.
class FinPass : public VulkanPass {
public:
    struct Desc {
        // in
        std::string_view                         result { SpecTex_Default }; // scene RT key
        rstd::Option<TextureRequest>             result_request;
        rstd::Option<resource::TextureUseHandle> result_use;

        // resolved in prepare()
        ImageParameters                         vk_result;
        resource_registry::PreparedBarrierBatch result_barrier;

        rstd::Option<resource_registry::PreparedExternalFrame> frame_surface;
    };

    FinPass(Desc&&);
    virtual ~FinPass();

    bool setFrameSurface(owe::FrameSurfaceLease, resource_registry::ExternalResourceBridge&,
                         const DeviceCapabilities&, u32 graphics_queue_family);
    bool setResultRequest(rstd::Option<TextureRequest>);
    void setResultUse(rstd::Option<resource::TextureUseHandle> use) {
        m_desc.result_use = rstd::move(use);
    }
    std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;
    bool prepareResourceStates(resource_registry::ResourceStateTracker&) override;

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void record(PassRecordContext&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    Desc m_desc;
    bool m_path_logged { false };
};

} // namespace owe::vulkan
