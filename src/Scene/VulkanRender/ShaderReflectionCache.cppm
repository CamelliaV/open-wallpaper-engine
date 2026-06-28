module;

export module wescene.vulkan_render:shader_reflection_cache;
import wescene.core;
import wescene.types;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

export namespace owe::vulkan
{

struct ShaderReflectionKey {
    const SceneShader* shader { nullptr };
    uint32_t           shader_id { 0 };
    std::size_t        code_hash { 0 };

    bool operator==(const ShaderReflectionKey&) const = default;
};

struct ShaderReflectionKeyHash {
    std::size_t operator()(const ShaderReflectionKey&) const;
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
    const CachedShaderReflection* Query(const SceneShader&);
    void                          Clear();

private:
    std::unordered_map<ShaderReflectionKey, CachedShaderReflection, ShaderReflectionKeyHash>
        m_entries;
};

std::vector<Uni_ShaderSpv> CloneShaderSpvs(const CachedShaderReflection&);

} // namespace owe::vulkan
