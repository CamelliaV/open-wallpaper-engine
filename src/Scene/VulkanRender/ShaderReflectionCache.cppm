module;

export module wescene.vulkan_render:shader_reflection_cache;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.vulkan;
import wescene.scene;

export namespace owe::vulkan
{

struct ShaderReflectionKey {
    rstd::u32   shader_id { 0 };
    rstd::usize code_hash { 0 };

    bool operator==(const ShaderReflectionKey&) const = default;
};

struct ShaderReflectionKeyHash {
    rstd::hash::RandomState state;

    auto operator()(const ShaderReflectionKey& key) const noexcept -> rstd::u64 {
        auto seed = state(key.shader_id);
        seed ^= state(key.code_hash)
                    .wrapping_add(rstd::u64(0x9e3779b97f4a7c15ULL))
                    .wrapping_add(seed.wrapping_shl(rstd::u64(6)))
                    .wrapping_add(seed >> rstd::u64(2));
        return seed;
    }
};

struct CachedShaderStage {
    std::string               entry_point;
    ShaderType                stage;
    std::vector<unsigned int> spirv;
};

struct CachedShaderReflection {
    std::vector<CachedShaderStage> stages;
    ShaderReflected                reflected;
};

class ShaderReflectionCache : NoCopy, NoMove {
public:
    auto Query(const SceneShader&) -> rstd::Option<rstd::ref<CachedShaderReflection>>;
    void Clear();

private:
    rstd::collections::HashMap<ShaderReflectionKey, CachedShaderReflection, ShaderReflectionKeyHash>
        m_entries;
};

auto MakeSceneShaderRequest(const SceneShader&) -> resource::ShaderRequest;

class SceneShaderArtifactProvider {
public:
    SceneShaderArtifactProvider(ShaderReflectionCache&, const SceneShader&);

    auto Request() const -> resource::ShaderRequest;
    auto LoadShader(const resource::ShaderRequest&)
        -> rstd::Result<resource::ShaderArtifact, resource::ResourceError>;

private:
    rstd::mut_ref<ShaderReflectionCache> m_cache;
    rstd::ref<SceneShader>               m_shader;
};

std::vector<Uni_ShaderSpv> ShaderSpvsFromArtifact(const resource::ShaderArtifact&);
auto ShaderReflectionFromArtifact(const resource::ShaderArtifact&) -> ShaderReflected;

} // namespace owe::vulkan
