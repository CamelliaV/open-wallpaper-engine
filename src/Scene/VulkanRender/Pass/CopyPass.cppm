module;

export module wescene.vulkan_render:copy_pass;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

import :vulkan_pass;
import :resource;

export namespace owe::vulkan
{

class CopyPass : public VulkanPass {
public:
    struct Desc {
        std::string                              src;
        std::string                              dst;
        rstd::Option<resource::TextureUseHandle> src_use;
        rstd::Option<resource::TextureUseHandle> dst_use;
        rstd::Option<TextureRequest>             src_request;
        rstd::Option<TextureRequest>             dst_request;

        ImageParameters                         vk_src;
        ImageParameters                         vk_dst;
        resource_registry::PreparedBarrierBatch before_barriers;
        resource_registry::PreparedBarrierBatch after_barriers;
    };

    CopyPass(Desc&&);
    virtual ~CopyPass();

    PassInvalidationFlags finalizeResourceRequests(Scene&) override;
    bool                  prepareResourceStates(resource_registry::ResourceStateTracker&) override;
    std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void record(PassRecordContext&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    Desc m_desc;
};

} // namespace owe::vulkan
