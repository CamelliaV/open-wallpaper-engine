module;

module wescene.vulkan_render;
import wescene.core;
import wescene.types;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

namespace owe::vulkan
{

namespace
{

ShaderReflectionKey MakeShaderReflectionKey(const SceneShader& shader) {
    return ShaderReflectionKey {
        .shader    = &shader,
        .shader_id = shader.id,
        .code_hash = SceneShaderCodeHash(shader),
    };
}

CachedShaderReflection MakeCachedReflection(std::vector<Uni_ShaderSpv> spvs,
                                            ShaderReflected            reflected) {
    CachedShaderReflection out;
    out.stages.reserve(spvs.size());
    for (auto& spv : spvs) {
        if (! spv) continue;
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

std::size_t ShaderReflectionKeyHash::operator()(const ShaderReflectionKey& key) const {
    std::size_t seed { 0 };
    utils::hash_combine(seed, key.shader);
    utils::hash_combine(seed, key.shader_id);
    utils::hash_combine(seed, key.code_hash);
    return seed;
}

const CachedShaderReflection* ShaderReflectionCache::Query(const SceneShader& shader) {
    auto key = MakeShaderReflectionKey(shader);
    if (auto it = m_entries.find(key); it != m_entries.end()) {
        return &it->second;
    }

    std::vector<Uni_ShaderSpv> spvs;
    ShaderReflected            reflected;
    if (! GenReflect(shader.codes, spvs, reflected)) return nullptr;

    auto [it, inserted] = m_entries.emplace(
        std::move(key), MakeCachedReflection(std::move(spvs), std::move(reflected)));
    (void)inserted;
    return &it->second;
}

void ShaderReflectionCache::Clear() { m_entries.clear(); }

SceneShaderArtifactProvider::SceneShaderArtifactProvider(ShaderReflectionCache& cache,
                                                         const SceneShader&     shader)
    : m_cache(&cache), m_shader(&shader) {}

auto SceneShaderArtifactProvider::Request() const -> resource::ShaderRequest {
    return resource::ShaderRequest {
        .name = rstd::string::String::make(rstd::cppstd::as_str(m_shader->name)),
        .source =
            resource::ShaderDefinitionId {
                .index      = m_shader->id,
                .generation = 1,
            },
        .content_version = static_cast<rstd::u64>(SceneShaderCodeHash(*m_shader)),
    };
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
    if (reflection == nullptr) {
        return rstd::Err(resource::ResourceError {
            .kind    = resource::ResourceErrorKind::BackendFailure,
            .message = rstd::format("compile shader {} failed", m_shader->name),
        });
    }

    resource::ShaderArtifact artifact {
        .source          = request.source,
        .content_version = request.content_version,
    };
    artifact.stages.reserve(reflection->stages.size());
    for (const auto& stage : reflection->stages) {
        auto code = rstd::vec::Vec<rstd::u32>::with_capacity(stage.spirv.size());
        for (auto word : stage.spirv) code.push(rstd::u32(word));
        artifact.stages.push(resource::ShaderArtifactStage {
            .stage       = stage.stage,
            .entry_point = rstd::string::String::make(rstd::cppstd::as_str(stage.entry_point)),
            .code        = rstd::move(code),
        });
    }
    return rstd::Ok(rstd::move(artifact));
}

auto SceneShaderArtifactProvider::Reflection() -> const CachedShaderReflection* {
    return m_cache->Query(*m_shader);
}

std::vector<Uni_ShaderSpv> CloneShaderSpvs(const CachedShaderReflection& cached) {
    std::vector<Uni_ShaderSpv> out;
    out.reserve(cached.stages.size());
    for (const auto& stage : cached.stages) {
        auto spv         = std::make_unique<ShaderSpv>();
        spv->entry_point = stage.entry_point;
        spv->stage       = stage.stage;
        spv->spirv       = stage.spirv;
        out.push_back(std::move(spv));
    }
    return out;
}

} // namespace owe::vulkan
