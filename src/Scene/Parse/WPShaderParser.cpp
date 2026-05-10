module;

#include <rstd/macro.hpp>

#include <nlohmann/json.hpp>
#include "WPJson.hpp"

#include "Utils/String.h"

#include <cassert>

module wescene.parse;
import wescene.core;
import wescene.types;
import rstd.log;
import rstd.cppstd;
import cppstd;
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

// HLSL prologue. WE shaders are written in a hybrid dialect that already
// uses HLSL idioms (mul, texSample2D, float2/3/4, saturate, lerp, frac,
// [maxvertexcount], OUT.Append); only the residual GLSL bits (vec*, mat*,
// attribute, varying, gl_*) need bridging. The glslang→GLSL prologue used
// to invert this; this prologue keeps HLSL native and bridges the GLSL
// residue.
//
// Pipeline: this prologue + the user source is fed through DXC's
// preprocessor (-P, see vulkan::Preprocess) which expands every #if /
// #include / #define. Then a regex pass extracts the surviving
// `attribute`/`varying`/`uniform` declarations (live code only — combo-
// gated dead branches are gone), and Finalprocessor strips them and
// re-emits canonical statics + cbuffer + samplers + entry-point wrapper.
static constexpr const char* pre_shader_code = R"(// auto-generated WE→HLSL prologue
#define HLSL 1
#define GLSL 0
#define highp
#define mediump
#define lowp

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define ivec2 int2
#define ivec3 int3
#define ivec4 int4
#define uvec2 uint2
#define uvec3 uint3
#define uvec4 uint4
#define bvec2 bool2
#define bvec3 bool3
#define bvec4 bool4
#define mat2 float2x2
#define mat3 float3x3
#define mat4 float4x4
// GLSL `matCxR` declares C columns × R rows. HLSL `floatRxC` declares R rows
// × C columns — the indices are swapped. With -Zpc (column-major packing)
// the in-memory layout matches and `mul(M, v)` produces the same result, so
// these macros transpose the type-name indices and leave semantics alone.
#define mat2x2 float2x2
#define mat3x3 float3x3
#define mat4x4 float4x4
#define mat2x3 float3x2
#define mat2x4 float4x2
#define mat3x2 float2x3
#define mat3x4 float4x3
#define mat4x2 float2x4
#define mat4x3 float3x4

#define CAST2(x) ((float2)(x))
#define CAST3(x) ((float3)(x))
#define CAST4(x) ((float4)(x))
#define CAST3X3(x) ((float3x3)(x))

#define mix(a,b,t) lerp((a),(b),(t))
#define fract frac
#define atan(a,b) atan2((a),(b))
#define dFdx ddx
#define dFdy(x) (-ddy(x))
// HLSL has saturate, mul, lerp, frac, ddx/ddy, fwidth, max, min as builtins.

// WE shaders write `mul(vec, matrix)` (HLSL vector-first row-vector
// convention). The C++ side uploads matrices designed for GLSL-style
// column-vector multiply (`MVP * v`). Under HLSL native semantics this
// would compute `transpose(M) * v`, transposing every transform.
//
// The original glslang prologue compensated with
//     #define mul(x, y) ((y) * (x))
// which converts WE `mul(v, M)` to GLSL `M * v` (matrix-times-column).
// We can't simply do that in HLSL because `M * v` is component-wise (or
// errors on dim mismatch). Instead, we overload a `_ww_mul` function
// for each common type combination — each overload calls HLSL native
// `mul` with operands swapped — then `#define mul _ww_mul` to redirect.
// The function bodies see the still-native `mul` (the macro is below).
float2   _ww_mul(float2   v, float2x2 M) { return mul(M, v); }
float3   _ww_mul(float3   v, float3x3 M) { return mul(M, v); }
float4   _ww_mul(float4   v, float4x4 M) { return mul(M, v); }
float2x2 _ww_mul(float2x2 A, float2x2 B) { return mul(B, A); }
float3x3 _ww_mul(float3x3 A, float3x3 B) { return mul(B, A); }
float4x4 _ww_mul(float4x4 A, float4x4 B) { return mul(B, A); }
// Matrix-first WE callsites (less common, e.g. tangent-space rebases):
// `mul(M, v)` should also reverse to `v * M` (row-times-matrix).
float2   _ww_mul(float2x2 M, float2   v) { return mul(v, M); }
float3   _ww_mul(float3x3 M, float3   v) { return mul(v, M); }
float4   _ww_mul(float4x4 M, float4   v) { return mul(v, M); }
// Rectangular matrix variants (e.g. bone transforms `mat4x3 g_Bones[]`
// = HLSL float3x4). Both orderings appear in WE shaders — skinning
// callsites do `mul(v4, weighted_sum_of_g_Bones)` (vec-first), while
// tangent-space rebases sometimes do `mul(g_Bones[i], pos4)`
// (matrix-first). Without the vec-first overload HLSL's implicit
// truncations make several square/scalar candidates match and the
// resolver reports ambiguity.
float3   _ww_mul(float4   v, float3x4 M) { return mul(M, v); }
float3   _ww_mul(float3x4 M, float4   v) { return mul(M, v); }
// Scalar passthroughs (HLSL `mul` accepts these but our overload needs
// to match): we let the macro expand and the resolver pick the right
// case. For scalar*x or x*scalar, define minimal cases.
float    _ww_mul(float a, float b) { return a * b; }
float2   _ww_mul(float a, float2 b) { return a * b; }
float3   _ww_mul(float a, float3 b) { return a * b; }
float4   _ww_mul(float a, float4 b) { return a * b; }
float2   _ww_mul(float2 a, float b) { return a * b; }
float3   _ww_mul(float3 a, float b) { return a * b; }
float4   _ww_mul(float4 a, float b) { return a * b; }
// Vector-vector: HLSL `mul(v, v)` returns dot product.
float    _ww_mul(float2 a, float2 b) { return dot(a, b); }
float    _ww_mul(float3 a, float3 b) { return dot(a, b); }
float    _ww_mul(float4 a, float4 b) { return dot(a, b); }

#define mul _ww_mul

// `uniform` is intentionally NOT #define'd here. The DXC preprocess pass
// (-P) runs over this prologue; if `uniform` were stripped to empty, the
// post-preprocess regex couldn't tell `uniform vec4 X;` apart from a
// plain global decl. We let the keyword survive -P, the regex strips
// every `uniform TYPE NAME;` line in Finalprocessor, and the union of
// names is re-emitted as a shared cbuffer ww_Uniforms member set.

// WE-dialect texture sampling. Each sampler2D NAME generates a paired
// SamplerState NAME_ww_sampler in the synth.pre block; texSample2D thus
// expands to NAME.Sample(NAME_ww_sampler, uv).
#define texSample2D(t, uv)         ((t).Sample(t##_ww_sampler, (uv)))
#define texSample2DLod(t, uv, lod) ((t).SampleLevel(t##_ww_sampler, (uv), (lod)))
#define texture(t, uv)             texSample2D((t), (uv))
#define textureLod(t, uv, lod)     texSample2DLod((t), (uv), (lod))

// Stage I/O is plumbed by the synthesizer (Finalprocessor). It strips
// every `attribute|varying TYPE NAME;` line from the user source and
// re-emits canonical `static TYPE NAME;` declarations of its own — that
// way #if-gated decls don't desync from compile-time visibility, and
// vert/frag stages get the same union of names. The keywords themselves
// MUST NOT be #define'd here; if they were, the synthesizer's regex
// would still match the unsubstituted text but DXC would see the
// substituted text, drifting the two views apart.

static float4 gl_Position;
static float4 gl_FragCoord;
static float4 glOutColor;
#define gl_FragColor glOutColor

// Rename the user's main() so a synthesized HLSL entry point can wrap it.
// The wrapper (main_vs / main_ps) is appended in Finalprocessor.
#define main shader_main

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
            if (PARSE_JSON(line.substr(line.find_first_of('{')), combo_json)) {
                if (combo_json.contains("combo")) {
                    std::string name;
                    int32_t     value = 0;
                    GET_JSON_NAME_VALUE(combo_json, "combo", name);
                    GET_JSON_NAME_VALUE(combo_json, "default", value);
                    combos[name] = std::to_string(value);
                }
            }
        } else if (line.find("uniform ") != std::string::npos) {
            if (line.find("// {") != std::string::npos) {
                nlohmann::json sv_json;
                if (PARSE_JSON(line.substr(line.find_first_of('{')), sv_json)) {
                    std::vector<std::string> defines =
                        utils::SpliteString(line.substr(0, line.find_first_of(';')), ' ');

                    std::string material;
                    GET_JSON_NAME_VALUE_NOWARN(sv_json, "material", material);
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
                                GET_JSON_VALUE(value, v);
                                sv = std::span<const float>(v);
                            } else if (value.is_number()) {
                                sv.setSize(1);
                                GET_JSON_VALUE(value, sv[0]);
                            }
                            shadervalues[name] = sv;
                        }
                        if (sv_json.contains("combo")) {
                            std::string name;
                            GET_JSON_NAME_VALUE(sv_json, "combo", name);
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
    // directive. DXC would error on it, so comment it out before -P.
    {
        std::regex re_require("(^|\r?\n)#require (.+)(\r?\n)");
        with_prologue = std::regex_replace(with_prologue, re_require, "$1//#require $2$3");
    }

    // Run DXC's preprocessor: every `#if SKINNING` / `#if FOG_COMPUTED &&
    // (...)` / `#if BLENDMODE == 0` block is resolved, every macro (combo
    // names like BONECOUNT, type aliases like vec3 → float3, function-like
    // macros like texSample2D) is expanded, every `#include` (already
    // inlined in PreShaderSrc, but harmless to re-run) is handled. The
    // regex extraction below sees only live declarations.
    std::string src;
    if (! vulkan::Preprocess(with_prologue, src)) {
        // Fall through: subsequent compile will fail loudly with the same
        // diagnostics. Keep with_prologue so the failing path matches what
        // a developer would see if they bypassed the preprocess step.
        src = std::move(with_prologue);
    }

    std::regex re_io(R"((^|\n)\s*(attribute|varying)\s+([\w]+)\s+(\w+)\s*[;\[])",
                     std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re_io);
         it != std::sregex_iterator();
         it++) {
        std::smatch mc      = *it;
        const auto& storage = mc[2];
        const auto& name    = mc[4];
        // attribute-in-vertex and varying-in-fragment both behave as inputs;
        // varying-in-vertex behaves as output.
        bool is_input = (storage == "attribute") ||
                        (storage == "varying" && type == ShaderType::FRAGMENT);
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

// Map a WE/GLSL type name to its HLSL equivalent. The prologue defines
// these as macros for the user shader body; for the synthesized entry
// point we emit pure HLSL directly so the synthesis is self-contained.
inline std::string ToHLSLType(std::string_view t) {
    if (t == "vec2") return "float2";
    if (t == "vec3") return "float3";
    if (t == "vec4") return "float4";
    if (t == "ivec2") return "int2";
    if (t == "ivec3") return "int3";
    if (t == "ivec4") return "int4";
    if (t == "uvec2") return "uint2";
    if (t == "uvec3") return "uint3";
    if (t == "uvec4") return "uint4";
    if (t == "bvec2") return "bool2";
    if (t == "bvec3") return "bool3";
    if (t == "bvec4") return "bool4";
    if (t == "mat2" || t == "mat2x2") return "float2x2";
    if (t == "mat3" || t == "mat3x3") return "float3x3";
    if (t == "mat4" || t == "mat4x4") return "float4x4";
    // Rectangular matrices: GLSL `matCxR` ↔ HLSL `floatRxC` (indices swap).
    if (t == "mat2x3") return "float3x2";
    if (t == "mat2x4") return "float4x2";
    if (t == "mat3x2") return "float2x3";
    if (t == "mat3x4") return "float4x3";
    if (t == "mat4x2") return "float2x4";
    if (t == "mat4x3") return "float3x4";
    return std::string(t); // already HLSL-native (float, float2/3/4 literal, etc.)
}

struct IODecl {
    char        storage; // 'a' for attribute, 'v' for varying
    std::string type;    // GLSL type as captured (vec2/vec4/mat3/...)
    std::string name;
    std::string array;   // "[N]" or empty
};

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
        R"((^|\n)[ \t]*(attribute|varying)[ \t]+([\w]+)[ \t]+(\w+)[ \t]*(\[[^\]]*\])?[ \t]*;)",
        std::regex::ECMAScript);
    return re;
}

inline std::optional<IODecl> ParseIODecl(const std::string& line) {
    std::smatch mc;
    if (! std::regex_search(line, mc, IORegex())) return std::nullopt;
    return IODecl { mc[2].str() == "attribute" ? 'a' : 'v',
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

        decls.push_back({ mc[2].str() == "attribute" ? 'a' : 'v',
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

// Emit the synthesized HLSL entry-point that wraps the user's renamed
// shader_main(). Builds:
//   - `static TYPE NAME [array];` decls for every attr/varying (so the
//     user shader's references resolve regardless of #if state).
//   - VS: WW_VSIn struct (attributes with [[vk::location(N)]]) +
//         WW_VSOut struct (varyings + SV_Position).
//   - PS: WW_PSIn struct (varyings + SV_Position) returning SV_Target0.
//   - main_vs / main_ps body that copies between the static globals
//     and the entry-point structs and calls shader_main().
//
// Locations are assigned in alphabetic order so vert/frag stages agree
// without explicit coordination — both call this function with the
// same union of names from cross-stage matching. Geometry shaders use
// HLSL-flavoured `[maxvertexcount]` / `OUT.Append` / `IN[N]` syntax
// directly and don't need a wrapper.
inline SynthOutput SynthesizeHLSLEntry(ShaderType stage, std::vector<IODecl> attrs,
                                       std::vector<IODecl> varyings) {
    SynthOutput so;
    if (stage == ShaderType::GEOMETRY) return so;

    auto by_name = [](const IODecl& a, const IODecl& b) { return a.name < b.name; };
    std::sort(attrs.begin(), attrs.end(), by_name);
    std::sort(varyings.begin(), varyings.end(), by_name);

    so.pre += "\n// === auto-generated stage I/O statics ===\n";
    for (const auto& d : attrs) {
        so.pre += "static " + ToHLSLType(d.type) + " " + d.name + d.array + ";\n";
    }
    for (const auto& d : varyings) {
        so.pre += "static " + ToHLSLType(d.type) + " " + d.name + d.array + ";\n";
    }

    std::string& out = so.post;
    out += "\n// === auto-generated entry point ===\n";
    // HLSL requires a semantic on every entry-point struct field. DXC also
    // uses the semantic to name the SPIR-V input/output variable: the
    // generated name is `in.var.<SEMANTIC>` / `out.var.<SEMANTIC>`. The
    // C++ vertex-buffer setup matches attributes by NAME (looking up
    // `a_Position`, `a_TexCoord` etc. in `attrs_map`), so we use the
    // attribute/varying name *as* the semantic — that way reflection
    // reports something C++ can match after stripping the `in.var.`
    // prefix in GenReflect.
    auto sem_named = [](const std::string& name) { return " : " + name; };

    // A `varying TYPE name[N];` consumes N consecutive location slots.
    // Without this, a single-slot varying that follows an array-typed
    // varying gets an aliased location and DXC rejects the struct
    // ("stage output location #N already consumed").
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

    if (stage == ShaderType::VERTEX) {
        out += "struct WW_VSIn {\n";
        {
            usize loc = 0;
            for (const auto& a : attrs) {
                out += "    [[vk::location(" + std::to_string(loc) + ")]] " +
                       ToHLSLType(a.type) + " " + a.name + a.array +
                       sem_named(a.name) + ";\n";
                loc += array_slots(a.array);
            }
        }
        out += "};\n";
        out += "struct WW_VSOut {\n";
        out += "    float4 _ww_sv_position : SV_Position;\n";
        {
            usize loc = 0;
            for (const auto& v : varyings) {
                out += "    [[vk::location(" + std::to_string(loc) + ")]] " +
                       ToHLSLType(v.type) + " " + v.name + v.array +
                       sem_named(v.name) + ";\n";
                loc += array_slots(v.array);
            }
        }
        out += "};\n";
        out += "WW_VSOut main_vs(WW_VSIn _ww_in) {\n";
        for (const auto& a : attrs) {
            out += "    " + a.name + " = _ww_in." + a.name + ";\n";
        }
        out += "    shader_main();\n";
        out += "    WW_VSOut _ww_out;\n";
        out += "    _ww_out._ww_sv_position = gl_Position;\n";
        for (const auto& v : varyings) {
            out += "    _ww_out." + v.name + " = " + v.name + ";\n";
        }
        out += "    return _ww_out;\n";
        out += "}\n";
    } else { // FRAGMENT
        out += "struct WW_PSIn {\n";
        out += "    float4 _ww_sv_position : SV_Position;\n";
        {
            usize loc = 0;
            for (const auto& v : varyings) {
                out += "    [[vk::location(" + std::to_string(loc) + ")]] " +
                       ToHLSLType(v.type) + " " + v.name + v.array +
                       sem_named(v.name) + ";\n";
                loc += array_slots(v.array);
            }
        }
        out += "};\n";
        out += "float4 main_ps(WW_PSIn _ww_in) : SV_Target0 {\n";
        out += "    gl_FragCoord = _ww_in._ww_sv_position;\n";
        for (const auto& v : varyings) {
            out += "    " + v.name + " = _ww_in." + v.name + ";\n";
        }
        out += "    shader_main();\n";
        out += "    return glOutColor;\n";
        out += "}\n";
    }
    return so;
}

inline std::string Finalprocessor(const WPShaderUnit& unit, const WPPreprocessorInfo* pre,
                                  const WPPreprocessorInfo* next) {
    // Strip attribute/varying lines and collect them as structured decls.
    auto [io_decls, stage1] = ScanAndStripIO(unit.src);

    // Strip `uniform sampler2D NAME;` (and 3D / cube) lines; we re-emit
    // them as paired Texture2D + SamplerState in the synth.pre block.
    auto [sampler_decls, stage2] = ScanAndStripSamplers(stage1);

    // Strip non-sampler `uniform TYPE NAME;` lines too; we re-emit them
    // as members of a shared cbuffer ww_Uniforms (set 0 binding 0). The
    // member set is the cross-stage union from process_info.uniforms,
    // alphabetically ordered, so vert and frag agree on offsets.
    std::string stage3 = StripUniforms(stage2);

    // File-scope `const TYPE NAME[N] = { ... };` (e.g. WE bokeh's `kernel`)
    // lands as a `$Globals` cbuffer member under DXC unless it's `static
    // const`. We don't allocate or bind a $Globals descriptor, so a draw
    // hits an unbound descriptor and the GPU hangs (validation reports
    // "$Globals at Binding 4 never updated"). Promote line-start `const`
    // array decls to `static const` so DXC inlines them as a constant
    // table instead of materializing through the cbuffer.
    {
        static const std::regex re_const_arr(
            R"((^|\n)const[ \t]+([\w]+)[ \t]+(\w+)[ \t]*\[)",
            std::regex::ECMAScript);
        stage3 = std::regex_replace(stage3, re_const_arr, "$1static const $2 $3[");
    }

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

    auto synth = SynthesizeHLSLEntry(unit.stage, std::move(attrs), std::move(varyings));

    // Build the cross-stage uniform union. Map<> iterates sorted, so the
    // ordering is deterministic and matches between vert and frag.
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
        uniform_block += "[[vk::binding(0, 0)]] cbuffer ww_Uniforms {\n";
        for (const auto& [name, ty] : uniforms_union) {
            // ty already carries the optional [N] suffix from Preprocessor.
            // Split off the array part for HLSL syntax: TYPE NAME[N];
            std::string base_ty = ty;
            std::string array;
            if (auto pos = ty.find('['); pos != std::string::npos) {
                base_ty = ty.substr(0, pos);
                array   = ty.substr(pos);
            }
            uniform_block += "    " + ToHLSLType(base_ty) + " " + name + array + ";\n";
        }
        uniform_block += "};\n";
    }

    // Emit Texture2D + paired SamplerState for every stripped sampler.
    // [[vk::combinedImageSampler]] tells DXC to merge them into a single
    // VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER on the SPIR-V side, which
    // is what `GenReflect` (SPIRV-Reflect path) expects in binding_map.
    // Bindings start at 1; binding 0 in set 0 holds the ww_Uniforms cbuffer.
    Set<std::string> sampler_seen;
    std::string      sampler_block;
    if (! sampler_decls.empty()) sampler_block += "\n// === auto-generated samplers ===\n";
    usize sampler_idx = 1;
    for (const auto& s : sampler_decls) {
        if (! sampler_seen.insert(s.name).second) continue;
        const std::string bind =
            "[[vk::combinedImageSampler]][[vk::binding(" + std::to_string(sampler_idx) + ", 0)]] ";
        sampler_block += bind + HLSLSamplerType(s.sampler_type) + " " + s.name + ";\n";
        sampler_block += bind + HLSLSamplerStateType(s.sampler_type) + " " + s.name +
                         "_ww_sampler;\n";
        ++sampler_idx;
    }

    std::regex  re_hold(SHADER_PLACEHOLD.data());
    std::string with_decls =
        std::regex_replace(stage3, re_hold, synth.pre + uniform_block + sampler_block);
    return with_decls + synth.post;
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
    assert(count <= 16 && count >= 0);
    if (count > 16) return false;

    codes.resize(count);
    for (usize i = 0; i < count; i++) {
        auto& c = codes[i];

        u32 size = file.ReadUint32();
        assert(size % 4 == 0);
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
    (void)type;
    std::string pre(pre_shader_code);

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

// glslang has been replaced by DXC; these are kept as no-ops to preserve the
// public API for callers (VulkanRender.cpp, WPSceneParser.cpp).
void WPShaderParser::InitGlslang() {}
void WPShaderParser::FinalGlslang() {}

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

    std::array<WPShaderUnit, 2> units {
        WPShaderUnit { ShaderType::VERTEX,   std::move(vert_src), {} },
        WPShaderUnit { ShaderType::FRAGMENT, std::move(frag_src), {} },
    };

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
