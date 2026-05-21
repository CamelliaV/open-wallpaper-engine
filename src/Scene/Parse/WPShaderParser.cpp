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

// HLSL prologue. WE shaders are written in a hybrid dialect that already
// uses HLSL idioms (mul, texSample2D, float2/3/4, saturate, lerp, frac,
// [maxvertexcount], OUT.Append); only the residual GLSL bits (vec*, mat*,
// attribute, varying, gl_*) need bridging. Routing VS/FS through glslang's
// HLSL frontend lets the parser handle implicit conversions HLSL allows
// (scalar→vec broadcast on assignment, bool→float, etc.) — which would
// otherwise fault in glslang's strict GLSL mode.
//
// Pipeline: this prologue + the user source is fed through glslang's own
// preprocessor (TShader::preprocess) which expands every #if / #include /
// #define. Then a regex pass extracts the surviving `attribute`/`varying`/
// `uniform` declarations (live code only — combo-gated dead branches are
// gone) and Finalprocessor strips them, then re-emits canonical
// `static TYPE NAME;` decls + a paired Texture2D/SamplerState block + a
// shared cbuffer ww_Uniforms + an HLSL entry-point wrapper (main_vs /
// main_ps) that shuffles between the static globals and the
// SV_*-annotated entry struct.
static constexpr const char* pre_shader_code = R"(// auto-generated WE→HLSL prologue
// glslang's HLSL frontend defaults to row-major matrix packing in SPIR-V
// (RowMajor decoration on cbuffer members). The C++ uploader writes
// column-major data (Eigen default), so force column-major packing here.
#pragma pack_matrix(column_major)
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
// × C columns — the indices are swapped. With column-major packing the
// in-memory layout matches and `mul(M, v)` produces the same result, so the
// macros transpose the type-name indices and leave semantics alone.
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

// GLSL `mod(a, b)` is `a - b * floor(a / b)` and isn't an HLSL builtin
// (HLSL has `fmod`, but it uses trunc for the quotient — different sign
// behavior for negative args). Provide a `mod` function so shaders that
// call it without supplying their own definition still compile. A macro
// rewrite (`#define mod fmod`) was tried but mangled user `float mod(...)`
// declarations.
float  mod(float  a, float  b) { return a - b * floor(a / b); }
float2 mod(float2 a, float2 b) { return a - b * floor(a / b); }
float3 mod(float3 a, float3 b) { return a - b * floor(a / b); }
float4 mod(float4 a, float4 b) { return a - b * floor(a / b); }
float2 mod(float2 a, float  b) { return a - b * floor(a / b); }
float3 mod(float3 a, float  b) { return a - b * floor(a / b); }
float4 mod(float4 a, float  b) { return a - b * floor(a / b); }
// HLSL has saturate, mul, lerp, frac, ddx/ddy, fwidth, max, min, clip, log10,
// pow as builtins — most of the C++-side overload workarounds needed for the
// GLSL frontend disappear here.

// WE shaders write `mul(vec, matrix)` (HLSL vector-first row-vector
// convention). The C++ side uploads matrices designed for GLSL-style
// column-vector multiply (`MVP * v`). Under HLSL native semantics this
// would compute `transpose(M) * v`, transposing every transform. Overload
// `_ww_mul` for each common type combination — each overload calls HLSL
// native `mul` with operands swapped — then `#define mul _ww_mul` redirects.
float2   _ww_mul(float2   v, float2x2 M) { return mul(M, v); }
float3   _ww_mul(float3   v, float3x3 M) { return mul(M, v); }
float4   _ww_mul(float4   v, float4x4 M) { return mul(M, v); }
float2x2 _ww_mul(float2x2 A, float2x2 B) { return mul(B, A); }
float3x3 _ww_mul(float3x3 A, float3x3 B) { return mul(B, A); }
float4x4 _ww_mul(float4x4 A, float4x4 B) { return mul(B, A); }
float2   _ww_mul(float2x2 M, float2   v) { return mul(v, M); }
float3   _ww_mul(float3x3 M, float3   v) { return mul(v, M); }
float4   _ww_mul(float4x4 M, float4   v) { return mul(v, M); }
// Rectangular variants (`mat4x3 g_Bones[]` → HLSL float3x4). Vec-first and
// matrix-first WE callsites both appear; without explicit overloads, HLSL's
// implicit truncations make several candidates match and the resolver
// reports ambiguity.
float3   _ww_mul(float4   v, float3x4 M) { return mul(M, v); }
float3   _ww_mul(float3x4 M, float4   v) { return mul(M, v); }
// Scalar passthroughs.
float    _ww_mul(float a, float b)   { return a * b; }
float2   _ww_mul(float a, float2 b)  { return a * b; }
float3   _ww_mul(float a, float3 b)  { return a * b; }
float4   _ww_mul(float a, float4 b)  { return a * b; }
float2   _ww_mul(float2 a, float b)  { return a * b; }
float3   _ww_mul(float3 a, float b)  { return a * b; }
float4   _ww_mul(float4 a, float b)  { return a * b; }
// Vector-vector: HLSL `mul(v, v)` returns dot product.
float    _ww_mul(float2 a, float2 b) { return dot(a, b); }
float    _ww_mul(float3 a, float3 b) { return dot(a, b); }
float    _ww_mul(float4 a, float4 b) { return dot(a, b); }
#define mul _ww_mul

// `uniform`, `attribute`, `varying` are intentionally NOT #define'd here.
// glslang's preprocess pass runs over this prologue; if any of them were
// stripped to empty, the post-preprocess regex in Finalprocessor wouldn't
// find live declarations. We let the keywords survive preprocess, strip
// the matching lines, and re-emit canonical `static TYPE NAME;` decls +
// `cbuffer ww_Uniforms` + Texture2D/SamplerState pairs at the placeholder.

// WE-dialect texture sampling. Each `uniform sampler2D NAME` becomes a
// `Texture2D<float4> NAME;` + paired `SamplerState NAME_ww_sampler;` in
// the Finalprocessor synth block; texSample2D thus expands to
// `NAME.Sample(NAME_ww_sampler, uv)`. The `texture()` overloads accept
// vec2/vec3/vec4 UV (HLSL Sample takes float2 — the auto-truncation
// matches what WE shaders rely on for `texture(g_T, v_TexCoord)` when
// v_TexCoord is vec4).
#define texSample2D(t, uv)         ((t).Sample(t##_ww_sampler, (uv)))
#define texSample2DLod(t, uv, lod) ((t).SampleLevel(t##_ww_sampler, (uv), (lod)))
#define texture(t, uv)             texSample2D((t), (uv))
#define textureLod(t, uv, lod)     texSample2DLod((t), (uv), (lod))

// PerformLighting_V1 is referenced by WE's generic4/genericparticle PBR
// shaders but its body is normally injected by WE's HLSL toolchain based
// on `LIGHTS_*` combos. We don't have that injection step; stub here so
// compilation succeeds. The stub is "albedo × view-aligned shading" —
// darker than WE but visible.
float3 PerformLighting_V1(float3 worldPos, float3 albedo, float3 normal, float3 viewVector,
                          float3 specularTint, float3 f0, float roughness, float metallic) {
    return albedo * max(dot(normalize(normal), normalize(viewVector)), 0.0);
}
float3 PerformLighting_V1(float3 worldPos, float3 albedo, float3 normal, float3 viewVector,
                          float3 specularTint, float3 f0, float roughness, float metallic,
                          float ao) {
    return albedo * ao * max(dot(normalize(normal), normalize(viewVector)), 0.0);
}

__SHADER_TAIL__
__SHADER_PLACEHOLD__

)";

// VS/FS tail: stage I/O is plumbed by the Finalprocessor synthesizer. It
// strips every `attribute|varying TYPE NAME;` line and re-emits canonical
// `static TYPE NAME;` decls; combo-gated `#if` branches drop their decls
// at preprocess time, so vert/frag stages get a matching live name set.
// The keywords MUST NOT be #define'd here; if they were, the regex would
// see unsubstituted text but the HLSL parser would see the substituted
// text, drifting the two views apart.
static constexpr const char* pre_shader_tail_vert = R"(
static float4 gl_Position;
// Rename the user's main() so a synthesized HLSL entry point can wrap it.
// The wrapper (main_vs) is appended in Finalprocessor.
#define main shader_main
)";

static constexpr const char* pre_shader_tail_frag = R"(
static float4 gl_FragCoord;
static float4 glOutColor;
#define gl_FragColor glOutColor
#define main shader_main
)";

static constexpr const char* pre_shader_tail_geom = R"()";

// HLSL prologue used when type==GEOMETRY. WE's .geom source is a hybrid:
// GLSL-flavoured top-level `in vec4 X;` / `out vec4 X;` decls + HLSL-style
// `[maxvertexcount] void main() { ... IN[0].X ... v.Y = ...; OUT.Append(v); }`
// body. We feed it to glslang's HLSL frontend (EShSourceHlsl); this prologue
// bridges GLSL types/builtins to HLSL and Finalprocessor strips the `in`/`out`
// lines + emits `struct WW_VSOut/WW_PSIn` + `cbuffer ww_Uniforms` + replaces
// `void main()` with the GS entry signature.
static constexpr const char* pre_shader_code_gs_hlsl = R"(// auto-generated WE→HLSL prologue (GS)
// glslang's HLSL frontend defaults to row-major matrix packing in SPIR-V
// (RowMajor decoration on cbuffer members). The VS/FS GLSL synth emits a
// std140 UBO with default column-major matrices and the C++ uploader writes
// column-major data, so a row-major GS reads the transpose. Force column-
// major packing here so the GS sees the same matrix as the rest.
#pragma pack_matrix(column_major)
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
#define mat2 float2x2
#define mat3 float3x3
#define mat4 float4x4
#define mat2x2 float2x2
#define mat3x3 float3x3
#define mat4x4 float4x4
#define mat2x3 float3x2
#define mat2x4 float4x2
#define mat3x2 float2x3
#define mat3x4 float4x3
#define mat4x2 float2x4
#define mat4x3 float3x4
#define CAST2(x)   ((float2)(x))
#define CAST3(x)   ((float3)(x))
#define CAST4(x)   ((float4)(x))
#define CAST3X3(x) ((float3x3)(x))
#define mix(a,b,t) lerp((a),(b),(t))
#define fract      frac
#define atan(a,b)  atan2((a),(b))
#define dFdx       ddx
#define dFdy(x)    (-ddy(x))

// glslang's HLSL frontend always tags cbuffer matrices `RowMajor` in SPIR-V
// regardless of `#pragma pack_matrix` or `column_major` qualifiers (verified
// on glslang 16.3.0). With column-major data uploaded from C++ (Eigen
// default), the shader's effective matrix is the transpose of the source.
// HLSL `mul(M, v)` lowers (via glslang) to `OpVectorTimesMatrix V M`, which
// combined with the implicit transpose yields `source_M * V` — exactly the
// transform WE intends. `_ww_mul` swaps WE's vec-first `mul(v, M)` to that
// form.
float2   _ww_mul(float2   v, float2x2 M) { return mul(M, v); }
float3   _ww_mul(float3   v, float3x3 M) { return mul(M, v); }
float4   _ww_mul(float4   v, float4x4 M) { return mul(M, v); }
float2x2 _ww_mul(float2x2 A, float2x2 B) { return mul(B, A); }
float3x3 _ww_mul(float3x3 A, float3x3 B) { return mul(B, A); }
float4x4 _ww_mul(float4x4 A, float4x4 B) { return mul(B, A); }
float2   _ww_mul(float2x2 M, float2   v) { return mul(v, M); }
float3   _ww_mul(float3x3 M, float3   v) { return mul(v, M); }
float4   _ww_mul(float4x4 M, float4   v) { return mul(v, M); }
float3   _ww_mul(float4   v, float3x4 M) { return mul(M, v); }
float3   _ww_mul(float3x4 M, float4   v) { return mul(v, M); }
float    _ww_mul(float a, float b)        { return a * b; }
float2   _ww_mul(float a, float2 b)       { return a * b; }
float3   _ww_mul(float a, float3 b)       { return a * b; }
float4   _ww_mul(float a, float4 b)       { return a * b; }
float2   _ww_mul(float2 a, float b)       { return a * b; }
float3   _ww_mul(float3 a, float b)       { return a * b; }
float4   _ww_mul(float4 a, float b)       { return a * b; }
float    _ww_mul(float2 a, float2 b)      { return dot(a, b); }
float    _ww_mul(float3 a, float3 b)      { return dot(a, b); }
float    _ww_mul(float4 a, float4 b)      { return dot(a, b); }
#define mul _ww_mul

// `gl_Position` is the SV_Position struct field's GLSL name; rename to the
// canonical struct field name so `IN[0].gl_Position` / `v.gl_Position` both
// resolve correctly.
#define gl_Position _ww_sv_position
#define PS_INPUT    WW_PSIn

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

// Comment out stray `#endif` directives with no matching `#if`. A class of
// WE-shipped community shader templates (audio_bars / dot_matrix / sine_wave
// variants — 244 of the corpus failures pre-fix) has one extra `#endif`
// past the file's last `#if`. WE's HLSL toolchain tolerates this; glslang
// rejects it as a preprocess error. Stack-walk the source, and when
// `#endif` would pop an empty stack, comment the line instead.
inline std::string BalanceConditionals(std::string src) {
    auto is_id_char = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    auto starts_with_word = [&](std::string_view s, std::string_view w) {
        if (s.size() < w.size()) return false;
        if (s.substr(0, w.size()) != w) return false;
        if (s.size() == w.size()) return true;
        return ! is_id_char(s[w.size()]);
    };

    int         depth = 0;
    std::string out;
    out.reserve(src.size() + 32);
    usize cursor = 0;
    while (cursor < src.size()) {
        usize eol = src.find('\n', cursor);
        if (eol == std::string::npos) eol = src.size();
        std::string_view line(src.data() + cursor, eol - cursor);

        // Trim leading whitespace to find the directive start.
        usize s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        bool stray_endif = false;
        if (s < line.size() && line[s] == '#') {
            usize t = s + 1;
            while (t < line.size() && (line[t] == ' ' || line[t] == '\t')) ++t;
            std::string_view rest = line.substr(t);
            if (starts_with_word(rest, "if") || starts_with_word(rest, "ifdef") ||
                starts_with_word(rest, "ifndef")) {
                ++depth;
            } else if (starts_with_word(rest, "endif")) {
                if (depth == 0) stray_endif = true;
                else --depth;
            }
        }

        if (stray_endif) {
            out.append("// (ww stray-endif) ");
        }
        out.append(line);
        if (eol < src.size()) out.push_back('\n');
        cursor = eol + 1;
    }
    return out;
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

    with_prologue = BalanceConditionals(std::move(with_prologue));

    // Run glslang's own preprocessor: every `#if SKINNING` / `#if FOG_COMPUTED
    // && (...)` / `#if BLENDMODE == 0` block resolves, combo names (BONECOUNT,
    // …) expand, and `#include`s (already inlined in PreShaderSrc, but
    // harmless to re-run) get handled. The regex extraction below then sees
    // only live declarations.
    // All stages route through glslang's HLSL frontend. Bridging macros in
    // the prologue turn GLSL types/intrinsics into HLSL equivalents.
    vulkan::SourceLang lang = vulkan::SourceLang::Hlsl;
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

// Inverse of ToGLSLType: bridge GLSL aliases back to HLSL canonical names
// (used by the GS synth which feeds HLSL to glslang's HLSL frontend).
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
    if (t == "mat2" || t == "mat2x2") return "float2x2";
    if (t == "mat3" || t == "mat3x3") return "float3x3";
    if (t == "mat4" || t == "mat4x4") return "float4x4";
    if (t == "mat2x3") return "float3x2";
    if (t == "mat2x4") return "float4x2";
    if (t == "mat3x2") return "float2x3";
    if (t == "mat3x4") return "float4x3";
    if (t == "mat4x2") return "float2x4";
    if (t == "mat4x3") return "float3x4";
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

inline usize ArraySlots(const std::string& arr) {
    if (arr.size() < 3 || arr.front() != '[' || arr.back() != ']') return 1;
    usize n = 0;
    for (usize i = 1; i + 1 < arr.size(); ++i) {
        const char c = arr[i];
        if (c < '0' || c > '9') return 1;
        n = n * 10 + (usize)(c - '0');
    }
    return n > 0 ? n : 1;
}

// Build `layout(location=N) in/out TYPE NAME[arr];` declarations from a
// list of IO decls, with locations assigned alphabetically so neighbouring
// stages agree without explicit coordination. `is_input` picks the storage
// qualifier (in vs out). Returns the joined block.
inline std::string EmitStageIOLayout(std::vector<IODecl> decls, bool is_input) {
    // gl_Position is a GLSL builtin; never re-declare it. _ww_sv_position is
    // the GS-side macro alias for the same slot.
    decls.erase(std::remove_if(decls.begin(), decls.end(), [](const IODecl& d) {
                    return d.name == "gl_Position" || d.name == "_ww_sv_position";
                }),
                decls.end());
    std::sort(decls.begin(), decls.end(),
              [](const IODecl& a, const IODecl& b) { return a.name < b.name; });
    const char* qual = is_input ? "in" : "out";
    std::string out;
    usize       loc = 0;
    for (const auto& d : decls) {
        out += "layout(location = " + std::to_string(loc) + ") " + qual + " " +
               ToGLSLType(d.type) + " " + d.name + d.array + ";\n";
        loc += ArraySlots(d.array);
    }
    return out;
}

// HLSL-side struct emission for the GS synth. Each non-position field gets
// `[[vk::location(N)]]` and a name-as-semantic so SPIRV-Reflect picks up the
// same VS/FS names through the GS-emitted SPIR-V.
inline std::string EmitGSHLSLStruct(std::string_view name, std::vector<IODecl> decls) {
    decls.erase(std::remove_if(decls.begin(), decls.end(), [](const IODecl& d) {
                    return d.name == "gl_Position" || d.name == "_ww_sv_position";
                }),
                decls.end());
    std::sort(decls.begin(), decls.end(),
              [](const IODecl& a, const IODecl& b) { return a.name < b.name; });
    std::string out;
    out += "struct ";
    out += name;
    out += " {\n";
    out += "    float4 _ww_sv_position : SV_Position;\n";
    usize loc = 0;
    for (const auto& d : decls) {
        out += "    [[vk::location(" + std::to_string(loc) + ")]] " +
               ToHLSLType(d.type) + " " + d.name + d.array + " : " + d.name + ";\n";
        loc += ArraySlots(d.array);
    }
    out += "};\n";
    return out;
}

// HLSL-side struct emission for VS/FS entry points. Same as the GS variant
// but the SV_Position field is included only when the struct represents a
// VS output / FS input (HLSL needs it for rasterizer setup); attributes
// (VS input) don't carry it.
inline std::string EmitVSFSStruct(std::string_view name, std::vector<IODecl> decls,
                                  bool include_sv_position) {
    decls.erase(std::remove_if(decls.begin(), decls.end(), [](const IODecl& d) {
                    return d.name == "gl_Position" || d.name == "_ww_sv_position";
                }),
                decls.end());
    std::sort(decls.begin(), decls.end(),
              [](const IODecl& a, const IODecl& b) { return a.name < b.name; });
    std::string out;
    out += "struct ";
    out += name;
    out += " {\n";
    if (include_sv_position) {
        out += "    float4 _ww_sv_position : SV_Position;\n";
    }
    usize loc = 0;
    for (const auto& d : decls) {
        out += "    [[vk::location(" + std::to_string(loc) + ")]] " +
               ToHLSLType(d.type) + " " + d.name + d.array + " : " + d.name + ";\n";
        loc += ArraySlots(d.array);
    }
    out += "};\n";
    return out;
}

// Emit the HLSL synth block (pre = decls / structs / cbuffer / samplers,
// post = entry-point wrapper) for VS or FS. Locations are alphabetical so
// vert/frag stages agree without explicit coordination — both are called
// with the same cross-stage varying union.
inline SynthOutput SynthesizeHLSLEntry(ShaderType stage, std::vector<IODecl> attrs,
                                       std::vector<IODecl> varyings) {
    SynthOutput so;
    if (stage == ShaderType::GEOMETRY) return so;

    // gl_Position propagates via the SV_Position field, not a regular slot.
    // Filter both names (the GS prologue rewrites `gl_Position` to
    // `_ww_sv_position`, so its post-preprocess form needs filtering too).
    auto drop_position = [](std::vector<IODecl>& v) {
        v.erase(std::remove_if(v.begin(), v.end(), [](const IODecl& d) {
                    return d.name == "gl_Position" || d.name == "_ww_sv_position";
                }),
                v.end());
    };
    drop_position(attrs);
    drop_position(varyings);

    auto by_name = [](const IODecl& a, const IODecl& b) { return a.name < b.name; };
    std::sort(attrs.begin(), attrs.end(), by_name);
    std::sort(varyings.begin(), varyings.end(), by_name);

    // Static globals so the user shader body resolves `a_Position`,
    // `v_TexCoord`, etc. regardless of #if-branch visibility — the wrapper
    // copies from/to the entry struct.
    so.pre += "\n// === auto-generated stage I/O statics ===\n";
    for (const auto& d : attrs) {
        so.pre += "static " + ToHLSLType(d.type) + " " + d.name + d.array + ";\n";
    }
    for (const auto& d : varyings) {
        so.pre += "static " + ToHLSLType(d.type) + " " + d.name + d.array + ";\n";
    }

    std::string& out = so.post;
    out += "\n// === auto-generated entry point ===\n";
    if (stage == ShaderType::VERTEX) {
        out += EmitVSFSStruct("WW_VSIn",  attrs,    /*sv_pos=*/false);
        out += EmitVSFSStruct("WW_VSOut", varyings, /*sv_pos=*/true);
        out += "WW_VSOut main_vs(WW_VSIn _ww_in) {\n";
        for (const auto& a : attrs) {
            out += "    " + a.name + " = _ww_in." + a.name + ";\n";
        }
        out += "    shader_main();\n";
        out += "    WW_VSOut _ww_out;\n";
        out += "    _ww_out._ww_sv_position = gl_Position;\n";
        for (const auto& v : varyings) {
            if (v.name == "gl_Position" || v.name == "_ww_sv_position") continue;
            out += "    _ww_out." + v.name + " = " + v.name + ";\n";
        }
        out += "    return _ww_out;\n";
        out += "}\n";
    } else { // FRAGMENT
        out += EmitVSFSStruct("WW_PSIn", varyings, /*sv_pos=*/true);
        out += "float4 main_ps(WW_PSIn _ww_in) : SV_Target0 {\n";
        out += "    gl_FragCoord = _ww_in._ww_sv_position;\n";
        for (const auto& v : varyings) {
            if (v.name == "gl_Position" || v.name == "_ww_sv_position") continue;
            out += "    " + v.name + " = _ww_in." + v.name + ";\n";
        }
        out += "    shader_main();\n";
        out += "    return glOutColor;\n";
        out += "}\n";
    }
    return so;
}

// Find a literal `void main()` call in `src` (no regex). Replace with the GS
// entry-point signature. Returns the modified source unchanged if no match.
inline std::string RewriteGSMain(std::string src) {
    static constexpr std::string_view marker { "void main()" };
    static constexpr std::string_view repl {
        "void main_gs(point WW_VSOut IN[1], inout TriangleStream<WW_PSIn> OUT)"
    };
    if (auto pos = src.find(marker); pos != std::string::npos) {
        src.replace(pos, marker.size(), repl);
    }
    return src;
}

inline std::string Finalprocessor(const WPShaderUnit& unit, const WPPreprocessorInfo* pre,
                                  const WPPreprocessorInfo* next,
                                  const Map<std::string, std::string>* uniforms_union_in = nullptr) {
    // GS: feed glslang's HLSL frontend. Strip GLSL-style top-level `in`/`out`
    // decls, emit HLSL structs (WW_VSOut/WW_PSIn) + ww_Uniforms cbuffer, and
    // rewrite `void main()` to the entry signature `point WW_VSOut IN[1],
    // inout TriangleStream<WW_PSIn> OUT`.
    if (unit.stage == ShaderType::GEOMETRY) {
        auto [io_decls, stripped] = ScanAndStripIO(unit.src);
        std::string body          = StripUniforms(stripped);

        std::vector<IODecl> in_decls, out_decls;
        Set<std::string>    in_seen, out_seen;
        auto add_in  = [&](const IODecl& d) {
            if (in_seen.insert(d.name).second) in_decls.push_back(d);
        };
        auto add_out = [&](const IODecl& d) {
            if (out_seen.insert(d.name).second) out_decls.push_back(d);
        };
        for (const auto& d : io_decls) {
            if (d.storage == 'i')      add_in(d);
            else if (d.storage == 'o') add_out(d);
        }
        if (pre)  for (auto& [k, v] : pre->output) {
            if (auto d = ParseIODecl(v); d) add_in(*d);
        }
        if (next) for (auto& [k, v] : next->input) {
            if (auto d = ParseIODecl(v); d) add_out(*d);
        }

        std::string synth;
        synth += "\n// === auto-generated GS stage I/O (HLSL) ===\n";
        synth += EmitGSHLSLStruct("WW_VSOut", std::move(in_decls));
        synth += EmitGSHLSLStruct("WW_PSIn",  std::move(out_decls));

        // Cross-stage uniform union as an HLSL cbuffer matching VS/FS UBO
        // layout (binding=0, set=0). std140 / column-major matches the
        // glslang GLSL-side block; uploader writes one buffer used by all
        // stages.
        Map<std::string, std::string> uniforms_union_local;
        if (! uniforms_union_in) {
            auto absorb = [&](const Map<std::string, std::string>& m) {
                for (const auto& [k, v] : m) uniforms_union_local.try_emplace(k, v);
            };
            absorb(unit.preprocess_info.uniforms);
            if (pre)  absorb(pre->uniforms);
            if (next) absorb(next->uniforms);
        }
        const Map<std::string, std::string>& uniforms_union =
            uniforms_union_in ? *uniforms_union_in : uniforms_union_local;
        if (! uniforms_union.empty()) {
            synth += "\n// === auto-generated shared uniforms (HLSL) ===\n";
            synth += "[[vk::binding(0, 0)]] cbuffer ww_Uniforms {\n";
            for (const auto& [name, ty] : uniforms_union) {
                std::string base_ty = ty;
                std::string array;
                if (auto pos = ty.find('['); pos != std::string::npos) {
                    base_ty = ty.substr(0, pos);
                    array   = ty.substr(pos);
                }
                std::string hlsl_ty = ToHLSLType(base_ty);
                // glslang's HLSL frontend defaults to row-major SPIR-V
                // decoration for cbuffer matrices regardless of
                // `#pragma pack_matrix`. Force column-major per member so
                // the GS reads the same matrix the VS/FS GLSL UBO sees.
                bool is_matrix =
                    hlsl_ty == "float2x2" || hlsl_ty == "float3x3" || hlsl_ty == "float4x4" ||
                    hlsl_ty == "float2x3" || hlsl_ty == "float2x4" || hlsl_ty == "float3x2" ||
                    hlsl_ty == "float3x4" || hlsl_ty == "float4x2" || hlsl_ty == "float4x3";
                if (is_matrix) synth += "    column_major ";
                else           synth += "    ";
                synth += hlsl_ty + " " + name + array + ";\n";
            }
            synth += "};\n";
        }

        body = RewriteGSMain(std::move(body));
        std::regex  re_hold(SHADER_PLACEHOLD.data());
        return std::regex_replace(body, re_hold, synth);
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

    // Synthesize the HLSL entry point: static globals for every attr /
    // varying, WW_VSIn/WW_VSOut/WW_PSIn structs, and a main_vs / main_ps
    // wrapper that copies between the struct and the statics.
    SynthOutput synth = SynthesizeHLSLEntry(unit.stage, attrs, varyings);

    // Cross-stage uniform union → single HLSL cbuffer at (set=0, binding=0).
    // Uses the global union from CompileToSpv when provided so VS / GS / FS
    // all see the same offsets (alphabetical, identical layout).
    Map<std::string, std::string> uniforms_union_local;
    if (! uniforms_union_in) {
        auto absorb = [&](const Map<std::string, std::string>& m) {
            for (const auto& [k, v] : m) uniforms_union_local.try_emplace(k, v);
        };
        absorb(unit.preprocess_info.uniforms);
        if (pre)  absorb(pre->uniforms);
        if (next) absorb(next->uniforms);
    }
    const Map<std::string, std::string>& uniforms_union =
        uniforms_union_in ? *uniforms_union_in : uniforms_union_local;

    std::string uniform_block;
    if (! uniforms_union.empty()) {
        uniform_block += "\n// === auto-generated shared uniforms (HLSL) ===\n";
        uniform_block += "[[vk::binding(0, 0)]] cbuffer ww_Uniforms {\n";
        for (const auto& [name, ty] : uniforms_union) {
            std::string base_ty = ty;
            std::string array;
            if (auto pos = ty.find('['); pos != std::string::npos) {
                base_ty = ty.substr(0, pos);
                array   = ty.substr(pos);
            }
            std::string hlsl_ty = ToHLSLType(base_ty);
            // glslang's HLSL frontend defaults to row-major SPIR-V decoration
            // for cbuffer matrices regardless of `#pragma pack_matrix`. Force
            // column-major per member so the C++ uploader's column-major data
            // is interpreted correctly.
            bool is_matrix =
                hlsl_ty == "float2x2" || hlsl_ty == "float3x3" || hlsl_ty == "float4x4" ||
                hlsl_ty == "float2x3" || hlsl_ty == "float2x4" || hlsl_ty == "float3x2" ||
                hlsl_ty == "float3x4" || hlsl_ty == "float4x2" || hlsl_ty == "float4x3";
            if (is_matrix) uniform_block += "    column_major ";
            else           uniform_block += "    ";
            uniform_block += hlsl_ty + " " + name + array + ";\n";
        }
        uniform_block += "};\n";
    }

    // Texture2D + paired SamplerState per stripped sampler. Bindings start
    // at 1 (binding 0 holds the ww_Uniforms cbuffer). `vk::combinedImageSampler`
    // marks the pair as a single descriptor on the Vulkan side.
    Set<std::string> sampler_seen;
    std::string      sampler_block;
    if (! sampler_decls.empty()) sampler_block += "\n// === auto-generated samplers (HLSL) ===\n";
    usize sampler_idx = 1;
    for (const auto& s : sampler_decls) {
        if (! sampler_seen.insert(s.name).second) continue;
        const char* tex_ty   = HLSLSamplerType(s.sampler_type);
        const char* state_ty = HLSLSamplerStateType(s.sampler_type);
        sampler_block += "[[vk::combinedImageSampler]][[vk::binding(" +
                         std::to_string(sampler_idx) + ", 0)]] " +
                         tex_ty + " " + s.name + ";\n";
        sampler_block += "[[vk::combinedImageSampler]][[vk::binding(" +
                         std::to_string(sampler_idx) + ", 0)]] " +
                         state_ty + " " + s.name + "_ww_sampler;\n";
        ++sampler_idx;
    }

    // Splice synth.pre into the placeholder slot, then append synth.post
    // (which contains the entry-point wrapper that has to follow the user's
    // shader_main()).
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
    // Expand `#include "FILE"` in place: replace each include line with its
    // resolved content (recursively expanded). Preserves the include's
    // original position so a `struct Grid { ... }; #include "common.h"`
    // pattern doesn't end up nesting the include's functions inside the
    // struct body. ParseWPShader still runs over the resolved include text
    // (for `// [COMBO]` / `uniform NAME // {json}` extraction) and over the
    // user source (sans include directives).
    std::string newsrc;
    newsrc.reserve(src.size());
    std::string all_includes;

    usize cursor = 0;
    while (true) {
        auto inc = src.find("#include", cursor);
        if (inc == std::string::npos) {
            newsrc.append(src, cursor, std::string::npos);
            break;
        }
        // Copy up to the include line.
        newsrc.append(src, cursor, inc - cursor);
        auto eol = src.find('\n', inc);
        if (eol == std::string::npos) eol = src.size();
        auto line = src.substr(inc, eol - inc);

        // Resolve this one include (recursively) and splice in.
        std::string expanded = LoadGlslInclude(vfs, line + "\n");
        newsrc.append(expanded);
        all_includes.append(expanded);

        cursor = eol;
    }

    ParseWPShader(all_includes, pWPShaderInfo, texinfos);
    ParseWPShader(newsrc, pWPShaderInfo, texinfos);

    return newsrc;
}

std::string WPShaderParser::PreShaderHeader(const std::string& src, const Combos& combos,
                                            ShaderType type) {
    // All stages route through glslang's HLSL frontend.
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
        // Build the cross-stage uniform union UP FRONT over ALL stages. Doing
        // this per-unit with just pre/next neighbours misses any uniform that
        // lives on a non-adjacent stage (e.g. FS-only `g_Brightness` not seen
        // by VS in a 3-stage VS→GS→FS chain), which results in different UBO
        // sizes per stage and the runtime allocating a buffer too small for
        // the longest stage.
        Map<std::string, std::string> uniforms_union;
        for (auto& unit : units) {
            for (const auto& [name, ty] : unit.preprocess_info.uniforms) {
                uniforms_union.try_emplace(name, ty);
            }
        }

        std::vector<vulkan::ShaderCompUnit> vunits(units.size());
        for (usize i = 0; i < units.size(); i++) {
            auto&               unit     = units[i];
            auto&               vunit    = vunits[i];
            WPPreprocessorInfo* pre_info = i >= 1 ? &units[i - 1].preprocess_info : nullptr;
            WPPreprocessorInfo* post_info =
                i + 1 < units.size() ? &units[i + 1].preprocess_info : nullptr;

            unit.src = Finalprocessor(unit, pre_info, post_info, &uniforms_union);

            vunit.src   = unit.src;
            vunit.stage = unit.stage;
            vunit.lang  = vulkan::SourceLang::Hlsl;
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
