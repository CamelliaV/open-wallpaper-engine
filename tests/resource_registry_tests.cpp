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

    owe::vulkan::ImageSlotsRef   image;
    owe::vulkan::ImageParameters first_image;
    first_image.generation = 7;
    image.slots.push_back(first_image);
    auto first = registry.Publish(handle, image, owe::resource::ReadyToken { .value = 3 });
    ASSERT_TRUE(first.is_some());
    EXPECT_EQ(*first, 1u);

    auto same = registry.Publish(handle, image, owe::resource::ReadyToken { .value = 4 });
    ASSERT_TRUE(same.is_some());
    EXPECT_EQ(*same, 1u);

    image.slots[0].generation = 8;
    auto replaced = registry.Publish(handle, image, owe::resource::ReadyToken { .value = 5 });
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
    EXPECT_EQ((**entry).physical_generation, 2u);

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

TEST(PreparedResourceTable, ResolvesTypedUsesWithoutRegistryLookup) {
    owe::resource_registry::PreparedResourceTable table(9);
    auto use = owe::resource::TextureUseHandle { .index = 4, .generation = 9 };
    owe::vulkan::ImageSlotsRef   image;
    owe::vulkan::ImageParameters prepared_image;
    prepared_image.generation = 11;
    image.slots.push_back(prepared_image);

    EXPECT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use      = use,
        .resource = owe::resource::TextureHandle { .index = 2, .generation = 1 },
        .request =
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make(rstd::cppstd::as_str("prepared")),
            },
        .image               = image,
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

    owe::resource_registry::DescriptorArena   arena;
    owe::resource_registry::SubmissionTracker submissions;
    auto                                      completion = submissions.Begin(table, arena);
    EXPECT_TRUE(completion.Valid());
    EXPECT_EQ(submissions.InFlight(), 1u);
    EXPECT_EQ(arena.InFlightSubmissions(), 1u);
    EXPECT_EQ(arena.InFlightBindings(), 1u);
    auto completed = submissions.Complete(completion, arena);
    ASSERT_TRUE(completed.is_some());
    EXPECT_EQ(completed->program_generation, 0u);
    EXPECT_EQ(submissions.InFlight(), 0u);
    EXPECT_EQ(arena.InFlightSubmissions(), 0u);
}

TEST(UploadScheduler, TracksTimelineReadiness) {
    owe::resource_registry::UploadScheduler uploads;
    auto                                    first  = uploads.Reserve();
    auto                                    second = uploads.Reserve();
    EXPECT_LT(first.value, second.value);
    EXPECT_TRUE(uploads.MarkSubmitted(first));
    EXPECT_TRUE(uploads.MarkSubmitted(second));
    ASSERT_TRUE(uploads.Pending().is_some());
    EXPECT_EQ(uploads.Pending()->value, second.value);
    EXPECT_EQ(uploads.InFlight(), 2u);

    uploads.CompleteThrough(first.value);
    EXPECT_EQ(uploads.InFlight(), 1u);
    EXPECT_EQ(uploads.Pending()->value, second.value);
    uploads.CompleteThrough(second.value);
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

    owe::vulkan::ImageSlotsRef image;
    image.slots.push_back(owe::vulkan::ImageParameters {});
    owe::resource_registry::PreparedResourceTable table(3);
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use                 = use,
        .resource            = owe::resource::TextureHandle { .index = 8, .generation = 1 },
        .request             = rstd::move(request),
        .image               = image,
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
    owe::vulkan::ImageParameters source;
    source.handle = reinterpret_cast<VkImage>(1);
    source.extent = VkExtent3D { 64, 32, 1 };
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
    auto prepared = bridge.Prepare(owe::vulkan::DeviceCapabilities {}, source, lease, 3);
    ASSERT_TRUE(prepared.is_ok());
    auto contract = rstd::move(prepared).unwrap_unchecked();
    EXPECT_EQ(contract.before_copy.Size(), 2u);
    EXPECT_EQ(contract.after_copy.Size(), 2u);
    EXPECT_EQ(contract.lease.identity.acquire_serial, 4u);
}
