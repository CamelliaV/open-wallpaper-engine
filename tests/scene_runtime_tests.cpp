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

std::shared_ptr<owe::Scene>
LoadWorkshopScene(std::string_view                                id,
                  std::unordered_map<std::string, nlohmann::json> overrides = {}) {
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
    for (auto& [key, value] : overrides) user_props[key] = std::move(value);

    wavsen::audio::SoundManager sm;
    owe::WPSceneParser          parser;
    parser.SetUserProperties(&user_props);
    return parser.Parse(std::string(id), *doc, vfs, sm);
}

nlohmann::json UserPropertyValue(nlohmann::json value) {
    nlohmann::json out;
    out["value"] = std::move(value);
    return out;
}

std::array<float, 3> VertexPosition(const owe::SceneVertexArray& vertices, std::size_t index) {
    auto offsets = vertices.GetAttrOffsetMap();
    auto it      = offsets.find(std::string(owe::WE_IN_POSITION));
    EXPECT_NE(it, offsets.end());
    if (it == offsets.end()) return { 0.0f, 0.0f, 0.0f };

    const std::size_t offset = it->second.offset / sizeof(float);
    const float*      p      = vertices.Data() + index * vertices.OneSize() + offset;
    return { p[0], p[1], p[2] };
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

TEST(SceneVisibilityBindings, ComboConditionUpdatesWorkshop3462491575) {
    auto scene = LoadWorkshopScene("3462491575");
    if (! scene) GTEST_SKIP() << "workshop 3462491575 is not available";

    std::vector<owe::SceneNode*> background_nodes;
    std::vector<owe::SceneNode*> solid_nodes;
    CollectNodesById(scene->sceneGraph, 10859, background_nodes);
    CollectNodesById(scene->sceneGraph, 1143, solid_nodes);
    ASSERT_FALSE(background_nodes.empty());
    ASSERT_FALSE(solid_nodes.empty());
    auto* background = background_nodes.front();
    auto* solid      = solid_nodes.front();

    EXPECT_TRUE(background->Visible());
    EXPECT_EQ(scene->visibility_elidable_layer_ids.count(10859), 0u);
    EXPECT_EQ(scene->visibility_elidable_layer_ids.count(1143), 1u);

    EXPECT_TRUE(scene->ApplyUserNodeVisibilityBindings("beijing", UserPropertyValue("1")));
    EXPECT_FALSE(background->Visible());
    EXPECT_TRUE(solid->Visible());
    EXPECT_EQ(scene->visibility_elidable_layer_ids.count(10859), 1u);
    EXPECT_EQ(scene->visibility_elidable_layer_ids.count(1143), 0u);
}

TEST(SceneVisibilityBindings, ParticleBoolUpdatesWorkshop3480296606) {
    auto scene = LoadWorkshopScene("3480296606");
    if (! scene) GTEST_SKIP() << "workshop 3480296606 is not available";

    std::vector<owe::SceneNode*> fog_nodes;
    CollectNodesById(scene->sceneGraph, 225, fog_nodes);
    ASSERT_FALSE(fog_nodes.empty());
    auto* fog = fog_nodes.front();

    EXPECT_TRUE(fog->Visible());
    EXPECT_EQ(scene->visibility_elidable_layer_ids.count(225), 0u);

    EXPECT_TRUE(scene->ApplyUserNodeVisibilityBindings("newproperty11", UserPropertyValue(false)));
    EXPECT_FALSE(fog->Visible());
    EXPECT_EQ(scene->visibility_elidable_layer_ids.count(225), 1u);

    EXPECT_TRUE(scene->ApplyUserNodeVisibilityBindings("newproperty11", UserPropertyValue(true)));
    EXPECT_TRUE(fog->Visible());
    EXPECT_EQ(scene->visibility_elidable_layer_ids.count(225), 0u);
}

TEST(SceneVisibilityBindings, ImageEffectBoolUpdatesWorkshop3480296606) {
    auto scene = LoadWorkshopScene("3480296606");
    if (! scene) GTEST_SKIP() << "workshop 3480296606 is not available";

    std::vector<owe::SceneNode*> green_nodes;
    CollectNodesById(scene->sceneGraph, 900, green_nodes);
    ASSERT_FALSE(green_nodes.empty());
    auto* green = green_nodes.front();
    ASSERT_FALSE(green->Camera().empty());
    ASSERT_TRUE(scene->cameras.contains(green->Camera()));
    auto& camera = scene->cameras.at(green->Camera());
    ASSERT_TRUE(camera->HasImgEffect());
    auto& effect_layer = camera->GetImgEffect();

    owe::SceneImageEffect* audio_effect = nullptr;
    for (std::size_t i = 0; i < effect_layer->EffectCount(); ++i) {
        auto& effect = effect_layer->GetEffect(i);
        if (effect && effect->visible_user_binding.key == "newproperty1") {
            audio_effect = effect.get();
            break;
        }
    }
    ASSERT_NE(audio_effect, nullptr);
    EXPECT_TRUE(audio_effect->runtime_visible);
    EXPECT_TRUE(effect_layer->HasRuntimeVisibleEffect());

    EXPECT_TRUE(
        scene->ApplyUserImageEffectVisibilityBindings("newproperty1", UserPropertyValue(false)));
    EXPECT_FALSE(audio_effect->runtime_visible);
    EXPECT_FALSE(effect_layer->HasRuntimeVisibleEffect());

    EXPECT_TRUE(
        scene->ApplyUserImageEffectVisibilityBindings("newproperty1", UserPropertyValue(true)));
    EXPECT_TRUE(audio_effect->runtime_visible);
    EXPECT_TRUE(effect_layer->HasRuntimeVisibleEffect());

    auto hidden_scene =
        LoadWorkshopScene("3480296606", { { "newproperty1", UserPropertyValue(false) } });
    ASSERT_NE(hidden_scene, nullptr);
    green_nodes.clear();
    CollectNodesById(hidden_scene->sceneGraph, 900, green_nodes);
    ASSERT_FALSE(green_nodes.empty());
    auto* hidden_green = green_nodes.front();
    ASSERT_TRUE(hidden_scene->cameras.contains(hidden_green->Camera()));
    auto& hidden_effect_layer = hidden_scene->cameras.at(hidden_green->Camera())->GetImgEffect();
    ASSERT_TRUE(hidden_effect_layer);
    EXPECT_FALSE(hidden_effect_layer->HasRuntimeVisibleEffect());
    EXPECT_TRUE(hidden_scene->ApplyUserImageEffectVisibilityBindings("newproperty1",
                                                                     UserPropertyValue(true)));
    EXPECT_TRUE(hidden_effect_layer->HasRuntimeVisibleEffect());
}

TEST(SceneImageAlignment, TopLeftAlignedImageOffsetsLocalGeometry) {
    auto scene = LoadWorkshopScene("3002120692");
    if (! scene) GTEST_SKIP() << "workshop 3002120692 is not available";

    std::vector<owe::SceneNode*> nodes;
    CollectNodesById(scene->sceneGraph, 204, nodes);
    ASSERT_FALSE(nodes.empty());
    auto* node = nodes.front();
    ASSERT_NE(node->Mesh(), nullptr);
    ASSERT_EQ(node->Mesh()->VertexCount(), 1u);

    const auto& vertices = node->Mesh()->GetVertexArray(0);
    ASSERT_EQ(vertices.VertexCount(), 4u);

    const auto top_left     = VertexPosition(vertices, 0);
    const auto bottom_left  = VertexPosition(vertices, 1);
    const auto top_right    = VertexPosition(vertices, 2);
    const auto bottom_right = VertexPosition(vertices, 3);

    EXPECT_NEAR(top_left[0], 0.0f, 0.001f);
    EXPECT_NEAR(top_right[0], 818.0f, 0.001f);
    EXPECT_NEAR(bottom_left[0], 0.0f, 0.001f);
    EXPECT_NEAR(bottom_right[0], 818.0f, 0.001f);
    EXPECT_NEAR(top_left[1], 0.0f, 0.001f);
    EXPECT_NEAR(top_right[1], 0.0f, 0.001f);
    EXPECT_NEAR(bottom_left[1], -818.0f, 0.001f);
    EXPECT_NEAR(bottom_right[1], -818.0f, 0.001f);
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

TEST(SceneUniformUpdaterRuntimeAlpha, VisibleTrueRestoresLayerAlpha) {
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
    node.SetBaseColor({ 0.0f, 0.0f, 0.0f }, 0.35f);

    owe::SceneUniformUpdater updater(&scene);
    updater.InitUniforms(&node, [](std::string_view name) {
        return name == "g_Color4";
    });

    std::unordered_map<std::string, owe::ShaderValue> values;
    node.SetVisible(true);
    updater.UpdateUniforms(&node, sprites, [&](std::string_view name, owe::ShaderValue value) {
        values[std::string(name)] = value;
    });
    ASSERT_EQ(values.count("g_Color4"), 1u);
    const auto& visible_color = values.at("g_Color4");
    ASSERT_EQ(visible_color.size(), 4u);
    EXPECT_FLOAT_EQ(visible_color[3], 0.35f);

    values.clear();
    node.SetVisible(false);
    updater.UpdateUniforms(&node, sprites, [&](std::string_view name, owe::ShaderValue value) {
        values[std::string(name)] = value;
    });
    ASSERT_EQ(values.count("g_Color4"), 1u);
    const auto& hidden_color = values.at("g_Color4");
    ASSERT_EQ(hidden_color.size(), 4u);
    EXPECT_FLOAT_EQ(hidden_color[3], 0.0f);

    values.clear();
    node.SetVisible(true);
    updater.UpdateUniforms(&node, sprites, [&](std::string_view name, owe::ShaderValue value) {
        values[std::string(name)] = value;
    });
    ASSERT_EQ(values.count("g_Color4"), 1u);
    const auto& restored_color = values.at("g_Color4");
    ASSERT_EQ(restored_color.size(), 4u);
    EXPECT_FLOAT_EQ(restored_color[3], 0.35f);
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
