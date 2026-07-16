module;

export module wescene.vulkan_render:pre_pass;
import wescene.spec_names;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

import :vulkan_pass;
import :resource;

export namespace owe::vulkan
{

class PrePass : public VulkanPass {
public:
    struct Desc {
        // in
        std::string_view result { SpecTex_Default };
        VkImageLayout    layout { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        rstd::Option<resource::TextureUseHandle> result_use;
        rstd::Option<resource::TextureUseHandle> result_msaa_use;
        rstd::Option<TextureRequest>             result_request;
        rstd::Option<TextureRequest>             result_msaa_request;

        // prepared
        ImageParameters       vk_result;
        ImageParameters       vk_result_msaa;
        VkSampleCountFlagBits samples { VK_SAMPLE_COUNT_1_BIT };
        vvk::RenderPass       msaa_clear_pass;
        vvk::Framebuffer      msaa_clear_fb;
        VkClearValue          clear_value;
    };

    PrePass(Desc&&);
    virtual ~PrePass();

    bool setResultRequest(rstd::Option<TextureRequest>,
                          rstd::Option<TextureRequest> msaa_request = rstd::None());
    void setResultUse(rstd::Option<resource::TextureUseHandle> use) {
        m_desc.result_use = rstd::move(use);
    }
    void setResultMsaaUse(rstd::Option<resource::TextureUseHandle> use) {
        m_desc.result_msaa_use = rstd::move(use);
    }
    bool prepareResourceStates(resource_registry::ResourceStateTracker&) override;
    std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void record(PassRecordContext&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    Desc m_desc;
};

} // namespace owe::vulkan
