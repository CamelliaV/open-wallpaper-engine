#include <vulkan/vulkan_core.h>

#include <gtest/gtest.h>

import rstd;
import rstd.cppstd;
import wescene.scene;
import wescene.types;
import wescene.vulkan;
import wescene.vulkan_render;

namespace
{

std::vector<std::byte> Bytes(std::initializer_list<unsigned char> values) {
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (auto value : values) bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}

VkImageView ImageView(std::uintptr_t value) { return reinterpret_cast<VkImageView>(value); }

owe::vulkan::FramebufferAttachmentIdentity
AttachmentIdentity(std::size_t value, std::initializer_list<unsigned char> bytes) {
    return owe::vulkan::FramebufferAttachmentIdentity {
        .value = value,
        .bytes = Bytes(bytes),
    };
}

owe::vulkan::FramebufferAttachmentDesc Attachment(std::uintptr_t view, std::size_t identity,
                                                  std::initializer_list<unsigned char> bytes) {
    return owe::vulkan::FramebufferAttachmentDesc {
        .view     = ImageView(view),
        .identity = AttachmentIdentity(identity, bytes),
    };
}

} // namespace

TEST(TextureRequest, BuildsImportedRequestWithoutCacheKey) {
    auto request = owe::vulkan::MakeImportedTextureRequest("textures/main.png");

    EXPECT_EQ(request.kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(rstd::cppstd::as_string_view(request.name.as_str()), "textures/main.png");
    EXPECT_TRUE(request.source.is_none());
    EXPECT_TRUE(request.definition.is_none());
    EXPECT_EQ(request.lifetime, owe::resource::TextureLifetimeClass::Retained);
}

TEST(TextureBindingRequest, CarriesNameAndTypedRequest) {
    auto request = owe::vulkan::MakeImportedTextureRequest("texture-slot");
    owe::vulkan::TextureBindingRequest binding {
        .name    = rstd::string::String::make(rstd::cppstd::as_str("texture-slot")),
        .request = rstd::Some(std::move(request)),
    };

    EXPECT_FALSE(binding.empty());
    ASSERT_TRUE(binding.request.is_some());
    EXPECT_EQ(binding.request->kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(rstd::cppstd::as_string_view(binding.request->name.as_str()), "texture-slot");

    owe::vulkan::TextureBindingRequest empty;
    EXPECT_TRUE(empty.empty());
}

TEST(TextureRequest, ResolvesImportedTextureNameFromSnapshotCatalog) {
    owe::Scene scene;
    scene.textures["texture-slot"] = owe::SceneTexture { .url = "textures/main.png" };

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto desc_id  = snapshot.textureDescId("texture-slot");
    ASSERT_TRUE(desc_id.has_value());

    auto request = owe::vulkan::MakeImportedTextureRequest("texture-slot", desc_id);
    EXPECT_EQ(request.source->index, desc_id->index);

    auto resolved = owe::vulkan::ResolveImportedTextureName(snapshot, request);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, "textures/main.png");

    auto lookup_request = owe::vulkan::MakeImportedTextureRequest("texture-slot");
    resolved            = owe::vulkan::ResolveImportedTextureName(snapshot, lookup_request);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, "textures/main.png");

    auto missing_request = owe::vulkan::MakeImportedTextureRequest("missing");
    EXPECT_FALSE(owe::vulkan::ResolveImportedTextureName(snapshot, missing_request).has_value());
}

TEST(TextureRequest, BuildsRenderTargetCacheKey) {
    owe::SceneRenderTarget rt {
        .width        = 256,
        .height       = 128,
        .allowReuse   = false,
        .mipmap_level = 3,
    };

    auto request = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);

    EXPECT_EQ(request.kind, owe::vulkan::TextureRequestKind::RenderTarget);
    EXPECT_EQ(rstd::cppstd::as_string_view(request.name.as_str()), "_rt_default");
    ASSERT_TRUE(request.definition.is_some());
    EXPECT_EQ(request.definition->width, 256);
    EXPECT_EQ(request.definition->height, 128);
    EXPECT_EQ(request.definition->usage, owe::resource::TextureUsage::Color);
    EXPECT_EQ(request.definition->format, owe::TextureFormat::RGBA8);
    EXPECT_EQ(request.definition->mip_levels, 3u);
    EXPECT_EQ(request.lifetime, owe::resource::TextureLifetimeClass::Retained);

    rt.allowReuse = true;
    EXPECT_EQ(owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt).lifetime,
              owe::resource::TextureLifetimeClass::FrameLocal);

    auto no_mip = owe::vulkan::MakeRenderTargetNoMipTextureRequest("_rt_default", rt);
    ASSERT_TRUE(no_mip.definition.is_some());
    EXPECT_EQ(no_mip.definition->mip_levels, 1u);
}

TEST(TextureRequest, DetectsRequestChanges) {
    owe::SceneRenderTarget rt {
        .width        = 256,
        .height       = 128,
        .allowReuse   = false,
        .mipmap_level = 3,
    };

    auto a = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);
    auto b = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);

    EXPECT_TRUE(owe::vulkan::SameTextureRequest(a, b));

    rt.width     = 512;
    auto resized = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);
    EXPECT_FALSE(owe::vulkan::SameTextureRequest(a, resized));

    rstd::Option<owe::vulkan::TextureRequest> target = rstd::Some(a.clone());
    EXPECT_FALSE(owe::vulkan::SetTextureRequestIfChanged(target, b.clone()));
    EXPECT_TRUE(owe::vulkan::SetTextureRequestIfChanged(target, std::move(resized)));
    ASSERT_TRUE(target.is_some());
    ASSERT_TRUE(target->definition.is_some());
    EXPECT_EQ(target->definition->width, 512);
}

TEST(TextureRequest, BuildsMsaaAndDepthCacheKeys) {
    owe::SceneRenderTarget rt {
        .width        = 512,
        .height       = 256,
        .allowReuse   = false,
        .mipmap_level = 2,
        .sample_count = 4,
    };

    auto msaa =
        owe::vulkan::MakeMsaaTextureRequest("_rt_default::msaa4", rt, VK_SAMPLE_COUNT_4_BIT);
    EXPECT_EQ(msaa.kind, owe::vulkan::TextureRequestKind::RenderTargetMsaa);
    EXPECT_EQ(rstd::cppstd::as_string_view(msaa.name.as_str()), "_rt_default::msaa4");
    ASSERT_TRUE(msaa.definition.is_some());
    EXPECT_EQ(msaa.definition->samples, 4u);
    EXPECT_EQ(msaa.lifetime, owe::resource::TextureLifetimeClass::Dedicated);

    auto depth = owe::vulkan::MakeDepthTextureRequest("_rt_default::depth", rt);
    EXPECT_EQ(depth.kind, owe::vulkan::TextureRequestKind::DepthAttachment);
    ASSERT_TRUE(depth.definition.is_some());
    EXPECT_EQ(depth.definition->usage, owe::resource::TextureUsage::Depth);
    EXPECT_EQ(depth.definition->format, owe::TextureFormat::D32F);
    EXPECT_EQ(depth.definition->mip_levels, 1u);
    EXPECT_EQ(depth.definition->samples, 4u);
    EXPECT_EQ(depth.lifetime, owe::resource::TextureLifetimeClass::Retained);
}

TEST(PassTextureRequestDiagnostics, ReportsPassOwnedTextureRequests) {
    owe::SceneRenderTarget rt {
        .width        = 512,
        .height       = 256,
        .allowReuse   = false,
        .mipmap_level = 2,
        .sample_count = 4,
    };

    auto imported = owe::vulkan::MakeImportedTextureRequest("textures/main.png");
    auto output   = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);
    auto msaa =
        owe::vulkan::MakeMsaaTextureRequest("_rt_default::msaa4", rt, VK_SAMPLE_COUNT_4_BIT);
    auto depth = owe::vulkan::MakeDepthTextureRequest("_rt_default::depth", rt);

    owe::vulkan::CustomShaderPass custom(owe::vulkan::CustomShaderPass::Desc {
        .texture_bindings =
            [](owe::vulkan::TextureRequest request) {
                std::vector<owe::vulkan::TextureBindingRequest> bindings;
                bindings.push_back(owe::vulkan::TextureBindingRequest {
                    .name = rstd::string::String::make(rstd::cppstd::as_str("textures/main.png")),
                    .request = rstd::Some(std::move(request)),
                });
                return bindings;
            }(std::move(imported)),
        .output         = "_rt_default",
        .output_request = rstd::Some(output.clone()),
        .depth_request  = rstd::Some(std::move(depth)),
    });
    auto                          custom_diag = custom.textureRequestDiagnostics();
    ASSERT_EQ(custom_diag.size(), 3u);
    EXPECT_EQ(custom_diag[0].role, "sampled");
    EXPECT_EQ(custom_diag[0].slot, 0u);
    EXPECT_EQ(custom_diag[0].request->kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(custom_diag[1].role, "output");
    EXPECT_EQ(custom_diag[1].request->kind, owe::vulkan::TextureRequestKind::RenderTarget);
    EXPECT_EQ(custom_diag[2].role, "depth");
    EXPECT_EQ(custom_diag[2].request->kind, owe::vulkan::TextureRequestKind::DepthAttachment);

    owe::vulkan::CopyPass copy(owe::vulkan::CopyPass::Desc {
        .src         = "_rt_a",
        .dst         = "_rt_b",
        .src_request = rstd::Some(output.clone()),
        .dst_request = rstd::Some(output.clone()),
    });
    auto                  copy_diag = copy.textureRequestDiagnostics();
    ASSERT_EQ(copy_diag.size(), 2u);
    EXPECT_EQ(copy_diag[0].role, "copy-src");
    EXPECT_EQ(copy_diag[1].role, "copy-dst");

    owe::vulkan::PrePass pre(owe::vulkan::PrePass::Desc {
        .result              = "_rt_default",
        .result_request      = rstd::Some(output.clone()),
        .result_msaa_request = rstd::Some(std::move(msaa)),
    });
    auto                 pre_diag = pre.textureRequestDiagnostics();
    ASSERT_EQ(pre_diag.size(), 2u);
    EXPECT_EQ(pre_diag[0].role, "frame-result");
    EXPECT_EQ(pre_diag[1].role, "frame-result-msaa");

    owe::vulkan::FinPass fin(owe::vulkan::FinPass::Desc {
        .result         = "_rt_default",
        .result_request = rstd::Some(std::move(output)),
    });
    auto                 fin_diag = fin.textureRequestDiagnostics();
    ASSERT_EQ(fin_diag.size(), 1u);
    EXPECT_EQ(fin_diag[0].role, "frame-result");
}

TEST(PipelineCacheDiagnostics, RecordsStableKeys) {
    auto make_request = [](VkPrimitiveTopology topology) {
        owe::vulkan::PipelineResourceRequest request;
        request.topology = topology;
        request.descriptor_sets.push_back(owe::vulkan::DescriptorSetInfo {
            .push_descriptor = true,
            .bindings =
                {
                    VkDescriptorSetLayoutBinding {
                        .binding         = 0,
                        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                        .descriptorCount = 1,
                        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT,
                    },
                },
        });
        request.vertex_bindings.push_back(VkVertexInputBindingDescription {
            .binding   = 0,
            .stride    = 16,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        });
        request.vertex_attrs.push_back(VkVertexInputAttributeDescription {
            .location = 0,
            .binding  = 0,
            .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset   = 0,
        });
        auto spv         = std::make_unique<owe::vulkan::ShaderSpv>();
        spv->stage       = owe::ShaderType::VERTEX;
        spv->entry_point = "main";
        spv->spirv       = { 1u, 2u, 3u, 4u };
        request.shader_stages.push_back(std::move(spv));
        return request;
    };

    auto key_a =
        owe::vulkan::MakePipelineCacheKey(make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST));
    auto key_b =
        owe::vulkan::MakePipelineCacheKey(make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST));
    auto key_c = owe::vulkan::MakePipelineCacheKey(make_request(VK_PRIMITIVE_TOPOLOGY_POINT_LIST));
    auto desc_request = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    auto key_from_desc =
        owe::vulkan::MakePipelineCacheKey(owe::vulkan::MakePipelineResourceDesc(desc_request));
    auto primitive_restart_request = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    primitive_restart_request.primitive_restart_enable = true;
    auto key_primitive_restart       = owe::vulkan::MakePipelineCacheKey(primitive_restart_request);
    auto viewport_request            = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    viewport_request.viewport_count  = 2u;
    auto key_viewport                = owe::vulkan::MakePipelineCacheKey(viewport_request);
    auto logic_op_request            = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    logic_op_request.logic_op_enable = true;
    logic_op_request.logic_op        = VK_LOGIC_OP_XOR;
    auto key_logic_op                = owe::vulkan::MakePipelineCacheKey(logic_op_request);
    auto create_flags_request        = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    create_flags_request.create_flags =
        static_cast<VkPipelineCreateFlags>(VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT);
    auto key_create_flags        = owe::vulkan::MakePipelineCacheKey(create_flags_request);
    auto subpass_request         = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    subpass_request.subpass      = 1u;
    auto key_subpass             = owe::vulkan::MakePipelineCacheKey(subpass_request);
    auto blend_constants_request = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    blend_constants_request.blend_constants[0] = 1.0f;
    auto key_blend_constants   = owe::vulkan::MakePipelineCacheKey(blend_constants_request);
    auto dynamic_state_request = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    dynamic_state_request.dynamic_states = { VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VIEWPORT };
    auto key_dynamic_state_order         = owe::vulkan::MakePipelineCacheKey(dynamic_state_request);
    dynamic_state_request.dynamic_states = { VK_DYNAMIC_STATE_VIEWPORT };
    auto key_dynamic_state               = owe::vulkan::MakePipelineCacheKey(dynamic_state_request);

    EXPECT_FALSE(key_a.bytes.empty());
    EXPECT_TRUE(owe::vulkan::SamePipelineCacheKey(key_a, key_b));
    EXPECT_TRUE(owe::vulkan::SamePipelineCacheKey(key_a, key_from_desc));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_c));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_primitive_restart));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_viewport));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_logic_op));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_create_flags));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_subpass));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_blend_constants));
    EXPECT_TRUE(owe::vulkan::SamePipelineCacheKey(key_a, key_dynamic_state_order));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_dynamic_state));

    auto colliding_a = owe::vulkan::PipelineCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x01u }),
    };
    auto colliding_b = owe::vulkan::PipelineCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x02u }),
    };
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(colliding_a, colliding_b));

    owe::vulkan::PipelineCacheDiagnostics diagnostics;
    auto                                  first = diagnostics.Record(key_a);
    EXPECT_FALSE(first.hit);
    EXPECT_EQ(first.observed_count, 1u);

    auto second = diagnostics.Record(key_b);
    EXPECT_TRUE(second.hit);
    EXPECT_EQ(second.observed_count, 2u);

    auto third = diagnostics.Record(key_c);
    EXPECT_FALSE(third.hit);
    EXPECT_EQ(third.observed_count, 1u);

    auto collision_first = diagnostics.Record(colliding_a);
    EXPECT_FALSE(collision_first.hit);
    EXPECT_EQ(collision_first.observed_count, 1u);

    auto collision_second = diagnostics.Record(colliding_b);
    EXPECT_FALSE(collision_second.hit);
    EXPECT_EQ(collision_second.observed_count, 1u);

    std::unordered_map<owe::vulkan::PipelineCacheKey,
                       int,
                       owe::vulkan::CanonicalCacheKeyHash,
                       owe::vulkan::PipelineCacheKeyEqual>
        cache_entries;
    cache_entries.emplace(colliding_a, 1);
    cache_entries.emplace(colliding_b, 2);
    EXPECT_EQ(cache_entries.size(), 2u);
}

TEST(RenderPassCacheKey, TracksRenderPassCompatibilityInputs) {
    auto make_request = [](VkFormat color_format, VkSampleCountFlagBits samples, bool depth) {
        owe::vulkan::PipelineResourceRequest request;
        request.color_format                     = color_format;
        request.color_final_layout               = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        request.color_load_op                    = VK_ATTACHMENT_LOAD_OP_CLEAR;
        request.depth_load_op                    = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        request.has_depth_attachment             = depth;
        request.multisample.rasterizationSamples = samples;
        return request;
    };

    auto key_a = owe::vulkan::MakeRenderPassCacheKey(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, true));
    auto key_b = owe::vulkan::MakeRenderPassCacheKey(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, true));
    auto key_format = owe::vulkan::MakeRenderPassCacheKey(
        make_request(VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, true));
    auto key_samples = owe::vulkan::MakeRenderPassCacheKey(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, true));
    auto key_depth = owe::vulkan::MakeRenderPassCacheKey(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, false));
    auto desc_store = owe::vulkan::MakeRenderPassResourceDesc(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, true));
    desc_store.color_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    auto key_store            = owe::vulkan::MakeRenderPassCacheKey(desc_store);
    auto desc_layout          = owe::vulkan::MakeRenderPassResourceDesc(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, true));
    desc_layout.color_attachment_layout = VK_IMAGE_LAYOUT_GENERAL;
    auto key_layout                     = owe::vulkan::MakeRenderPassCacheKey(desc_layout);

    EXPECT_FALSE(key_a.bytes.empty());
    EXPECT_TRUE(owe::vulkan::SameRenderPassCacheKey(key_a, key_b));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_format));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_samples));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_depth));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_store));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_layout));

    auto colliding_a = owe::vulkan::RenderPassCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x01u }),
    };
    auto colliding_b = owe::vulkan::RenderPassCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x02u }),
    };
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(colliding_a, colliding_b));

    std::unordered_map<owe::vulkan::RenderPassCacheKey,
                       int,
                       owe::vulkan::CanonicalCacheKeyHash,
                       owe::vulkan::RenderPassCacheKeyEqual>
        cache_entries;
    cache_entries.emplace(colliding_a, 1);
    cache_entries.emplace(colliding_b, 2);
    EXPECT_EQ(cache_entries.size(), 2u);
}

TEST(FramebufferCacheDiagnostics, RecordsStableFramebufferKeys) {
    owe::vulkan::FramebufferResourceRequest request {
        .render_pass_key =
            owe::vulkan::RenderPassCacheKey {
                .value = 17u,
                .bytes = Bytes({ 0x17u }),
            },
        .attachments = { Attachment(0x101u, 101u, { 0x01u }), Attachment(0x102u, 102u, { 0x02u }) },
        .extent      = { 320u, 180u },
    };

    auto key_a = owe::vulkan::MakeFramebufferCacheKey(request);
    auto key_b = owe::vulkan::MakeFramebufferCacheKey(request);

    auto resized         = request;
    resized.extent.width = 640u;
    auto key_resized     = owe::vulkan::MakeFramebufferCacheKey(resized);

    auto layered    = request;
    layered.layers  = 2u;
    auto key_layers = owe::vulkan::MakeFramebufferCacheKey(layered);

    auto different_attachment           = request;
    different_attachment.attachments[1] = Attachment(0x103u, 103u, { 0x03u });
    auto key_attachment = owe::vulkan::MakeFramebufferCacheKey(different_attachment);

    auto different_attachment_identity                    = request;
    different_attachment_identity.attachments[1].identity = AttachmentIdentity(104u, { 0x04u });
    auto key_attachment_identity =
        owe::vulkan::MakeFramebufferCacheKey(different_attachment_identity);

    auto different_attachment_view                = request;
    different_attachment_view.attachments[1].view = ImageView(0x104u);
    auto key_attachment_view = owe::vulkan::MakeFramebufferCacheKey(different_attachment_view);

    auto different_render_pass            = request;
    different_render_pass.render_pass_key = owe::vulkan::RenderPassCacheKey {
        .value = 17u,
        .bytes = Bytes({ 0x18u }),
    };
    auto key_render_pass = owe::vulkan::MakeFramebufferCacheKey(different_render_pass);

    EXPECT_FALSE(key_a.bytes.empty());
    EXPECT_TRUE(owe::vulkan::SameFramebufferCacheKey(key_a, key_b));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_resized));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_layers));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_attachment));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_attachment_identity));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_attachment_view));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_render_pass));

    auto colliding_a = owe::vulkan::FramebufferCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x01u }),
    };
    auto colliding_b = owe::vulkan::FramebufferCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x02u }),
    };
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(colliding_a, colliding_b));

    owe::vulkan::FramebufferCacheDiagnostics diagnostics;
    auto                                     first = diagnostics.Record(key_a);
    EXPECT_FALSE(first.hit);
    EXPECT_EQ(first.observed_count, 1u);

    auto second = diagnostics.Record(key_b);
    EXPECT_TRUE(second.hit);
    EXPECT_EQ(second.observed_count, 2u);

    auto third = diagnostics.Record(key_resized);
    EXPECT_FALSE(third.hit);
    EXPECT_EQ(third.observed_count, 1u);

    auto collision_first = diagnostics.Record(colliding_a);
    EXPECT_FALSE(collision_first.hit);
    EXPECT_EQ(collision_first.observed_count, 1u);

    auto collision_second = diagnostics.Record(colliding_b);
    EXPECT_FALSE(collision_second.hit);
    EXPECT_EQ(collision_second.observed_count, 1u);

    std::unordered_map<owe::vulkan::FramebufferCacheKey,
                       int,
                       owe::vulkan::CanonicalCacheKeyHash,
                       owe::vulkan::FramebufferCacheKeyEqual>
        cache_entries;
    cache_entries.emplace(colliding_a, 1);
    cache_entries.emplace(colliding_b, 2);
    EXPECT_EQ(cache_entries.size(), 2u);
}

TEST(FramebufferAttachmentIdentity, TracksTextureGeneration) {
    owe::SceneRenderTarget rt {
        .width      = 320,
        .height     = 180,
        .allowReuse = true,
    };
    auto request = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);

    owe::vulkan::ImageParameters image_a;
    image_a.handle       = reinterpret_cast<VkImage>(0x201u);
    image_a.view         = ImageView(0x301u);
    image_a.extent       = { 320u, 180u, 1u };
    image_a.mipmap_level = 1u;
    image_a.generation   = 10u;
    auto image_b         = image_a;
    image_b.generation   = 11u;

    auto attachment_a = owe::vulkan::MakeFramebufferAttachment(request, image_a);
    auto attachment_b = owe::vulkan::MakeFramebufferAttachment(request, image_b);
    EXPECT_NE(attachment_a.identity.value, 0u);
    EXPECT_FALSE(attachment_a.identity.bytes.empty());
    EXPECT_NE(attachment_a.identity.bytes, attachment_b.identity.bytes);

    owe::vulkan::FramebufferResourceRequest framebuffer_a {
        .render_pass_key =
            owe::vulkan::RenderPassCacheKey {
                .value = 17u,
                .bytes = Bytes({ 0x17u }),
            },
        .attachments = { attachment_a },
        .extent      = { 320u, 180u },
    };
    auto framebuffer_b        = framebuffer_a;
    framebuffer_b.attachments = { attachment_b };

    EXPECT_FALSE(
        owe::vulkan::SameFramebufferCacheKey(owe::vulkan::MakeFramebufferCacheKey(framebuffer_a),
                                             owe::vulkan::MakeFramebufferCacheKey(framebuffer_b)));
}

TEST(PipelineRetireQueue, IgnoresEmptyPipelineParameters) {
    owe::vulkan::PipelineRetireQueue retire_queue;
    owe::vulkan::PipelineParameters  empty;
    vvk::Framebuffer                 empty_framebuffer;

    retire_queue.Retire(std::move(empty));
    retire_queue.Retire(std::move(empty_framebuffer));
    EXPECT_EQ(retire_queue.pending(), 0u);

    retire_queue.ReleaseAllReady();
    EXPECT_EQ(retire_queue.pending(), 0u);
}
