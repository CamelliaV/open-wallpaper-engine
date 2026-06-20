module;

export module wescene.vulkan_render:custom_shader_pass;
import wescene.core;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

import :vulkan_pass;
import :resource;

export namespace owe::vulkan
{

class CustomShaderPass : public VulkanPass {
public:
    struct Desc {
        // in
        SceneNode* node { nullptr };
        // Which submesh of node->Mesh() this pass renders. SceneToRenderGraph
        // emits one pass per (node, submesh).
        uint32_t                 submesh_index { 0 };
        std::vector<std::string> textures;
        std::string              output;
        sprite_map_t             sprites_map;

        // -----prepared
        // vulkan texs
        std::vector<ImageSlotsRef> vk_textures;
        std::vector<i32>           vk_tex_binding;
        ImageParameters            vk_output;
        // MSAA twin (color attachment) when output RT has sample_count>1.
        // Empty handle means no MSAA; framebuffer attaches only vk_output.
        ImageParameters       vk_output_msaa;
        ImageParameters       vk_depth;
        VkSampleCountFlagBits samples { VK_SAMPLE_COUNT_1_BIT };
        bool                  has_depth_attachment { false };

        // bufs
        bool dyn_vertex { false };
        // Static mesh: cached in Device::mesh_cache() across render-graph rebuilds.
        std::vector<MeshBufferRef> vertex_bufs;
        MeshBufferRef              index_buf;
        // Dynamic mesh: re-allocated from dyn_buf each rebuild.
        std::vector<StagingBufferRef> vertex_dyn_bufs;
        StagingBufferRef              index_dyn_buf;
        StagingBufferRef              ubo_buf;

        // pipeline
        VkClearValue clear_value;
        // When non-null this pass tracks `scene.clearColor` per-frame
        // (re-syncing `clear_value.color` from the array each execute);
        // null means the pass owns a hard-coded clear (e.g. effect-layer
        // ppong RTs that always reset transparent).
        const std::array<float, 3>* clear_value_src { nullptr };
        bool                        blending { false };
        bool                        clear_output { false };
        bool                        clear_depth { false };
        bool                        preserve_output { false };
        VkAttachmentLoadOp          color_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        VkAttachmentLoadOp          depth_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        vvk::Framebuffer            fb;
        PipelineParameters          pipeline;
        u32                         draw_count { 0 };

        // uniforms
        std::function<void()> update_op;
    };

    CustomShaderPass(const Desc&);
    virtual ~CustomShaderPass();

    void setDescTex(u32 index, std::string_view tex_key);

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;
    bool canJoinRenderScopeAfter(const CustomShaderPass& previous) const;
    void prepareRenderScopeDraw(RenderingResources&);
    void recordSampledImageBarriers(RenderingResources&);
    void beginRenderScope(RenderingResources&);
    void recordRenderScopeDraw(RenderingResources&);
    void endRenderScope(RenderingResources&);

private:
    Desc m_desc;
};

} // namespace owe::vulkan
