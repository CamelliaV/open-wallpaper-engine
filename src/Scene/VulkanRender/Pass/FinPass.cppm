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
        std::string_view              result { SpecTex_Default }; // scene RT key
        std::optional<TextureRequest> result_request;

        // resolved in prepare()
        ImageParameters vk_result;

        std::optional<owe::FrameSurfaceLease> frame_surface;
    };

    FinPass(const Desc&);
    virtual ~FinPass();

    bool                                      setFrameSurface(owe::FrameSurfaceLease);
    bool                                      setResultRequest(std::optional<TextureRequest>);
    std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    Desc m_desc;
    bool m_path_logged { false };
};

} // namespace owe::vulkan
