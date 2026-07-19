#include <gtest/gtest.h>

import eigen;
import rstd;
import rstd.cppstd;
import wescene.json;
import wescene.pkg.parse;
import wescene.scene;
import wescene.text;

namespace scene_test
{

class UniformSink {
public:
    explicit UniformSink(owe::UniformOutputId output): m_output(output) {}

    bool Wants(owe::UniformOutputId output) const { return output == m_output; }

    auto Write(owe::UniformOutputId output, owe::UniformValueView value)
        -> rstd::Result<rstd::empty, owe::UniformError> {
        if (! Wants(output)) {
            return rstd::Err(owe::UniformError {
                .message = rstd::string::String::make("unexpected uniform output"),
            });
        }
        m_value   = owe::UniformValue(value.data, value.size);
        m_written = true;
        return rstd::Ok(rstd::empty {});
    }

    const owe::UniformValue& Value() const { return m_value; }
    bool                     Written() const { return m_written; }

private:
    owe::UniformOutputId m_output;
    owe::UniformValue    m_value;
    bool                 m_written { false };
};

class EmptyResources {
public:
    auto Texture(rstd::usize) const -> rstd::Option<owe::UniformTextureView> {
        return rstd::None();
    }
    auto Viewport() const -> rstd::array<rstd::f32, 2> { return { 1920.0f, 1080.0f }; }
    auto TexelSize() const -> rstd::array<rstd::f32, 2> {
        return { 1.0f / 1920.0f, 1.0f / 1080.0f };
    }
};

class UpdateContext {
public:
    UpdateContext(const owe::SceneFrame& frame, const EmptyResources& resources)
        : m_frame(rstd::ref<owe::SceneFrame>::from_raw_parts(rstd::addressof(frame))),
          m_resources(rstd::dyn<owe::UniformResourceView>::from_ref(resources)) {}

    auto Frame() const -> rstd::ref<owe::SceneFrame> { return m_frame; }
    auto Resources() const -> rstd::ref<rstd::dyn<owe::UniformResourceView>> { return m_resources; }

private:
    rstd::ref<owe::SceneFrame>                     m_frame;
    rstd::ref<rstd::dyn<owe::UniformResourceView>> m_resources;
};

template<typename Source, typename Output>
auto Capture(const owe::SceneFrame& frame, const Source& source, Output output)
    -> owe::UniformValue {
    EmptyResources resources;
    UpdateContext  context_impl(frame, resources);
    UniformSink    sink_impl(owe::ToUniformOutput(output));
    auto           context = rstd::dyn<owe::UniformUpdateContext>::from_ref(context_impl);
    auto           sink    = rstd::dyn<owe::UniformValueSink>::from_ref(sink_impl);
    auto           result  = source.Evaluate(context.as_ref(), sink.as_mut_ref());
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(sink_impl.Written());
    return sink_impl.Value();
}

} // namespace scene_test

TEST(AudioResponseDemand, AggregatesLeasesAndHonorsRuntimeGate) {
    owe::AudioResponseDemand demand;
    std::vector<bool>        changes;
    demand.SetCallback([&changes](bool active) {
        changes.push_back(active);
    });
    ASSERT_EQ(changes, (std::vector<bool> { false }));

    auto first  = demand.Acquire();
    auto second = demand.Acquire();
    EXPECT_TRUE(demand.Active());
    EXPECT_EQ(changes, (std::vector<bool> { false, true }));
    first.reset();
    EXPECT_TRUE(demand.Active());
    second.reset();
    EXPECT_FALSE(demand.Active());
    EXPECT_EQ(changes, (std::vector<bool> { false, true, false }));

    auto gated = demand.Acquire();
    demand.SetEnabled(false);
    demand.SetEnabled(true);
    gated.reset();
    EXPECT_EQ(changes, (std::vector<bool> { false, true, false, true, false, true, false }));
}

TEST(SceneUserTextBinding, AppliesDescriptorPayloadToMatchingBindings) {
    owe::Scene  scene;
    std::string first;
    std::string second;
    scene.RegisterUserTextBinding("title", [&](std::string_view value) {
        first = value;
    });
    scene.RegisterUserTextBinding("title", [&](std::string_view value) {
        second = value;
    });

    auto property = owe::ParseJson(R"({"type":"textinput","value":"updated"})").unwrap();
    EXPECT_TRUE(scene.ApplyUserTextBindings("title", property));
    EXPECT_EQ(first, "updated");
    EXPECT_EQ(second, "updated");
    EXPECT_FALSE(scene.ApplyUserTextBindings("other", property));
}

TEST(SceneUserTextBinding, AppliesEmptyString) {
    owe::Scene  scene;
    std::string value = "default";
    scene.RegisterUserTextBinding("title", [&](std::string_view next) {
        value = next;
    });

    auto property = owe::ParseJson(R"({"type":"textinput","value":""})").unwrap();
    EXPECT_TRUE(scene.ApplyUserTextBindings("title", property));
    EXPECT_TRUE(value.empty());
}

TEST(TextUniformSource, OwnsTextProjectionOutputs) {
    owe::Scene scene;
    auto       node   = rstd::sync::Arc<owe::SceneNode>::make();
    auto       camera = std::make_shared<owe::SceneCamera>(1920, 1080, -1.0, 1.0);
    auto       state  = std::make_shared<owe::text::TextUniformState>(node.clone());
    state->camera     = camera;

    owe::text::TextUniformSource source(state);
    auto                         value = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::text::TextUniformOutput::ModelViewProjection);

    EXPECT_EQ(value.size(), 16u);
}

TEST(WPUniformSourceRuntimeAlpha, Color4UsesBaseColorAndRuntimeAlpha) {
    owe::Scene scene;
    auto       node = rstd::sync::Arc<owe::SceneNode>::make();
    node->SetBaseColor({ 0.25f, 0.5f, 0.75f }, 0.8f);
    node->SetUserAlpha(0.125f);

    owe::WPColorUniformSource source(node.clone());
    const auto                color =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::WPColorUniformOutput::Color4);
    ASSERT_EQ(color.size(), 4u);
    EXPECT_FLOAT_EQ(color[0], 0.25f);
    EXPECT_FLOAT_EQ(color[1], 0.5f);
    EXPECT_FLOAT_EQ(color[2], 0.75f);
    EXPECT_FLOAT_EQ(color[3], 0.125f);
}

TEST(WPUniformSourceRuntimeAlpha, VisibleTrueRestoresLayerAlpha) {
    owe::Scene scene;
    auto       node = rstd::sync::Arc<owe::SceneNode>::make();
    node->SetBaseColor({ 0.0f, 0.0f, 0.0f }, 0.35f);
    owe::WPColorUniformSource source(node.clone());

    node->SetVisible(true);
    auto visible =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::WPColorUniformOutput::Color4);
    ASSERT_EQ(visible.size(), 4u);
    EXPECT_FLOAT_EQ(visible[3], 0.35f);

    node->SetVisible(false);
    auto hidden =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::WPColorUniformOutput::Color4);
    ASSERT_EQ(hidden.size(), 4u);
    EXPECT_FLOAT_EQ(hidden[3], 0.0f);

    node->SetVisible(true);
    auto restored =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::WPColorUniformOutput::Color4);
    ASSERT_EQ(restored.size(), 4u);
    EXPECT_FLOAT_EQ(restored[3], 0.35f);
}

TEST(WPUniformSourceParallax, ParentPropagationSelectsAncestorConfiguration) {
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
    auto effect = rstd::sync::Arc<owe::SceneNode>::make();
    auto mesh   = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = 0;
    mesh->Submeshes().push_back(std::move(submesh));
    child->AddMesh(mesh);
    parent->AppendChild(child.clone());
    scene.sceneGraph->AppendChild(parent.clone());
    scene.RebuildResourceIndex();
    effect->SetParentAnchor(child.as_ptr());

    auto state              = std::make_shared<owe::WPUniformSceneState>();
    state->CameraParallax() = { true, 0.03f, 0.0f, 0.36f };
    state->SetOrtho(3840.0f, 2160.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = std::make_shared<owe::WPUniformCameraResolver>(camera);
    camera_resolver->Add("default", camera);

    auto parent_state             = std::make_shared<owe::WPUniformNodeState>(parent.clone());
    parent_state->camera_resolver = camera_resolver;
    parent_state->propagated_parallax_depth      = { -1.56f, -0.79f };
    parent_state->propagate_parallax_to_children = true;
    auto child_state             = std::make_shared<owe::WPUniformNodeState>(child.clone());
    child_state->camera_resolver = camera_resolver;
    child_state->propagated_parallax_depth      = { -1.12f, -1.36f };
    child_state->propagate_parallax_to_children = true;
    auto effect_state             = std::make_shared<owe::WPUniformNodeState>(effect.clone());
    effect_state->camera_resolver = camera_resolver;
    effect_state->propagated_parallax_depth      = { 0.0f, 0.0f };
    effect_state->propagate_parallax_to_children = true;
    (void)state->SetNodeState({ .index = 1, .generation = 1 }, parent_state);
    (void)state->SetNodeState({ .index = 2, .generation = 1 }, child_state);
    (void)state->SetNodeState({ .index = 3, .generation = 1 }, effect_state);
    owe::WPTransformUniformSource source(state, effect_state);

    auto capture_mvp = [&]() {
        return scene_test::Capture(
            scene.Runtime().Frame(), source, owe::WPTransformUniformOutput::ModelViewProjection);
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

    parent_state->propagate_parallax_to_children = false;
    mvp                                          = capture_mvp();
    auto expected_child = expected_translation({ 1906.0f, 1050.0f }, { -1.12f, -1.36f });
    EXPECT_NEAR(mvp[12], expected_child.x(), 1e-5f);
    EXPECT_NEAR(mvp[13], expected_child.y(), 1e-5f);

    auto layer_camera = std::make_shared<owe::SceneCamera>(3840, 2160, -1.0, 1.0);
    layer_camera->AttatchNode(effect.as_ptr());
    layer_camera->AttatchImgEffect(
        std::make_shared<owe::SceneImageEffectLayer>(child.as_ptr(),
                                                     3840.0f,
                                                     2160.0f,
                                                     "_rt_effect_pingpong_a_test",
                                                     "_rt_effect_pingpong_b_test"));
    scene.cameras["layer"] = layer_camera;
    camera_resolver->Add("layer", layer_camera);
    effect->SetCamera("layer");
    parent_state->propagate_parallax_to_children = true;
    mvp                                          = capture_mvp();
    EXPECT_NEAR(mvp[12], 0.0f, 1e-5f);
    EXPECT_NEAR(mvp[13], 0.0f, 1e-5f);
}
