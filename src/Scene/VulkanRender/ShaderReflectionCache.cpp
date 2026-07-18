module;

module wescene.vulkan_render;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

using namespace rstd::prelude;

namespace owe::vulkan
{

namespace
{

ShaderReflectionKey MakeShaderReflectionKey(const SceneShader& shader) {
    return ShaderReflectionKey {
        .shader_id = shader.id,
        .code_hash = SceneShaderCodeHash(shader),
    };
}

CachedShaderReflection MakeCachedReflection(std::vector<Uni_ShaderSpv> spvs,
                                            ShaderReflected            reflected) {
    CachedShaderReflection out;
    out.stages.reserve(spvs.size());
    for (auto& spv : spvs) {
        out.stages.push_back(CachedShaderStage {
            .entry_point = spv->entry_point,
            .stage       = spv->stage,
            .spirv       = std::move(spv->spirv),
        });
    }
    out.reflected = std::move(reflected);
    return out;
}

} // namespace

auto ShaderReflectionCache::Query(const SceneShader& shader)
    -> rstd::Option<rstd::ref<CachedShaderReflection>> {
    auto key    = MakeShaderReflectionKey(shader);
    auto cached = m_entries.get(key);
    if (cached.is_some()) return cached;

    std::vector<Uni_ShaderSpv> spvs;
    ShaderReflected            reflected;
    if (! GenReflect(shader.codes, spvs, reflected)) return rstd::None();

    (void)m_entries.insert(key, MakeCachedReflection(std::move(spvs), std::move(reflected)));
    return m_entries.get(key);
}

void ShaderReflectionCache::Clear() { m_entries.clear(); }

SceneShaderArtifactProvider::SceneShaderArtifactProvider(ShaderReflectionCache& cache,
                                                         const SceneShader&     shader)
    : m_cache(rstd::mut_ref<ShaderReflectionCache>::from_raw_parts(rstd::addressof(cache))),
      m_shader(rstd::ref<SceneShader>::from_raw_parts(rstd::addressof(shader))) {}

auto MakeSceneShaderRequest(const SceneShader& shader) -> resource::ShaderRequest {
    return resource::ShaderRequest {
        .name = rstd::string::String::make(rstd::cppstd::as_str(shader.name)),
        .source =
            resource::ShaderDefinitionId {
                .index      = shader.id,
                .generation = 1,
            },
        .content_version = static_cast<rstd::u64>(SceneShaderCodeHash(shader)),
    };
}

auto SceneShaderArtifactProvider::Request() const -> resource::ShaderRequest {
    return MakeSceneShaderRequest(*m_shader);
}

auto SceneShaderArtifactProvider::LoadShader(const resource::ShaderRequest& request)
    -> rstd::Result<resource::ShaderArtifact, resource::ResourceError> {
    if (request.source.index != m_shader->id ||
        request.content_version != SceneShaderCodeHash(*m_shader)) {
        return rstd::Err(resource::ResourceError {
            .kind    = resource::ResourceErrorKind::MissingDefinition,
            .message = rstd::format("shader request does not match {}", m_shader->name),
        });
    }
    auto reflection = m_cache->Query(*m_shader);
    if (reflection.is_none()) {
        return rstd::Err(resource::ResourceError {
            .kind    = resource::ResourceErrorKind::BackendFailure,
            .message = rstd::format("compile shader {} failed", m_shader->name),
        });
    }

    resource::ShaderArtifact artifact {
        .source          = request.source,
        .content_version = request.content_version,
    };
    artifact.stages.reserve((**reflection).stages.size());
    for (const auto& stage : (**reflection).stages) {
        auto code = rstd::vec::Vec<rstd::u32>::with_capacity(stage.spirv.size());
        for (auto word : stage.spirv) code.push(rstd::u32(word));
        artifact.stages.push(resource::ShaderArtifactStage {
            .stage       = stage.stage,
            .entry_point = rstd::string::String::make(rstd::cppstd::as_str(stage.entry_point)),
            .code        = rstd::move(code),
        });
    }
    artifact.uniform_blocks.reserve((**reflection).reflected.blocks.size());
    for (const auto& block : (**reflection).reflected.blocks) {
        auto members = rstd::vec::Vec<resource::ShaderArtifactUniformMember>::with_capacity(
            block.member_map.size());
        for (const auto& [name, member] : block.member_map) {
            members.push(resource::ShaderArtifactUniformMember {
                .name   = rstd::string::String::make(rstd::cppstd::as_str(name)),
                .offset = member.offset,
                .size   = member.size,
                .count  = member.num,
            });
        }
        artifact.uniform_blocks.push(resource::ShaderArtifactUniformBlock {
            .name    = rstd::string::String::make(rstd::cppstd::as_str(block.name)),
            .size    = block.size,
            .members = rstd::move(members),
        });
    }
    artifact.descriptor_bindings.reserve((**reflection).reflected.binding_map.size());
    for (const auto& [name, binding] : (**reflection).reflected.binding_map) {
        artifact.descriptor_bindings.push(resource::ShaderArtifactDescriptorBinding {
            .name             = rstd::string::String::make(rstd::cppstd::as_str(name)),
            .binding          = binding.binding,
            .descriptor_type  = static_cast<rstd::u32>(binding.descriptorType),
            .descriptor_count = binding.descriptorCount,
            .stage_flags      = binding.stageFlags,
        });
    }
    artifact.vertex_inputs.reserve((**reflection).reflected.input_location_map.size());
    for (const auto& [name, input] : (**reflection).reflected.input_location_map) {
        artifact.vertex_inputs.push(resource::ShaderArtifactVertexInput {
            .name     = rstd::string::String::make(rstd::cppstd::as_str(name)),
            .location = input.location,
            .format   = static_cast<rstd::u32>(input.format),
        });
    }
    return rstd::Ok(rstd::move(artifact));
}

std::vector<Uni_ShaderSpv> ShaderSpvsFromArtifact(const resource::ShaderArtifact& artifact) {
    std::vector<Uni_ShaderSpv> out;
    out.reserve(artifact.stages.len());
    for (const auto& stage : artifact.stages) {
        auto spv         = Box<ShaderSpv>::make();
        spv->entry_point = rstd::cppstd::to_string(stage.entry_point.as_str());
        spv->stage       = stage.stage;
        spv->spirv.reserve(stage.code.len());
        for (auto word : stage.code) spv->spirv.push_back(word);
        out.push_back(std::move(spv));
    }
    return out;
}

auto ShaderReflectionFromArtifact(const resource::ShaderArtifact& artifact) -> ShaderReflected {
    ShaderReflected reflected;
    reflected.blocks.reserve(artifact.uniform_blocks.len());
    for (rstd::usize index = 0; index < artifact.uniform_blocks.len(); ++index) {
        const auto&            block = artifact.uniform_blocks[index];
        ShaderReflected::Block prepared {
            .index = static_cast<int>(index),
            .size  = static_cast<unsigned>(block.size),
            .name  = rstd::cppstd::to_string(block.name.as_str()),
        };
        for (const auto& member : block.members) {
            prepared.member_map.emplace(rstd::cppstd::to_string(member.name.as_str()),
                                        ShaderReflected::BlockedUniform {
                                            .block_index = static_cast<int>(index),
                                            .offset      = member.offset,
                                            .size        = member.size,
                                            .num         = member.count,
                                        });
        }
        reflected.blocks.push_back(std::move(prepared));
    }
    for (const auto& binding : artifact.descriptor_bindings) {
        reflected.binding_map.emplace(
            rstd::cppstd::to_string(binding.name.as_str()),
            VkDescriptorSetLayoutBinding {
                .binding            = binding.binding,
                .descriptorType     = static_cast<VkDescriptorType>(binding.descriptor_type),
                .descriptorCount    = binding.descriptor_count,
                .stageFlags         = binding.stage_flags,
                .pImmutableSamplers = nullptr,
            });
    }
    for (const auto& input : artifact.vertex_inputs) {
        reflected.input_location_map.emplace(rstd::cppstd::to_string(input.name.as_str()),
                                             ShaderReflected::Input {
                                                 .location = input.location,
                                                 .format   = static_cast<VkFormat>(input.format),
                                             });
    }
    return reflected;
}

} // namespace owe::vulkan
