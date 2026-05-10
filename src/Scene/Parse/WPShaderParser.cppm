module;

#include <nlohmann/json.hpp>

export module wescene.parse:wp_shader_parser;
import wescene.core;
import wescene.types;
import cppstd;
import wescene.shader_compile;
import wescene.scene;
import wescene.fs;

export namespace owe

{
using Combos = Map<std::string, std::string>;

// ui material name to gl uniform name
using WPAliasValueDict = Map<std::string, std::string>;

using WPDefaultTexs = std::vector<std::pair<i32, std::string>>;

struct WPShaderInfo {
    Combos           combos;
    ShaderValueMap   svs;
    ShaderValueMap   baseConstSvs;
    WPAliasValueDict alias;
    WPDefaultTexs    defTexs;
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

class WPShaderParser {
public:
    static std::string PreShaderSrc(fs::VFS&, const std::string& src, WPShaderInfo* pWPShaderInfo,
                                    const std::vector<WPShaderTexInfo>& texs);

    static std::string PreShaderHeader(const std::string& src, const Combos& combos, ShaderType);

    static void InitGlslang();
    static void FinalGlslang();

    static bool CompileToSpv(std::string_view         scene_id, std::span<WPShaderUnit>,
                             std::vector<ShaderCode>& spvs, fs::VFS&, WPShaderInfo*,
                             std::span<const WPShaderTexInfo>);

    // Lightweight entry point: compile the vert+frag shader pair for one
    // material directly, without instantiating a Scene or running the
    // full SceneParser pipeline.
    //
    // Inputs come from the material JSON (parsed via WPMaterial::FromJson)
    // plus the VFS that resolves /assets/shaders/<material.shader>.{vert,frag}
    // and #include directives. combos_override entries win over the
    // material's own combos. BLENDMODE=0 and BONECOUNT=1 are seeded if
    // absent.
    //
    // Caveat: combos that ParseImageObj derives from object-level state
    // (color-blend mode, sprite-sheet flags, puppet bone count beyond
    // default, etc.) are NOT injected. Materials that hard-require them
    // will fail compile here; supply the right values via combos_override.
    static CompileMaterialShaderResult
    CompileMaterialShader(const nlohmann::json& material_json, fs::VFS& vfs,
                          std::string_view scene_id      = "test",
                          const Combos&    combos_override = {});
};
} // namespace owe
