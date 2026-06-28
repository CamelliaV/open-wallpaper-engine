#include <gtest/gtest.h>

import wescene.scene;
import wescene.types;
import wescene.vulkan;
import wescene.vulkan_render;

TEST(TextureRequest, BuildsImportedRequestWithoutCacheKey) {
    auto request = owe::vulkan::MakeImportedTextureRequest("textures/main.png");

    EXPECT_EQ(request.kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(request.name, "textures/main.png");
    EXPECT_FALSE(request.imported_texture.has_value());
    EXPECT_FALSE(request.cache_key.has_value());
    EXPECT_FALSE(request.persist);
}

TEST(TextureBindingRequest, CarriesNameAndTypedRequest) {
    auto request = owe::vulkan::MakeImportedTextureRequest("texture-slot");
    owe::vulkan::TextureBindingRequest binding {
        .name    = "texture-slot",
        .request = request,
    };

    EXPECT_FALSE(binding.empty());
    ASSERT_TRUE(binding.request.has_value());
    EXPECT_EQ(binding.request->kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(binding.request->name, "texture-slot");

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
    EXPECT_EQ(request.imported_texture->index, desc_id->index);

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
    EXPECT_EQ(request.name, "_rt_default");
    ASSERT_TRUE(request.cache_key.has_value());
    EXPECT_EQ(request.cache_key->width, 256);
    EXPECT_EQ(request.cache_key->height, 128);
    EXPECT_EQ(request.cache_key->usage, owe::vulkan::TexUsage::COLOR);
    EXPECT_EQ(request.cache_key->format, owe::TextureFormat::RGBA8);
    EXPECT_EQ(request.cache_key->mipmap_level, 3u);
    EXPECT_TRUE(request.persist);

    rt.allowReuse = true;
    EXPECT_FALSE(owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt).persist);

    auto no_mip = owe::vulkan::MakeRenderTargetNoMipTextureRequest("_rt_default", rt);
    ASSERT_TRUE(no_mip.cache_key.has_value());
    EXPECT_EQ(no_mip.cache_key->mipmap_level, 1u);
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

    std::optional<owe::vulkan::TextureRequest> target = a;
    EXPECT_FALSE(owe::vulkan::SetTextureRequestIfChanged(target, b));
    EXPECT_TRUE(owe::vulkan::SetTextureRequestIfChanged(target, resized));
    ASSERT_TRUE(target.has_value());
    ASSERT_TRUE(target->cache_key.has_value());
    EXPECT_EQ(target->cache_key->width, 512);
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
    EXPECT_EQ(msaa.name, "_rt_default::msaa4");
    ASSERT_TRUE(msaa.cache_key.has_value());
    EXPECT_EQ(msaa.cache_key->samples, VK_SAMPLE_COUNT_4_BIT);
    EXPECT_TRUE(msaa.persist);

    auto depth = owe::vulkan::MakeDepthTextureRequest("_rt_default::depth", rt);
    EXPECT_EQ(depth.kind, owe::vulkan::TextureRequestKind::DepthAttachment);
    ASSERT_TRUE(depth.cache_key.has_value());
    EXPECT_EQ(depth.cache_key->usage, owe::vulkan::TexUsage::DEPTH);
    EXPECT_EQ(depth.cache_key->format, owe::TextureFormat::D32F);
    EXPECT_EQ(depth.cache_key->mipmap_level, 1u);
    EXPECT_EQ(depth.cache_key->samples, VK_SAMPLE_COUNT_4_BIT);
    EXPECT_TRUE(depth.persist);
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
        .texture_bindings = { owe::vulkan::TextureBindingRequest {
            .name    = "textures/main.png",
            .request = imported,
        } },
        .output           = "_rt_default",
        .output_request   = output,
        .depth_request    = depth,
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
        .src_request = output,
        .dst_request = output,
    });
    auto                  copy_diag = copy.textureRequestDiagnostics();
    ASSERT_EQ(copy_diag.size(), 2u);
    EXPECT_EQ(copy_diag[0].role, "copy-src");
    EXPECT_EQ(copy_diag[1].role, "copy-dst");

    owe::vulkan::PrePass pre(owe::vulkan::PrePass::Desc {
        .result              = "_rt_default",
        .result_request      = output,
        .result_msaa_request = msaa,
    });
    auto                 pre_diag = pre.textureRequestDiagnostics();
    ASSERT_EQ(pre_diag.size(), 2u);
    EXPECT_EQ(pre_diag[0].role, "frame-result");
    EXPECT_EQ(pre_diag[1].role, "frame-result-msaa");

    owe::vulkan::FinPass fin(owe::vulkan::FinPass::Desc {
        .result         = "_rt_default",
        .result_request = output,
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

    EXPECT_TRUE(owe::vulkan::SamePipelineCacheKey(key_a, key_b));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_c));

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

    EXPECT_TRUE(owe::vulkan::SameRenderPassCacheKey(key_a, key_b));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_format));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_samples));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_depth));
}

TEST(FramebufferCacheDiagnostics, RecordsStableFramebufferKeys) {
    auto image_view = [](std::uintptr_t value) {
        return reinterpret_cast<VkImageView>(value);
    };

    owe::vulkan::FramebufferResourceRequest request {
        .render_pass_key = owe::vulkan::RenderPassCacheKey { .value = 17u },
        .attachments     = { image_view(0x101u), image_view(0x102u) },
        .extent          = { 320u, 180u },
    };

    auto key_a = owe::vulkan::MakeFramebufferCacheKey(request);
    auto key_b = owe::vulkan::MakeFramebufferCacheKey(request);

    auto resized         = request;
    resized.extent.width = 640u;
    auto key_resized     = owe::vulkan::MakeFramebufferCacheKey(resized);

    auto different_attachment           = request;
    different_attachment.attachments[1] = image_view(0x103u);
    auto key_attachment = owe::vulkan::MakeFramebufferCacheKey(different_attachment);

    auto different_render_pass            = request;
    different_render_pass.render_pass_key = owe::vulkan::RenderPassCacheKey { .value = 19u };
    auto key_render_pass = owe::vulkan::MakeFramebufferCacheKey(different_render_pass);

    EXPECT_TRUE(owe::vulkan::SameFramebufferCacheKey(key_a, key_b));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_resized));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_attachment));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_render_pass));

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
