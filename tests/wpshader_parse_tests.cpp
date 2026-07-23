// Unit coverage for the WE shader annotation collector
// (WPShaderParser_Pegtl.cpp). The collector is `#if`-agnostic by design —
// dead-branch GLSL stripping is glslang's job downstream. These tests assert
// the collector grabs annotations unconditionally and the comment/keyword
// handling resists obvious false positives.

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>

import rstd.cppstd;
import wescene.fs;
import wescene.pkg.parse;
import wescene.scene;
import wescene.types;

using owe::ParseWPShader;
using owe::WPShaderInfo;
using owe::WPShaderTexInfo;
using namespace rstd::literals;
using namespace rstd::prelude;

namespace
{

WPShaderInfo Parse(const std::string& src, std::size_t n_tex_slots = 8) {
    WPShaderInfo                 info {};
    std::vector<WPShaderTexInfo> texs(n_tex_slots);
    for (auto& t : texs) t.enabled = true;
    ParseWPShader(src, &info, texs);
    return info;
}

} // namespace

// --- annotation collection: unconditional ----------------------------------

TEST(WPShaderParser, TextureDefaultCollectedRegardlessOfIfdef) {
    // Mirrors `pulse.frag` / `genericimage2.frag` shape: the texture uniform
    // sits inside `#if SOME_COMBO`, and SOME_COMBO is itself derived from
    // the uniform's own `combo:` annotation. Collector must not gate on #if.
    const std::string src  = R"(
#if LIGHTS_SHADOW_MAPPING
uniform sampler2D g_Texture6; // {"hidden":true,"default":"_rt_shadowAtlas"}
#endif
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.defTexs.size(), 1u);
    EXPECT_EQ(info.defTexs[0].first, 6);
    EXPECT_EQ(info.defTexs[0].second, "_rt_shadowAtlas");
}

TEST(WPShaderParser, TextureComboFlagSetRegardlessOfIfdef) {
    // The combo flag on a texture binding is the chicken in the
    // chicken-and-egg. With 8 slots all enabled, slot 2 is bound, so MASK=1.
    const std::string src  = R"(
#if MASK == 1
uniform sampler2D g_Texture2; // {"material":"mask","combo":"MASK","default":"util/white"}
#endif
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.combos.at("MASK"), "1");
}

TEST(WPShaderParser, ComboLineCollectedRegardlessOfIfdef) {
    const std::string src  = R"(
#if 0
// [COMBO] {"combo":"NEVER","default":1}
#endif
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.combos.at("NEVER"), "1");
}

// --- comment / false-positive handling -------------------------------------

TEST(WPShaderParser, LineCommentUniformNotCollected) {
    // A line that begins with `//` is a comment, even if it contains the
    // word `uniform`. Legacy substring scanner false-positived this.
    const std::string src  = R"(
// uniform vec4 g_Foo; // {"default":42}
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.svs.count("g_Foo"), 0u);
}

TEST(WPShaderParser, BlockCommentUniformNotCollected) {
    const std::string src  = R"(
/*
uniform vec4 g_Y; // {"default":1}
*/
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.svs.count("g_Y"), 0u);
}

TEST(WPShaderParser, BlockCommentDoesNotEatLaterUniforms) {
    // Make sure the block-comment pre-strip terminates at the closing `*/`
    // and lets real declarations through.
    const std::string src  = R"(
/* unused */
uniform float g_Real; // {"default":2.5}
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.svs.count("g_Real"), 1u);
}

// --- combo + uniform schema -------------------------------------------------

TEST(WPShaderParser, ComboDefaultIsRecorded) {
    const std::string src  = R"(
// [COMBO] {"combo":"BLENDMODE","default":9}
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.combos.at("BLENDMODE"), "9");
}

TEST(WPShaderParser, ComboMaterialKeyIsRecorded) {
    const std::string src  = R"(
// [COMBO] {"material":"toggle","combo":"USE_FEATURE","default":1}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.combo_defs.len(), usize(1));
    EXPECT_EQ(info.combo_defs[usize()].material, "toggle"_str);
    EXPECT_EQ(info.combo_defs[usize()].combo, "USE_FEATURE"_str);
}

TEST(WPShaderParser, ComboOptionsUseOwnedAnnotationKeys) {
    const std::string src  = R"(
// [COMBO] {"material":"quality","combo":"QUALITY","type":"options","options":{"Low":0,"High":2}}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.combo_defs.len(), usize(1));
    auto high = info.combo_defs[usize()].options.get("High"_str);
    ASSERT_TRUE(high.is_some());
    EXPECT_EQ(**high, i32(2));
}

TEST(WPShaderParser, ScalarDefaultPushedToSvs) {
    const std::string src  = R"(
uniform float g_Brightness; // {"material":"brightness","default":1.5,"range":[0,10]}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.svs.count("g_Brightness"), 1u);
    EXPECT_EQ(info.alias.at("brightness"), "g_Brightness");
}

TEST(WPShaderParser, ScalarAnnotationAcceptsLeadingZeroRangeNumber) {
    const std::string src  = R"(
uniform float u_userSpeed; // {"material":"Speed","default":1,"range":[0,01]}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.svs.count("u_userSpeed"), 1u);
    EXPECT_EQ(info.alias.at("Speed"), "u_userSpeed");
}

TEST(WPShaderParser, TextureAliasRecorded) {
    const std::string src  = R"(
uniform sampler2D g_Texture0; // {"material":"albedo","label":"Albedo","default":"util/white"}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.defTexs.size(), 1u);
    EXPECT_EQ(info.defTexs[0].first, 0);
    EXPECT_EQ(info.defTexs[0].second, "util/white");
    EXPECT_EQ(info.alias.at("albedo"), "g_Texture0");
}

TEST(WPShaderParser, TextureBoundIfSlotEnabled) {
    // Slot 0 in texinfos is enabled by default in Parse(); the texture-side
    // combo flag therefore reads "1".
    const std::string src  = R"(
uniform sampler2D g_Texture0; // {"combo":"HASTEX","default":"util/white"}
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.combos.at("HASTEX"), "1");
}

TEST(WPShaderParser, UndefsBuiltinMacroBeforeUserRedefine) {
    const std::string out = owe::WPShaderParser::PreShaderHeader(
        "#define M_PI_2 1.57079632679\nfloat f() { return M_PI_2; }\n",
        {},
        owe::ShaderType::FRAGMENT);

    const auto undef_pos  = out.find("#undef M_PI_2");
    const auto define_pos = out.find("#define M_PI_2 1.57079632679");
    ASSERT_NE(undef_pos, std::string::npos);
    ASSERT_NE(define_pos, std::string::npos);
    EXPECT_LT(undef_pos, define_pos);
}

TEST(WPShaderParser, PreShaderHeaderFlattensPackedAudioSpectrumAccess) {
    const std::string out = owe::WPShaderParser::PreShaderHeader(
        R"(
uniform float g_AudioSpectrum64Left[64];
float sample(float barID) {
    return g_AudioSpectrum64Left[barID / 4][barID % 4];
}
)",
        {},
        owe::ShaderType::FRAGMENT);

    EXPECT_EQ(out.find("g_AudioSpectrum64Left[barID / 4][barID % 4]"), std::string::npos);
    EXPECT_NE(out.find("g_AudioSpectrum64Left[(int)(barID)]"), std::string::npos);
}

TEST(WPShaderParser, PreShaderHeaderTransposesLocalMatrixConstructorMul) {
    const std::string out = owe::WPShaderParser::PreShaderHeader(
        R"(
vec2 rotate(vec2 uv, float th) {
    return mul(uv, mat2(cos(th), sin(th), -sin(th), cos(th)));
}
)",
        {},
        owe::ShaderType::FRAGMENT);

    EXPECT_NE(out.find("mul(uv, transpose(mat2(cos(th), sin(th), -sin(th), cos(th))))"),
              std::string::npos);
}

TEST(WPShaderParser, CommonPerspectiveIncludeCompensatesLocalMatrixMul) {
    auto root =
        std::filesystem::temp_directory_path() /
        ("owe-wpshader-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "shaders");

    {
        std::ofstream out(root / "shaders" / "common_perspective.h");
        out << R"(
mat3 squareToQuad(vec2 p0, vec2 p1, vec2 p2, vec2 p3) {
	mat3 m = mat3(1.0);
	if (p0.x == p1.x) {
		return m;
	}
	return m;
}
)";
    }

    owe::fs::VFS vfs;
    auto         physical = owe::fs::make_physical_fs(owe::fs::ToPath(root.string()));
    ASSERT_TRUE(physical.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(physical).unwrap_unchecked()).is_ok());

    const std::string out = owe::WPShaderParser::PreShaderSrc(
        vfs, "#include \"common_perspective.h\"\nvoid main(){}\n", nullptr, {});

    EXPECT_NE(out.find("_ww_perspective_mat"), std::string::npos);
    EXPECT_NE(out.find("return _ww_perspective_mat(m);"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(WPShaderParser, CompileSceneShaderVariantRejectsInvalidDescriptor) {
    owe::fs::VFS vfs;

    const auto result = owe::WPShaderParser::CompileSceneShaderVariant({}, vfs);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.shader);
    EXPECT_FALSE(result.error.empty());
}

TEST(WPShaderParser, CompileSceneShaderVariantAcceptsPackedAudioSpectrumAccess) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "packed-audio-spectrum-test";
    desc.shader_name = "packed-audio-spectrum-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/packed-audio-spectrum-test.vert",
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/packed-audio-spectrum-test.frag",
        .source     = R"(
varying vec2 v_TexCoord;
uniform float g_AudioSpectrum64Left[64];
void main() {
    float barID = v_TexCoord.x * 8.0;
    float value = g_AudioSpectrum64Left[barID / 4][barID % 4];
    gl_FragColor = vec4(value, value, value, 1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::WPShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    ASSERT_EQ(result.shader->codes.size(), 2u);
    EXPECT_FALSE(result.shader->codes[1].empty());
}

TEST(WPShaderParser, CompileSceneShaderVariantUsesPhysicalFileCache) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("owe-shader-cache-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(root);

    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "physical-cache-test";
    desc.shader_name = "physical-cache-test";
    desc.texture_infos.resize(1);
    desc.texture_infos[0].enabled = true;
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/physical-cache-test.vert",
        .source     = R"(
attribute vec3 a_Position;
void main() {
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/physical-cache-test.frag",
        .source     = R"(
uniform float g_Brightness;
uniform sampler2D g_Texture0;
void main() {
    gl_FragColor = texSample2D(g_Texture0, vec2(0.5)) * g_Brightness;
}
)",
    });

    const auto   cache_text = root.string();
    const auto   cache_path = rstd::path::PathBuf::from(rstd::cppstd::as_str(cache_text).unwrap());
    owe::fs::VFS vfs;
    const auto   first =
        owe::WPShaderParser::CompileSceneShaderVariant(desc, vfs, {}, Some(cache_path.as_path()));
    ASSERT_TRUE(first.ok) << first.error;
    ASSERT_TRUE(first.shader);

    const auto shader_cache = root / desc.scene_id / "spvs03";
    ASSERT_TRUE(std::filesystem::is_directory(shader_cache));
    const auto files = std::filesystem::directory_iterator(shader_cache);
    ASSERT_NE(files, std::filesystem::directory_iterator {});
    const auto artifact_path = files->path();
    EXPECT_EQ(artifact_path.extension(), ".spvs");
    EXPECT_GT(files->file_size(), 112u);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(shader_cache),
                            std::filesystem::directory_iterator {}),
              1);

    std::array<unsigned char, 28> header {};
    {
        std::ifstream artifact_file(artifact_path, std::ios::binary);
        ASSERT_TRUE(artifact_file.read(reinterpret_cast<char*>(header.data()), header.size()));
    }
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(header.data()), 8),
              std::string("OWESPV3\0", 8));
    const auto read_u32 = [&header](std::size_t offset) {
        return static_cast<std::uint32_t>(header[offset]) |
               (static_cast<std::uint32_t>(header[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(header[offset + 2]) << 16) |
               (static_cast<std::uint32_t>(header[offset + 3]) << 24);
    };
    EXPECT_EQ(read_u32(8), 3u);
    EXPECT_EQ(read_u32(12), 1u);
    EXPECT_EQ(read_u32(16), 112u);
    EXPECT_EQ(read_u32(24), 2u);
    const auto initial_write_time = std::filesystem::last_write_time(artifact_path);

    const auto second =
        owe::WPShaderParser::CompileSceneShaderVariant(desc, vfs, {}, Some(cache_path.as_path()));
    ASSERT_TRUE(second.ok) << second.error;
    ASSERT_TRUE(second.shader);
    EXPECT_EQ(second.shader->codes, first.shader->codes);
    EXPECT_TRUE(second.variant.stages[1].uniforms.contains("g_Brightness"));
    EXPECT_TRUE(second.variant.stages[1].active_texture_slots.contains(0));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(shader_cache),
                            std::filesystem::directory_iterator {}),
              1);
    EXPECT_EQ(std::filesystem::last_write_time(artifact_path), initial_write_time);

    std::filesystem::resize_file(artifact_path, 16);
    const auto after_truncation =
        owe::WPShaderParser::CompileSceneShaderVariant(desc, vfs, {}, Some(cache_path.as_path()));
    ASSERT_TRUE(after_truncation.ok) << after_truncation.error;
    ASSERT_TRUE(after_truncation.shader);
    EXPECT_EQ(after_truncation.shader->codes, first.shader->codes);
    EXPECT_GT(std::filesystem::file_size(artifact_path), 112u);

    {
        std::fstream artifact(artifact_path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(artifact.seekg(52));
        char byte = 0;
        ASSERT_TRUE(artifact.read(&byte, 1));
        byte ^= 1;
        ASSERT_TRUE(artifact.seekp(52));
        ASSERT_TRUE(artifact.write(&byte, 1));
    }
    const auto after_identity_corruption =
        owe::WPShaderParser::CompileSceneShaderVariant(desc, vfs, {}, Some(cache_path.as_path()));
    ASSERT_TRUE(after_identity_corruption.ok) << after_identity_corruption.error;
    ASSERT_TRUE(after_identity_corruption.shader);
    EXPECT_EQ(after_identity_corruption.shader->codes, first.shader->codes);

    {
        std::fstream artifact(artifact_path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(artifact.seekg(-1, std::ios::end));
        char byte = 0;
        ASSERT_TRUE(artifact.read(&byte, 1));
        byte ^= 1;
        ASSERT_TRUE(artifact.seekp(-1, std::ios::end));
        ASSERT_TRUE(artifact.write(&byte, 1));
    }
    const auto after_payload_corruption =
        owe::WPShaderParser::CompileSceneShaderVariant(desc, vfs, {}, Some(cache_path.as_path()));
    ASSERT_TRUE(after_payload_corruption.ok) << after_payload_corruption.error;
    ASSERT_TRUE(after_payload_corruption.shader);
    EXPECT_EQ(after_payload_corruption.shader->codes, first.shader->codes);

    const auto different_combo = owe::WPShaderParser::CompileSceneShaderVariant(
        desc, vfs, { { "CACHE_VARIANT", "1" } }, Some(cache_path.as_path()));
    ASSERT_TRUE(different_combo.ok) << different_combo.error;
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(shader_cache),
                            std::filesystem::directory_iterator {}),
              2);

    desc.stages[1].source += "\n// source identity variant";
    const auto different_source =
        owe::WPShaderParser::CompileSceneShaderVariant(desc, vfs, {}, Some(cache_path.as_path()));
    ASSERT_TRUE(different_source.ok) << different_source.error;
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(shader_cache),
                            std::filesystem::directory_iterator {}),
              3);
    for (const auto& entry : std::filesystem::directory_iterator(shader_cache)) {
        EXPECT_EQ(entry.path().extension(), ".spvs");
    }

    std::filesystem::remove_all(root);
}

TEST(WPShaderParser, CompileSceneShaderVariantExportsSamplerBindings) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "sampler-binding-test";
    desc.shader_name = "sampler-binding-test";
    desc.texture_infos.resize(4);
    desc.texture_infos[3].enabled = true;
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/sampler-binding-test.vert",
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/sampler-binding-test.frag",
        .source     = R"(
varying vec2 v_TexCoord;
uniform sampler2D g_Texture3;
void main() {
    gl_FragColor = texSample2D(g_Texture3, v_TexCoord);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::WPShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    ASSERT_EQ(result.variant.sampler_bindings.size(), 1u);
    EXPECT_EQ(result.variant.sampler_bindings[0].texture_slot, 3u);
    EXPECT_EQ(result.variant.sampler_bindings[0].shader_member, "g_Texture3");
    EXPECT_EQ(result.shader->SamplerMember(3), "g_Texture3");
}

TEST(WPShaderParser, CompileSceneShaderVariantUsesDescriptorAndComboOverride) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id        = "variant-test";
    desc.shader_name     = "variant-test";
    desc.resolved_combos = { { "USE_COLOR", "0" } };
    desc.texture_infos.push_back(owe::SceneShaderTextureCompileInfo { .enabled = false });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/variant-test.vert",
        .source     = R"(
// [COMBO] {"combo":"USE_COLOR","default":0}
attribute vec3 a_Position;
varying vec4 v_Color;
void main() {
    gl_Position = vec4(a_Position, 1.0);
#if USE_COLOR == 1
    v_Color = vec4(1.0, 0.0, 0.0, 1.0);
#else
    v_Color = vec4(0.0, 1.0, 0.0, 1.0);
#endif
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/variant-test.frag",
        .source     = R"(
varying vec4 v_Color;
uniform float g_Brightness; // {"material":"brightness","default":1.0,"range":[0,2]}
void main() {
    gl_FragColor = v_Color * g_Brightness;
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result =
        owe::WPShaderParser::CompileSceneShaderVariant(desc, vfs, { { "USE_COLOR", "1" } });

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    EXPECT_EQ(result.shader->name, "variant-test");
    ASSERT_EQ(result.shader->codes.size(), 2u);
    EXPECT_FALSE(result.shader->codes[0].empty());
    EXPECT_EQ(result.variant.resolved_combos.at("USE_COLOR"), "1");
    EXPECT_EQ(result.variant.input_combos.at("USE_COLOR"), "1");
    EXPECT_EQ(result.variant.uniform_aliases.at("brightness"), "g_Brightness");
    EXPECT_TRUE(result.variant.default_uniforms.contains("g_Brightness"));
    EXPECT_NE(result.variant.descriptor_layout_hash, 0u);
    ASSERT_EQ(result.variant.stages.size(), 2u);
    EXPECT_EQ(result.variant.stages[0].source_key, "/assets/shaders/variant-test.vert");
    EXPECT_NE(result.variant.stages[0].code_hash, rstd::usize());
    EXPECT_TRUE(result.variant.stages[1].uniforms.contains("g_Brightness"));
    EXPECT_NE(result.variant.stages[1].code_hash, rstd::usize());
}
