#include <gtest/gtest.h>

import rstd.cppstd;
import rstd;
import eigen;
import nlohmann.json;
import wavsen.audio;
import wescene.fs;
import wescene.pkg_fs;
import wescene.pkg.parse;
import wescene.pkg.scene_obj;
import wescene.scene;
import wescene.scene_uniform_updater;
import wescene.spec_texs;
import wescene.script;

namespace
{

void CollectNodesById(const rstd::sync::Arc<owe::SceneNode>& root, std::int32_t id,
                      std::vector<owe::SceneNode*>& out) {
    if (root->ID() == id) out.push_back(root.as_ptr());
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
        std::ifstream  in(project_path);
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

    for (std::int32_t id : { 275, 279, 283, 291, 301, 260, 288, 297, 276, 158, 313, 309,
                             307, 315, 317, 319, 342, 346, 331, 413, 414, 415, 416 }) {
        std::vector<owe::SceneNode*> nodes;
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
    owe::Scene        scene;
    owe::SceneNode    node;
    owe::sprite_map_t sprites;

    auto camera_node = rstd::sync::Arc<owe::SceneNode>::make();
    auto camera      = std::make_shared<owe::SceneCamera>(1920, 1080, -1.0, 1.0);
    camera->AttatchNode(camera_node.as_ptr());
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

TEST(SceneUniformUpdaterParallax, ParentPropagationSelectsAncestorParallaxSource) {
    owe::Scene scene;
    scene.ortho[0] = 3840;
    scene.ortho[1] = 2160;

    auto camera_node =
        rstd::sync::Arc<owe::SceneNode>::make(Eigen::Vector3f { 1920.0f, 1080.0f, 0.0f },
                                              Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                              Eigen::Vector3f::Zero());
    auto camera = std::make_shared<owe::SceneCamera>(3840, 2160, -1.0, 1.0);
    camera->AttatchNode(camera_node.as_ptr());
    scene.cameras["default"] = camera;
    scene.activeCamera       = camera.get();

    auto parent = rstd::sync::Arc<owe::SceneNode>::make(Eigen::Vector3f { 1982.0f, 1053.0f, 0.0f },
                                                        Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                                        Eigen::Vector3f::Zero());
    auto child  = rstd::sync::Arc<owe::SceneNode>::make(Eigen::Vector3f { -76.0f, -3.0f, 0.0f },
                                                        Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                                        Eigen::Vector3f::Zero());
    auto mesh   = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    child->AddMesh(mesh);
    parent->AppendChild(child.clone());

    owe::SceneUniformUpdater updater(&scene);
    updater.SetCameraParallax({ true, 0.03f, 0.0f, 0.36f });
    updater.MouseInput(0.0, 1.0);
    updater.FrameBegin();

    owe::SceneUniformNodeData parent_data;
    parent_data.parallaxDepth           = { -1.56f, -0.79f };
    parent_data.propagatedParallaxDepth = parent_data.parallaxDepth;
    updater.SetNodeData(parent.as_ptr(), parent_data);

    owe::SceneUniformNodeData child_data;
    child_data.parallaxDepth           = { -1.12f, -1.36f };
    child_data.propagatedParallaxDepth = child_data.parallaxDepth;
    updater.SetNodeData(child.as_ptr(), child_data);

    updater.InitUniforms(child.as_ptr(), [](std::string_view name) {
        return name == owe::G_MVP;
    });

    auto capture_mvp = [&]() {
        owe::sprite_map_t                                 sprites;
        std::unordered_map<std::string, owe::ShaderValue> values;
        updater.UpdateUniforms(
            child.as_ptr(), sprites, [&](std::string_view name, owe::ShaderValue value) {
                values[std::string(name)] = value;
            });
        return values.at(std::string(owe::G_MVP));
    };
    auto expected_translation = [](Eigen::Vector2f base, Eigen::Vector2f depth) {
        const Eigen::Vector2f camera_pos { 1920.0f, 1080.0f };
        const Eigen::Vector2f mouse_vec { 691.2f, 388.8f };
        Eigen::Vector2f       offset = (base - camera_pos + mouse_vec).cwiseProduct(depth) * 0.03f;
        Eigen::Vector2f       final_pos = Eigen::Vector2f { 1906.0f, 1050.0f } + offset;
        return Eigen::Vector2f {
            (final_pos.x() - 1920.0f) / 1920.0f,
            (final_pos.y() - 1080.0f) / 1080.0f,
        };
    };

    auto mvp             = capture_mvp();
    auto expected_parent = expected_translation({ 1982.0f, 1053.0f }, { -1.56f, -0.79f });
    ASSERT_GT(mvp.size(), 13u);
    EXPECT_NEAR(mvp[12], expected_parent.x(), 1e-5f);
    EXPECT_NEAR(mvp[13], expected_parent.y(), 1e-5f);

    parent_data.propagate_parallax_to_children = false;
    updater.SetNodeData(parent.as_ptr(), parent_data);
    mvp                 = capture_mvp();
    auto expected_child = expected_translation({ 1906.0f, 1050.0f }, { -1.12f, -1.36f });
    EXPECT_NEAR(mvp[12], expected_child.x(), 1e-5f);
    EXPECT_NEAR(mvp[13], expected_child.y(), 1e-5f);

    auto layer_camera = std::make_shared<owe::SceneCamera>(3840, 2160, -1.0, 1.0);
    layer_camera->AttatchNode(child.as_ptr());
    layer_camera->AttatchImgEffect(
        std::make_shared<owe::SceneImageEffectLayer>(child.as_ptr(),
                                                     3840.0f,
                                                     2160.0f,
                                                     "_rt_effect_pingpong_a_test",
                                                     "_rt_effect_pingpong_b_test"));
    scene.cameras["layer"] = layer_camera;
    child->SetCamera("layer");

    parent_data.propagate_parallax_to_children = true;
    updater.SetNodeData(parent.as_ptr(), parent_data);
    mvp = capture_mvp();
    EXPECT_NEAR(mvp[12], 0.0f, 1e-5f);
    EXPECT_NEAR(mvp[13], 0.0f, 1e-5f);
}
