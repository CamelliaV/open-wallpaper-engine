#include <gtest/gtest.h>

import rstd.cppstd;
import nlohmann.json;
import wavsen.audio;
import wescene.fs;
import wescene.pkg_fs;
import wescene.pkg.parse;
import wescene.pkg.scene_obj;
import wescene.scene;
import wescene.scene_uniform_updater;
import wescene.script;

namespace
{

void CollectNodesById(const std::shared_ptr<owe::SceneNode>&               root,
                      std::int32_t                                         id,
                      std::vector<std::shared_ptr<owe::SceneNode>>&        out) {
    if (! root) return;
    if (root->ID() == id) out.push_back(root);
    for (const auto& child : root->GetChildren()) {
        CollectNodesById(child, id, out);
    }
}

std::shared_ptr<owe::Scene> LoadWorkshopScene(std::string_view id) {
    const std::filesystem::path workshop_dir = std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / id;
    const std::filesystem::path pkg_path     = workshop_dir / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) return nullptr;

    owe::fs::VFS vfs;
    if (auto assets_fs = owe::fs::CreatePhysicalFs(WAYWALLEN_ASSETS_DIR)) {
        if (! vfs.Mount("/assets", std::move(assets_fs))) return nullptr;
    }
    auto pkg_fs = owe::fs::WPPkgFs::CreatePkgFs(pkg_path.string());
    if (! pkg_fs) return nullptr;
    if (! vfs.Mount("/assets", std::move(pkg_fs))) return nullptr;

    auto doc = owe::wpscene::LoadSceneDocumentFromPkg(pkg_path.string());
    if (! doc) return nullptr;

    std::unordered_map<std::string, nlohmann::json> user_props;
    const auto                                      project_path = workshop_dir / "project.json";
    if (std::filesystem::exists(project_path)) {
        std::ifstream in(project_path);
        nlohmann::json project = nlohmann::json::parse(in, nullptr, false);
        if (! project.is_discarded() && project.contains("general") &&
            project.at("general").contains("properties") &&
            project.at("general").at("properties").is_object()) {
            const auto& props = project.at("general").at("properties");
            for (auto it = props.begin(); it != props.end(); ++it) {
                user_props[it.key()] = it.value();
            }
        }
    }

    wavsen::audio::SoundManager sm;
    owe::WPSceneParser          parser;
    parser.SetUserProperties(&user_props);
    return parser.Parse(std::string(id), *doc, vfs, sm);
}

} // namespace

TEST(SceneRuntimeScripts, EarthWorkshopMenuAlphaDefaultsHidden) {
    auto scene = LoadWorkshopScene("3557068717");
    if (! scene) GTEST_SKIP() << "workshop 3557068717 is not available";

    owe::script::FrameInputs fi {};
    fi.runtime = 8.0f;
    owe::script::TickSceneScripts(*scene, fi);

    for (std::int32_t id : { 275, 279, 283, 291, 301, 260, 288, 297, 276, 158, 313,
                             309, 307, 315, 317, 319, 342, 346, 331, 413, 414, 415,
                             416 }) {
        std::vector<std::shared_ptr<owe::SceneNode>> nodes;
        CollectNodesById(scene->sceneGraph, id, nodes);
        ASSERT_FALSE(nodes.empty()) << "missing menu node " << id;

        bool hidden_output = false;
        for (const auto& node : nodes) {
            if (node->IsAlphaOverridden() && node->EffectiveAlpha() == 0.0f) {
                hidden_output = true;
                break;
            }
        }
        EXPECT_TRUE(hidden_output) << "menu node " << id << " should hide";
    }
}

TEST(SceneUniformUpdaterRuntimeAlpha, Color4OnlyShaderUsesBaseColorAndRuntimeAlpha) {
    owe::Scene       scene;
    owe::SceneNode   node;
    owe::sprite_map_t sprites;

    auto camera_node = std::make_shared<owe::SceneNode>();
    auto camera      = std::make_shared<owe::SceneCamera>(1920, 1080, -1.0, 1.0);
    camera->AttatchNode(camera_node);
    scene.cameras["default"] = camera;
    scene.activeCamera       = camera.get();

    auto mesh = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    node.AddMesh(mesh);
    node.SetBaseColor({ 0.25f, 0.5f, 0.75f }, 0.8f);
    node.SetUserAlpha(0.125f);

    owe::SceneUniformUpdater updater(&scene);
    updater.InitUniforms(&node, [](std::string_view name) {
        return name == "g_Color4";
    });

    std::unordered_map<std::string, owe::ShaderValue> values;
    updater.UpdateUniforms(&node, sprites, [&](std::string_view name, owe::ShaderValue value) {
        values[std::string(name)] = value;
    });

    ASSERT_EQ(values.count("g_Color4"), 1u);
    const auto& color = values.at("g_Color4");
    ASSERT_EQ(color.size(), 4u);
    EXPECT_FLOAT_EQ(color[0], 0.25f);
    EXPECT_FLOAT_EQ(color[1], 0.5f);
    EXPECT_FLOAT_EQ(color[2], 0.75f);
    EXPECT_FLOAT_EQ(color[3], 0.125f);
    EXPECT_EQ(values.count("g_UserAlpha"), 0u);
}
