// scene.json parse regression net.
//
// One TEST_P per observed PKGV version. For each, every workshop with
// that version is re-opened, scene.json read via VFS, and parsed via the
// canonical version-bearing SceneMetadata::FromJson(json, pkg_version) path.
// Establishes the baseline for the upcoming SceneMetadata refactor that splits
// FromJson by version: a regression here means the refactor changed
// what was previously parseable.
//
// Deliberately does not depend on Corpus / DumpWorkshop — those exercise
// WPMdlParser / WPTexImageParser too, which can hit unrelated assertions
// on rare .mdl inputs and would mask scene.json regressions.

#include <gtest/gtest.h>

#include <cmath>

import rstd.cppstd;
import rstd;
import wavsen.audio;
import wescene.fs;
import wescene.json;
import wescene.pkg.parse;
import wescene.pkg.scene_obj;
import wescene.scene;
import wescene.testing.scene_parse_probe;
import wescene.types;

using namespace rstd::literals;

TEST(FieldBindingJson, CompatibilityReaderPopulatesAnimationMetadata) {
    auto parsed = owe::ParseJson(R"({"enabled":true,"x":{"value":1.5},"y":-2.0,"magic":7})");
    ASSERT_TRUE(parsed.is_ok());

    owe::wpscene::AnimKeyframeTangent tangent;
    ASSERT_TRUE(owe::wpscene::ParseAnimKeyframeTangent(parsed.unwrap(), tangent));
    EXPECT_TRUE(tangent.enabled);
    EXPECT_FLOAT_EQ(tangent.x, 1.5f);
    EXPECT_FLOAT_EQ(tangent.y, -2.0f);
    EXPECT_EQ(tangent.magic, 7);
}

TEST(SceneObjectClone, MembersProvideCloneTraitImplementation) {
    owe::wpscene::AnimCurve curve;
    curve.relative = true;
    curve.c0.push_back({ .frame = 3, .value = 1.5f });

    auto direct_curve = curve.clone();
    auto trait_curve  = rstd::as<rstd::clone::Clone>(curve).clone();
    ASSERT_EQ(direct_curve.c0.size(), 1u);
    EXPECT_FLOAT_EQ(trait_curve.c0[0].value, 1.5f);

    owe::wpscene::Material material;
    material.shader      = "generic";
    auto direct_material = material.clone();
    auto trait_material  = rstd::as<rstd::clone::Clone>(material).clone();
    EXPECT_EQ(direct_material.shader, "generic");
    EXPECT_EQ(trait_material.shader, "generic");
}

TEST(ObjectInstanceJson, AppliesMaterialBindingOverridesBySlot) {
    auto parsed = owe::ParseJson(R"({
        "textures": [null, "linked"],
        "usertextures": [null, "replacement"],
        "combos": {"version": 2}
    })");
    ASSERT_TRUE(parsed.is_ok());

    owe::wpscene::ObjectInstance instance;
    ASSERT_TRUE(instance.FromJson(parsed.unwrap()));
    owe::wpscene::Material material;
    material.textures = { "base-0", "base-1" };
    instance.ApplyTo(material);

    ASSERT_EQ(material.textures.size(), 2u);
    EXPECT_EQ(material.textures[0], "base-0");
    EXPECT_EQ(material.textures[1], "linked");
    ASSERT_EQ(material.usertextures.len(), rstd::usize(2));
    EXPECT_TRUE(material.usertextures[rstd::usize()].is_null());
    ASSERT_TRUE(material.usertextures[rstd::usize(1)].is_string());
    EXPECT_EQ(rstd::cppstd::to_string(*material.usertextures[rstd::usize(1)].as_str()),
              "replacement");
    EXPECT_EQ(material.combos.at("version"), 2);
}

TEST(TextObjectJson, ReadsDirectUserValueBinding) {
    auto parsed = owe::ParseJson(R"({"text":{"user":"title","value":"default"}})");
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS             vfs;
    owe::wpscene::TextObject text;
    ASSERT_TRUE(text.FromJson(parsed.unwrap(), vfs));
    EXPECT_EQ(text.text_user.name, "title");
    EXPECT_TRUE(text.reflected);
}

TEST(TextObjectJson, ReadsReflectionParticipation) {
    auto parsed = owe::ParseJson(R"({"reflected":false})");
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS             vfs;
    owe::wpscene::TextObject text;
    ASSERT_TRUE(text.FromJson(parsed.unwrap(), vfs));
    EXPECT_FALSE(text.reflected);
}

TEST(TextRenderMode, UsesDirectRenderingOnlyWithoutIndependentSurfaceRequirements) {
    EXPECT_EQ(owe::ResolveTextRenderMode({}), owe::TextRenderMode::Direct);
    EXPECT_EQ(owe::ResolveTextRenderMode({ .has_effect = true }), owe::TextRenderMode::Offscreen);
    EXPECT_EQ(owe::ResolveTextRenderMode({ .copy_background = true }),
              owe::TextRenderMode::Offscreen);
    EXPECT_EQ(owe::ResolveTextRenderMode({ .opaque_background = true }),
              owe::TextRenderMode::Offscreen);
    EXPECT_EQ(owe::ResolveTextRenderMode({ .linked_source = true }),
              owe::TextRenderMode::Offscreen);
}

TEST(SceneObjectExpansion, PreservesHiddenTextLayers) {
    auto parsed = owe::ParseJson(R"({
        "objects": [{
            "id": 7,
            "name": "Style1",
            "text": "progress",
            "visible": false
        }]
    })");
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS vfs;
    auto objects = owe::ExpandObjects(parsed.unwrap(), vfs, owe::wpscene::kSceneVersionUnknown);

    ASSERT_EQ(objects.len(), rstd::usize(1));
    ASSERT_TRUE(objects[rstd::usize()].is_Text());
    EXPECT_FALSE(objects[rstd::usize()].as_Text().value.visible);
}

TEST(SceneObjectExpansion, ShapeOwnsItsWallpaperLayerIdentity) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 42,
                "name": "Direct Draw Shape",
                "shape": "quad",
                "origin": [0.0, 0.0, 0.0],
                "angles": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "effects": [{
                    "file": "effects/lightshafts/effect.json",
                    "visible": true,
                    "passes": [{
                        "combos": {"DIRECTDRAW": 1, "RAYMODE": 1},
                        "constantshadervalues": {
                            "point0": "-1.0 -1.0",
                            "point1": "-1.0 1.0",
                            "point2": "1.0 1.0",
                            "point3": "1.0 -1.0"
                        }
                    }]
                }]
            }]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.has_value());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    auto effect_assets = owe::fs::make_physical_fs(
        owe::fs::ToPath(std::string(WAYWALLEN_ASSETS_DIR) + "/effects/lightshafts"));
    ASSERT_TRUE(effect_assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(effect_assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::WPSceneParser          parser;
    auto                        parsed = parser.Parse(
        "shape-layer-identity"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene = rstd::move(parsed).unwrap();
    auto shape = scene.scene->RootMut()->FindByName("Direct Draw Shape");
    ASSERT_NE(shape, nullptr);
    ASSERT_TRUE(shape->WallpaperIdentity().is_some());
    EXPECT_EQ(shape->WallpaperIdentity()->value, rstd::i32(42));
}

TEST(ImageColorBlendParsing, LinearDodgeUsesAdditiveAttachmentOwner) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 31,
                "name": "Linear Dodge",
                "image": "models/util/fullscreenlayer.json",
                "colorBlendMode": 31,
                "visible": true
            }]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.has_value());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::WPSceneParser          parser;
    auto                        parsed = parser.Parse(
        "linear-dodge-owner"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene = rstd::move(parsed).unwrap();
    auto node  = scene.scene->RootMut()->FindByName("Linear Dodge");
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->Mesh(), nullptr);
    ASSERT_NE(node->Mesh()->Material(), nullptr);
    EXPECT_EQ(node->Mesh()->Material()->blenmode, owe::BlendMode::Additive);
    ASSERT_TRUE(node->Mesh()->Material()->customShader.variant.has_value());
    EXPECT_EQ(node->Mesh()->Material()->customShader.variant->input_combos.at("SCENE_ORTHO"), "1");
    EXPECT_EQ(node->Mesh()->Material()->customShader.variant->input_combos.at("OWE_IMAGE_LAYER"),
              "1");
}

TEST(SceneLightParsing, RecognizesPrefixedKindsAndFullConeAngles) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 1,
                "name": "Spot",
                "light": "lspot",
                "origin": [0.0, 0.0, 0.0],
                "angles": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "innercone": 72.29,
                "outercone": 77.94
            }, {
                "id": 2,
                "name": "Directional",
                "light": "ldirectional",
                "origin": [0.0, 0.0, 0.0],
                "angles": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0]
            }]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.has_value());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::WPSceneParser          parser;
    auto                        parsed = parser.Parse(
        "prefixed-light-kinds"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene  = rstd::move(parsed).unwrap();
    auto lights = scene.scene->Lights();
    ASSERT_EQ(lights.len(), rstd::usize(2));
    EXPECT_EQ(lights[rstd::usize()]->type(), owe::SceneLightType::Spot);
    EXPECT_EQ(lights[rstd::usize(1)]->type(), owe::SceneLightType::Directional);
    const float deg_to_rad = rstd::f32::consts::PI.to_primitive() / 180.0f;
    EXPECT_NEAR(lights[rstd::usize()]->desc().inner_cone_cos, std::cos(72.29f * deg_to_rad), 1e-5f);
    EXPECT_NEAR(lights[rstd::usize()]->desc().outer_cone_cos, std::cos(77.94f * deg_to_rad), 1e-5f);
}

TEST(ModelObjectJson, ReadsMaterialSkin) {
    auto parsed = owe::ParseJson(R"({"model":"models/prism.mdl","skin":2})");
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS              vfs;
    owe::wpscene::ModelObject model;
    ASSERT_TRUE(model.FromJson(parsed.unwrap(), vfs));
    EXPECT_EQ(model.skin, 2u);
}

TEST(ImageEffectJson, FailedEffectsAreAbsentFromParsedObjects) {
    auto image_json = owe::ParseJson(R"({
        "image": "models/util/fullscreenlayer.json",
        "effects": [
            {"file": "effects/_empty/effect.json", "visible": true},
            {"file": "effects/missing/effect.json", "visible": true}
        ]
    })");
    auto shape_json = owe::ParseJson(R"({
        "shape": "rectangle",
        "origin": [0.0, 0.0, 0.0],
        "angles": [0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0],
        "effects": [
            {"file": "effects/_empty/effect.json", "visible": true},
            {"file": "effects/missing/effect.json", "visible": true}
        ]
    })");
    ASSERT_TRUE(image_json.is_ok());
    ASSERT_TRUE(shape_json.is_ok());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    auto effect_assets = owe::fs::make_physical_fs(
        owe::fs::ToPath(std::string(WAYWALLEN_ASSETS_DIR) + "/effects/_empty"));
    ASSERT_TRUE(effect_assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(effect_assets).unwrap_unchecked()).is_ok());

    owe::wpscene::ImageObject image;
    ASSERT_TRUE(image.FromJson(image_json.unwrap(), vfs));
    ASSERT_EQ(image.effects.size(), 1u);
    EXPECT_FALSE(image.effects[0].materials.empty());

    owe::wpscene::ShapeObject shape;
    ASSERT_TRUE(shape.FromJson(shape_json.unwrap(), vfs, owe::wpscene::kSceneVersionUnknown));
    ASSERT_EQ(shape.effects.size(), 1u);
    EXPECT_FALSE(shape.effects[0].materials.empty());
}

namespace
{

const std::vector<owe::testing::WorkshopProbe>& AllWorkshopProbes() {
    static const auto v = owe::testing::EnumerateWorkshopProbes(WAYWALLEN_WORKSHOP_DIR);
    return v;
}

const std::vector<std::string>& ObservedPkgVersionStamps() {
    static const auto v = [] {
        std::set<std::string> uniq;
        for (const auto& p : AllWorkshopProbes())
            if (! p.pkg_stamp.empty()) uniq.insert(p.pkg_stamp);
        return std::vector<std::string>(uniq.begin(), uniq.end());
    }();
    return v;
}

} // namespace

class ScenePkgVersionParseTest : public ::testing::TestWithParam<std::string> {};

TEST_P(ScenePkgVersionParseTest, AllWorkshopsParseAtExplicitVersion) {
    const auto& stamp = GetParam();
    std::size_t hits  = 0;
    for (const auto& p : AllWorkshopProbes()) {
        if (p.pkg_stamp != stamp) continue;
        ++hits;
        SCOPED_TRACE("workshop " + p.id + " " + p.pkg_stamp);

        auto r = owe::testing::ProbeSceneParse(p.dir);
        EXPECT_TRUE(r.ok) << "ProbeSceneParse failed: " << r.error;
        EXPECT_EQ(r.pkg_version, p.pkg_version) << "pkg_version mismatch for " << p.id;
    }
    EXPECT_GT(hits, 0u) << "no workshops for " << stamp;
}

INSTANTIATE_TEST_SUITE_P(All, ScenePkgVersionParseTest,
                         ::testing::ValuesIn(ObservedPkgVersionStamps()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             return info.param;
                         });

TEST(SceneParseSmoke, EnumeratesNonEmpty) {
    EXPECT_FALSE(AllWorkshopProbes().empty());
    EXPECT_FALSE(ObservedPkgVersionStamps().empty());
}
