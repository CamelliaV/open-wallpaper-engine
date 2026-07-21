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
        std::string_view                          result { SpecTex_Default }; // scene RT key
        rstd::Option<TextureRequest>              result_request;
        rstd::Option<resource::TextureUseHandle>  result_use;
        rstd::Option<resource::ExternalUseHandle> external_use;
        resource_registry::PreparedBarrierBatch   result_barrier;
    };

    FinPass(Desc&&);
    virtual ~FinPass();

    bool setFrameSurface(owe::FrameSurfaceLease,
                         rstd::mut_ref<rstd::dyn<resource_registry::ExternalResourcePreparer>>,
                         const DeviceCapabilities&, rstd::uint32_t graphics_queue_family);
    bool setResultRequest(rstd::Option<TextureRequest>);
    void setResultUse(rstd::Option<resource::TextureUseHandle> use) {
        m_desc.result_use = rstd::move(use);
    }
    void             declareResources(ResourceDeclarationContext&) override;
    PassResourceUses resourceUses() const override;
    std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;
    bool                                      prepareResourceStates(
        rstd::mut_ref<rstd::dyn<resource_registry::TextureStatePreparer>>) override;

    void prepare(Scene&, const Device&, PassPrepareContext&) override;
    void record(PassRecordContext&) override;
    void destory(const Device&) override;

private:
    Desc m_desc;
    bool m_path_logged { false };
};

} // namespace owe::vulkan
