module;

#include <rstd/macro.hpp>


#include "Utils/String.h"


module wescene.parse;
import nlohmann.json;
import wescene.core;
import wescene.types;
import rstd.log;
import rstd.cppstd;
import wescene.shader_compile;
import wescene.scene;
import wescene.common;
import wescene.utils;

static constexpr std::string_view SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__" };

#define SHADER_DIR    "spvs01"
#define SHADER_SUFFIX "spvs"

using namespace owe;

namespace
{

// GLSL prologue. WE shaders use a hybrid GLSL/HLSL dialect: `vec*` / `mat*`
// types, `mul(v, M)`, `texSample2D`, `attribute` / `varying`, `gl_Position`,
// `gl_FragColor`. Targeting glslang in GLSL mode lets us keep all of those
// natively — only HLSL-leaning macros (`saturate`, `lerp`, `frac`, `mul`
// argument order, `CAST*`, `mix` aliasing, etc.) need bridging.
//
// Pipeline: this prologue + the user source is fed through glslang's own
// preprocessor (TShader::preprocess) which expands every #if/#include/
// #define. Then a regex pass extracts surviving `attribute`/`varying`/
// `uniform` declarations (live code only — combo-gated dead branches are
// gone), and Finalprocessor strips them and re-emits explicit GLSL
// layouts: a shared `layout(set=0, binding=0, std140) uniform ww_Uniforms`
// block (cross-stage union, alphabetically ordered), per-sampler
// `layout(set=0, binding=N) uniform sampler2D` decls, and
// `layout(location=N) in/out` redeclarations for `attribute`/`varying`.
static constexpr const char* pre_shader_code = R"(#version 450 core
// auto-generated WE→GLSL prologue
#define HLSL 0
#define GLSL 1
#define highp
#define mediump
#define lowp

// HLSL-leaning intrinsics that aren't GLSL builtins.
#define saturate(x) clamp((x), 0.0, 1.0)
#define lerp(a,b,t) mix((a),(b),(t))
#define frac        fract
#define ddx         dFdx
#define ddy         dFdy
#define atan2(y,x)  atan((y),(x))
#define fmod(a,b)   mod((a),(b))

#define CAST2(x)   (vec2((x)))
#define CAST3(x)   (vec3((x)))
#define CAST4(x)   (vec4((x)))
#define CAST3X3(x) (mat3((x)))

// WE writes `mul(vec, matrix)` (row-vector / vec-first). GLSL's `*`
// between a matrix and column-vector wants matrix-first. The macro flips
// the operand order; with column-major matrices uploaded from the C++
// side, the result matches what WE expects.
#define mul(x, y) ((y) * (x))

// WE-dialect texture sampling. GLSL has `texture()` natively; the rest
// are just shorthand aliases.
#define texSample2D(t, uv)         texture((t), (uv))
#define texSample2DLod(t, uv, lod) textureLod((t), (uv), (lod))

// `uniform`, `attribute`, `varying` are intentionally NOT #define'd here.
// glslang's preprocess pass runs over this prologue; stripping any of
// them to empty would prevent the post-preprocess regex in Finalprocessor
// from finding live declarations. We let the keywords survive preprocess,
// strip the matching lines, and re-emit canonical `layout(location=N)
// in/out` and `uniform ww_Uniforms { ... }` blocks at the placeholder.

__SHADER_TAIL__
__SHADER_PLACEHOLD__

)";

// Vertex-stage tail: nothing — `gl_Position` and `gl_VertexIndex` are GLSL
// builtins, and user code writes them directly.
static constexpr const char* pre_shader_tail_vert = R"()";

// Fragment-stage tail: WE writes `gl_FragColor = color;` (GLSL 1.20-style
// builtin). Modern GLSL 450 core requires an explicit user-declared
// output. Finalprocessor emits `layout(location=0) out vec4 glOutColor;`
// at the placeholder; this macro aliases `gl_FragColor` to it.
static constexpr const char* pre_shader_tail_frag = R"(
#define gl_FragColor glOutColor
)";

// Geometry-stage tail: the .geom in assets is authored as DXC-style HLSL
// (`[maxvertexcount]`, `TriangleStream<T>::Append`, `IN[N].field`). We
// route GS units to glslang's HLSL frontend (EShSourceHlsl). No bridging
// macros are needed because the prologue is GLSL-only — the HLSL path
// uses its own minimal prologue in PreShaderHeader.
static constexpr const char* pre_shader_tail_geom = R"()";

// HLSL prologue used when type==GEOMETRY (the .geom is upstream HLSL).
// Implementation deferred until VS/FS path is verified end-to-end.
static constexpr const char* pre_shader_code_gs_hlsl = R"(// (TODO) GS HLSL prologue
__SHADER_PLACEHOLD__

)";

inline std::string LoadGlslInclude(fs::VFS& vfs, const std::string& input) {
    std::string::size_type pos = 0;
    std::string            output;
    std::string::size_type linePos = std::string::npos;

    while (linePos = input.find("#include", pos), linePos != std::string::npos) {
        auto lineEnd  = input.find_first_of('\n', linePos);
        auto lineSize = lineEnd - linePos;
        auto lineStr  = input.substr(linePos, lineSize);
        output.append(input.substr(pos, linePos - pos));

        auto inP         = lineStr.find_first_of('\"') + 1;
        auto inE         = lineStr.find_last_of('\"');
        auto includeName = lineStr.substr(inP, inE - inP);
        auto includeSrc  = fs::GetFileContent(vfs, "/assets/shaders/" + includeName);
        output.append("\n//-----include " + includeName + "\n");
        output.append(LoadGlslInclude(vfs, includeSrc));
        output.append("\n//-----include end\n");

        pos = lineEnd;
    }
    output.append(input.substr(pos));
    return output;
}

inline void ParseWPShader(const std::string& src, WPShaderInfo* pWPShaderInfo,
                          const std::vector<WPShaderTexInfo>& texinfos) {
    auto& combos       = pWPShaderInfo->combos;
    auto& wpAliasDict  = pWPShaderInfo->alias;
    auto& shadervalues = pWPShaderInfo->svs;
    auto& defTexs      = pWPShaderInfo->defTexs;
    idx   texcount     = std::ssize(texinfos);

    // pos start of line
    std::string::size_type pos = 0, lineEnd = std::string::npos;
    while ((lineEnd = src.find_first_of(('\n'), pos)), true) {
        const auto clineEnd = lineEnd;
        const auto line     = src.substr(pos, lineEnd - pos);

        /*
        if(line.find("attribute ") != std::string::npos || line.find("in ") != std::string::npos) {
            update_pos = true;
        }
        */
        if (line.find("// [COMBO]") != std::string::npos) {
            nlohmann::json combo_json;
            if (owe::ParseJson(line.substr(line.find_first_of('{')), combo_json)) {
                if (combo_json.contains("combo")) {
                    std::string name;
                    int32_t     value = 0;
                    owe::GetJsonValue(combo_json, "combo", name);
                    owe::GetJsonValue(combo_json, "default", value);
                    combos[name] = std::to_string(value);
                }
            }
        } else if (line.find("uniform ") != std::string::npos) {
            if (line.find("// {") != std::string::npos) {
                nlohmann::json sv_json;
                if (owe::ParseJson(line.substr(line.find_first_of('{')), sv_json)) {
                    std::vector<std::string> defines =
                        utils::SpliteString(line.substr(0, line.find_first_of(';')), ' ');

                    std::string material;
                    owe::GetJsonValue(sv_json, "material", material, false);
                    if (! material.empty()) wpAliasDict[material] = defines.back();

                    ShaderValue sv;
                    std::string name  = defines.back();
                    bool        istex = name.compare(0, 9, "g_Texture") == 0;
                    if (istex) {
                        wpscene::WPUniformTex wput;
                        wput.FromJson(sv_json);
                        i32 index { 0 };
                        STRTONUM(name.substr(9), index);
                        if (! wput.default_.empty()) defTexs.push_back({ index, wput.default_ });
                        if (! wput.combo.empty()) {
                            if (index >= texcount)
                                combos[wput.combo] = "0";
                            else
                                combos[wput.combo] = "1";
                        }
                        if (index < texcount && texinfos[(usize)index].enabled) {
                            auto& compos = texinfos[(usize)index].composEnabled;

                            usize num = std::min(std::size(compos), std::size(wput.components));
                            for (usize i = 0; i < num; i++) {
                                if (compos[i]) combos[wput.components[i].combo] = "1";
                            }
                        }

                    } else {
                        if (sv_json.contains("default")) {
                            auto        value = sv_json.at("default");
                            ShaderValue sv;
                            name = defines.back();
                            if (value.is_string()) {
                                std::vector<float> v;
                                owe::GetJsonValue(value, v);
                                sv = std::span<const float>(v);
                            } else if (value.is_number()) {
                                sv.setSize(1);
                                owe::GetJsonValue(value, sv[0]);
                            }
                            shadervalues[name] = sv;
                        }
                        if (sv_json.contains("combo")) {
                            std::string name;
                            owe::GetJsonValue(sv_json, "combo", name);
                            combos[name] = "1";
                        }
                    }
                    if (defines.back()[0] != 'g') {
                        rstd_info("PreShaderSrc User shadervalue not supported");
                    }
                }
            }
        }

        // end
        if (line.find("void main()") != std::string::npos || clineEnd == std::string::npos) {
            break;
        }
        pos = lineEnd + 1;
    }
}

inline usize FindIncludeInsertPos(const std::string& src, usize startPos) {
    /* rule:
    after attribute/varying/uniform/struct
    befor any func
    not in {}
    not in #if #endif
    */
    (void)startPos;

    auto NposToZero = [](usize p) {
        return p == std::string::npos ? 0 : p;
    };
    auto search = [](const std::string& p, usize pos, const auto& re) {
        auto        startpos = p.begin() + (isize)pos;
        std::smatch match;
        if (startpos < p.end() && std::regex_search(startpos, p.end(), match, re)) {
            return pos + (usize)match.position();
        }
        return std::string::npos;
    };
    auto searchLast = [](const std::string& p, const auto& re) {
        auto        startpos = p.begin();
        std::smatch match;
        while (startpos < p.end() && std::regex_search(startpos, p.end(), match, re)) {
            startpos++;
            startpos += match.position();
        }
        return startpos >= p.end() ? std::string::npos : usize(startpos - p.begin());
    };
    auto nextLinePos = [](const std::string& p, usize pos) {
        return p.find_first_of('\n', pos) + 1;
    };

    usize mainPos  = src.find("void main(");
    bool  two_main = src.find("void main(", mainPos + 2) != std::string::npos;
    if (two_main) return 0;

    usize pos;
    {
        const std::regex reAfters(R"(\n(attribute|varying|uniform|struct) )");
        usize            afterPos = searchLast(src, reAfters);
        if (afterPos != std::string::npos) {
            afterPos = nextLinePos(src, afterPos + 1);
        }
        pos = std::min({ NposToZero(afterPos), mainPos });
    }
    {
        std::stack<usize> ifStack;
        usize             nowPos { 0 };
        const std::regex  reIfs(R"((#if|#endif))");
        while (true) {
            auto p = search(src, nowPos + 1, reIfs);
            if (p > mainPos || p == std::string::npos) break;
            if (src.substr(p, 3) == "#if") {
                ifStack.push(p);
            } else {
                if (ifStack.empty()) break;
                usize ifp = ifStack.top();
                ifStack.pop();
                usize endp = p;
                if (pos > ifp && pos <= endp) {
                    pos = nextLinePos(src, endp + 1);
                }
            }
            nowPos = p;
        }
        pos = std::min({ pos, mainPos });
    }

    return NposToZero(pos);
}

inline std::string Preprocessor(const std::string& in_src, ShaderType type, const Combos& combos,
                                WPPreprocessorInfo& process_info) {
    std::string with_prologue = owe::WPShaderParser::PreShaderHeader(in_src, combos, type);

    // #require is a WE-specific extension marker, not a real preprocessor
    // directive. Comment it out before passing to glslang's preprocessor.
    {
        std::regex re_require("(^|\r?\n)#require (.+)(\r?\n)");
        with_prologue = std::regex_replace(with_prologue, re_require, "$1//#require $2$3");
    }

    // Run glslang's own preprocessor: every `#if SKINNING` / `#if FOG_COMPUTED
    // && (...)` / `#if BLENDMODE == 0` block resolves, combo names (BONECOUNT,
    // …) expand, and `#include`s (already inlined in PreShaderSrc, but
    // harmless to re-run) get handled. The regex extraction below then sees
    // only live declarations.
    vulkan::SourceLang lang =
        (type == ShaderType::GEOMETRY) ? vulkan::SourceLang::Hlsl : vulkan::SourceLang::Glsl;
    std::string src;
    if (! vulkan::Preprocess(with_prologue, type, lang, src)) {
        // Fall through: subsequent compile will fail loudly with the same
        // diagnostics. Keep with_prologue so the failing path matches what
        // a developer would see if they bypassed the preprocess step.
        src = std::move(with_prologue);
    }

    // GS source uses `in`/`out` storage classes; VS/FS use `attribute`/`varying`.
    std::regex re_io(R"((^|\n)\s*(attribute|varying|in|out)\s+([\w]+)\s+(\w+)\s*[;\[])",
                     std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re_io);
         it != std::sregex_iterator();
         it++) {
        std::smatch mc      = *it;
        const auto& storage = mc[2];
        const auto& name    = mc[4];
        // attribute-in-vertex and varying-in-fragment both behave as inputs;
        // varying-in-vertex behaves as output. GS: `in` is input (from VS),
        // `out` is output (to FS).
        bool is_input = (storage == "attribute") ||
                        (storage == "varying" && type == ShaderType::FRAGMENT) ||
                        (storage == "in" && type == ShaderType::GEOMETRY);
        if (is_input) {
            process_info.input[name] = mc[0].str();
        } else {
            process_info.output[name] = mc[0].str();
        }
    }

    // Capture non-sampler uniform declarations so Finalprocessor can emit
    // a single shared cbuffer with the cross-stage union. Each stage's
    // captured types must match (vert and frag must declare the same
    // `g_Foo` with the same type) — WE shaders honor this convention.
    std::regex re_uniform(
        R"((^|\n)[ \t]*uniform[ \t]+([\w]+)[ \t]+(\w+)[ \t]*(\[[^\]]*\])?[ \t]*;)",
        std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re_uniform);
         it != std::sregex_iterator();
         ++it) {
        std::smatch mc   = *it;
        std::string ty   = mc[2].str();
        std::string name = mc[3].str();
        // Skip sampler types; those are handled by ScanAndStripSamplers
        // and emitted as Texture2D + SamplerState pairs.
        if (ty == "sampler2D" || ty == "sampler3D" || ty == "samplerCube" ||
            ty == "sampler2DComparison" || ty == "sampler2DShadow")
            continue;
        std::string array = mc[4].matched ? mc[4].str() : "";
        process_info.uniforms[name] = ty + array;
    }

    std::regex re_tex(R"(uniform\s+sampler2D\s+g_Texture(\d+))", std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re_tex);
         it != std::sregex_iterator();
         it++) {
        std::smatch mc  = *it;
        auto        str = mc[1].str();
        unsigned        slot;
        auto [ptr, ec] { std::from_chars(str.c_str(), str.c_str() + str.size(), slot) };
        if (ec != std::errc()) continue;
        process_info.active_tex_slots.insert(slot);
    }
    return src;
}

// Pass GLSL type names through unchanged; aliases like `float`/`float2` get
// re-emitted as is for HLSL-flavoured leftovers.
inline std::string ToGLSLType(std::string_view t) {
    if (t == "float2") return "vec2";
    if (t == "float3") return "vec3";
    if (t == "float4") return "vec4";
    if (t == "int2") return "ivec2";
    if (t == "int3") return "ivec3";
    if (t == "int4") return "ivec4";
    if (t == "uint2") return "uvec2";
    if (t == "uint3") return "uvec3";
    if (t == "uint4") return "uvec4";
    if (t == "float2x2") return "mat2";
    if (t == "float3x3") return "mat3";
    if (t == "float4x4") return "mat4";
    return std::string(t);
}

struct IODecl {
    char        storage; // 'a' for attribute, 'v' for varying, 'i' for GS `in`, 'o' for GS `out'
    std::string type;    // GLSL type as captured (vec2/vec4/mat3/...)
    std::string name;
    std::string array;   // "[N]" or empty
};

inline char StorageCharFor(const std::string& storage_word) {
    if (storage_word == "attribute") return 'a';
    if (storage_word == "in")        return 'i';
    if (storage_word == "out")       return 'o';
    return 'v'; // varying
}

struct SamplerDecl {
    std::string sampler_type; // "sampler2D" / "samplerCube" / ...
    std::string name;
};

inline const std::regex& SamplerRegex() {
    // Order matters: regex alternation matches the FIRST alternative,
    // so longer-prefix names must come before shorter ones — otherwise
    // `sampler2D` swallows `sampler2DComparison` and the trailing
    // `Comparison` text breaks the match.
    static const std::regex re(
        R"((^|\n)[ \t]*uniform[ \t]+(sampler2DComparison|sampler2DShadow|samplerCube|sampler2D|sampler3D)[ \t]+(\w+)[ \t]*;)",
        std::regex::ECMAScript);
    return re;
}

inline std::pair<std::vector<SamplerDecl>, std::string>
ScanAndStripSamplers(const std::string& src) {
    std::vector<SamplerDecl> decls;
    std::string              out;
    out.reserve(src.size());

    auto       it     = std::sregex_iterator(src.begin(), src.end(), SamplerRegex());
    const auto end    = std::sregex_iterator();
    usize      cursor = 0;
    for (; it != end; ++it) {
        std::smatch mc          = *it;
        const usize match_start = mc.position(0);
        const usize match_end   = match_start + mc.length(0);
        const usize keep_len    = mc[1].length(); // keep the leading newline

        out.append(src, cursor, match_start - cursor);
        out.append(src, match_start, keep_len);
        cursor = match_end;

        decls.push_back({ mc[2].str(), mc[3].str() });
    }
    out.append(src, cursor, std::string::npos);
    return { std::move(decls), std::move(out) };
}

inline const char* HLSLSamplerType(std::string_view glsl) {
    if (glsl == "sampler2D")   return "Texture2D<float4>";
    if (glsl == "sampler3D")   return "Texture3D<float4>";
    if (glsl == "samplerCube") return "TextureCube<float4>";
    // GLSL shadow / comparison samplers: scalar-result texture with a
    // SamplerComparisonState. We bind a Texture2D<float> and a paired
    // SamplerComparisonState (the latter chosen via HLSLSamplerStateType).
    if (glsl == "sampler2DComparison" || glsl == "sampler2DShadow")
        return "Texture2D<float>";
    return "Texture2D<float4>";
}

inline const char* HLSLSamplerStateType(std::string_view glsl) {
    if (glsl == "sampler2DComparison" || glsl == "sampler2DShadow")
        return "SamplerComparisonState";
    return "SamplerState";
}

inline bool IsSamplerCombinedImage(std::string_view glsl) {
    // All sampler types in WE are combined image samplers from the
    // descriptor-set side. The HLSL sampling intrinsic differs (Sample
    // vs SampleCmp) but binding semantics are identical.
    (void)glsl;
    return true;
}

// Match `uniform TYPE NAME[opt-array];` for any TYPE (we filter samplers
// out by name in the caller). Used to strip uniform decls from the
// source so we can re-emit them as members of a shared cbuffer.
inline const std::regex& UniformRegex() {
    static const std::regex re(
        R"((^|\n)[ \t]*uniform[ \t]+([\w]+)[ \t]+(\w+)[ \t]*(\[[^\]]*\])?[ \t]*;)",
        std::regex::ECMAScript);
    return re;
}

// Strip every `uniform TYPE NAME;` declaration from the source. The
// caller will re-emit them as cbuffer members. Returns the stripped
// source. Sampler-typed uniforms are stripped here too (they were
// already handled by ScanAndStripSamplers, this is idempotent if
// they've already been removed from the input).
inline std::string StripUniforms(const std::string& src) {
    std::string out;
    out.reserve(src.size());

    auto       it     = std::sregex_iterator(src.begin(), src.end(), UniformRegex());
    const auto end    = std::sregex_iterator();
    usize      cursor = 0;
    for (; it != end; ++it) {
        std::smatch mc          = *it;
        const usize match_start = mc.position(0);
        const usize match_end   = match_start + mc.length(0);
        const usize keep_len    = mc[1].length(); // keep the leading newline

        out.append(src, cursor, match_start - cursor);
        out.append(src, match_start, keep_len);
        cursor = match_end;
    }
    out.append(src, cursor, std::string::npos);
    return out;
}

// Regex used by both the source-stripping pass and the cross-stage
// re-parse. (^|\n) anchors at line start; trailing optional `[N]` lets
// arrays through; trailing optional whitespace before `;` is permitted.
inline const std::regex& IORegex() {
    static const std::regex re(
        R"((^|\n)[ \t]*(attribute|varying|in|out)[ \t]+([\w]+)[ \t]+(\w+)[ \t]*(\[[^\]]*\])?[ \t]*;)",
        std::regex::ECMAScript);
    return re;
}

inline std::optional<IODecl> ParseIODecl(const std::string& line) {
    std::smatch mc;
    if (! std::regex_search(line, mc, IORegex())) return std::nullopt;
    return IODecl { StorageCharFor(mc[2].str()),
                    mc[3].str(),
                    mc[4].str(),
                    mc[5].matched ? mc[5].str() : "" };
}

// Pull all `attribute|varying TYPE NAME;` declarations out of the source
// (returning them as structured IODecls) and produce a copy of the
// source with those lines removed. Stripping is essential: `attribute`
// and `varying` are not HLSL keywords, and we re-emit canonical `static`
// decls so the synthesized entry point doesn't drift from what DXC's
// preprocessor actually compiled (combo-gated `#if` branches drop their
// declarations at compile time).
inline std::pair<std::vector<IODecl>, std::string> ScanAndStripIO(const std::string& src) {
    std::vector<IODecl> decls;
    std::string         out;
    out.reserve(src.size());

    auto       it      = std::sregex_iterator(src.begin(), src.end(), IORegex());
    const auto end     = std::sregex_iterator();
    usize      cursor  = 0;
    for (; it != end; ++it) {
        std::smatch mc          = *it;
        const usize match_start = mc.position(0);
        const usize match_end   = match_start + mc.length(0);

        // Keep the leading newline (mc[1]) — it's part of the match but
        // not the declaration we're stripping.
        const usize keep_start = match_start;
        const usize keep_len   = mc[1].length();

        out.append(src, cursor, keep_start - cursor);
        out.append(src, keep_start, keep_len);
        cursor = match_end;

        decls.push_back({ StorageCharFor(mc[2].str()),
                          mc[3].str(),
                          mc[4].str(),
                          mc[5].matched ? mc[5].str() : "" });
    }
    out.append(src, cursor, std::string::npos);
    return { std::move(decls), std::move(out) };
}

// Synthesizer output split in two: `pre` is `static TYPE NAME;` decls
// that must precede the user `void main()` (HLSL needs identifiers
// declared before use). `post` is the HLSL entry-point wrapper that
// must follow `void main()` so it can call the renamed `shader_main()`.
struct SynthOutput {
    std::string pre;
    std::string post;
};

// Build `layout(location=N) in/out TYPE NAME[arr];` declarations from a
// list of IO decls, with locations assigned alphabetically so neighbouring
// stages agree without explicit coordination. `is_input` picks the storage
// qualifier (in vs out). Returns the joined block.
inline std::string EmitStageIOLayout(std::vector<IODecl> decls, bool is_input) {
    // gl_Position is a GLSL builtin; never re-declare it.
    decls.erase(std::remove_if(decls.begin(), decls.end(),
                               [](const IODecl& d) { return d.name == "gl_Position"; }),
                decls.end());
    std::sort(decls.begin(), decls.end(),
              [](const IODecl& a, const IODecl& b) { return a.name < b.name; });
    auto array_slots = [](const std::string& arr) -> usize {
        if (arr.size() < 3 || arr.front() != '[' || arr.back() != ']') return 1;
        usize n = 0;
        for (usize i = 1; i + 1 < arr.size(); ++i) {
            const char c = arr[i];
            if (c < '0' || c > '9') return 1;
            n = n * 10 + (usize)(c - '0');
        }
        return n > 0 ? n : 1;
    };
    const char* qual = is_input ? "in" : "out";
    std::string out;
    usize       loc = 0;
    for (const auto& d : decls) {
        out += "layout(location = " + std::to_string(loc) + ") " + qual + " " +
               ToGLSLType(d.type) + " " + d.name + d.array + ";\n";
        loc += array_slots(d.array);
    }
    return out;
}

inline std::string Finalprocessor(const WPShaderUnit& unit, const WPPreprocessorInfo* pre,
                                  const WPPreprocessorInfo* next) {
    // GS path stays HLSL-flavoured for now (the .geom is upstream HLSL); the
    // GS-specific synth will land in a follow-up. For this turn just hand the
    // raw GS source back without splicing in anything.
    if (unit.stage == ShaderType::GEOMETRY) {
        std::regex re_hold(SHADER_PLACEHOLD.data());
        return std::regex_replace(unit.src, re_hold, std::string {});
    }

    // Strip `attribute/varying` lines and collect them as structured decls.
    auto [io_decls, stage1] = ScanAndStripIO(unit.src);

    // Strip `uniform sampler2D NAME;` lines; re-emitted below as explicit
    // `layout(set=0, binding=N) uniform sampler2D NAME;` decls.
    auto [sampler_decls, stage2] = ScanAndStripSamplers(stage1);

    // Strip non-sampler `uniform TYPE NAME;` lines too; re-emitted as
    // members of a cross-stage UBO `ww_Uniforms` at (set=0, binding=0).
    std::string stage3 = StripUniforms(stage2);

    // Partition IO decls into VS-attributes (`a` storage) and varyings
    // (everything else). The cross-stage union ensures vert and frag pick
    // identical location indices alphabetically.
    std::vector<IODecl> attrs, varyings;
    Set<std::string>    seen;
    auto add = [&](const IODecl& d) {
        if (! seen.insert(d.name).second) return;
        if (d.storage == 'a') attrs.push_back(d);
        else                  varyings.push_back(d);
    };
    for (const auto& d : io_decls) add(d);
    auto add_from_line = [&](const std::string& line) {
        if (auto d = ParseIODecl(line); d) add(*d);
    };
    if (pre)  for (auto& [k, v] : pre->output) add_from_line(v);
    if (next) for (auto& [k, v] : next->input) add_from_line(v);

    std::string io_block;
    if (unit.stage == ShaderType::VERTEX) {
        io_block += "// === auto-generated stage I/O (GLSL) ===\n";
        io_block += EmitStageIOLayout(std::move(attrs), /*is_input=*/true);
        io_block += EmitStageIOLayout(std::move(varyings), /*is_input=*/false);
    } else { // FRAGMENT
        io_block += "// === auto-generated stage I/O (GLSL) ===\n";
        io_block += EmitStageIOLayout(std::move(varyings), /*is_input=*/true);
        // FS output: WE writes `gl_FragColor = color;` (aliased to glOutColor
        // by the FS prologue). Declare the slot here.
        io_block += "layout(location = 0) out vec4 glOutColor;\n";
    }

    // Cross-stage uniform union → single std140 UBO at (set=0, binding=0).
    Map<std::string, std::string> uniforms_union; // name -> "TYPE[arr]"
    auto absorb = [&](const Map<std::string, std::string>& m) {
        for (const auto& [k, v] : m) uniforms_union.try_emplace(k, v);
    };
    absorb(unit.preprocess_info.uniforms);
    if (pre)  absorb(pre->uniforms);
    if (next) absorb(next->uniforms);

    std::string uniform_block;
    if (! uniforms_union.empty()) {
        uniform_block += "\n// === auto-generated shared uniforms ===\n";
        uniform_block += "layout(set = 0, binding = 0, std140) uniform ww_Uniforms {\n";
        for (const auto& [name, ty] : uniforms_union) {
            std::string base_ty = ty;
            std::string array;
            if (auto pos = ty.find('['); pos != std::string::npos) {
                base_ty = ty.substr(0, pos);
                array   = ty.substr(pos);
            }
            uniform_block += "    " + ToGLSLType(base_ty) + " " + name + array + ";\n";
        }
        uniform_block += "};\n";
    }

    // One `layout(set=0, binding=N) uniform sampler2D NAME;` per unique
    // sampler. Bindings start at 1 (binding 0 holds ww_Uniforms).
    Set<std::string> sampler_seen;
    std::string      sampler_block;
    if (! sampler_decls.empty()) sampler_block += "\n// === auto-generated samplers ===\n";
    usize sampler_idx = 1;
    for (const auto& s : sampler_decls) {
        if (! sampler_seen.insert(s.name).second) continue;
        sampler_block += "layout(set = 0, binding = " + std::to_string(sampler_idx) +
                         ") uniform " + s.sampler_type + " " + s.name + ";\n";
        ++sampler_idx;
    }

    std::regex  re_hold(SHADER_PLACEHOLD.data());
    std::string with_decls =
        std::regex_replace(stage3, re_hold, io_block + uniform_block + sampler_block);
    return with_decls;
}

inline std::string GenSha1(std::span<const WPShaderUnit> units) {
    std::string shas;
    for (auto& unit : units) {
        shas += utils::genSha1(unit.src);
    }
    return utils::genSha1(shas);
}
inline std::string GetCachePath(std::string_view scene_id, std::string_view filename) {
    return std::string("/cache/") + std::string(scene_id) + "/" SHADER_DIR "/" +
           std::string(filename) + "." SHADER_SUFFIX;
}

inline bool LoadShaderFromFile(std::vector<ShaderCode>& codes, fs::IBinaryStream& file) {
    codes.clear();
    i32 ver = ReadSPVVesion(file);

    usize count = file.ReadUint32();
    rstd_assert(count <= 16 && count >= 0);
    if (count > 16) return false;

    codes.resize(count);
    for (usize i = 0; i < count; i++) {
        auto& c = codes[i];

        u32 size = file.ReadUint32();
        rstd_assert(size % 4 == 0);
        if (size % 4 != 0) return false;

        c.resize(size / 4);
        file.Read((char*)c.data(), size);
    }
    return true;
}

inline void SaveShaderToFile(std::span<const ShaderCode> codes, fs::IBinaryStreamW& file) {
    char nop[256] { '\0' };

    WriteSPVVesion(file, 1);
    file.WriteUint32((u32)codes.size());
    for (const auto& c : codes) {
        u32 size = (u32)c.size() * 4;
        file.WriteUint32(size);
        file.Write((const char*)c.data(), size);
    }
    file.Write(nop, sizeof(nop));
}

} // namespace

std::string WPShaderParser::PreShaderSrc(fs::VFS& vfs, const std::string& src,
                                         WPShaderInfo*                       pWPShaderInfo,
                                         const std::vector<WPShaderTexInfo>& texinfos) {
    std::string            newsrc(src);
    std::string::size_type pos = 0;
    std::string            include;
    while (pos = src.find("#include", pos), pos != std::string::npos) {
        auto begin = pos;
        pos        = src.find_first_of('\n', pos);
        newsrc.replace(begin, pos - begin, pos - begin, ' ');
        include.append(src.substr(begin, pos - begin) + "\n");
    }
    include = LoadGlslInclude(vfs, include);

    ParseWPShader(include, pWPShaderInfo, texinfos);
    ParseWPShader(newsrc, pWPShaderInfo, texinfos);

    newsrc.insert(FindIncludeInsertPos(newsrc, 0), include);
    return newsrc;
}

std::string WPShaderParser::PreShaderHeader(const std::string& src, const Combos& combos,
                                            ShaderType type) {
    // GS routes through the HLSL frontend; VS/FS through the GLSL prologue.
    std::string pre;
    if (type == ShaderType::GEOMETRY) {
        pre = pre_shader_code_gs_hlsl;
    } else {
        pre = pre_shader_code;
        const char* tail = (type == ShaderType::FRAGMENT) ? pre_shader_tail_frag
                                                          : pre_shader_tail_vert;
        if (auto pos = pre.find("__SHADER_TAIL__"); pos != std::string::npos) {
            pre.replace(pos, std::string_view("__SHADER_TAIL__").size(), tail);
        }
    }

    std::string combo_defines;
    for (const auto& c : combos) {
        std::string cup(c.first);
        std::transform(c.first.begin(), c.first.end(), cup.begin(), ::toupper);
        if (c.second.empty()) {
            rstd_error("combo '{}' can't be empty", cup);
            continue;
        }
        combo_defines += "#define " + cup + " " + c.second + "\n";
    }

    // Combo `#define`s land before __SHADER_PLACEHOLD__ so they're visible
    // throughout the user source during the DXC -P pass. The placeholder
    // slot itself is filled by Finalprocessor *after* preprocessing, so
    // the synthesized cbuffer always sees combo references already
    // expanded to literal numbers (e.g. `g_Bones[BONECOUNT]` → `[4]`).
    if (auto pos = pre.find(SHADER_PLACEHOLD); pos != std::string::npos) {
        pre.insert(pos, combo_defines);
    } else {
        pre += combo_defines;
    }
    return pre + src;
}

void WPShaderParser::InitGlslang() { vulkan::InitProcess(); }
void WPShaderParser::FinalGlslang() { vulkan::FinalizeProcess(); }

namespace
{

// Serialize one CompileToSpv invocation as a JSON object. Captures the
// raw post-PreShaderSrc state (includes resolved, prologue not yet
// applied, regex extraction not yet run) so a replay through the full
// pipeline exercises every transform downstream.
nlohmann::json BuildShaderRecord(std::string_view scene_id, std::span<const WPShaderUnit> units,
                                 const WPShaderInfo*                  shader_info,
                                 std::span<const WPShaderTexInfo>     texs) {
    auto stage_name = [](ShaderType s) -> const char* {
        switch (s) {
        case ShaderType::VERTEX:   return "VERTEX";
        case ShaderType::FRAGMENT: return "FRAGMENT";
        case ShaderType::GEOMETRY: return "GEOMETRY";
        }
        return "UNKNOWN";
    };

    nlohmann::json rec;
    rec["scene_id"] = std::string(scene_id);

    nlohmann::json js_stages = nlohmann::json::array();
    for (const auto& u : units) {
        js_stages.push_back({ { "stage", stage_name(u.stage) }, { "src", u.src } });
    }
    rec["stages"] = std::move(js_stages);

    nlohmann::json js_combos = nlohmann::json::object();
    if (shader_info) {
        for (const auto& [k, v] : shader_info->combos) js_combos[k] = v;
    }
    rec["combos"] = std::move(js_combos);

    nlohmann::json js_texs = nlohmann::json::array();
    for (const auto& t : texs) {
        js_texs.push_back({ { "enabled", t.enabled },
                            { "compos",
                              nlohmann::json::array(
                                  { t.composEnabled[0], t.composEnabled[1], t.composEnabled[2] }) } });
    }
    rec["tex_infos"] = std::move(js_texs);

    return rec;
}

// Appends one JSONL line to WP_SHADER_RECORD's path. O_APPEND is atomic
// for writes ≤ PIPE_BUF on Linux, which is more than enough for a single
// JSON line; concurrent recorders won't interleave.
void MaybeRecordCompile(std::string_view scene_id, std::span<const WPShaderUnit> units,
                        const WPShaderInfo* shader_info, std::span<const WPShaderTexInfo> texs) {
    const char* path = std::getenv("WP_SHADER_RECORD");
    if (! path || path[0] == '\0') return;
    nlohmann::json rec  = BuildShaderRecord(scene_id, units, shader_info, texs);
    std::string    line = rec.dump();
    line.push_back('\n');
    if (FILE* f = std::fopen(path, "a")) {
        std::fwrite(line.data(), 1, line.size(), f);
        std::fclose(f);
    } else {
        rstd_warn("WP_SHADER_RECORD: cannot open '{}' for append", path);
    }
}

} // namespace

bool WPShaderParser::CompileToSpv(std::string_view scene_id, std::span<WPShaderUnit> units,
                                  std::vector<ShaderCode>& codes, fs::VFS& vfs,
                                  WPShaderInfo* shader_info, std::span<const WPShaderTexInfo> texs) {
    MaybeRecordCompile(scene_id, units, shader_info, texs);

    std::for_each(units.begin(), units.end(), [shader_info](auto& unit) {
        unit.src = Preprocessor(unit.src, unit.stage, shader_info->combos, unit.preprocess_info);
    });

    auto compile = [](std::span<WPShaderUnit> units, std::vector<ShaderCode>& codes) {
        std::vector<vulkan::ShaderCompUnit> vunits(units.size());
        for (usize i = 0; i < units.size(); i++) {
            auto&               unit     = units[i];
            auto&               vunit    = vunits[i];
            WPPreprocessorInfo* pre_info = i >= 1 ? &units[i - 1].preprocess_info : nullptr;
            WPPreprocessorInfo* post_info =
                i + 1 < units.size() ? &units[i + 1].preprocess_info : nullptr;

            unit.src = Finalprocessor(unit, pre_info, post_info);

            vunit.src   = unit.src;
            vunit.stage = unit.stage;
            vunit.lang  = (unit.stage == ShaderType::GEOMETRY) ? vulkan::SourceLang::Hlsl
                                                                : vulkan::SourceLang::Glsl;
        }

        vulkan::ShaderCompOpt opt;
        opt.target   = vulkan::VulkanTarget::Vulkan_1_1;
        opt.optimize = false;

        std::vector<vulkan::Uni_ShaderSpv> spvs(units.size());

        if (! vulkan::CompileAndLinkShaderUnits(vunits, opt, spvs)) {
            return false;
        }

        codes.clear();
        for (auto& spv : spvs) {
            codes.emplace_back(std::move(spv->spirv));
        }
        return true;
    };

    bool has_cache_dir = vfs.IsMounted("cache");

    if (has_cache_dir) {
        std::string sha1            = GenSha1(units);
        std::string cache_file_path = GetCachePath(scene_id, sha1);

        if (vfs.Contains(cache_file_path)) {
            auto cache_file = vfs.Open(cache_file_path);
            if (! cache_file || ! ::LoadShaderFromFile(codes, *cache_file)) {
                rstd_error("load shader from \'{}\' failed", cache_file_path);
                return false;
            }
        } else {
            if (! compile(units, codes)) return false;
            if (auto cache_file = vfs.OpenW(cache_file_path); cache_file) {
                ::SaveShaderToFile(codes, *cache_file);
            }
        }
        return true;

    } else {
        return compile(units, codes);
    }
}

CompileMaterialShaderResult
WPShaderParser::CompileMaterialShader(const nlohmann::json& material_json, fs::VFS& vfs,
                                      std::string_view scene_id,
                                      const Combos&    combos_override) {
    CompileMaterialShaderResult r;

    wpscene::WPMaterial mat;
    if (! mat.FromJson(material_json)) {
        r.error = "WPMaterial::FromJson failed";
        return r;
    }
    r.shader_name = mat.shader;

    if (mat.shader.empty()) {
        r.error = "material has no shader name";
        return r;
    }

    const std::string shader_path = "/assets/shaders/" + mat.shader;
    std::string       vert_src    = fs::GetFileContent(vfs, shader_path + ".vert");
    std::string       frag_src    = fs::GetFileContent(vfs, shader_path + ".frag");
    std::string       geom_src;
    if (mat.shader == "genericropeparticle") {
        geom_src = fs::GetFileContent(vfs, shader_path + ".geom");
    }
    if (vert_src.empty() || frag_src.empty()) {
        r.error = "shader source missing: " + shader_path + ".{vert,frag}";
        return r;
    }

    // Texture info: enabled flag from non-empty material.textures.
    // composEnabled[3] would normally come from each .tex header
    // (extraHeader.compoN). Skipping the header parse keeps this entry
    // path lightweight; sprite-sheet / packed-channel materials may
    // accordingly compile a different variant than the production path.
    r.tex_info.reserve(mat.textures.size());
    for (const auto& t : mat.textures) {
        r.tex_info.push_back({ ! t.empty(), { false, false, false } });
    }

    // Combos: material's int combos -> string, then override wins.
    // Inject defaults that ParseImageObj always sets.
    for (const auto& kv : mat.combos) {
        r.info.combos[kv.first] = std::to_string(kv.second);
    }
    for (const auto& kv : combos_override) {
        r.info.combos[kv.first] = kv.second;
    }
    if (r.info.combos.find("BLENDMODE") == r.info.combos.end()) r.info.combos["BLENDMODE"] = "0";
    if (r.info.combos.find("BONECOUNT") == r.info.combos.end()) r.info.combos["BONECOUNT"] = "1";

    std::vector<WPShaderUnit> units;
    units.push_back({ ShaderType::VERTEX, std::move(vert_src), {} });
    if (! geom_src.empty()) {
        units.push_back({ ShaderType::GEOMETRY, std::move(geom_src), {} });
        r.info.combos["GS_ENABLED"] = "1";
    }
    units.push_back({ ShaderType::FRAGMENT, std::move(frag_src), {} });

    for (auto& u : units) {
        u.src = WPShaderParser::PreShaderSrc(vfs, u.src, &r.info, r.tex_info);
    }

    InitGlslang();
    const bool ok = WPShaderParser::CompileToSpv(
        scene_id, std::span<WPShaderUnit>(units.data(), units.size()), r.spvs, vfs, &r.info,
        r.tex_info);
    FinalGlslang();

    r.ok = ok;
    if (! ok) r.error = "CompileToSpv failed";
    return r;
}
