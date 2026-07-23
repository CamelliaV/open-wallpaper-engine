#include <vulkan/vulkan_core.h>

#include <gtest/gtest.h>

import rstd;
import rstd.cppstd;
import wescene.scene;
import wescene.types;
import wescene.vulkan;
import wescene.vulkan_render;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace uniform_test
{

struct StaticSourceState {
    float     value { 0.0f };
    rstd::u64 version { 1 };
};

class BufferWriter {
public:
    auto UpdateBuffer(owe::resource::BufferUseHandle use, rstd::slice<rstd::u8> content,
                      rstd::u64 content_version)
        -> rstd::Result<rstd::empty, owe::resource::ResourceError> {
        last_use     = use;
        last_version = content_version;
        ++update_count;
        bytes.clear();
        bytes.reserve(content.len().to_primitive());
        for (rstd::usize index; index < content.len(); ++index) {
            bytes.push_back(content[index]);
        }
        return rstd::Ok(rstd::empty {});
    }

    owe::resource::BufferUseHandle last_use;
    rstd::u64                      last_version { 0 };
    rstd::u64                      update_count { 0 };
    std::vector<rstd::u8>          bytes;
};

class StaticSource {
public:
    StaticSource(std::string name, float value)
        : StaticSource(std::move(name),
                       std::make_shared<StaticSourceState>(StaticSourceState { .value = value })) {}
    StaticSource(std::string name, std::shared_ptr<StaticSourceState> state)
        : m_name(std::move(name)), m_state(std::move(state)) {}
    StaticSource(std::string name, float value, rstd::sync::Arc<owe::AudioResponseDemand> demand)
        : StaticSource(std::move(name), value) {
        m_demand = rstd::Some(rstd::move(demand));
    }

    auto Describe(rstd::mut_ref<rstd::dyn<owe::UniformBindingSink>> sink) const
        -> rstd::Result<rstd::empty, owe::UniformError> {
        auto result = sink->Bind(m_output,
                                 rstd::cppstd::as_str(m_name).unwrap(),
                                 owe::UniformValueShape::FloatRange(rstd::u32(1), rstd::u32(4)));
        if (result.is_err()) return rstd::Err(std::move(result).unwrap_err_unchecked());
        return rstd::Ok(rstd::empty {});
    }
    auto Version(rstd::ref<rstd::dyn<owe::UniformUpdateContext>>) const -> rstd::u64 {
        return m_state->version;
    }
    auto Evaluate(rstd::ref<rstd::dyn<owe::UniformUpdateContext>>,
                  rstd::mut_ref<rstd::dyn<owe::UniformValueSink>> sink) const
        -> rstd::Result<rstd::empty, owe::UniformError> {
        if (! sink->Wants(m_output)) return rstd::Ok(rstd::empty {});
        auto value = owe::UniformValue(m_state->value);
        return sink->Write(m_output, value.View());
    }
    auto AcquireBindingLease() const
        -> rstd::Option<rstd::boxed::Box<rstd::dyn<owe::UniformBindingLease>>> {
        if (m_demand.is_none()) return rstd::None();
        return rstd::Some((*m_demand)->Acquire());
    }

private:
    std::string                                             m_name;
    std::shared_ptr<StaticSourceState>                      m_state;
    rstd::Option<rstd::sync::Arc<owe::AudioResponseDemand>> m_demand;
    owe::UniformOutputId                                    m_output { .value = rstd::u32() };
};

class TextureMetadataSource {
public:
    auto Describe(rstd::mut_ref<rstd::dyn<owe::UniformBindingSink>> sink) const
        -> rstd::Result<rstd::empty, owe::UniformError> {
        auto result =
            sink->Bind(m_output, "texture_extent"_str, owe::UniformValueShape::Float(rstd::u32(4)));
        if (result.is_err()) return rstd::Err(rstd::move(result).unwrap_err_unchecked());
        return rstd::Ok(rstd::empty {});
    }
    auto Version(rstd::ref<rstd::dyn<owe::UniformUpdateContext>> context) const -> rstd::u64 {
        return context->Frame()->revision;
    }
    auto Evaluate(rstd::ref<rstd::dyn<owe::UniformUpdateContext>> context,
                  rstd::mut_ref<rstd::dyn<owe::UniformValueSink>> sink) const
        -> rstd::Result<rstd::empty, owe::UniformError> {
        if (! sink->Wants(m_output)) return rstd::Ok(rstd::empty {});
        auto texture = context->Resources()->Texture(rstd::usize());
        if (texture.is_none() || ! texture->has_extent) return rstd::Ok(rstd::empty {});
        auto value = owe::UniformValue(rstd::array<float, 4> {
            texture->source_extent[rstd::usize()],
            texture->source_extent[rstd::usize(1)],
            texture->sample_extent[rstd::usize()],
            texture->sample_extent[rstd::usize(1)],
        });
        return sink->Write(m_output, value.View());
    }
    auto AcquireBindingLease() const
        -> rstd::Option<rstd::boxed::Box<rstd::dyn<owe::UniformBindingLease>>> {
        return rstd::None();
    }

private:
    owe::UniformOutputId m_output { .value = rstd::u32() };
};

} // namespace uniform_test

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

std::shared_ptr<owe::SceneMesh> MakeUniformMesh(std::shared_ptr<owe::SceneShader> shader) {
    auto               mesh = std::make_shared<owe::SceneMesh>();
    owe::SceneMaterial material;
    material.customShader.shader = std::move(shader);
    mesh->AddMaterial(std::move(material));
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = 0;
    mesh->Submeshes().push_back(std::move(submesh));
    return mesh;
}

} // namespace

TEST(UniformBufferLayout, PreservesReflectedSlots) {
    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("g_Time"_str),
        .offset = rstd::u32(),
        .size   = rstd::usize(4),
    });
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("time_alias"_str),
        .offset = rstd::u32(16),
        .size   = rstd::usize(4),
    });
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("unmapped"_str),
        .offset = rstd::u32(32),
        .size   = rstd::usize(4),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(48),
        .members = rstd::move(members),
    };
    auto result = owe::vulkan::CompileUniformBufferLayout(block);

    ASSERT_TRUE(result.is_ok());
    auto layout = std::move(result).unwrap_unchecked();
    ASSERT_EQ(layout.slots.len(), rstd::usize(3));
    EXPECT_EQ(rstd::cppstd::as_string_view(layout.slots[rstd::usize()].name.as_str()), "g_Time");
    EXPECT_EQ(rstd::cppstd::as_string_view(layout.slots[rstd::usize(1)].name.as_str()),
              "time_alias");
    EXPECT_EQ(rstd::cppstd::as_string_view(layout.slots[rstd::usize(2)].name.as_str()), "unmapped");
}

TEST(UniformBufferLayout, RejectsMemberOutsideBlock) {
    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("outside"_str),
        .offset = rstd::u32(8),
        .size   = rstd::usize(8),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(12),
        .members = rstd::move(members),
    };

    EXPECT_TRUE(owe::vulkan::CompileUniformBufferLayout(block).is_err());
}

TEST(UniformBufferBinding, UpdatesGenericSceneThroughBufferWriterTrait) {
    owe::Scene scene;
    auto       camera_node = Arc<owe::SceneNode>::make();
    auto       camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto registrar   = rstd::dyn<owe::UniformSourceRegistrar>::from_ref(scene);
    auto attachments = rstd::dyn<owe::UniformAttachmentWriter>::from_ref(scene);
    auto source      = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::StaticSource("scene_time", 2.5f)));
    ASSERT_TRUE(attachments->AttachGlobal(source));

    auto shader = std::make_shared<owe::SceneShader>();
    auto node   = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID()  = rstd::i32(1);
    node->AddMesh(MakeUniformMesh(std::move(shader)));
    scene.RootMut()->AppendChild(node.clone());
    scene.RebuildResourceIndex();
    auto node_id = scene.ResourceIndex().nodeId(*node.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, rstd::u32());
    ASSERT_TRUE(draw_id.is_some());

    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("scene_time"_str),
        .offset = rstd::u32(),
        .size   = rstd::usize(16),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(16),
        .members = rstd::move(members),
    };
    const auto buffer =
        owe::resource::BufferUseHandle { .index = rstd::u64(3), .generation = rstd::u64(1) };
    owe::vulkan::SceneUniformBindingPrepareContext prepare_impl(scene);
    auto prepare = rstd::dyn<owe::vulkan::UniformBindingPrepareContext>::from_ref(prepare_impl);
    auto binding = owe::vulkan::MakeUniformBufferBinding(prepare.as_ref(), *draw_id, buffer, block);
    ASSERT_TRUE(binding.is_ok());

    uniform_test::BufferWriter writer;
    auto writer_trait   = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    auto texture_frames = rstd::dyn<owe::SceneTextureAnimationView>::from_ref(scene);
    owe::vulkan::ProgramUniformFrameContext frame_impl(
        scene.Runtime().Frame(), { 1920.0f, 1080.0f }, texture_frames.as_ref());
    auto frame   = rstd::dyn<owe::vulkan::UniformBufferFrameContext>::from_ref(frame_impl);
    auto updated = binding.unwrap_unchecked()->Update(frame.as_ref(), writer_trait.as_mut_ref());

    ASSERT_TRUE(updated.is_ok());
    EXPECT_EQ(writer.last_use, buffer);
    EXPECT_GT(writer.last_version, rstd::u64());
    ASSERT_EQ(writer.bytes.size(), 16u);
    std::array<float, 4> values {};
    std::memcpy(values.data(), writer.bytes.data(), writer.bytes.size());
    EXPECT_FLOAT_EQ(values[0], 2.5f);
    EXPECT_FLOAT_EQ(values[1], 0.0f);
    EXPECT_FLOAT_EQ(values[2], 0.0f);
    EXPECT_FLOAT_EQ(values[3], 0.0f);
}

TEST(UniformBufferBinding, HoldsDemandOnlyForAReflectedLiveOutput) {
    owe::Scene scene;
    auto       demand = rstd::sync::Arc<owe::AudioResponseDemand>::make();
    bool       active = false;
    demand->SetCallback([&active](bool next) {
        active = next;
    });
    auto registrar   = rstd::dyn<owe::UniformSourceRegistrar>::from_ref(scene);
    auto attachments = rstd::dyn<owe::UniformAttachmentWriter>::from_ref(scene);
    auto source      = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::StaticSource("audio_signal", 0.0f, demand.clone())));
    ASSERT_TRUE(attachments->AttachGlobal(source));

    auto node = rstd::sync::Arc<owe::SceneNode>::make();
    node->AddMesh(MakeUniformMesh(std::make_shared<owe::SceneShader>()));
    scene.RootMut()->AppendChild(node.clone());
    scene.RebuildResourceIndex();
    auto node_id = scene.ResourceIndex().nodeId(*node.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, rstd::u32());
    ASSERT_TRUE(draw_id.is_some());
    owe::vulkan::SceneUniformBindingPrepareContext prepare_impl(scene);
    auto prepare = rstd::dyn<owe::vulkan::UniformBindingPrepareContext>::from_ref(prepare_impl);

    auto make_block = [](std::string_view name) {
        auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
        members.push(owe::resource::ShaderArtifactUniformMember {
            .name   = rstd::string::String::make(rstd::cppstd::as_str(name).unwrap()),
            .offset = rstd::u32(),
            .size   = rstd::usize(4),
        });
        return owe::resource::ShaderArtifactUniformBlock {
            .name    = rstd::string::String::make("Globals"_str),
            .size    = rstd::usize(4),
            .members = rstd::move(members),
        };
    };

    {
        auto unbound = owe::vulkan::MakeUniformBufferBinding(
            prepare.as_ref(),
            *draw_id,
            owe::resource::BufferUseHandle { .index = rstd::u64(1), .generation = rstd::u64(1) },
            make_block("unrelated"));
        ASSERT_TRUE(unbound.is_ok());
        EXPECT_FALSE(active);
    }
    {
        auto bound = owe::vulkan::MakeUniformBufferBinding(
            prepare.as_ref(),
            *draw_id,
            owe::resource::BufferUseHandle { .index = rstd::u64(2), .generation = rstd::u64(1) },
            make_block("audio_signal"));
        ASSERT_TRUE(bound.is_ok());
        EXPECT_TRUE(active);
    }
    EXPECT_FALSE(active);
}

TEST(UniformBufferBinding, ProvidesPreparedTextureMetadataToGenericSource) {
    owe::Scene scene;
    auto       registrar   = rstd::dyn<owe::UniformSourceRegistrar>::from_ref(scene);
    auto       attachments = rstd::dyn<owe::UniformAttachmentWriter>::from_ref(scene);
    auto       source = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::TextureMetadataSource {}));
    ASSERT_TRUE(attachments->AttachGlobal(source));

    auto shader = std::make_shared<owe::SceneShader>();
    auto node   = rstd::sync::Arc<owe::SceneNode>::make();
    node->AddMesh(MakeUniformMesh(std::move(shader)));
    scene.RootMut()->AppendChild(node.clone());
    scene.RebuildResourceIndex();
    auto node_id = scene.ResourceIndex().nodeId(*node.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, rstd::u32());
    ASSERT_TRUE(draw_id.is_some());

    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("texture_extent"_str),
        .offset = rstd::u32(),
        .size   = rstd::usize(16),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(16),
        .members = rstd::move(members),
    };
    auto textures = rstd::vec::Vec<owe::vulkan::PreparedUniformTextureMetadata>::make();
    textures.push(owe::vulkan::PreparedUniformTextureMetadata {
        .available     = true,
        .source_extent = { 640.0f, 360.0f },
        .sample_extent = { 1024.0f, 512.0f },
        .revision      = rstd::u64(4),
    });
    owe::vulkan::SceneUniformBindingPrepareContext prepare_impl(scene);
    auto prepare = rstd::dyn<owe::vulkan::UniformBindingPrepareContext>::from_ref(prepare_impl);
    auto binding = owe::vulkan::MakeUniformBufferBinding(
        prepare.as_ref(),
        *draw_id,
        owe::resource::BufferUseHandle { .index = rstd::u64(3), .generation = rstd::u64(1) },
        block,
        rstd::move(textures));
    ASSERT_TRUE(binding.is_ok());

    uniform_test::BufferWriter writer;
    auto writer_trait   = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    auto texture_frames = rstd::dyn<owe::SceneTextureAnimationView>::from_ref(scene);
    owe::vulkan::ProgramUniformFrameContext frame_impl(
        scene.Runtime().Frame(), { 1920.0f, 1080.0f }, texture_frames.as_ref());
    auto frame = rstd::dyn<owe::vulkan::UniformBufferFrameContext>::from_ref(frame_impl);
    ASSERT_TRUE(
        binding.unwrap_unchecked()->Update(frame.as_ref(), writer_trait.as_mut_ref()).is_ok());

    ASSERT_EQ(writer.bytes.size(), 16u);
    std::array<float, 4> values {};
    std::memcpy(values.data(), writer.bytes.data(), writer.bytes.size());
    EXPECT_EQ(values, (std::array<float, 4> { 640.0f, 360.0f, 1024.0f, 512.0f }));
}

TEST(UniformBufferBinding, OrdersSourcesAndSkipsUnchangedVersions) {
    owe::Scene scene;
    auto       camera_node = Arc<owe::SceneNode>::make();
    auto       camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto registrar   = rstd::dyn<owe::UniformSourceRegistrar>::from_ref(scene);
    auto attachments = rstd::dyn<owe::UniformAttachmentWriter>::from_ref(scene);
    auto low_state   = std::make_shared<uniform_test::StaticSourceState>(
        uniform_test::StaticSourceState { .value = 3.0f });
    auto high_priority = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::StaticSource("static_value", 7.0f)));
    auto low_priority  = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::StaticSource("static_value", low_state)));
    auto shader        = std::make_shared<owe::SceneShader>();
    auto node          = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID()         = rstd::i32(1);
    node->AddMesh(MakeUniformMesh(std::move(shader)));
    scene.RootMut()->AppendChild(node.clone());
    scene.RebuildResourceIndex();
    auto node_id = scene.ResourceIndex().nodeId(*node.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    ASSERT_TRUE(attachments->AttachNode(*node_id, high_priority, 10));
    ASSERT_TRUE(attachments->AttachNode(*node_id, low_priority, -10));
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, rstd::u32());
    ASSERT_TRUE(draw_id.is_some());

    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("static_value"_str),
        .offset = rstd::u32(),
        .size   = rstd::usize(4),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(4),
        .members = rstd::move(members),
    };
    owe::vulkan::SceneUniformBindingPrepareContext prepare_impl(scene);
    auto prepare = rstd::dyn<owe::vulkan::UniformBindingPrepareContext>::from_ref(prepare_impl);
    auto binding = owe::vulkan::MakeUniformBufferBinding(
        prepare.as_ref(),
        *draw_id,
        owe::resource::BufferUseHandle { .index = rstd::u64(3), .generation = rstd::u64(1) },
        block);
    ASSERT_TRUE(binding.is_ok());

    uniform_test::BufferWriter writer;
    auto writer_trait   = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    auto update         = binding.unwrap_unchecked();
    auto texture_frames = rstd::dyn<owe::SceneTextureAnimationView>::from_ref(scene);
    owe::vulkan::ProgramUniformFrameContext frame_impl(
        scene.Runtime().Frame(), { 1920.0f, 1080.0f }, texture_frames.as_ref());
    auto frame = rstd::dyn<owe::vulkan::UniformBufferFrameContext>::from_ref(frame_impl);
    ASSERT_TRUE(update->Update(frame.as_ref(), writer_trait.as_mut_ref()).is_ok());
    ASSERT_TRUE(update->Update(frame.as_ref(), writer_trait.as_mut_ref()).is_ok());

    EXPECT_EQ(writer.update_count, rstd::u64(1));
    low_state->value = 5.0f;
    ++low_state->version;
    ASSERT_TRUE(update->Update(frame.as_ref(), writer_trait.as_mut_ref()).is_ok());

    EXPECT_EQ(writer.update_count, rstd::u64(2));
    ASSERT_EQ(writer.bytes.size(), 4u);
    float value = 0.0f;
    std::memcpy(&value, writer.bytes.data(), writer.bytes.size());
    EXPECT_FLOAT_EQ(value, 7.0f);
}

TEST(ShaderArtifact, ReconstructsPreparedInterfaceWithoutCacheLookup) {
    owe::resource::ShaderArtifact artifact;
    auto                          code = rstd::vec::Vec<rstd::u32>::make();
    code.push(rstd::u32(1));
    code.push(rstd::u32(2));
    code.push(rstd::u32(3));
    artifact.stages.push(owe::resource::ShaderArtifactStage {
        .stage       = owe::ShaderType::VERTEX,
        .entry_point = rstd::string::String::make("main"_str),
        .code        = rstd::move(code),
    });
    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("g_Time"_str),
        .offset = rstd::u32(16),
        .size   = rstd::usize(4),
    });
    artifact.uniform_blocks.push(owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(32),
        .members = rstd::move(members),
    });
    artifact.descriptor_bindings.push(owe::resource::ShaderArtifactDescriptorBinding {
        .name    = rstd::string::String::make("g_Texture0"_str),
        .binding = rstd::u32(2),
        .descriptor_type =
            rstd::u32(static_cast<rstd::uint32_t>(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)),
        .descriptor_count = rstd::u32(1),
        .stage_flags      = rstd::u32(static_cast<rstd::uint32_t>(VK_SHADER_STAGE_FRAGMENT_BIT)),
    });
    artifact.vertex_inputs.push(owe::resource::ShaderArtifactVertexInput {
        .name     = rstd::string::String::make("a_Position"_str),
        .location = rstd::u32(3),
        .format   = rstd::u32(static_cast<rstd::uint32_t>(VK_FORMAT_R32G32_SFLOAT)),
    });

    auto stages     = owe::vulkan::ShaderSpvsFromArtifact(artifact);
    auto reflection = owe::vulkan::ShaderReflectionFromArtifact(artifact);
    ASSERT_EQ(stages.size(), 1u);
    EXPECT_EQ(stages[0]->entry_point, "main");
    EXPECT_EQ(stages[0]->spirv.size(), 3u);
    ASSERT_EQ(reflection.blocks.size(), 1u);
    EXPECT_EQ(reflection.blocks[0].member_map.at("g_Time").offset, 16u);
    EXPECT_EQ(reflection.binding_map.at("g_Texture0").binding, 2u);
    EXPECT_EQ(reflection.input_location_map.at("a_Position").format, VK_FORMAT_R32G32_SFLOAT);
}

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
        .name    = rstd::string::String::make("texture-slot"_str),
        .request = rstd::Some(std::move(request)),
    };

    EXPECT_FALSE(binding.empty());
    ASSERT_TRUE(binding.request.is_some());
    EXPECT_EQ(binding.request->kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(rstd::cppstd::as_string_view(binding.request->name.as_str()), "texture-slot");

    owe::vulkan::TextureBindingRequest empty;
    EXPECT_TRUE(empty.empty());
}

TEST(PreparedPassResources, ResolvesOnlyDeclaredUses) {
    owe::resource_registry::PreparedResourceTable table(rstd::u64(6));
    auto                                          allowed =
        owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(6) };
    auto hidden =
        owe::resource::TextureUseHandle { .index = rstd::u64(2), .generation = rstd::u64(6) };
    auto insert = [&](owe::resource::TextureUseHandle use, std::string_view name) {
        owe::vulkan::ImageSlots slots;
        slots.slots.resize(1);
        auto allocation = rstd::sync::Arc<owe::vulkan::TextureAllocation>::make(rstd::move(slots));
        return table.Insert(owe::resource_registry::PreparedTexture {
            .use = use,
            .resource =
                owe::resource::TextureHandle { .index = use.index, .generation = rstd::u64(1) },
            .request =
                owe::resource::TextureRequest {
                    .name = rstd::string::String::make(rstd::cppstd::as_str(name).unwrap()),
                },
            .physical = allocation.clone(),
            .image    = allocation->View(),
        });
    };
    ASSERT_TRUE(insert(allowed, "allowed"));
    ASSERT_TRUE(insert(hidden, "hidden"));

    owe::vulkan::PassResourceUses uses;
    uses.textures.push(owe::resource::TextureUseHandle(allowed));
    owe::vulkan::PreparedPassResources view(table, uses);

    EXPECT_TRUE(view.Resolve(allowed).is_some());
    EXPECT_TRUE(view.Resolve(hidden).is_none());
}

TEST(TextureRequest, ResolvesImportedTextureNameFromSnapshotCatalog) {
    owe::Scene scene;
    scene.RegisterTexture(String::make("texture-slot"_str),
                          owe::SceneTexture { .url = "textures/main.png" });

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto desc_id  = snapshot.textureDescId("texture-slot"_str);
    ASSERT_TRUE(desc_id.is_some());

    auto request = owe::vulkan::MakeImportedTextureRequest("texture-slot", desc_id);
    EXPECT_EQ(request.source->index, desc_id->index);

    auto resolved = owe::vulkan::ResolveImportedTextureName(snapshot, request);
    ASSERT_TRUE(resolved.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(resolved->as_str()), "textures/main.png");

    auto lookup_request = owe::vulkan::MakeImportedTextureRequest("texture-slot");
    resolved            = owe::vulkan::ResolveImportedTextureName(snapshot, lookup_request);
    ASSERT_TRUE(resolved.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(resolved->as_str()), "textures/main.png");

    auto missing_request = owe::vulkan::MakeImportedTextureRequest("missing");
    EXPECT_TRUE(owe::vulkan::ResolveImportedTextureName(snapshot, missing_request).is_none());
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
    EXPECT_EQ(request.definition->width, rstd::i32(256));
    EXPECT_EQ(request.definition->height, rstd::i32(128));
    EXPECT_EQ(request.definition->usage, owe::resource::TextureUsage::Color);
    EXPECT_EQ(request.definition->format, owe::TextureFormat::RGBA8);
    EXPECT_EQ(request.definition->mip_levels, rstd::u32(3));
    EXPECT_EQ(request.lifetime, owe::resource::TextureLifetimeClass::Retained);

    rt.allowReuse = true;
    EXPECT_EQ(owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt).lifetime,
              owe::resource::TextureLifetimeClass::FrameLocal);

    auto no_mip = owe::vulkan::MakeRenderTargetNoMipTextureRequest("_rt_default", rt);
    ASSERT_TRUE(no_mip.definition.is_some());
    EXPECT_EQ(no_mip.definition->mip_levels, rstd::u32(1));
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
    EXPECT_EQ(target->definition->width, rstd::i32(512));
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
    EXPECT_EQ(msaa.definition->samples, rstd::u32(4));
    EXPECT_EQ(msaa.lifetime, owe::resource::TextureLifetimeClass::Dedicated);

    auto depth = owe::vulkan::MakeDepthTextureRequest("_rt_default::depth", rt);
    EXPECT_EQ(depth.kind, owe::vulkan::TextureRequestKind::DepthAttachment);
    ASSERT_TRUE(depth.definition.is_some());
    EXPECT_EQ(depth.definition->usage, owe::resource::TextureUsage::Depth);
    EXPECT_EQ(depth.definition->format, owe::TextureFormat::D32F);
    EXPECT_EQ(depth.definition->mip_levels, rstd::u32(1));
    EXPECT_EQ(depth.definition->samples, rstd::u32(4));
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
                    .name    = rstd::string::String::make("textures/main.png"_str),
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
    EXPECT_EQ(custom_diag[0].slot, rstd::u32());
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
        auto spv         = rstd::boxed::Box<owe::vulkan::ShaderSpv>::make();
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
    blend_constants_request.blend_constants[rstd::usize()] = 1.0f;
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
    EXPECT_EQ(first.observed_count, rstd::u64(1));

    auto second = diagnostics.Record(key_b);
    EXPECT_TRUE(second.hit);
    EXPECT_EQ(second.observed_count, rstd::u64(2));

    auto third = diagnostics.Record(key_c);
    EXPECT_FALSE(third.hit);
    EXPECT_EQ(third.observed_count, rstd::u64(1));

    auto collision_first = diagnostics.Record(colliding_a);
    EXPECT_FALSE(collision_first.hit);
    EXPECT_EQ(collision_first.observed_count, rstd::u64(1));

    auto collision_second = diagnostics.Record(colliding_b);
    EXPECT_FALSE(collision_second.hit);
    EXPECT_EQ(collision_second.observed_count, rstd::u64(1));

    std::unordered_map<owe::vulkan::PipelineCacheKey,
                       int,
                       owe::vulkan::CanonicalCacheKeyStdHash,
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
                       owe::vulkan::CanonicalCacheKeyStdHash,
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
    EXPECT_EQ(first.observed_count, rstd::u64(1));

    auto second = diagnostics.Record(key_b);
    EXPECT_TRUE(second.hit);
    EXPECT_EQ(second.observed_count, rstd::u64(2));

    auto third = diagnostics.Record(key_resized);
    EXPECT_FALSE(third.hit);
    EXPECT_EQ(third.observed_count, rstd::u64(1));

    auto collision_first = diagnostics.Record(colliding_a);
    EXPECT_FALSE(collision_first.hit);
    EXPECT_EQ(collision_first.observed_count, rstd::u64(1));

    auto collision_second = diagnostics.Record(colliding_b);
    EXPECT_FALSE(collision_second.hit);
    EXPECT_EQ(collision_second.observed_count, rstd::u64(1));

    std::unordered_map<owe::vulkan::FramebufferCacheKey,
                       int,
                       owe::vulkan::CanonicalCacheKeyStdHash,
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
    image_a.generation   = rstd::u64(10);
    auto image_b         = image_a;
    image_b.generation   = rstd::u64(11);

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
