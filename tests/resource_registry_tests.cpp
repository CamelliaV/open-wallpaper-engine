#include <gtest/gtest.h>
#include <algorithm>

import rstd;
import rstd.cppstd;
import wescene.resource_registry;
import wescene.vulkan;

namespace
{

struct ShaderArtifactProvider {
    rstd::usize loads { 0 };

    auto LoadShader(const owe::resource::ShaderRequest& request)
        -> rstd::Result<owe::resource::ShaderArtifact, owe::resource::ResourceError> {
        ++loads;
        return rstd::Ok(owe::resource::ShaderArtifact {
            .source          = request.source,
            .content_version = request.content_version,
        });
    }
};

struct BufferContentProvider {
    rstd::usize loads { 0 };

    auto LoadBuffer(const owe::resource::BufferRequest&)
        -> rstd::Result<rstd::vec::Vec<rstd::u8>, owe::resource::ResourceError> {
        ++loads;
        auto bytes = rstd::vec::Vec<rstd::u8>::make();
        bytes.push(rstd::u8(1));
        bytes.push(rstd::u8(2));
        return rstd::Ok(rstd::move(bytes));
    }
};

struct BufferUploadBackend {
    rstd::usize                      uploads { 0 };
    rstd::usize                      updates { 0 };
    owe::vulkan::BufferUploadRequest last_request;

    auto UploadBuffer(rstd::slice<rstd::u8>                   content,
                      const owe::vulkan::BufferUploadRequest& request)
        -> rstd::Option<owe::vulkan::BufferAllocation> {
        ++uploads;
        last_request = request;
        if (request.size == 0 || request.size < content.len().to_primitive()) return rstd::None();
        return rstd::Some(owe::vulkan::BufferAllocation {});
    }

    bool UpdateBuffer(rstd::mut_ref<owe::vulkan::BufferAllocation>, rstd::slice<rstd::u8>) {
        ++updates;
        return true;
    }
};

auto ShaderRequest(rstd::uint64_t version) -> owe::resource::ShaderRequest {
    return owe::resource::ShaderRequest {
        .name = rstd::string::String::make(rstd::cppstd::as_str("sprite")),
        .source =
            owe::resource::ShaderDefinitionId {
                .index      = rstd::u32(3),
                .generation = rstd::u64(1),
            },
        .content_version = rstd::u64(version),
    };
}

auto TextureAllocation(rstd::uint64_t generation)
    -> rstd::sync::Arc<owe::vulkan::TextureAllocation> {
    owe::vulkan::ImageSlots slots;
    slots.slots.resize(1);
    slots.slots[0].generation = rstd::u64(generation);
    return rstd::sync::Arc<owe::vulkan::TextureAllocation>::make(rstd::move(slots));
}

} // namespace

TEST(TextureRegistry, OwnsLogicalEntriesBehindGenerationalHandles) {
    owe::resource::TextureRegistry registry;
    auto                           handle = registry.Register(owe::resource::TextureRequest {
        .kind       = owe::resource::TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make(rstd::cppstd::as_str("frame")),
        .definition = rstd::Some(owe::resource::TextureDefinition {
            .width  = rstd::i32(256),
            .height = rstd::i32(128),
        }),
    });
    ASSERT_TRUE(handle.Valid());
    EXPECT_EQ(registry.Size(), rstd::usize(1));

    auto found = registry.Find(owe::resource::TextureRequestKind::RenderTarget,
                               rstd::cppstd::as_str("frame"));
    ASSERT_TRUE(found.is_some());
    EXPECT_EQ(*found, handle);

    auto entry = registry.ResolveTexture(handle);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view((**entry).request.name.as_str()), "frame");
    EXPECT_EQ((**entry).definition_version, rstd::u64(1));

    auto resized = registry.Register(owe::resource::TextureRequest {
        .kind       = owe::resource::TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make(rstd::cppstd::as_str("frame")),
        .definition = rstd::Some(owe::resource::TextureDefinition {
            .width  = rstd::i32(512),
            .height = rstd::i32(128),
        }),
    });
    EXPECT_EQ(resized, handle);
    entry = registry.ResolveTexture(handle);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ((**entry).definition_version, rstd::u64(2));

    auto view  = rstd::dyn<owe::resource::TextureLogicalRegistryView>::from_ref(registry);
    auto state = view->ResolveTextureState(handle);
    ASSERT_TRUE(state.is_some());
    EXPECT_EQ(state->definition_version, rstd::u64(2));
    EXPECT_EQ(state->content_version, rstd::u64(1));
}

TEST(TextureRegistry, InvalidatesOldHandlesOnReset) {
    owe::resource::TextureRegistry registry;
    auto                           handle     = registry.Register(owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::Imported,
        .name = rstd::string::String::make(rstd::cppstd::as_str("asset")),
    });
    auto                           generation = registry.Generation();

    registry.Reset();

    EXPECT_GT(registry.Generation(), generation);
    EXPECT_TRUE(registry.ResolveTexture(handle).is_none());
    EXPECT_EQ(registry.Size(), rstd::usize());
}

TEST(TextureRegistry, PublishesVersionedPhysicalGenerations) {
    owe::resource::TextureRegistry registry;
    auto                           handle = registry.Register(owe::resource::TextureRequest {
        .kind       = owe::resource::TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make(rstd::cppstd::as_str("frame")),
        .definition = rstd::Some(owe::resource::TextureDefinition {
            .width  = rstd::i32(256),
            .height = rstd::i32(128),
        }),
    });

    auto allocation = TextureAllocation(7);
    auto first      = registry.Publish(
        handle, allocation.clone(), owe::resource::ReadyToken { .value = rstd::u64(3) });
    ASSERT_TRUE(first.is_some());
    EXPECT_EQ(*first, rstd::u64(1));

    auto same = registry.Publish(
        handle, allocation.clone(), owe::resource::ReadyToken { .value = rstd::u64(4) });
    ASSERT_TRUE(same.is_some());
    EXPECT_EQ(*same, rstd::u64(1));

    auto replaced = registry.Publish(
        handle, TextureAllocation(8), owe::resource::ReadyToken { .value = rstd::u64(5) });
    ASSERT_TRUE(replaced.is_some());
    EXPECT_EQ(*replaced, rstd::u64(2));

    auto physical = registry.Resolve(handle);
    ASSERT_TRUE(physical.is_some());
    EXPECT_EQ((**physical).generation, rstd::u64(2));
    EXPECT_EQ((**physical).ready.value, rstd::u64(5));
}

TEST(ShaderRegistry, OwnsVersionedArtifactsBehindStableHandles) {
    owe::resource_registry::ShaderRegistry registry;
    ShaderArtifactProvider                 provider;
    auto object = rstd::dyn<owe::resource::ShaderArtifactProvider>::from_ref(provider);

    auto first = registry.Ensure(ShaderRequest(4), object);
    ASSERT_TRUE(first.is_ok());
    auto handle = rstd::move(first).unwrap_unchecked();
    EXPECT_EQ(provider.loads, rstd::usize(1));

    auto same = registry.Ensure(ShaderRequest(4), object);
    ASSERT_TRUE(same.is_ok());
    EXPECT_EQ(rstd::move(same).unwrap_unchecked(), handle);
    EXPECT_EQ(provider.loads, rstd::usize(1));

    auto changed = registry.Ensure(ShaderRequest(5), object);
    ASSERT_TRUE(changed.is_ok());
    EXPECT_EQ(rstd::move(changed).unwrap_unchecked(), handle);
    EXPECT_EQ(provider.loads, rstd::usize(2));
    auto entry = registry.Resolve(handle);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ((**entry).physical->physical_generation, rstd::u64(2));

    registry.Reset();
    EXPECT_TRUE(registry.Resolve(handle).is_none());
}

TEST(BufferRegistry, OwnsLogicalDefinitionsBehindStableHandles) {
    owe::resource_registry::BufferRegistry registry;
    auto                                   first = registry.Declare(owe::resource::BufferRequest {
        .name       = rstd::string::String::make(rstd::cppstd::as_str("vertices")),
        .definition = { .size = rstd::usize(128), .usage = owe::resource::BufferUsage::Vertex },
        .content_version = rstd::u64(3),
    });
    ASSERT_TRUE(first.Valid());

    auto changed = registry.Declare(owe::resource::BufferRequest {
        .name       = rstd::string::String::make(rstd::cppstd::as_str("vertices")),
        .definition = { .size = rstd::usize(256), .usage = owe::resource::BufferUsage::Vertex },
        .content_version = rstd::u64(4),
    });
    EXPECT_EQ(changed, first);
    auto entry = registry.ResolveBuffer(first);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ((**entry).definition_version, rstd::u64(2));
    EXPECT_EQ((**entry).content_version, rstd::u64(2));

    registry.Reset();
    EXPECT_TRUE(registry.ResolveBuffer(first).is_none());
}

TEST(BufferRegistry, UpdatesDynamicContentWithoutReplacingItsPlannedCapacity) {
    owe::resource_registry::BufferRegistry registry;
    BufferUploadBackend                    upload_backend;
    auto upload  = rstd::dyn<owe::vulkan::BufferUploadBackend>::from_ref(upload_backend);
    auto content = rstd::vec::Vec<rstd::u8>::make();
    content.push(rstd::u8(1));
    content.push(rstd::u8(2));
    auto request = owe::resource::BufferRequest {
        .name       = rstd::string::String::make(rstd::cppstd::as_str("dynamic-vertices")),
        .definition = { .size      = rstd::usize(128),
                        .usage     = owe::resource::BufferUsage::Vertex,
                        .alignment = rstd::usize(16) },
        .lifetime   = owe::resource::BufferLifetimeClass::Dynamic,
    };

    auto prepared = registry.Ensure(request.clone(), content.as_slice(), upload.as_mut_ref());
    ASSERT_TRUE(prepared.is_ok());
    auto buffer = rstd::move(prepared).unwrap_unchecked();
    EXPECT_EQ(upload_backend.last_request.size, 128u);
    EXPECT_EQ(upload_backend.last_request.alignment, 16u);
    EXPECT_EQ(upload_backend.last_request.usage, owe::vulkan::BufferUploadClass::Vertex);

    content.push(rstd::u8(3));
    auto updated =
        registry.Update(buffer.resource, content.as_slice(), rstd::u64(2), upload.as_mut_ref());
    ASSERT_TRUE(updated.is_ok());
    EXPECT_EQ(upload_backend.updates, rstd::usize(1));
    auto physical = registry.Resolve(buffer.resource);
    ASSERT_TRUE(physical.is_some());
    EXPECT_EQ((**physical).source_generation, rstd::u64(2));
}

TEST(ResourcePrepareService, VisitsBufferAndShaderPlansThroughTypedProviders) {
    owe::resource::TextureRegistry         textures;
    owe::resource_registry::BufferRegistry buffers;
    owe::resource_registry::ShaderRegistry shaders;
    BufferUploadBackend                    upload_backend;
    BufferContentProvider                  buffer_provider;
    ShaderArtifactProvider                 shader_provider;
    auto upload = rstd::dyn<owe::vulkan::BufferUploadBackend>::from_ref(upload_backend);
    auto buffer = rstd::dyn<owe::resource::BufferContentProvider>::from_ref(buffer_provider);
    auto shader = rstd::dyn<owe::resource::ShaderArtifactProvider>::from_ref(shader_provider);

    owe::resource::ResourcePlan plan { .generation = rstd::u64(12) };
    plan.buffers.push(owe::resource::BufferPlanEntry {
        .handle =
            owe::resource::BufferUseHandle { .index = rstd::u64(3), .generation = rstd::u64(12) },
        .request =
            owe::resource::BufferRequest {
                .name            = rstd::string::String::make(rstd::cppstd::as_str("vertices")),
                .definition      = { .size  = rstd::usize(2),
                                     .usage = owe::resource::BufferUsage::Vertex },
                .content_version = rstd::u64(4),
            },
    });
    plan.shaders.push(owe::resource::ShaderPlanEntry {
        .handle =
            owe::resource::ShaderUseHandle { .index = rstd::u64(7), .generation = rstd::u64(12) },
        .request = ShaderRequest(5),
    });

    owe::resource_registry::ResourcePrepareService service(
        textures,
        rstd::None<rstd::mut_ref<rstd::dyn<owe::vulkan::ImagePrepareBackend>>>(),
        buffers,
        upload.as_mut_ref(),
        shaders);
    auto prepared = service.Prepare(plan,
                                    owe::resource_registry::ResourceContentProviders {
                                        .buffer = rstd::Some(buffer.as_mut_ref()),
                                        .shader = rstd::Some(shader.as_mut_ref()),
                                    });

    ASSERT_TRUE(prepared.is_ok());
    auto table = rstd::move(prepared).unwrap_unchecked();
    EXPECT_EQ(table.Generation(), rstd::u64(12));
    EXPECT_EQ(table.BufferCount(), rstd::usize(1));
    EXPECT_EQ(table.ShaderCount(), rstd::usize(1));
    EXPECT_EQ(buffer_provider.loads, rstd::usize(1));
    EXPECT_EQ(upload_backend.uploads, rstd::usize(1));
    EXPECT_EQ(shader_provider.loads, rstd::usize(1));
}

TEST(PreparedResourceTable, ResolvesTypedUsesWithoutRegistryLookup) {
    owe::resource_registry::PreparedResourceTable table(rstd::u64(9));
    auto                                          use =
        owe::resource::TextureUseHandle { .index = rstd::u64(4), .generation = rstd::u64(9) };
    auto allocation = TextureAllocation(11);

    EXPECT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use = use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(2), .generation = rstd::u64(1) },
        .request =
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make(rstd::cppstd::as_str("prepared")),
            },
        .physical            = allocation.clone(),
        .image               = allocation->View(),
        .physical_generation = rstd::u64(3),
        .ready               = owe::resource::ReadyToken { .value = rstd::u64(8) },
    }));

    auto prepared = table.Resolve(use);
    ASSERT_TRUE(prepared.is_some());
    EXPECT_EQ(table.Generation(), rstd::u64(9));
    EXPECT_EQ(table.TextureCount(), rstd::usize(1));
    EXPECT_EQ((**prepared).image.getActive().generation, rstd::u64(11));
    EXPECT_EQ((**prepared).physical_generation, rstd::u64(3));
    auto leases = table.Leases();
    ASSERT_EQ(leases.textures.len(), rstd::usize(1));
    EXPECT_EQ(leases.textures[rstd::usize()].resource.index, rstd::u64(2));
    EXPECT_EQ(leases.textures[rstd::usize()].physical_generation, rstd::u64(3));
}

TEST(PreparedResourceTable, PinsEveryPreparedGenerationInOneLeaseSet) {
    owe::resource_registry::PreparedResourceTable table(rstd::u64(4));

    auto texture_physical = TextureAllocation(2);
    auto texture_use =
        owe::resource::TextureUseHandle { .index = rstd::u64(), .generation = rstd::u64(4) };
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use = texture_use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(10), .generation = rstd::u64(2) },
        .request =
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make(rstd::cppstd::as_str("leased")),
            },
        .physical = texture_physical.clone(),
        .image    = texture_physical->View(),
    }));

    auto buffer_physical = rstd::sync::Arc<owe::resource_registry::BufferPhysical>::make(
        owe::vulkan::BufferAllocation {},
        rstd::u64(3),
        rstd::u64(7),
        owe::resource::ReadyToken { .value = rstd::u64(7) });
    auto buffer_use =
        owe::resource::BufferUseHandle { .index = rstd::u64(1), .generation = rstd::u64(4) };
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedBufferUse {
        .use = buffer_use,
        .buffer =
            owe::resource_registry::PreparedBuffer {
                .resource = owe::resource::BufferHandle { .index      = rstd::u64(11),
                                                          .generation = rstd::u64(2) },
                .physical = buffer_physical.clone(),
            },
    }));

    auto shader_physical = rstd::sync::Arc<owe::resource_registry::ShaderPhysical>::make(
        owe::resource::ShaderArtifact {}, rstd::u64(5));
    auto shader_use =
        owe::resource::ShaderUseHandle { .index = rstd::u64(2), .generation = rstd::u64(4) };
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedShaderUse {
        .use = shader_use,
        .shader =
            owe::resource_registry::PreparedShader {
                .resource = owe::resource::ShaderHandle { .index      = rstd::u64(12),
                                                          .generation = rstd::u64(2) },
                .physical = shader_physical.clone(),
            },
    }));

    auto pipeline = rstd::sync::Arc<owe::resource_registry::PipelineResourceEntry>::make();
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedPipeline {
        .use =
            owe::resource::PipelineUseHandle { .index = rstd::u64(3), .generation = rstd::u64(4) },
        .resource =
            owe::resource::PipelineHandle { .index = rstd::u64(13), .generation = rstd::u64(2) },
        .physical = pipeline.clone(),
    }));
    auto render_pass = rstd::sync::Arc<vvk::RenderPass>::make();
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedRenderPass {
        .use = owe::resource::RenderPassUseHandle { .index      = rstd::u64(4),
                                                    .generation = rstd::u64(4) },
        .resource =
            owe::resource::RenderPassHandle { .index = rstd::u64(14), .generation = rstd::u64(2) },
        .physical = render_pass.clone(),
    }));
    auto framebuffer = rstd::sync::Arc<vvk::Framebuffer>::make();
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedFramebuffer {
        .use = owe::resource::FramebufferUseHandle { .index      = rstd::u64(5),
                                                     .generation = rstd::u64(4) },
        .resource =
            owe::resource::FramebufferHandle { .index = rstd::u64(15), .generation = rstd::u64(2) },
        .physical = framebuffer.clone(),
    }));
    auto external_use =
        owe::resource::ExternalUseHandle { .index = rstd::u64(6), .generation = rstd::u64(4) };
    table.Insert(owe::resource_registry::PreparedExternalUse {
        .use   = external_use,
        .frame = owe::resource_registry::PreparedExternalFrame {},
    });

    ASSERT_TRUE(table.Resolve(texture_use).is_some());
    ASSERT_TRUE(table.Resolve(buffer_use).is_some());
    ASSERT_TRUE(table.Resolve(shader_use).is_some());
    ASSERT_TRUE(table.Resolve(external_use).is_some());
    auto leases = table.Leases();
    EXPECT_EQ(leases.textures.len(), rstd::usize(1));
    EXPECT_EQ(leases.buffers.len(), rstd::usize(1));
    EXPECT_EQ(leases.shaders.len(), rstd::usize(1));
    EXPECT_EQ(leases.pipelines.len(), rstd::usize(1));
    EXPECT_EQ(leases.render_passes.len(), rstd::usize(1));
    EXPECT_EQ(leases.framebuffers.len(), rstd::usize(1));
    EXPECT_EQ(leases.externals.len(), rstd::usize(1));
    EXPECT_EQ(texture_physical.strong_count(), rstd::usize(3));
    EXPECT_EQ(buffer_physical.strong_count(), rstd::usize(3));
    EXPECT_EQ(shader_physical.strong_count(), rstd::usize(3));
    EXPECT_EQ(pipeline.strong_count(), rstd::usize(3));
    EXPECT_EQ(render_pass.strong_count(), rstd::usize(3));
    EXPECT_EQ(framebuffer.strong_count(), rstd::usize(3));

    owe::resource_registry::SubmissionTracker submissions;
    auto                                      completion = submissions.Begin(table);
    ASSERT_TRUE(completion.Valid());
    auto submitted = submissions.Complete(completion);
    ASSERT_TRUE(submitted.is_some());
    EXPECT_EQ(submitted->textures.len(), rstd::usize(1));
    EXPECT_EQ(submitted->buffers.len(), rstd::usize(1));
    EXPECT_EQ(submitted->shaders.len(), rstd::usize(1));
    EXPECT_EQ(submitted->pipelines.len(), rstd::usize(1));
    EXPECT_EQ(submitted->render_passes.len(), rstd::usize(1));
    EXPECT_EQ(submitted->framebuffers.len(), rstd::usize(1));
}

TEST(DescriptorLayoutRegistry, CanonicalizesBindingOrder) {
    owe::vulkan::DescriptorSetInfo first {
        .push_descriptor = true,
        .bindings = {
            VkDescriptorSetLayoutBinding {
                .binding         = 3,
                .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
            VkDescriptorSetLayoutBinding {
                .binding         = 0,
                .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT,
            },
        },
    };
    auto second = first;
    std::reverse(second.bindings.begin(), second.bindings.end());

    auto first_schema  = owe::resource_registry::DescriptorLayoutRegistry::CanonicalSchema(first);
    auto second_schema = owe::resource_registry::DescriptorLayoutRegistry::CanonicalSchema(second);
    ASSERT_TRUE(first_schema.is_ok());
    ASSERT_TRUE(second_schema.is_ok());
    EXPECT_TRUE(rstd::move(first_schema).unwrap_unchecked() ==
                rstd::move(second_schema).unwrap_unchecked());
}

TEST(DescriptorLayoutRegistry, RejectsDuplicateBindings) {
    owe::vulkan::DescriptorSetInfo info {
        .bindings = {
            VkDescriptorSetLayoutBinding {
                .binding         = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT,
            },
            VkDescriptorSetLayoutBinding {
                .binding         = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
        },
    };

    EXPECT_TRUE(owe::resource_registry::DescriptorLayoutRegistry::CanonicalSchema(info).is_err());
}

TEST(DescriptorSystem, PreparesOwnedPushBindingPacket) {
    owe::resource_registry::DescriptorSystem descriptors;
    auto images = rstd::vec::Vec<owe::resource_registry::DescriptorImageBinding>::make();
    owe::vulkan::ImageParameters image;
    image.generation = rstd::u64(17);
    images.push(owe::resource_registry::DescriptorImageBinding {
        .binding = 2,
        .image   = image,
    });
    auto prepared =
        descriptors.PreparePush(images.as_slice(),
                                rstd::Some(owe::resource_registry::DescriptorBufferBinding {
                                    .binding = 0,
                                    .offset  = 64,
                                    .size    = 128,
                                }));
    auto handle = prepared.handle;

    EXPECT_TRUE(handle.Valid());
    EXPECT_EQ(prepared.images.len(), rstd::usize(1));
    EXPECT_EQ(prepared.images[rstd::usize()].binding, 2u);
    EXPECT_EQ(prepared.images[rstd::usize()].image.generation, rstd::u64(17));
    ASSERT_TRUE(prepared.buffer.is_some());
    EXPECT_EQ(prepared.buffer->offset, 64u);
    EXPECT_EQ(prepared.buffer->size, 128u);

    owe::resource_registry::PreparedResourceTable table;
    EXPECT_TRUE(table.Insert(rstd::move(prepared)));
    auto resolved = table.Resolve(handle);
    ASSERT_TRUE(resolved.is_some());
    EXPECT_EQ(table.DescriptorCount(), rstd::usize(1));
    EXPECT_EQ((**resolved).images[rstd::usize()].image.generation, rstd::u64(17));

    owe::resource_registry::SubmissionTracker submissions;
    auto                                      completion = submissions.Begin(table);
    EXPECT_TRUE(completion.Valid());
    EXPECT_EQ(submissions.InFlight(), rstd::usize(1));
    auto completed = submissions.Complete(completion);
    ASSERT_TRUE(completed.is_some());
    EXPECT_EQ(completed->program_generation, rstd::u64());
    EXPECT_EQ(completed->descriptors.len(), rstd::usize(1));
    EXPECT_EQ(submissions.InFlight(), rstd::usize());
}

TEST(UploadScheduler, TracksTimelineReadiness) {
    owe::resource_registry::UploadScheduler       uploads;
    owe::resource_registry::PreparedResourceTable table(rstd::u64(1));
    auto physical = rstd::sync::Arc<owe::resource_registry::BufferPhysical>::make(
        owe::vulkan::BufferAllocation {},
        rstd::u64(1),
        rstd::u64(1),
        owe::resource::ReadyToken { .value = rstd::u64(1) });
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedBufferUse {
        .use = owe::resource::BufferUseHandle { .index = rstd::u64(), .generation = rstd::u64(1) },
        .buffer =
            owe::resource_registry::PreparedBuffer {
                .resource = owe::resource::BufferHandle { .index      = rstd::u64(),
                                                          .generation = rstd::u64(1) },
                .physical = physical.clone(),
            },
    }));
    auto first  = uploads.Reserve();
    auto second = uploads.Reserve();
    EXPECT_LT(first.value, second.value);
    EXPECT_TRUE(uploads.MarkSubmitted(first, table));
    EXPECT_TRUE(uploads.MarkSubmitted(second, table));
    EXPECT_EQ(physical.strong_count(), rstd::usize(4));
    ASSERT_TRUE(uploads.Pending().is_some());
    EXPECT_EQ(uploads.Pending()->value, second.value);
    EXPECT_EQ(uploads.InFlight(), rstd::usize(2));

    uploads.CompleteThrough(first.value);
    EXPECT_EQ(physical.strong_count(), rstd::usize(3));
    EXPECT_EQ(uploads.InFlight(), rstd::usize(1));
    EXPECT_EQ(uploads.Pending()->value, second.value);
    uploads.CompleteThrough(second.value);
    EXPECT_EQ(physical.strong_count(), rstd::usize(2));
    EXPECT_EQ(uploads.InFlight(), rstd::usize());
    EXPECT_TRUE(uploads.Pending().is_none());
}

TEST(ResourceStateTracker, CompilesTypedUsesIntoBarrierPackets) {
    auto use =
        owe::resource::TextureUseHandle { .index = rstd::u64(6), .generation = rstd::u64(3) };
    auto request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::Imported,
        .name = rstd::string::String::make(rstd::cppstd::as_str("sampled")),
    };
    owe::resource::ResourcePlan plan { .generation = rstd::u64(3) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = use,
        .request = request.clone(),
        .access  = owe::resource::ResourceAccess::Read,
    });

    auto                                          allocation = TextureAllocation(1);
    owe::resource_registry::PreparedResourceTable table(rstd::u64(3));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use = use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(8), .generation = rstd::u64(1) },
        .request             = rstd::move(request),
        .physical            = allocation.clone(),
        .image               = allocation->View(),
        .physical_generation = rstd::u64(2),
        .ready               = owe::resource::ReadyToken { .value = rstd::u64(4) },
    }));

    owe::resource_registry::ResourceStateTracker states;
    ASSERT_TRUE(states.Compile(plan, table));
    auto sampled = states.Prepare(use, owe::resource_registry::TextureStateKind::Sampled);
    ASSERT_TRUE(sampled.is_some());
    EXPECT_EQ(sampled->barrier.oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(sampled->barrier.newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    auto transfer = states.Prepare(use, owe::resource_registry::TextureStateKind::TransferSource);
    ASSERT_TRUE(transfer.is_some());
    EXPECT_EQ(transfer->src_stage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    EXPECT_EQ(transfer->dst_stage, VK_PIPELINE_STAGE_TRANSFER_BIT);
    EXPECT_EQ(transfer->barrier.oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(transfer->barrier.newLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    auto discard = states.Prepare(
        use, owe::resource_registry::TextureStateKind::TransferDestination, {}, true);
    ASSERT_TRUE(discard.is_some());
    EXPECT_EQ(discard->barrier.oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(discard->barrier.newLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    auto sampled_after_clear =
        states.Prepare(use, owe::resource_registry::TextureStateKind::Sampled);
    ASSERT_TRUE(sampled_after_clear.is_some());
    EXPECT_EQ(sampled_after_clear->barrier.oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_EQ(sampled_after_clear->barrier.newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

TEST(ResourceStateTracker, SharesStateAcrossUsesOfOneTexture) {
    auto first_use =
        owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(3) };
    auto second_use =
        owe::resource::TextureUseHandle { .index = rstd::u64(2), .generation = rstd::u64(3) };
    auto resource =
        owe::resource::TextureHandle { .index = rstd::u64(8), .generation = rstd::u64(1) };
    auto request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::RenderTarget,
        .name = rstd::string::String::make(rstd::cppstd::as_str("frame")),
    };
    owe::resource::ResourcePlan plan { .generation = rstd::u64(3) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = first_use,
        .request = request.clone(),
        .access  = owe::resource::ResourceAccess::Write,
    });
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = second_use,
        .request = request.clone(),
        .access  = owe::resource::ResourceAccess::ReadWrite,
    });

    auto                                          allocation = TextureAllocation(1);
    owe::resource_registry::PreparedResourceTable table(rstd::u64(3));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use      = first_use,
        .resource = resource,
        .request  = request.clone(),
        .physical = allocation.clone(),
        .image    = allocation->View(),
    }));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use      = second_use,
        .resource = resource,
        .request  = rstd::move(request),
        .physical = allocation.clone(),
        .image    = allocation->View(),
    }));

    owe::resource_registry::ResourceStateTracker states;
    ASSERT_TRUE(states.Compile(plan, table));
    ASSERT_TRUE(states.Set(first_use, owe::resource_registry::TextureStateKind::Sampled));
    auto barrier =
        states.Prepare(second_use, owe::resource_registry::TextureStateKind::ColorAttachment);
    ASSERT_TRUE(barrier.is_some());
    EXPECT_EQ(barrier->barrier.oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(barrier->barrier.newLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

TEST(ResourceStateTracker, PreservesFrameBoundaryContentBeforeItsFirstRead) {
    auto use =
        owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(3) };
    auto request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::RenderTarget,
        .name = rstd::string::String::make(rstd::cppstd::as_str("history")),
        .content =
            owe::resource::TextureContentFlag(owe::resource::TextureContent::PreserveAcrossFrames),
    };
    owe::resource::ResourcePlan plan { .generation = rstd::u64(3) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = use,
        .request = request.clone(),
        .access  = owe::resource::ResourceAccess::ReadWrite,
    });

    auto                                          allocation = TextureAllocation(1);
    owe::resource_registry::PreparedResourceTable table(rstd::u64(3));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use = use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(8), .generation = rstd::u64(1) },
        .request  = rstd::move(request),
        .physical = allocation.clone(),
        .image    = allocation->View(),
    }));

    owe::resource_registry::ResourceStateTracker states;
    ASSERT_TRUE(states.Compile(plan, table));
    auto sampled = states.Prepare(use, owe::resource_registry::TextureStateKind::Sampled);
    ASSERT_TRUE(sampled.is_some());
    EXPECT_EQ(sampled->barrier.oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(sampled->barrier.newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

TEST(MemoryBudgetPolicy, ClassifiesBudgetPressure) {
    owe::resource_registry::MemoryBudgetPolicy memory;
    memory.Refresh(owe::vulkan::MemoryBudgetSnapshot {
        .usage  = 70,
        .budget = 100,
    });
    EXPECT_EQ(memory.Pressure(), owe::resource_registry::MemoryPressure::Normal);
    EXPECT_FALSE(memory.ShouldEvictTransient());

    memory.Refresh(owe::vulkan::MemoryBudgetSnapshot {
        .usage  = 85,
        .budget = 100,
    });
    EXPECT_EQ(memory.Pressure(), owe::resource_registry::MemoryPressure::Elevated);

    memory.Refresh(owe::vulkan::MemoryBudgetSnapshot {
        .usage  = 96,
        .budget = 100,
    });
    EXPECT_EQ(memory.Pressure(), owe::resource_registry::MemoryPressure::Critical);
}

TEST(ExternalResourceBridge, PreparesLayoutAndQueueOwnershipContract) {
    owe::FrameSurfaceLease lease {
        .identity =
            owe::FrameSurfaceIdentity {
                .owner_generation = rstd::u64(2),
                .image_index      = rstd::u32(1),
                .acquire_serial   = rstd::u64(4),
            },
        .reuse =
            owe::FrameSurfaceReuseProof {
                .kind = owe::FrameSurfaceReuseKind::QueueOrdered,
            },
        .format               = VK_FORMAT_R8G8B8A8_UNORM,
        .initial_layout       = VK_IMAGE_LAYOUT_UNDEFINED,
        .initial_queue_family = 3,
        .acquire =
            owe::FrameSurfaceAcquireDependency {
                .kind = owe::FrameSurfaceAcquireKind::QueueOrdered,
            },
        .final_layout       = VK_IMAGE_LAYOUT_GENERAL,
        .final_queue_family = 3,
        .discard_content    = true,
    };
    lease.image.handle = reinterpret_cast<VkImage>(2);
    lease.image.extent = VkExtent3D { 64, 32, 1 };

    owe::resource_registry::ExternalResourceBridge bridge;
    auto prepared = bridge.Prepare(owe::vulkan::DeviceCapabilities {}, lease, 3);
    ASSERT_TRUE(prepared.is_ok());
    auto contract = rstd::move(prepared).unwrap_unchecked();
    EXPECT_EQ(contract.before_copy.Size(), rstd::usize(1));
    EXPECT_EQ(contract.after_copy.Size(), rstd::usize(1));
    EXPECT_EQ(contract.lease.identity.acquire_serial, rstd::u64(4));
}
