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
        if (request.size == 0 || request.size < content.len()) return rstd::None();
        return rstd::Some(owe::vulkan::BufferAllocation {});
    }

    bool UpdateBuffer(rstd::mut_ref<owe::vulkan::BufferAllocation>, rstd::slice<rstd::u8>) {
        ++updates;
        return true;
    }
};

auto ShaderRequest(rstd::u64 version) -> owe::resource::ShaderRequest {
    return owe::resource::ShaderRequest {
        .name = rstd::string::String::make(rstd::cppstd::as_str("sprite")),
        .source =
            owe::resource::ShaderDefinitionId {
                .index      = 3,
                .generation = 1,
            },
        .content_version = version,
    };
}

auto TextureAllocation(rstd::u64 generation) -> rstd::sync::Arc<owe::vulkan::TextureAllocation> {
    owe::vulkan::ImageSlots slots;
    slots.slots.resize(1);
    slots.slots[0].generation = generation;
    return rstd::sync::Arc<owe::vulkan::TextureAllocation>::make(rstd::move(slots));
}

} // namespace

TEST(TextureRegistry, OwnsLogicalEntriesBehindGenerationalHandles) {
    owe::resource::TextureRegistry registry;
    auto                           handle = registry.Register(owe::resource::TextureRequest {
        .kind       = owe::resource::TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make(rstd::cppstd::as_str("frame")),
        .definition = rstd::Some(owe::resource::TextureDefinition {
            .width  = 256,
            .height = 128,
        }),
    });
    ASSERT_TRUE(handle.Valid());
    EXPECT_EQ(registry.Size(), 1u);

    auto found = registry.Find(owe::resource::TextureRequestKind::RenderTarget,
                               rstd::cppstd::as_str("frame"));
    ASSERT_TRUE(found.is_some());
    EXPECT_EQ(*found, handle);

    auto entry = registry.ResolveTexture(handle);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view((**entry).request.name.as_str()), "frame");
    EXPECT_EQ((**entry).definition_version, 1u);

    auto resized = registry.Register(owe::resource::TextureRequest {
        .kind       = owe::resource::TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make(rstd::cppstd::as_str("frame")),
        .definition = rstd::Some(owe::resource::TextureDefinition {
            .width  = 512,
            .height = 128,
        }),
    });
    EXPECT_EQ(resized, handle);
    entry = registry.ResolveTexture(handle);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ((**entry).definition_version, 2u);

    auto view  = rstd::dyn<owe::resource::TextureLogicalRegistryView>::from_ref(registry);
    auto state = view->ResolveTextureState(handle);
    ASSERT_TRUE(state.is_some());
    EXPECT_EQ(state->definition_version, 2u);
    EXPECT_EQ(state->content_version, 1u);
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
    EXPECT_EQ(registry.Size(), 0u);
}

TEST(TextureRegistry, PublishesVersionedPhysicalGenerations) {
    owe::resource::TextureRegistry registry;
    auto                           handle = registry.Register(owe::resource::TextureRequest {
        .kind       = owe::resource::TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make(rstd::cppstd::as_str("frame")),
        .definition = rstd::Some(owe::resource::TextureDefinition {
            .width  = 256,
            .height = 128,
        }),
    });

    auto allocation = TextureAllocation(7);
    auto first =
        registry.Publish(handle, allocation.clone(), owe::resource::ReadyToken { .value = 3 });
    ASSERT_TRUE(first.is_some());
    EXPECT_EQ(*first, 1u);

    auto same =
        registry.Publish(handle, allocation.clone(), owe::resource::ReadyToken { .value = 4 });
    ASSERT_TRUE(same.is_some());
    EXPECT_EQ(*same, 1u);

    auto replaced =
        registry.Publish(handle, TextureAllocation(8), owe::resource::ReadyToken { .value = 5 });
    ASSERT_TRUE(replaced.is_some());
    EXPECT_EQ(*replaced, 2u);

    auto physical = registry.Resolve(handle);
    ASSERT_TRUE(physical.is_some());
    EXPECT_EQ((**physical).generation, 2u);
    EXPECT_EQ((**physical).ready.value, 5u);
}

TEST(ShaderRegistry, OwnsVersionedArtifactsBehindStableHandles) {
    owe::resource_registry::ShaderRegistry registry;
    ShaderArtifactProvider                 provider;
    auto object = rstd::dyn<owe::resource::ShaderArtifactProvider>::from_ref(provider);

    auto first = registry.Ensure(ShaderRequest(4), object);
    ASSERT_TRUE(first.is_ok());
    auto handle = rstd::move(first).unwrap_unchecked();
    EXPECT_EQ(provider.loads, 1u);

    auto same = registry.Ensure(ShaderRequest(4), object);
    ASSERT_TRUE(same.is_ok());
    EXPECT_EQ(rstd::move(same).unwrap_unchecked(), handle);
    EXPECT_EQ(provider.loads, 1u);

    auto changed = registry.Ensure(ShaderRequest(5), object);
    ASSERT_TRUE(changed.is_ok());
    EXPECT_EQ(rstd::move(changed).unwrap_unchecked(), handle);
    EXPECT_EQ(provider.loads, 2u);
    auto entry = registry.Resolve(handle);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ((**entry).physical->physical_generation, 2u);

    registry.Reset();
    EXPECT_TRUE(registry.Resolve(handle).is_none());
}

TEST(BufferRegistry, OwnsLogicalDefinitionsBehindStableHandles) {
    owe::resource_registry::BufferRegistry registry;
    auto                                   first = registry.Declare(owe::resource::BufferRequest {
        .name            = rstd::string::String::make(rstd::cppstd::as_str("vertices")),
        .definition      = { .size = 128, .usage = owe::resource::BufferUsage::Vertex },
        .content_version = 3,
    });
    ASSERT_TRUE(first.Valid());

    auto changed = registry.Declare(owe::resource::BufferRequest {
        .name            = rstd::string::String::make(rstd::cppstd::as_str("vertices")),
        .definition      = { .size = 256, .usage = owe::resource::BufferUsage::Vertex },
        .content_version = 4,
    });
    EXPECT_EQ(changed, first);
    auto entry = registry.ResolveBuffer(first);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ((**entry).definition_version, 2u);
    EXPECT_EQ((**entry).content_version, 2u);

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
        .definition = { .size = 128, .usage = owe::resource::BufferUsage::Vertex, .alignment = 16 },
        .lifetime   = owe::resource::BufferLifetimeClass::Dynamic,
    };

    auto prepared = registry.Ensure(request.clone(), content.as_slice(), upload.as_mut_ref());
    ASSERT_TRUE(prepared.is_ok());
    auto buffer = rstd::move(prepared).unwrap_unchecked();
    EXPECT_EQ(upload_backend.last_request.size, 128u);
    EXPECT_EQ(upload_backend.last_request.alignment, 16u);
    EXPECT_EQ(upload_backend.last_request.usage, owe::vulkan::BufferUploadClass::Vertex);

    content.push(rstd::u8(3));
    auto updated = registry.Update(buffer.resource, content.as_slice(), 2, upload.as_mut_ref());
    ASSERT_TRUE(updated.is_ok());
    EXPECT_EQ(upload_backend.updates, 1u);
    auto physical = registry.Resolve(buffer.resource);
    ASSERT_TRUE(physical.is_some());
    EXPECT_EQ((**physical).source_generation, 2u);
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

    owe::resource::ResourcePlan plan { .generation = 12 };
    plan.buffers.push(owe::resource::BufferPlanEntry {
        .handle = owe::resource::BufferUseHandle { .index = 3, .generation = 12 },
        .request =
            owe::resource::BufferRequest {
                .name            = rstd::string::String::make(rstd::cppstd::as_str("vertices")),
                .definition      = { .size = 2, .usage = owe::resource::BufferUsage::Vertex },
                .content_version = 4,
            },
    });
    plan.shaders.push(owe::resource::ShaderPlanEntry {
        .handle  = owe::resource::ShaderUseHandle { .index = 7, .generation = 12 },
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
    EXPECT_EQ(table.Generation(), 12u);
    EXPECT_EQ(table.BufferCount(), 1u);
    EXPECT_EQ(table.ShaderCount(), 1u);
    EXPECT_EQ(buffer_provider.loads, 1u);
    EXPECT_EQ(upload_backend.uploads, 1u);
    EXPECT_EQ(shader_provider.loads, 1u);
}

TEST(PreparedResourceTable, ResolvesTypedUsesWithoutRegistryLookup) {
    owe::resource_registry::PreparedResourceTable table(9);
    auto use        = owe::resource::TextureUseHandle { .index = 4, .generation = 9 };
    auto allocation = TextureAllocation(11);

    EXPECT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use      = use,
        .resource = owe::resource::TextureHandle { .index = 2, .generation = 1 },
        .request =
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make(rstd::cppstd::as_str("prepared")),
            },
        .physical            = allocation.clone(),
        .image               = allocation->View(),
        .physical_generation = 3,
        .ready               = owe::resource::ReadyToken { .value = 8 },
    }));

    auto prepared = table.Resolve(use);
    ASSERT_TRUE(prepared.is_some());
    EXPECT_EQ(table.Generation(), 9u);
    EXPECT_EQ(table.TextureCount(), 1u);
    EXPECT_EQ((**prepared).image.getActive().generation, 11u);
    EXPECT_EQ((**prepared).physical_generation, 3u);
    auto leases = table.Leases();
    ASSERT_EQ(leases.textures.len(), 1u);
    EXPECT_EQ(leases.textures[0].resource.index, 2u);
    EXPECT_EQ(leases.textures[0].physical_generation, 3u);
}

TEST(PreparedResourceTable, PinsEveryPreparedGenerationInOneLeaseSet) {
    owe::resource_registry::PreparedResourceTable table(4);

    auto texture_physical = TextureAllocation(2);
    auto texture_use      = owe::resource::TextureUseHandle { .index = 0, .generation = 4 };
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use      = texture_use,
        .resource = owe::resource::TextureHandle { .index = 10, .generation = 2 },
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
        owe::resource::ReadyToken { .value = 7 });
    auto buffer_use = owe::resource::BufferUseHandle { .index = 1, .generation = 4 };
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedBufferUse {
        .use = buffer_use,
        .buffer =
            owe::resource_registry::PreparedBuffer {
                .resource = owe::resource::BufferHandle { .index = 11, .generation = 2 },
                .physical = buffer_physical.clone(),
            },
    }));

    auto shader_physical = rstd::sync::Arc<owe::resource_registry::ShaderPhysical>::make(
        owe::resource::ShaderArtifact {}, rstd::u64(5));
    auto shader_use = owe::resource::ShaderUseHandle { .index = 2, .generation = 4 };
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedShaderUse {
        .use = shader_use,
        .shader =
            owe::resource_registry::PreparedShader {
                .resource = owe::resource::ShaderHandle { .index = 12, .generation = 2 },
                .physical = shader_physical.clone(),
            },
    }));

    auto pipeline = rstd::sync::Arc<owe::resource_registry::PipelineResourceEntry>::make();
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedPipeline {
        .use      = owe::resource::PipelineUseHandle { .index = 3, .generation = 4 },
        .resource = owe::resource::PipelineHandle { .index = 13, .generation = 2 },
        .physical = pipeline.clone(),
    }));
    auto render_pass = rstd::sync::Arc<vvk::RenderPass>::make();
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedRenderPass {
        .use      = owe::resource::RenderPassUseHandle { .index = 4, .generation = 4 },
        .resource = owe::resource::RenderPassHandle { .index = 14, .generation = 2 },
        .physical = render_pass.clone(),
    }));
    auto framebuffer = rstd::sync::Arc<vvk::Framebuffer>::make();
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedFramebuffer {
        .use      = owe::resource::FramebufferUseHandle { .index = 5, .generation = 4 },
        .resource = owe::resource::FramebufferHandle { .index = 15, .generation = 2 },
        .physical = framebuffer.clone(),
    }));
    auto external_use = owe::resource::ExternalUseHandle { .index = 6, .generation = 4 };
    table.Insert(owe::resource_registry::PreparedExternalUse {
        .use   = external_use,
        .frame = owe::resource_registry::PreparedExternalFrame {},
    });

    ASSERT_TRUE(table.Resolve(texture_use).is_some());
    ASSERT_TRUE(table.Resolve(buffer_use).is_some());
    ASSERT_TRUE(table.Resolve(shader_use).is_some());
    ASSERT_TRUE(table.Resolve(external_use).is_some());
    auto leases = table.Leases();
    EXPECT_EQ(leases.textures.len(), 1u);
    EXPECT_EQ(leases.buffers.len(), 1u);
    EXPECT_EQ(leases.shaders.len(), 1u);
    EXPECT_EQ(leases.pipelines.len(), 1u);
    EXPECT_EQ(leases.render_passes.len(), 1u);
    EXPECT_EQ(leases.framebuffers.len(), 1u);
    EXPECT_EQ(leases.externals.len(), 1u);
    EXPECT_EQ(texture_physical.strong_count(), 3u);
    EXPECT_EQ(buffer_physical.strong_count(), 3u);
    EXPECT_EQ(shader_physical.strong_count(), 3u);
    EXPECT_EQ(pipeline.strong_count(), 3u);
    EXPECT_EQ(render_pass.strong_count(), 3u);
    EXPECT_EQ(framebuffer.strong_count(), 3u);

    owe::resource_registry::SubmissionTracker submissions;
    auto                                      completion = submissions.Begin(table);
    ASSERT_TRUE(completion.Valid());
    auto submitted = submissions.Complete(completion);
    ASSERT_TRUE(submitted.is_some());
    EXPECT_EQ(submitted->textures.len(), 1u);
    EXPECT_EQ(submitted->buffers.len(), 1u);
    EXPECT_EQ(submitted->shaders.len(), 1u);
    EXPECT_EQ(submitted->pipelines.len(), 1u);
    EXPECT_EQ(submitted->render_passes.len(), 1u);
    EXPECT_EQ(submitted->framebuffers.len(), 1u);
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
    image.generation = 17;
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
    EXPECT_EQ(prepared.images.len(), 1u);
    EXPECT_EQ(prepared.images[0].binding, 2u);
    EXPECT_EQ(prepared.images[0].image.generation, 17u);
    ASSERT_TRUE(prepared.buffer.is_some());
    EXPECT_EQ(prepared.buffer->offset, 64u);
    EXPECT_EQ(prepared.buffer->size, 128u);

    owe::resource_registry::PreparedResourceTable table;
    EXPECT_TRUE(table.Insert(rstd::move(prepared)));
    auto resolved = table.Resolve(handle);
    ASSERT_TRUE(resolved.is_some());
    EXPECT_EQ(table.DescriptorCount(), 1u);
    EXPECT_EQ((**resolved).images[0].image.generation, 17u);

    owe::resource_registry::SubmissionTracker submissions;
    auto                                      completion = submissions.Begin(table);
    EXPECT_TRUE(completion.Valid());
    EXPECT_EQ(submissions.InFlight(), 1u);
    auto completed = submissions.Complete(completion);
    ASSERT_TRUE(completed.is_some());
    EXPECT_EQ(completed->program_generation, 0u);
    EXPECT_EQ(completed->descriptors.len(), 1u);
    EXPECT_EQ(submissions.InFlight(), 0u);
}

TEST(UploadScheduler, TracksTimelineReadiness) {
    owe::resource_registry::UploadScheduler       uploads;
    owe::resource_registry::PreparedResourceTable table(1);
    auto physical = rstd::sync::Arc<owe::resource_registry::BufferPhysical>::make(
        owe::vulkan::BufferAllocation {},
        rstd::u64(1),
        rstd::u64(1),
        owe::resource::ReadyToken { .value = 1 });
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedBufferUse {
        .use = owe::resource::BufferUseHandle { .index = 0, .generation = 1 },
        .buffer =
            owe::resource_registry::PreparedBuffer {
                .resource = owe::resource::BufferHandle { .index = 0, .generation = 1 },
                .physical = physical.clone(),
            },
    }));
    auto first  = uploads.Reserve();
    auto second = uploads.Reserve();
    EXPECT_LT(first.value, second.value);
    EXPECT_TRUE(uploads.MarkSubmitted(first, table));
    EXPECT_TRUE(uploads.MarkSubmitted(second, table));
    EXPECT_EQ(physical.strong_count(), 4u);
    ASSERT_TRUE(uploads.Pending().is_some());
    EXPECT_EQ(uploads.Pending()->value, second.value);
    EXPECT_EQ(uploads.InFlight(), 2u);

    uploads.CompleteThrough(first.value);
    EXPECT_EQ(physical.strong_count(), 3u);
    EXPECT_EQ(uploads.InFlight(), 1u);
    EXPECT_EQ(uploads.Pending()->value, second.value);
    uploads.CompleteThrough(second.value);
    EXPECT_EQ(physical.strong_count(), 2u);
    EXPECT_EQ(uploads.InFlight(), 0u);
    EXPECT_TRUE(uploads.Pending().is_none());
}

TEST(ResourceStateTracker, CompilesTypedUsesIntoBarrierPackets) {
    auto use     = owe::resource::TextureUseHandle { .index = 6, .generation = 3 };
    auto request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::Imported,
        .name = rstd::string::String::make(rstd::cppstd::as_str("sampled")),
    };
    owe::resource::ResourcePlan plan { .generation = 3 };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = use,
        .request = request.clone(),
        .access  = owe::resource::ResourceAccess::Read,
    });

    auto                                          allocation = TextureAllocation(1);
    owe::resource_registry::PreparedResourceTable table(3);
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use                 = use,
        .resource            = owe::resource::TextureHandle { .index = 8, .generation = 1 },
        .request             = rstd::move(request),
        .physical            = allocation.clone(),
        .image               = allocation->View(),
        .physical_generation = 2,
        .ready               = owe::resource::ReadyToken { .value = 4 },
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
    auto first_use  = owe::resource::TextureUseHandle { .index = 1, .generation = 3 };
    auto second_use = owe::resource::TextureUseHandle { .index = 2, .generation = 3 };
    auto resource   = owe::resource::TextureHandle { .index = 8, .generation = 1 };
    auto request    = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::RenderTarget,
        .name = rstd::string::String::make(rstd::cppstd::as_str("frame")),
    };
    owe::resource::ResourcePlan plan { .generation = 3 };
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
    owe::resource_registry::PreparedResourceTable table(3);
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
                .owner_generation = 2,
                .image_index      = 1,
                .acquire_serial   = 4,
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
    EXPECT_EQ(contract.before_copy.Size(), 1u);
    EXPECT_EQ(contract.after_copy.Size(), 1u);
    EXPECT_EQ(contract.lease.identity.acquire_serial, 4u);
}
