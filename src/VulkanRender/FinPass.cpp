#include "FinPass.hpp"
#include "Vulkan/Shader.hpp"
#include "Resource.hpp"
#include "PassCommon.hpp"
#include "Utils/Logging.h"

#include <cstdlib>
#include <cstring>

using namespace wallpaper::vulkan;

constexpr std::string_view vert_code = R"(#version 320 es
layout(location = 0) in vec3 Position;
layout(location = 1) in vec2 Texcoord;
layout(location = 0) out vec2 v_Texcoord;

void main()
{
	v_Texcoord = Texcoord;
	gl_Position = vec4(Position, 1.0);
}
)";

constexpr std::string_view frag_code = R"(#version 320 es
layout(location = 0) in vec2 v_Texcoord;
layout(location = 0) out vec4 out_FragColor;

// 0 is global ublock
layout(binding = 1) uniform sampler2D u_Texture;

// The present buffer's fourcc may be either an alpha variant (ABGR/ARGB)
// or an "X" variant (XBGR/XRGB). The X variants reserve a 4th byte but
// the consumer treats it as 1.0 unconditionally. Render targets cannot
// use non-identity component swizzle in Vulkan, so we normalize alpha
// here in the shader: force fully opaque output regardless of what the
// scene render target sampled into the alpha channel. This makes the
// producer-side output identical in both fourcc families and removes
// the need to rebuild FinPass when the bridge re-negotiates within the
// same VkFormat (e.g. ABGR8888 ↔ XBGR8888 both map to R8G8B8A8_UNORM).
void main()
{
	out_FragColor = vec4(texture(u_Texture, v_Texcoord).rgb, 1.0);
}
)";

// Debug shader: outputs a solid color, ignoring the scene render target.
// Selected by env var WW_FINPASS_DEBUG=red|green|blue|white. Use this to
// bisect the render path: if a debug color reaches the display, the
// producer→consumer DMA-BUF + sync_fd path is healthy and the issue is
// upstream (scene render graph not producing into the result tex). If
// the debug color does NOT reach the display, the bug is in FinPass /
// bridge / queue family transfer / sync.
constexpr std::string_view frag_code_debug_red = R"(#version 320 es
layout(location = 0) out vec4 out_FragColor;
void main() { out_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }
)";
constexpr std::string_view frag_code_debug_green = R"(#version 320 es
layout(location = 0) out vec4 out_FragColor;
void main() { out_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }
)";
constexpr std::string_view frag_code_debug_blue = R"(#version 320 es
layout(location = 0) out vec4 out_FragColor;
void main() { out_FragColor = vec4(0.0, 0.0, 1.0, 1.0); }
)";
constexpr std::string_view frag_code_debug_white = R"(#version 320 es
layout(location = 0) out vec4 out_FragColor;
void main() { out_FragColor = vec4(1.0, 1.0, 1.0, 1.0); }
)";

namespace {
std::string_view pick_frag_source() {
    const char* env = std::getenv("WW_FINPASS_DEBUG");
    if (!env || !*env) return frag_code;
    if (std::strcmp(env, "red")   == 0) return frag_code_debug_red;
    if (std::strcmp(env, "green") == 0) return frag_code_debug_green;
    if (std::strcmp(env, "blue")  == 0) return frag_code_debug_blue;
    if (std::strcmp(env, "white") == 0) return frag_code_debug_white;
    LOG_ERROR("FinPass: unknown WW_FINPASS_DEBUG=\"%s\" (expected red|green|blue|white)", env);
    return frag_code;
}
}

struct VertexInput {
    std::array<float, 3> pos;
    std::array<float, 2> color;
};

constexpr std::array vertex_input = {
    VertexInput { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
    VertexInput { { -1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
    VertexInput { { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } },
    VertexInput { { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
};

FinPass::FinPass(const Desc&) {}
FinPass::~FinPass() {}
namespace
{
std::optional<vvk::RenderPass> CreateRenderPass(const vvk::Device& device, VkFormat format,
                                                VkImageLayout finalLayout) {
    VkAttachmentDescription attachment {
        .format         = format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = finalLayout,
    };
    VkAttachmentReference attachment_ref {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &attachment_ref,
    };

    VkRenderPassCreateInfo creatinfo {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &attachment,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
    };
    vvk::RenderPass pass;
    if (auto res = device.CreateRenderPass(creatinfo, pass); res == VK_SUCCESS) {
        return pass;
    } else {
        VVK_CHECK(res);
        return std::nullopt;
    }
}
} // namespace

void FinPass::setPresent(ImageParameters img) { m_desc.vk_present = img; }
void FinPass::setPresentLayout(VkImageLayout layout) { m_desc.present_layout = layout; }
void FinPass::setPresentFormat(VkFormat format) { m_desc.present_format = format; }
void FinPass::setPresentQueueIndex(uint32_t i) { m_desc.present_queue_index = i; }

bool FinPass::buildPresentPipeline(const Device& device) {
    if (!m_resources_ready) return false;
    if (m_desc.present_format == VK_FORMAT_UNDEFINED) return false;
    if (m_vert_spv.empty() || m_frag_spv.empty()) {
        LOG_ERROR("FinPass: shader bytecode missing (vert=%zu frag=%zu)",
                  m_vert_spv.size(), m_frag_spv.size());
        return false;
    }

    auto opt = CreateRenderPass(device.handle(), m_desc.present_format, m_desc.present_layout);
    if (! opt.has_value()) {
        LOG_ERROR("FinPass: CreateRenderPass failed (format=%d layout=%d)",
                  (int)m_desc.present_format, (int)m_desc.present_layout);
        return false;
    }
    auto pass = std::move(opt.value());

    GraphicsPipeline pipeline;
    pipeline.toDefault();
    pipeline.addDescriptorSetInfo(spanone { m_descriptor_info })
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
        .addInputBindingDescription(spanone { m_bind_description })
        .addInputAttributeDescription(m_attr_descriptions);

    // Pipeline takes Uni_ShaderSpv by &&, so we can't share cached
    // bytecode across builds via move. Wrap the cached SPIR-V into fresh
    // ShaderSpv objects each rebuild — bytes are copied, not the
    // VkShaderModule (the pipeline owns the module after create).
    auto vert_spv  = std::make_unique<ShaderSpv>();
    vert_spv->stage = ShaderType::VERTEX;
    vert_spv->spirv = m_vert_spv;
    auto frag_spv  = std::make_unique<ShaderSpv>();
    frag_spv->stage = ShaderType::FRAGMENT;
    frag_spv->spirv = m_frag_spv;
    pipeline.addStage(std::move(vert_spv));
    pipeline.addStage(std::move(frag_spv));

    PipelineParameters fresh;
    if (! pipeline.create(device, pass, fresh)) {
        LOG_ERROR("FinPass: GraphicsPipeline::create failed (format=%d)",
                  (int)m_desc.present_format);
        return false;
    }
    m_desc.pipeline = std::move(fresh);
    return true;
}

bool FinPass::rebuildPresent(const Device& device) {
    if (!m_resources_ready) return false;
    // Reset old pipeline + renderpass first so vvk RAII destroys them
    // before we allocate the new ones (avoids hitting any per-device
    // descriptor / pipeline-cache caps).
    m_desc.pipeline = PipelineParameters {};
    // Drop prepared state up-front: if the build fails we leave the pass
    // in a consistent "not prepared, no pipeline" state so any downstream
    // `if (prepared()) execute()` cannot deref the cleared pipeline.
    setPrepared(false);
    bool ok = buildPresentPipeline(device);
    setPrepared(ok);
    return ok;
}

void FinPass::prepare(Scene& scene, const Device& device, RenderingResources& rr) {
    {
        auto tex_name = std::string(m_desc.result);
        if (scene.renderTargets.count(tex_name) == 0) return;
        auto& rt = scene.renderTargets.at(tex_name);
        if (auto opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
            opt.has_value()) {
            m_desc.vk_result = opt.value();
        }
    }
    {
        ShaderCompOpt opt;
        opt.client_ver             = glslang::EShTargetVulkan_1_1;
        opt.relaxed_errors_glsl    = true;
        opt.relaxed_rules_vulkan   = true;
        opt.suppress_warnings_glsl = true;

        std::array<ShaderCompUnit, 2> units;
        // Fragment source may be the production sampler shader or one of
        // the WW_FINPASS_DEBUG=red|green|blue|white solid-color overrides.
        // The override is read once per prepare(); changing the env var
        // at runtime requires reloading the scene.
        const std::string_view frag_src = pick_frag_source();
        if (frag_src.data() != frag_code.data()) {
            LOG_INFO("FinPass: WW_FINPASS_DEBUG active — using solid-color fragment shader");
        }
        units[0] = ShaderCompUnit { .stage = EShLangVertex,   .src = std::string(vert_code) };
        units[1] = ShaderCompUnit { .stage = EShLangFragment, .src = std::string(frag_src) };
        std::vector<Uni_ShaderSpv> spvs;
        CompileAndLinkShaderUnits(units, opt, spvs);
        // Cache the SPIR-V bytes so rebuildPresent can build new
        // pipelines without re-running glslang (which requires the
        // process-global glslang::InitializeProcess scope set up by
        // VulkanRender::compileRenderGraph).
        m_vert_spv.clear();
        m_frag_spv.clear();
        for (auto& spv : spvs) {
            if (!spv) continue;
            switch (spv->stage) {
            case ShaderType::VERTEX:   m_vert_spv = spv->spirv; break;
            case ShaderType::FRAGMENT: m_frag_spv = spv->spirv; break;
            default: break;
            }
        }
        if (m_vert_spv.empty() || m_frag_spv.empty()) {
            LOG_ERROR("FinPass: shader compile produced no SPIR-V (vert=%zu frag=%zu)",
                      m_vert_spv.size(), m_frag_spv.size());
            return;
        }
    }

    {
        m_bind_description.stride    = (sizeof(VertexInput));
        m_bind_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        m_bind_description.binding   = (0);
        VkVertexInputAttributeDescription attr_pos, attr_color;
        attr_pos.binding    = (0);
        attr_pos.location   = (0);
        attr_pos.format     = VK_FORMAT_R32G32B32_SFLOAT;
        attr_pos.offset     = offsetof(VertexInput, pos);
        attr_color.binding  = (0);
        attr_color.location = (1);
        attr_color.format   = VK_FORMAT_R32G32_SFLOAT;
        attr_color.offset   = (offsetof(VertexInput, color));

        m_attr_descriptions.clear();
        m_attr_descriptions.push_back(attr_pos);
        m_attr_descriptions.push_back(attr_color);

        {
            auto& buf = m_desc.vertex_buf;
            // Guard against re-entrant prepare() (e.g. scene reload without
            // an intervening destory()). StagingBufferRef::operator bool
            // checks the underlying VmaVirtualAllocation; release the old
            // sub-ref before grabbing a fresh one.
            if (buf) {
                rr.vertex_buf->unallocateSubRef(buf);
                buf = StagingBufferRef {};
            }
            rr.vertex_buf->allocateSubRef(sizeof(decltype(vertex_input)), buf);
            rr.vertex_buf->writeToBuf(buf, { (uint8_t*)vertex_input.data(), buf.size });
        }
    }
    {
        m_descriptor_info = DescriptorSetInfo {};
        m_descriptor_info.push_descriptor = true;
        m_descriptor_info.bindings.resize(1);
        auto& binding           = m_descriptor_info.bindings.back();
        binding.binding         = (1);
        binding.descriptorCount = (1);
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    m_desc.render_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    {
        auto& sc           = scene.clearColor;
        m_desc.clear_value = VkClearValue { { sc[0], sc[1], sc[2], 1.0f } };
    }

    m_resources_ready = true;

    // Build the renderpass+pipeline if the swapchain has already
    // resolved a present format. Otherwise stay un-prepared and wait
    // for the first rebuildPresent() — VulkanRender::drawFrameOffscreen
    // calls it once the swapchain's negotiated format is known.
    if (m_desc.present_format != VK_FORMAT_UNDEFINED) {
        if (buildPresentPipeline(device)) {
            setPrepared();
        } else {
            LOG_ERROR("FinPass: initial buildPresentPipeline failed (format=%d)",
                      (int)m_desc.present_format);
        }
    }
}

void FinPass::execute(const Device& device, RenderingResources& rr) {
    auto& cmd    = rr.command;
    auto& outext = m_desc.vk_present.extent;

    VkImageSubresourceRange base_srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_MIP_LEVELS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_ARRAY_LAYERS,
    };
    {
        m_desc.fb = {};
        VkFramebufferCreateInfo info {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext           = nullptr,
            .renderPass      = *m_desc.pipeline.pass,
            .attachmentCount = 1,
            .pAttachments    = &m_desc.vk_present.view,
            .width           = m_desc.vk_present.extent.width,
            .height          = m_desc.vk_present.extent.height,
            .layers          = 1,
        };
        (void)device.handle().CreateFramebuffer(info, m_desc.fb);
    }
    {
        VkDescriptorImageInfo desc_img {
            .sampler     = m_desc.vk_result.sampler,
            .imageView   = m_desc.vk_result.view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet wset {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext           = nullptr,
            .dstSet          = {},
            .dstBinding      = 1,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &desc_img,
        };
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wset);
    }

    // No explicit acquire barrier — the renderpass attachment uses
    // initialLayout=VK_IMAGE_LAYOUT_UNDEFINED, which per spec discards
    // any previous contents *and forgets the prior queue family
    // ownership*. That gives us a free implicit "acquire from
    // FOREIGN/UNDEFINED" without needing a release-acquire pair on
    // every frame. The bridge creates slot images with
    // VK_SHARING_MODE_EXCLUSIVE + initialLayout=UNDEFINED, so the very
    // first frame is also fine. Mirrors the working image-renderer
    // plugin (waywallen-image-renderer/src/vk_producer.cpp).

    VkRenderPassBeginInfo pass_begin_info {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext       = nullptr,
        .renderPass  = *m_desc.pipeline.pass,
        .framebuffer = *m_desc.fb,
        .renderArea =
            VkRect2D {
                .offset = { 0, 0 },
                .extent = { outext.width, outext.height },
            },
        .clearValueCount = 1,
        .pClearValues    = &m_desc.clear_value,
    };
    cmd.BeginRenderPass(pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.handle);
    VkViewport viewport {
        .x        = 0,
        .y        = (float)outext.height,
        .width    = (float)outext.width,
        .height   = -(float)outext.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor { { 0, 0 }, { outext.width, outext.height } };
    cmd.SetViewport(0, viewport);
    cmd.SetScissor(0, scissor);

    cmd.BindVertexBuffers(
        0, 1, std::array { rr.vertex_buf->gpuBuf() }.data(), &m_desc.vertex_buf.offset);
    cmd.Draw(4, 1, 0, 0);
    cmd.EndRenderPass();

    // Surface-mode queue family transfer (graphics → present queue).
    // Only fires when the device exposes a separate present queue family;
    // common GPUs unify them and skip this barrier.
    //
    // Offscreen mode: VulkanRender sets present_queue_index =
    // graphics_queue.family_index, so this branch never fires. The
    // release-to-FOREIGN happens inside ExSwapchain implementations
    // (BridgeExSwapchain emits it in its own cmd buffer during
    // submitRendered; LocalExSwapchain doesn't need it because the
    // consumer is in-process).
    if (m_desc.present_queue_index != device.graphics_queue().family_index) {
        VkImageMemoryBarrier imb {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask       = 0,
            .oldLayout           = m_desc.present_layout,
            .newLayout           = m_desc.present_layout,
            .srcQueueFamilyIndex = device.graphics_queue().family_index,
            .dstQueueFamilyIndex = m_desc.present_queue_index,
            .image               = m_desc.vk_present.handle,
            .subresourceRange    = base_srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            imb);
    }
}
void FinPass::destory(const Device&, RenderingResources& rr) {
    setPrepared(false);
    clearReleaseTexs();
    rr.vertex_buf->unallocateSubRef(m_desc.vertex_buf);
}
