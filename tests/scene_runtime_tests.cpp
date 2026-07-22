#include <gtest/gtest.h>

import eigen;
import rstd;
import rstd.cppstd;
import wescene.json;
import wescene.pkg.parse;
import wescene.scene;
import wescene.text;

using namespace rstd::prelude;
using rstd::cppstd::to_string;
using rstd::sync::Arc;

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
    auto Viewport() const -> rstd::array<float, 2> { return { 1920.0f, 1080.0f }; }
    auto TexelSize() const -> rstd::array<float, 2> { return { 1.0f / 1920.0f, 1.0f / 1080.0f }; }
};

class ShapeSink {
public:
    auto Bind(owe::UniformOutputId, std::string_view name, owe::UniformValueShape shape)
        -> rstd::Result<bool, owe::UniformError> {
        if (name == "g_ModelMatrix") {
            model_shape = shape;
            found_model = true;
        }
        return rstd::Ok(true);
    }

    owe::UniformValueShape model_shape;
    bool                   found_model { false };
};

class UpdateContext {
public:
    UpdateContext(const owe::SceneFrame& frame, const EmptyResources& resources)
        : m_frame(rstd::ref<owe::SceneFrame>::from_raw_parts(rstd::addressof(frame))),
          m_resources(rstd::dyn<owe::UniformResourceView>::from_ref(resources)) {}

    auto Frame() const -> rstd::ref<owe::SceneFrame> { return m_frame; }
    auto Resources() const -> rstd::ref<rstd::dyn<owe::UniformResourceView>> { return m_resources; }
    auto RenderView() const -> owe::SceneRenderViewKind {
        return owe::SceneRenderViewKind::Primary;
    }

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

TEST(WPTransformUniformSource, AcceptsMat3AndMat4ModelMatrices) {
    auto state = Arc<owe::WPUniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1.0, 1.0, -1.0, 1.0));
    auto resolver   = Arc<owe::WPUniformCameraResolver>::make(rstd::move(camera));
    auto scene_node = Arc<owe::SceneNode>::make();
    auto node = Arc<owe::WPUniformNodeState>::make(rstd::move(scene_node), rstd::move(resolver));
    owe::WPTransformUniformSource source(rstd::move(state), rstd::move(node));
    scene_test::ShapeSink         sink_impl;
    auto                          sink = rstd::dyn<owe::UniformBindingSink>::from_ref(sink_impl);

    auto result = source.Describe(sink.as_mut_ref());

    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(sink_impl.found_model);
    EXPECT_EQ(sink_impl.model_shape.min_elements, rstd::u32(12));
    EXPECT_EQ(sink_impl.model_shape.max_elements, rstd::u32(16));
}

TEST(AudioResponseDemand, AggregatesLeasesAndHonorsRuntimeGate) {
    owe::AudioResponseDemand demand;
    std::vector<bool>        changes;
    demand.SetCallback([&changes](bool active) {
        changes.push_back(active);
    });
    ASSERT_EQ(changes, (std::vector<bool> { false }));

    auto first  = rstd::Some(demand.Acquire());
    auto second = rstd::Some(demand.Acquire());
    EXPECT_TRUE(demand.Active());
    EXPECT_EQ(changes, (std::vector<bool> { false, true }));
    first = rstd::None();
    EXPECT_TRUE(demand.Active());
    second = rstd::None();
    EXPECT_FALSE(demand.Active());
    EXPECT_EQ(changes, (std::vector<bool> { false, true, false }));

    auto gated = rstd::Some(demand.Acquire());
    demand.SetEnabled(false);
    demand.SetEnabled(true);
    gated = rstd::None();
    EXPECT_EQ(changes, (std::vector<bool> { false, true, false, true, false, true, false }));
}

TEST(SceneAudioAverage, SharesAtomicStateWithStreamOwner) {
    owe::Scene scene;
    auto       stream_owner = scene.AudioAverageHandle();

    stream_owner->Store(usize(3), f32(0.75f));

    EXPECT_FLOAT_EQ(scene.AudioAverage(usize(3)).to_primitive(), 0.75f);
}

TEST(SceneUserTextBinding, AppliesDescriptorPayloadToMatchingBindings) {
    owe::Scene  scene;
    std::string first;
    std::string second;
    scene.RegisterUserTextBinding(String::make("title"),
                                  Box<dyn<FnMut<void(ref<str>)>>>::make([&](ref<str> value) {
                                      first = to_string(value);
                                  }));
    scene.RegisterUserTextBinding(String::make("title"),
                                  Box<dyn<FnMut<void(ref<str>)>>>::make([&](ref<str> value) {
                                      second = to_string(value);
                                  }));

    auto property = owe::ParseJson(R"({"type":"textinput","value":"updated"})").unwrap();
    EXPECT_TRUE(scene.ApplyUserTextBindings("title", property));
    EXPECT_EQ(first, "updated");
    EXPECT_EQ(second, "updated");
    EXPECT_FALSE(scene.ApplyUserTextBindings("other", property));
}

TEST(SceneUserTextBinding, AppliesEmptyString) {
    owe::Scene  scene;
    std::string value = "default";
    scene.RegisterUserTextBinding(String::make("title"),
                                  Box<dyn<FnMut<void(ref<str>)>>>::make([&](ref<str> next) {
                                      value = to_string(next);
                                  }));

    auto property = owe::ParseJson(R"({"type":"textinput","value":""})").unwrap();
    EXPECT_TRUE(scene.ApplyUserTextBindings("title", property));
    EXPECT_TRUE(value.empty());
}

TEST(SceneUserPropertyBinding, AppliesJsonPayloadToOwnedCallback) {
    owe::Scene scene;
    bool       called = false;
    scene.RegisterUserPropertyBinding(
        String::make("camera"),
        Box<dyn<FnMut<void(const owe::Json&)>>>::make([&](const owe::Json& property) {
            called = property.is_object();
        }));

    auto property = owe::ParseJson(R"({"value":true})").unwrap();
    EXPECT_TRUE(scene.ApplyUserPropertyBindings("camera", property));
    EXPECT_TRUE(called);
    EXPECT_FALSE(scene.ApplyUserPropertyBindings("other", property));
}

TEST(SceneTransformUpdater, ReceivesRuntimeElapsedTime) {
    owe::Scene scene;
    f64        observed;
    scene.RegisterTransformUpdater(Box<dyn<FnMut<void(f64)>>>::make([&](f64 elapsed) {
        observed = elapsed;
    }));

    scene.PassFrameTime(0.25);
    scene.TickTransformUpdaters();
    EXPECT_DOUBLE_EQ(observed.to_primitive(), 0.25);
}

TEST(TextUniformSource, OwnsTextProjectionOutputs) {
    owe::Scene scene;
    auto       node = Arc<owe::SceneNode>::make();
    auto       camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    auto state    = std::make_shared<owe::text::TextUniformState>(node.clone());
    state->camera = Some(camera.clone());

    owe::text::TextUniformSource source(state);
    auto                         value = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::text::TextUniformOutput::ModelViewProjection);

    EXPECT_EQ(value.size().to_primitive(), 16u);
}

TEST(SceneCameraProjection, UsesExplicitProjectionFactories) {
    auto orthographic = owe::SceneCamera::MakeOrthographic(1920.5, 1080.25, -1.0, 1.0);
    EXPECT_FALSE(orthographic.IsPerspective());
    EXPECT_DOUBLE_EQ(orthographic.Width(), 1920.5);
    EXPECT_DOUBLE_EQ(orthographic.Height(), 1080.25);

    auto perspective = owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.01, 1000.0, 45.0);
    EXPECT_TRUE(perspective.IsPerspective());
    EXPECT_DOUBLE_EQ(perspective.Aspect(), 16.0 / 9.0);
    EXPECT_DOUBLE_EQ(perspective.Fov(), 45.0);
}

TEST(SceneCameraPath, UserBindingMutatesRegisteredArc) {
    owe::Scene scene;
    auto       path                = Arc<owe::SceneCameraPath>::make();
    path->visible_user_binding.key = String::make("camera-path");
    scene.RegisterCameraPath(path.clone());
    scene.RegisterCameraPathUserBinding(String::make("camera-path"), path.clone());

    auto disabled = owe::ParseJson(R"({"value":false})").unwrap();
    EXPECT_TRUE(scene.ApplyUserCameraPathVisibilityBindings("camera-path", disabled));
    EXPECT_FALSE(path->enabled);

    auto enabled = owe::ParseJson(R"({"value":true})").unwrap();
    EXPECT_TRUE(scene.ApplyUserCameraPathVisibilityBindings("camera-path", enabled));
    EXPECT_TRUE(path->enabled);
}

TEST(WPUniformSourceRuntimeAlpha, Color4UsesBaseColorAndRuntimeAlpha) {
    owe::Scene scene;
    auto       node = Arc<owe::SceneNode>::make();
    node->SetBaseColor({ 0.25f, 0.5f, 0.75f }, 0.8f);
    node->SetUserAlpha(0.125f);

    owe::WPColorUniformSource source(node.clone());
    const auto                color =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::WPColorUniformOutput::Color4);
    ASSERT_EQ(color.size().to_primitive(), 4u);
    EXPECT_FLOAT_EQ(color[rstd::usize()], 0.25f);
    EXPECT_FLOAT_EQ(color[rstd::usize(1)], 0.5f);
    EXPECT_FLOAT_EQ(color[rstd::usize(2)], 0.75f);
    EXPECT_FLOAT_EQ(color[rstd::usize(3)], 0.125f);
}

TEST(WPUniformSourceRuntimeAlpha, VisibleTrueRestoresLayerAlpha) {
    owe::Scene scene;
    auto       node = Arc<owe::SceneNode>::make();
    node->SetBaseColor({ 0.0f, 0.0f, 0.0f }, 0.35f);
    owe::WPColorUniformSource source(node.clone());

    node->SetVisible(true);
    auto visible =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::WPColorUniformOutput::Color4);
    ASSERT_EQ(visible.size().to_primitive(), 4u);
    EXPECT_FLOAT_EQ(visible[rstd::usize(3)], 0.35f);

    node->SetVisible(false);
    auto hidden =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::WPColorUniformOutput::Color4);
    ASSERT_EQ(hidden.size().to_primitive(), 4u);
    EXPECT_FLOAT_EQ(hidden[rstd::usize(3)], 0.0f);

    node->SetVisible(true);
    auto restored =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::WPColorUniformOutput::Color4);
    ASSERT_EQ(restored.size().to_primitive(), 4u);
    EXPECT_FLOAT_EQ(restored[rstd::usize(3)], 0.35f);
}

TEST(WPUniformSourceParallax, ParentPropagationSelectsAncestorConfiguration) {
    owe::Scene scene;
    scene.SetOrtho({ i32(3840), i32(2160) });

    auto camera_node = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1920.0f, 1080.0f, 0.0f },
                                                 Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                                 Eigen::Vector3f::Zero());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(3840, 2160, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("default"), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera(ref<str>("default")));

    auto parent = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1982.0f, 1053.0f, 0.0f },
                                            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                            Eigen::Vector3f::Zero());
    auto child  = Arc<owe::SceneNode>::make(Eigen::Vector3f { -76.0f, -3.0f, 0.0f },
                                            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                            Eigen::Vector3f::Zero());
    auto effect = Arc<owe::SceneNode>::make();
    auto mesh   = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = 0;
    mesh->Submeshes().push_back(std::move(submesh));
    child->AddMesh(mesh);
    parent->AppendChild(child.clone());
    scene.RootMut()->AppendChild(parent.clone());
    scene.RebuildResourceIndex();
    effect->SetParentAnchor(child.as_ptr());

    auto state = Arc<owe::WPUniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->CameraParallax() = { true, 0.03f, 0.0f, 0.36f };
    state->SetOrtho(3840.0f, 2160.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::WPUniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"), camera.clone());

    auto parent_state = Arc<owe::WPUniformNodeState>::make(parent.clone(), camera_resolver.clone());
    parent_state->propagated_parallax_depth      = { -1.56f, -0.79f };
    parent_state->propagate_parallax_to_children = true;
    auto child_state = Arc<owe::WPUniformNodeState>::make(child.clone(), camera_resolver.clone());
    child_state->propagated_parallax_depth      = { -1.12f, -1.36f };
    child_state->propagate_parallax_to_children = true;
    auto effect_state = Arc<owe::WPUniformNodeState>::make(effect.clone(), camera_resolver.clone());
    effect_state->propagated_parallax_depth      = { 0.0f, 0.0f };
    effect_state->propagate_parallax_to_children = true;
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) },
                        parent_state.clone());
    state->SetNodeState({ .index = rstd::u32(2), .generation = rstd::u32(1) }, child_state.clone());
    state->SetNodeState({ .index = rstd::u32(3), .generation = rstd::u32(1) },
                        effect_state.clone());
    owe::WPTransformUniformSource source(state.clone(), effect_state.clone());

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
    ASSERT_GT(mvp.size().to_primitive(), 13u);
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_parent.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_parent.y(), 1e-5f);

    parent_state->propagate_parallax_to_children = false;
    mvp                                          = capture_mvp();
    auto expected_child = expected_translation({ 1906.0f, 1050.0f }, { -1.12f, -1.36f });
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_child.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_child.y(), 1e-5f);

    auto layer_camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(3840, 2160, -1.0, 1.0));
    layer_camera->AttatchNode(effect.as_ptr());
    layer_camera->AttatchImgEffect(
        std::make_shared<owe::SceneImageEffectLayer>(child.as_ptr(),
                                                     3840.0f,
                                                     2160.0f,
                                                     "_rt_effect_pingpong_a_test",
                                                     "_rt_effect_pingpong_b_test"));
    scene.RegisterCamera(String::make("layer"), layer_camera.clone());
    camera_resolver->Add(String::make("layer"), layer_camera.clone());
    effect->SetCamera("layer");
    parent_state->propagate_parallax_to_children = true;
    mvp                                          = capture_mvp();
    EXPECT_NEAR(mvp[rstd::usize(12)], 0.0f, 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], 0.0f, 1e-5f);
}
