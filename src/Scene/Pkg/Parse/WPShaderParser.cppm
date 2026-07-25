module;

export module wescene.pkg.parse:wp_shader_parser;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;
import wescene.shader_compile;
import wescene.scene;
import wescene.fs;

export import :wp_uniform;

using namespace rstd::prelude;

export namespace owe

{
using Combos = Map<std::string, std::string>;

// ui material name to gl uniform name
using WPAliasValueDict = Map<std::string, std::string>;

using WPDefaultTexs = std::vector<std::pair<std::int32_t, std::string>>;

// Staged direct-route u_* uniforms (shader annotation's `material` field
// equals the wallpaper-level project.json key). LoadMaterial fills this
// during compile; the caller registers it through Scene after AddMaterial
// establishes the material's stable owner.
struct UserVarRecord {
    String material;      // project.json key (== shader annotation's material)
    String name;          // GLSL identifier (e.g. "u_Brightness")
    Json   default_value; // raw default from annotation; may be null
};

struct WPShaderInfo {
    Combos           combos;
    ShaderValueMap   svs;
    ShaderValueMap   baseConstSvs;
    WPAliasValueDict alias;
    WPDefaultTexs    defTexs;

    // Full annotation metadata. Renderer reads `combos / svs / defTexs /
    // alias` on the hot path; the editor / material UI and the user-property
    // bridge for `u_*` uniforms read the vectors below.
    Vec<wpscene::WPCombo>      combo_defs;
    Vec<wpscene::WPUniformTex> texture_uniforms;
    Vec<wpscene::WPUniformVar> scalar_uniforms;

    // Filled by LoadMaterial for the direct-binding u_* route. The
    // scene-instance-level user-binding route (effect-key → wallpaper-key)
    // is registered separately from `Material::constantshadervalues_user`.
    Vec<UserVarRecord> user_var_staging;
};

struct WPPreprocessorInfo {
    Map<std::string, std::string> input; // name to line
    Map<std::string, std::string> output;

    // `uniform TYPE NAME;` declarations for non-sampler types. Captured
    // per-stage so Finalprocessor can build a cross-stage union and emit
    // a single shared cbuffer (matching what glslang's iomapper used to
    // produce). Without this, DXC's per-stage $Globals cbuffers desync
    // and FS-only uniforms read as zero.
    Map<std::string, std::string> uniforms; // name -> "TYPE"

    Set<unsigned> active_tex_slots;
};

struct WPShaderTexInfo {
    bool                enabled { false };
    std::array<bool, 3> composEnabled { false, false, false };
};

struct WPShaderUnit {
    ShaderType         stage;
    std::string        src;
    WPPreprocessorInfo preprocess_info;
};

class WPShaderCache {
    struct SourceEntry {
        String       source;
        WPShaderInfo annotations;
        usize        bytes {};
    };

    struct CompiledStage {
        ShaderType                                 stage;
        rstd::collections::HashMap<String, String> uniforms;
        Vec<u32>                                   active_tex_slots;
    };

    struct CompileEntry {
        Vec<CompiledStage> stages;
        Vec<Vec<u32>>      codes;
        usize              bytes {};
    };

    static constexpr usize kMaxSourceBytes { 4 * 1024 * 1024 };
    static constexpr usize kMaxCompileBytes { 8 * 1024 * 1024 };
    static constexpr usize kMaxSourceEntries { 64 };
    static constexpr usize kMaxCompileEntries { 32 };

public:
    explicit WPShaderCache(Option<rstd::path::PathBuf> directory = None())
        : m_directory(rstd::move(directory)) {}

    auto directory() const noexcept -> Option<ref<rstd::path::Path>> {
        if (m_directory.is_none()) return None();
        return Some(m_directory->as_path());
    }

    void ReleaseTransientEntries() {
        m_source_entries  = rstd::collections::HashMap<String, SourceEntry>::make();
        m_compile_entries = rstd::collections::HashMap<String, CompileEntry>::make();
        m_source_order    = Vec<String>::make();
        m_compile_order   = Vec<String>::make();
        m_source_bytes    = usize {};
        m_compile_bytes   = usize {};
    }

private:
    bool ReserveSource(usize bytes) {
        if (bytes > kMaxSourceBytes) return false;
        while (! m_source_order.is_empty() && (m_source_entries.len() >= kMaxSourceEntries ||
                                               m_source_bytes + bytes > kMaxSourceBytes)) {
            auto key     = m_source_order.remove(usize {});
            auto removed = m_source_entries.remove(key.as_str());
            if (removed.is_some()) {
                m_source_bytes -= removed->bytes;
            }
        }
        return true;
    }

    bool ReserveCompile(usize bytes) {
        if (bytes > kMaxCompileBytes) return false;
        while (! m_compile_order.is_empty() && (m_compile_entries.len() >= kMaxCompileEntries ||
                                                m_compile_bytes + bytes > kMaxCompileBytes)) {
            auto key     = m_compile_order.remove(usize {});
            auto removed = m_compile_entries.remove(key.as_str());
            if (removed.is_some()) {
                m_compile_bytes -= removed->bytes;
            }
        }
        return true;
    }

    Option<rstd::path::PathBuf>                      m_directory;
    rstd::collections::HashMap<String, SourceEntry>  m_source_entries;
    rstd::collections::HashMap<String, CompileEntry> m_compile_entries;
    Vec<String>                                      m_source_order;
    Vec<String>                                      m_compile_order;
    usize                                            m_source_bytes {};
    usize                                            m_compile_bytes {};

    friend class WPShaderParser;
};

// Output of CompileMaterialShader. On ok=true, spvs holds one SPIR-V
// blob per stage (currently always vertex+fragment in that order).
// On ok=false, error carries a short diagnostic.
struct CompileMaterialShaderResult {
    bool                         ok { false };
    std::vector<ShaderCode>      spvs;
    WPShaderInfo                 info;
    std::vector<WPShaderTexInfo> tex_info;
    std::string                  error;
    std::string                  shader_name;
};

struct CompileSceneShaderVariantResult {
    bool                         ok { false };
    std::shared_ptr<SceneShader> shader;
    SceneShaderVariantDesc       variant;
    WPShaderInfo                 info;
    std::vector<WPShaderTexInfo> tex_info;
    std::string                  error;
};

// Per-stage shader-annotation parser. Implementation lives in
// WPShaderParser_Pegtl.cpp; declaration here so the rest of the parse
// module sees it. Not exported — internal helper.
void ParseWPShader(const std::string& src, WPShaderInfo* info,
                   const std::vector<WPShaderTexInfo>& texinfos);

class WPShaderParser {
public:
    static std::string PreShaderSrc(fs::VFS&, const std::string& src, WPShaderInfo* pWPShaderInfo,
                                    const std::vector<WPShaderTexInfo>& texs,
                                    WPShaderCache*                      cache = nullptr);

    static std::string PreShaderHeader(const std::string& src, const Combos& combos, ShaderType);

    static void InitGlslang();
    static void FinalGlslang();

    static bool CompileToSpv(std::string_view         scene_id, std::span<WPShaderUnit>,
                             std::vector<ShaderCode>& spvs, WPShaderInfo*,
                             std::span<const WPShaderTexInfo>, WPShaderCache* cache = nullptr);

    static void UpdateSceneShaderVariantDescFromCompiledUnits(SceneShaderVariantDesc&,
                                                              std::span<const WPShaderUnit>,
                                                              std::span<const ShaderCode>);

    // Lightweight entry point: compile the vert+frag shader pair for one
    // material directly, without instantiating a Scene or running the
    // full SceneParser pipeline.
    //
    // Inputs come from the material JSON (parsed via Material::FromJson)
    // plus the VFS that resolves /assets/shaders/<material.shader>.{vert,frag}
    // and #include directives. combos_override entries win over the
    // material's own combos. BLENDMODE=0 and BONECOUNT=1 are seeded if
    // absent.
    //
    // Caveat: combos that ParseImageObj derives from object-level state
    // (color-blend mode, sprite-sheet flags, puppet bone count beyond
    // default, etc.) are NOT injected. Materials that hard-require them
    // will fail compile here; supply the right values via combos_override.
    static CompileMaterialShaderResult CompileMaterialShader(const Json&      material_json,
                                                             fs::VFS&         vfs,
                                                             std::string_view scene_id = "test",
                                                             const Combos&    combos_override = {},
                                                             WPShaderCache*   cache = nullptr);

    static CompileSceneShaderVariantResult
    CompileSceneShaderVariant(const SceneShaderVariantDesc& desc, fs::VFS& vfs,
                              const Combos& combos_override = {}, WPShaderCache* cache = nullptr);
};
} // namespace owe
