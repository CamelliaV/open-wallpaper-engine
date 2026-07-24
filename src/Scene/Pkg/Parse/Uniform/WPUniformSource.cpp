module;

module wescene.pkg.parse;
import eigen;
import rstd;
import rstd.cppstd;
import wescene.scene;
import wescene.spec_names;

using namespace Eigen;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace owe
{

namespace
{

template<typename T>
struct ArcUniformBindingLease {
    Arc<T> state;

    void KeepAlive() const {}
};

template<typename T>
auto MakeArcUniformBindingLease(const Arc<T>& state) -> Option<Box<dyn<UniformBindingLease>>> {
    return Some(
        Box<dyn<UniformBindingLease>>::make(ArcUniformBindingLease<T> { .state = state.clone() }));
}

template<std::size_t N>
void AverageResample64(slice<float> bins, rstd::array<float, N>& out) {
    static_assert(64 % N == 0);
    constexpr std::size_t ratio = 64 / N;
    for (std::size_t index = 0; index < N; ++index) {
        float sum = 0.0f;
        for (std::size_t offset = 0; offset < ratio; ++offset) {
            sum += std::max(0.0f, bins[usize(index * ratio + offset)]);
        }
        out[usize(index)] = sum / static_cast<float>(ratio);
    }
}

float Smooth(float value) { return value * value * (3.0f - 2.0f * value); }

template<typename T>
constexpr T Clamp(T value, T minimum, T maximum) {
    return rstd::cmp::min(rstd::cmp::max(value, minimum), maximum);
}

auto UserScalar(const Json& property) -> Option<float> {
    const auto& value = SceneUserPropertyPayload(property);
    if (auto number = value.as_f64(); number.is_some()) {
        return Some(static_cast<float>(number->to_primitive()));
    }
    if (auto boolean = value.as_bool(); boolean.is_some()) return Some(*boolean ? 1.0f : 0.0f);
    if (auto string = value.as_str(); string.is_some()) {
        try {
            return Some(std::stof(rstd::cppstd::to_string(*string)));
        } catch (...) {
        }
    }
    return None();
}

Vector2f ShakeOffset(float x, float roughness) {
    const float r    = Clamp(roughness, 0.0f, 2.0f);
    const float over = Clamp(r - 1.0f, 0.0f, 1.0f);
    const float grow = over * over;

    constexpr float pi       = rstd::f32::consts::PI.to_primitive();
    const float     beat_pos = std::max(0.0f, x) / (pi * 0.5f);
    const auto      beat     = static_cast<std::int32_t>(std::floor(beat_pos));
    const float     local    = beat_pos - static_cast<float>(beat);
    const float     amount   = Smooth(local);

    static constexpr std::array<std::array<float, 2>, 8> directions {
        std::array<float, 2> { -1.0f, 1.0f }, std::array<float, 2> { 1.0f, -1.0f },
        std::array<float, 2> { -1.0f, 1.0f }, std::array<float, 2> { 1.0f, -1.0f },
        std::array<float, 2> { 1.0f, 1.0f },  std::array<float, 2> { -1.0f, -1.0f },
        std::array<float, 2> { 1.0f, 1.0f },  std::array<float, 2> { -1.0f, -1.0f },
    };
    static constexpr std::array<float, 8> base_factors {
        0.8f, 1.0f, 0.45f, 0.6f, 0.8f, 1.0f, 0.45f, 0.6f,
    };
    static constexpr std::array<float, 8> rough_factors {
        6.0f, 8.0f, 1.0f, 1.0f, 6.0f, 8.0f, 1.0f, 1.0f,
    };

    auto sample = [&](std::int32_t index) -> Vector2f {
        if ((index % 2) != 0) return Vector2f::Zero();
        const auto direction =
            static_cast<std::size_t>((index / 2) % static_cast<std::int32_t>(directions.size()));
        const float factor =
            base_factors[direction] * (1.0f + (rough_factors[direction] - 1.0f) * grow);
        return { directions[direction][0] * factor, directions[direction][1] * factor };
    };

    const Vector2f a     = sample(beat);
    const Vector2f b     = sample(beat + 1);
    const Vector2f delta = b - a;
    Vector2f       curve { -delta.y(), delta.x() };
    if (curve.squaredNorm() > 0.0f) curve.normalize();
    const float bend = std::sin(local * pi) * (0.09f + grow * 0.04f) * delta.norm();
    return a * (1.0f - amount) + b * amount + curve * bend;
}

class WPUniformWriter {
public:
    explicit WPUniformWriter(mut_ref<dyn<UniformValueSink>> sink): m_sink(sink) {}

    template<typename Output>
    bool Wants(Output output) {
        return m_sink->Wants(ToUniformOutput(output));
    }
    bool Wants(UniformOutputId output) { return m_sink->Wants(output); }

    template<typename Output, typename Value>
    void Write(Output output, const Value& value) {
        Write(ToUniformOutput(output), value);
    }

    template<typename Value>
    void Write(UniformOutputId output, const Value& value) {
        auto uniform = UniformValue(value);
        WriteView(output, uniform.View());
    }

    void Write(UniformOutputId output, const UniformValue& value) {
        WriteView(output, value.View());
    }

    void WriteView(UniformOutputId output, UniformValueView value) {
        if (m_failed || ! m_sink->Wants(output)) return;
        auto result = m_sink->Write(output, value);
        if (result.is_err()) {
            m_error  = rstd::move(result).unwrap_err_unchecked().message;
            m_failed = true;
        }
    }

    auto Finish() -> Result<empty, UniformError> {
        if (m_failed) return Err(UniformError { .message = rstd::move(m_error) });
        return Ok(empty {});
    }

private:
    mut_ref<dyn<UniformValueSink>> m_sink;
    String                         m_error;
    bool                           m_failed { false };
};

template<typename Output>
auto Bind(mut_ref<dyn<UniformBindingSink>> sink, Output output, ref<str> name,
          UniformValueShape shape) -> Result<empty, UniformError> {
    auto result = sink->Bind(ToUniformOutput(output), name, shape);
    if (result.is_err()) return Err(rstd::move(result).unwrap_err_unchecked());
    return Ok(empty {});
}

auto Bind(mut_ref<dyn<UniformBindingSink>> sink, UniformOutputId output, ref<str> name,
          UniformValueShape shape) -> Result<empty, UniformError> {
    auto result = sink->Bind(output, name, shape);
    if (result.is_err()) return Err(rstd::move(result).unwrap_err_unchecked());
    return Ok(empty {});
}

template<typename Output>
struct BindingEntry {
    Output            output;
    ref<str>          name;
    UniformValueShape shape;
};

template<typename Output, std::size_t N>
auto BindEntries(mut_ref<dyn<UniformBindingSink>>            sink,
                 const rstd::array<BindingEntry<Output>, N>& entries)
    -> Result<empty, UniformError> {
    for (const auto& entry : entries) {
        auto result = Bind(sink, entry.output, entry.name, entry.shape);
        if (result.is_err()) return result;
    }
    return Ok(empty {});
}

} // namespace

auto WPUniformNodeConfigDraft::Clone() const -> WPUniformNodeConfigDraft {
    WPUniformNodeConfigDraft result {
        .configured                     = configured,
        .parallax_depth                 = parallax_depth,
        .propagated_parallax_depth      = propagated_parallax_depth,
        .propagate_parallax_to_children = propagate_parallax_to_children,
        .use_camera_eye_position        = use_camera_eye_position,
        .effect_projection_node =
            effect_projection_node.is_some() ? Some((*effect_projection_node).clone()) : None(),
        .effect_projection_size = effect_projection_size,
    };
    return result;
}

void WPUniformCameraResolver::Add(String name, Arc<SceneCamera> camera) {
    (void)m_cameras.insert(rstd::move(name), rstd::move(camera));
}

auto WPUniformCameraResolver::Resolve(const SceneNode& node) const -> Option<mut_ref<SceneCamera>> {
    auto name = rstd::cppstd::as_str(node.Camera()).unwrap();
    if (name.is_empty()) {
        if (! node.Perspective()) return Some(m_active_camera.deref_mut());
        name = "global_perspective"_str;
    }
    auto camera = m_cameras.get(name);
    return camera.is_some() ? Some((**camera).deref_mut()) : None();
}

void WPUniformSceneState::SetNodeState(SceneNodeId id, Arc<WPUniformNodeState> state) {
    (void)m_nodes_by_address.insert(state->node.as_ptr().as_raw_ptr(), state.clone());
    (void)m_nodes.insert(Key(id), rstd::move(state));
}

bool WPUniformSceneState::SetEffectProjectionSize(SceneNodeId id, rstd::array<float, 2> size) {
    auto found = m_nodes.get_mut(Key(id));
    if (found.is_none()) return false;
    (**found)->effect_projection_size = size;
    return true;
}

auto WPUniformSceneState::ResolveParallaxState(const WPUniformNodeState& state) const
    -> const WPUniformNodeState& {
    auto* resolved = rstd::addressof(state);
    for (auto* parent = state.node->Parent(); parent != nullptr; parent = parent->Parent()) {
        auto found = m_nodes_by_address.get(parent);
        if (found.is_none()) continue;
        auto& candidate = ***found;
        if (! candidate.propagate_parallax_to_children) break;
        resolved = rstd::addressof(candidate);
    }
    return *resolved;
}

void WPUniformSceneState::SetPointerInput(double x, double y) {
    const auto now            = rstd::time::Instant::now();
    const auto elapsed        = (now - m_last_pointer_input_time).as_secs_f64();
    m_pointer_delayed_time    = std::max(0.0, m_pointer_delayed_time - elapsed);
    m_pointer_input           = { static_cast<float>(x), static_cast<float>(y) };
    m_last_pointer_input_time = now;
}

void WPUniformSceneState::SetPointerDelay(float delay) { m_pointer_delay = std::max(0.0f, delay); }

void WPUniformSceneState::SetAudioSpectrum(slice<float> left, slice<float> right) {
    if (left.len() != m_inputs.audio_left.len() || right.len() != m_inputs.audio_right.len()) {
        return;
    }
    for (usize index {}; index < m_inputs.audio_left.len(); ++index) {
        m_inputs.audio_left[index]  = left[index];
        m_inputs.audio_right[index] = right[index];
    }
}

void WPUniformSceneState::Advance(const SceneFrame& frame) {
    const auto delay       = static_cast<double>(m_pointer_delay);
    m_pointer_delayed_time = std::min(m_pointer_delayed_time + frame.delta.to_primitive(), delay);
    const auto amount      = delay > 0.0 ? m_pointer_delayed_time / delay : 1.0;

    m_inputs.pointer_last = m_inputs.pointer;
    for (usize index {}; index < m_inputs.pointer.len(); ++index) {
        const auto current = m_inputs.pointer[index];
        m_inputs.pointer[index] =
            static_cast<float>(current + (m_pointer_input[index] - current) * amount);
    }
}

void WPUniformSceneState::ApplyUserProperty(std::string_view field, const Json& property) {
    auto value = UserScalar(property);
    if (value.is_none()) return;
    if (field == "cameraparallaxmouseinfluence") {
        m_camera_parallax.mouse_influence = *value;
    } else if (field == "camerashake") {
        m_camera_shake.enable = *value >= 0.5f;
    } else if (field == "camerashakeamplitude") {
        m_camera_shake.amplitude = *value;
    } else if (field == "camerashakespeed") {
        m_camera_shake.speed = *value;
    } else if (field == "camerashakeroughness") {
        m_camera_shake.roughness = *value;
    }
}

auto WPTransformUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = WPTransformUniformOutput;
    auto model   = Bind(sink, Output::Model, G_M, UniformValueShape::Matrix(u32(4), u32(4)));
    if (model.is_err()) return model;

    const rstd::array<BindingEntry<Output>, 13> entries {
        BindingEntry<Output> {
            Output::ModelInverse, G_MI, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::NormalModel, G_NORMALMODELMATRIX, UniformValueShape::Matrix(u32(3), u32(3)) },
        BindingEntry<Output> {
            Output::AlternateModel, G_AM, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::ModelViewProjection, G_MVP, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::ModelViewProjectionInverse, G_MVPI, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::EyePosition, G_EYEPOSITION, UniformValueShape::Float(u32(3)) },
        BindingEntry<Output> {
            Output::EffectModel, G_EFFECTMODELMATRIX, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::EffectModelViewProjection, G_EMVP, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> { Output::EffectModelViewProjectionInverse,
                               G_EFFECTMODELVIEWPROJECTIONMATRIXINVERSE,
                               UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::LayerModel, G_LAYERMODELMATRIX, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> { Output::EffectTextureViewProjection,
                               G_ETVP,
                               UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> { Output::EffectTextureViewProjectionInverse,
                               G_ETVPI,
                               UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::ViewProjection, G_VP, UniformValueShape::Matrix(u32(4), u32(4)) },
    };
    return BindEntries(sink, entries);
}

auto WPTransformUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto WPTransformUniformSource::Evaluate(ref<dyn<UniformUpdateContext>> context,
                                        mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    auto camera_ref = m_node->camera_resolver->Resolve(*m_node->node);
    if (camera_ref.is_none()) return Ok(empty {});

    using Output = WPTransformUniformOutput;
    WPUniformWriter writer(sink);
    auto&           node        = *m_node->node;
    auto&           camera      = **camera_ref;
    const auto      render_view = context->RenderView();
    node.UpdateTrans();

    const auto frame            = context->Frame();
    const bool req_mi           = writer.Wants(Output::ModelInverse);
    const bool req_m            = writer.Wants(Output::Model);
    const bool req_normal_model = writer.Wants(Output::NormalModel);
    const bool req_am           = writer.Wants(Output::AlternateModel);
    const bool req_mvp          = writer.Wants(Output::ModelViewProjection);
    const bool req_mvpi         = writer.Wants(Output::ModelViewProjectionInverse);
    const bool req_emvp         = writer.Wants(Output::EffectModelViewProjection);
    const bool req_emvpi        = writer.Wants(Output::EffectModelViewProjectionInverse);
    const bool req_effect_model = writer.Wants(Output::EffectModel) || req_emvp || req_emvpi ||
                                  writer.Wants(Output::LayerModel);

    Matrix4d    view_projection   = camera.GetViewProjectionMatrix(render_view);
    const auto& shake             = m_state->CameraShake();
    auto        active_camera_ref = m_node->camera_resolver->Active();
    const bool  active_camera     = (*camera_ref).as_raw_ptr() == active_camera_ref.as_raw_ptr();
    if (shake.enable && active_camera && ! camera.IsPerspective() && shake.amplitude > 0.0f &&
        shake.speed > 0.0f) {
        const auto  ortho       = m_state->Ortho();
        const float base_extent = std::min(ortho[usize(0)], ortho[usize(1)]);
        const float scale       = shake.amplitude * base_extent * 0.01f;
        const float time   = static_cast<float>(frame->elapsed.to_primitive()) * shake.speed * 2.0f;
        const auto  offset = ShakeOffset(time, shake.roughness);
        view_projection =
            view_projection *
            Affine3d(Translation3d(Vector3d(offset.x() * scale, offset.y() * scale, 0.0))).matrix();
    }

    writer.Write(Output::ViewProjection, ShaderValue::fromMatrix(view_projection));
    if (m_node->use_camera_eye_position) {
        const auto position = camera.GetPosition(render_view).cast<float>();
        writer.Write(Output::EyePosition,
                     rstd::array<float, 3> { position.x(), position.y(), position.z() });
    }

    if (req_m || req_normal_model || req_am || req_mvp || req_mi || req_mvpi || req_effect_model) {
        Matrix4d    model    = node.ModelTrans();
        const auto& parallax = m_state->CameraParallax();
        auto        attached = camera.GetAttachedNode();
        const bool  own_image_effect =
            camera.HasImgEffect() && attached.is_some() && *attached == m_node->node.as_ptr();
        if (node.Camera() != "effect" && parallax.enable && ! own_image_effect) {
            const auto& parallax_state = m_state->ResolveParallaxState(*m_node);
            parallax_state.node->UpdateTrans();
            const Vector3f node_position =
                parallax_state.node->ModelTrans().block<3, 1>(0, 3).cast<float>();
            const Vector2f depth(parallax_state.propagated_parallax_depth.data());
            const auto     ortho_values = m_state->Ortho();
            const Vector2f ortho { ortho_values[usize(0)], ortho_values[usize(1)] };
            const Vector2f pointer(m_state->Inputs().pointer.data());
            const Vector2f pointer_offset =
                Scaling(1.0f, -1.0f) * (Vector2f { 0.5f, 0.5f } - pointer);
            const Vector2f mouse = pointer_offset.cwiseProduct(ortho) * parallax.mouse_influence;
            const auto     camera_position = camera.GetPosition(render_view).cast<float>();
            const Vector2f shift =
                (node_position.head<2>() - camera_position.head<2>() + mouse).cwiseProduct(depth) *
                parallax.amount;
            model = Affine3d(Translation3d(Vector3d(shift.x(), shift.y(), 0.0))).matrix() * model;
        }

        if (auto* mesh = node.Mesh(); mesh != nullptr) model *= mesh->GeometryTransform();

        if (req_m) writer.Write(Output::Model, ShaderValue::fromMatrix(model));
        if (req_normal_model) {
            Matrix3d normal_model = model.block<3, 3>(0, 0);
            if (std::abs(normal_model.determinant()) > 1e-12)
                normal_model = normal_model.inverse().transpose();
            else
                normal_model.setIdentity();
            writer.Write(Output::NormalModel, ShaderValue::fromMatrix(normal_model));
        }
        if (req_am) writer.Write(Output::AlternateModel, ShaderValue::fromMatrix(model));
        if (req_mi) writer.Write(Output::ModelInverse, ShaderValue::fromMatrix(model.inverse()));
        if (req_mvp || req_mvpi) {
            const Matrix4d mvp = view_projection * model;
            if (req_mvp) writer.Write(Output::ModelViewProjection, ShaderValue::fromMatrix(mvp));
            if (req_mvpi)
                writer.Write(Output::ModelViewProjectionInverse,
                             ShaderValue::fromMatrix(mvp.inverse()));
        }
        if (req_effect_model) {
            Matrix4d layer_model  = model;
            Matrix4d effect_model = model;
            if (m_node->effect_projection_node.is_some()) {
                auto& source = **m_node->effect_projection_node;
                source.UpdateTrans();
                layer_model  = source.ModelTrans();
                effect_model = layer_model;
                if (m_node->effect_projection_size[usize(0)] > 0.0f &&
                    m_node->effect_projection_size[usize(1)] > 0.0f) {
                    effect_model =
                        effect_model *
                        Affine3d(
                            Scaling(
                                static_cast<double>(m_node->effect_projection_size[usize(0)]) * 0.5,
                                static_cast<double>(m_node->effect_projection_size[usize(1)]) * 0.5,
                                1.0))
                            .matrix();
                }
            }
            writer.Write(Output::LayerModel, ShaderValue::fromMatrix(layer_model));
            writer.Write(Output::EffectModel, ShaderValue::fromMatrix(effect_model));
            if (req_emvp || req_emvpi) {
                const Matrix4d effect_view =
                    active_camera_ref->GetViewProjectionMatrix(render_view);
                const Matrix4d effect_mvp = effect_view * effect_model;
                if (req_emvp)
                    writer.Write(Output::EffectModelViewProjection,
                                 ShaderValue::fromMatrix(effect_mvp));
                if (req_emvpi)
                    writer.Write(Output::EffectModelViewProjectionInverse,
                                 ShaderValue::fromMatrix(effect_mvp.inverse()));
            }
        }
    }

    return writer.Finish();
}

auto WPFrameUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = WPFrameUniformOutput;
    const rstd::array<BindingEntry<Output>, 10> entries {
        BindingEntry<Output> { Output::Time, G_TIME, UniformValueShape::Float(u32(1)) },
        BindingEntry<Output> { Output::FrameTime, G_FRAMETIME, UniformValueShape::Float(u32(1)) },
        BindingEntry<Output> { Output::DayTime, G_DAYTIME, UniformValueShape::Float(u32(1)) },
        BindingEntry<Output> {
            Output::DayTime, G_DAYTIME_LEGACY, UniformValueShape::Float(u32(1)) },
        BindingEntry<Output> {
            Output::PointerPosition, G_POINTERPOSITION, UniformValueShape::Float(u32(2)) },
        BindingEntry<Output> {
            Output::PointerPositionLast, G_POINTERPOSITIONLAST, UniformValueShape::Float(u32(2)) },
        BindingEntry<Output> {
            Output::ParallaxPosition, G_PARALLAXPOSITION, UniformValueShape::Float(u32(2)) },
        BindingEntry<Output> { Output::TexelSize, G_TEXELSIZE, UniformValueShape::Float(u32(2)) },
        BindingEntry<Output> {
            Output::TexelSizeHalf, G_TEXELSIZEHALF, UniformValueShape::Float(u32(2)) },
        BindingEntry<Output> { Output::Screen, G_SCREEN, UniformValueShape::Float(u32(3)) },
    };
    return BindEntries(sink, entries);
}

auto WPFrameUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto WPFrameUniformSource::Evaluate(ref<dyn<UniformUpdateContext>> context,
                                    mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = WPFrameUniformOutput;
    WPUniformWriter writer(sink);
    const auto      frame     = context->Frame();
    const auto&     inputs    = m_state->Inputs();
    auto            resources = context->Resources();
    const auto      texel     = resources->TexelSize();
    const auto      viewport  = resources->Viewport();

    writer.Write(Output::Time, static_cast<float>(frame->elapsed.to_primitive()));
    writer.Write(Output::FrameTime, static_cast<float>(frame->delta.to_primitive()));
    writer.Write(Output::DayTime, 0.0f);
    writer.Write(Output::PointerPosition, inputs.pointer);
    writer.Write(Output::PointerPositionLast, inputs.pointer_last);
    writer.Write(Output::TexelSize, texel);
    writer.Write(Output::TexelSizeHalf,
                 rstd::array<float, 2> { texel[usize(0)] * 0.5f, texel[usize(1)] * 0.5f });
    const float aspect = viewport[usize(1)] > 0.0f ? viewport[usize(0)] / viewport[usize(1)] : 1.0f;
    writer.Write(Output::Screen,
                 rstd::array<float, 3> { viewport[usize(0)], viewport[usize(1)], aspect });

    Vector2f    parallax_position { 0.5f, 0.5f };
    const auto& parallax = m_state->CameraParallax();
    if (parallax.enable) {
        const Vector2f centered = Vector2f(inputs.pointer.data()) - Vector2f { 0.5f, 0.5f };
        parallax_position =
            Vector2f { 0.5f, 0.5f } + (Scaling(1.0f, -1.0f) * centered) * parallax.mouse_influence;
    }
    writer.Write(Output::ParallaxPosition,
                 rstd::array<float, 2> { parallax_position.x(), parallax_position.y() });
    return writer.Finish();
}

auto WPAudioUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = WPAudioUniformOutput;
    const rstd::array<BindingEntry<Output>, 6> entries {
        BindingEntry<Output> {
            Output::Spectrum16Left, G_AUDIO_SPEC_16_L, UniformValueShape::Float(u32(16)) },
        BindingEntry<Output> {
            Output::Spectrum16Right, G_AUDIO_SPEC_16_R, UniformValueShape::Float(u32(16)) },
        BindingEntry<Output> {
            Output::Spectrum32Left, G_AUDIO_SPEC_32_L, UniformValueShape::Float(u32(32)) },
        BindingEntry<Output> {
            Output::Spectrum32Right, G_AUDIO_SPEC_32_R, UniformValueShape::Float(u32(32)) },
        BindingEntry<Output> {
            Output::Spectrum64Left, G_AUDIO_SPEC_64_L, UniformValueShape::Float(u32(64)) },
        BindingEntry<Output> {
            Output::Spectrum64Right, G_AUDIO_SPEC_64_R, UniformValueShape::Float(u32(64)) },
    };
    return BindEntries(sink, entries);
}

auto WPAudioUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto WPAudioUniformSource::AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> {
    return Some(m_state->AcquireAudioResponse());
}

auto WPParticleTrailUniformSource::AcquireBindingLease() const
    -> Option<Box<dyn<UniformBindingLease>>> {
    return MakeArcUniformBindingLease(m_state);
}

auto WPAudioUniformSource::Evaluate(ref<dyn<UniformUpdateContext>>,
                                    mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = WPAudioUniformOutput;
    WPUniformWriter        writer(sink);
    const auto&            inputs = m_state->Inputs();
    rstd::array<float, 16> audio_16_left {};
    rstd::array<float, 16> audio_16_right {};
    rstd::array<float, 32> audio_32_left {};
    rstd::array<float, 32> audio_32_right {};
    AverageResample64(inputs.audio_left.as_slice(), audio_16_left);
    AverageResample64(inputs.audio_right.as_slice(), audio_16_right);
    AverageResample64(inputs.audio_left.as_slice(), audio_32_left);
    AverageResample64(inputs.audio_right.as_slice(), audio_32_right);
    auto write_audio = [&](Output output, slice<float> values) {
        if (writer.Wants(output))
            writer.Write(output, UniformValue(values.as_raw_ptr(), values.len()));
    };
    write_audio(Output::Spectrum16Left, audio_16_left.as_slice());
    write_audio(Output::Spectrum16Right, audio_16_right.as_slice());
    write_audio(Output::Spectrum32Left, audio_32_left.as_slice());
    write_audio(Output::Spectrum32Right, audio_32_right.as_slice());
    write_audio(Output::Spectrum64Left, inputs.audio_left.as_slice());
    write_audio(Output::Spectrum64Right, inputs.audio_right.as_slice());
    return writer.Finish();
}

auto WPColorUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = WPColorUniformOutput;
    const rstd::array<BindingEntry<Output>, 5> entries {
        BindingEntry<Output> { Output::UserAlpha, G_USERALPHA, UniformValueShape::Float(u32(1)) },
        BindingEntry<Output> { Output::Color4, G_COLOR4, UniformValueShape::Float(u32(4)) },
        BindingEntry<Output> { Output::Color, G_COLOR, UniformValueShape::Float(u32(3)) },
        BindingEntry<Output> { Output::Alpha, G_ALPHA, UniformValueShape::Float(u32(1)) },
        BindingEntry<Output> { Output::Brightness, G_BRIGHTNESS, UniformValueShape::Float(u32(1)) },
    };
    return BindEntries(sink, entries);
}

auto WPColorUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto WPColorUniformSource::Evaluate(ref<dyn<UniformUpdateContext>>,
                                    mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = WPColorUniformOutput;
    WPUniformWriter writer(sink);
    const auto&     node           = *m_node;
    const bool      has_user_alpha = writer.Wants(Output::UserAlpha);
    const bool      has_alpha      = writer.Wants(Output::Alpha);
    const bool      has_color4     = writer.Wants(Output::Color4);
    const bool      has_color      = writer.Wants(Output::Color);
    auto            write_color4   = [&](const Vector3f& color, float alpha) {
        writer.Write(Output::Color4,
                     rstd::array<float, 4> { color.x(), color.y(), color.z(), alpha });
    };
    auto write_color = [&](const Vector3f& color) {
        writer.Write(Output::Color, rstd::array<float, 3> { color.x(), color.y(), color.z() });
    };
    if (node.IsAlphaOverridden()) {
        if (has_user_alpha) writer.Write(Output::UserAlpha, node.EffectiveAlpha());
        if (has_alpha) writer.Write(Output::Alpha, node.EffectiveAlpha());
        if (has_color4) {
            if (! has_user_alpha) {
                write_color4(node.IsColorOverridden() ? node.Color() : node.BaseColor(),
                             node.EffectiveAlpha());
            } else if (node.IsColorOverridden()) {
                write_color4(node.Color(), node.BaseAlpha());
            }
        }
        if (has_color && node.IsColorOverridden()) write_color(node.Color());
    } else if (node.IsColorOverridden()) {
        if (has_color4) write_color4(node.Color(), node.BaseAlpha());
        if (has_color) write_color(node.Color());
    }
    if (node.IsBrightnessOverridden()) writer.Write(Output::Brightness, node.Brightness());
    return writer.Finish();
}

auto WPLightUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = WPLightUniformOutput;
    const rstd::array<BindingEntry<Output>, 3> entries {
        BindingEntry<Output> { Output::Position, G_LP, UniformValueShape::Float(u32(12)) },
        BindingEntry<Output> { Output::ColorLegacy, G_LCP, UniformValueShape::Float(u32(12)) },
        BindingEntry<Output> { Output::ColorRadius, G_LCR, UniformValueShape::Float(u32(16)) },
    };
    return BindEntries(sink, entries);
}

auto WPLightUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto WPLightUniformSource::Evaluate(ref<dyn<UniformUpdateContext>>,
                                    mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = WPLightUniformOutput;
    WPUniformWriter        writer(sink);
    constexpr usize        max_lights { 4 };
    rstd::array<float, 12> positions {};
    rstd::array<float, 16> colors_radius {};
    rstd::array<float, 12> colors_legacy {};
    for (usize index {}; index < rstd::cmp::min(max_lights, m_lights.len()); ++index) {
        const auto& light = *m_lights[index];
        if (light.node() == nullptr || ! light.runtimeVisible()) continue;
        const auto position                        = light.node()->Translate();
        positions[index * usize(3)]                = position.x();
        positions[index * usize(3) + usize(1)]     = position.y();
        positions[index * usize(3) + usize(2)]     = position.z();
        colors_radius[index * usize(4)]            = light.color().x();
        colors_radius[index * usize(4) + usize(1)] = light.color().y();
        colors_radius[index * usize(4) + usize(2)] = light.color().z();
        colors_radius[index * usize(4) + usize(3)] = light.radius();
        if (index < usize(3)) {
            const auto premultiplied = light.premultipliedColor();
            for (usize component {}; component < usize(3); ++component) {
                colors_legacy[index * usize(4) + component] =
                    premultiplied[component.to_primitive()];
            }
        }
    }
    writer.Write(Output::Position, positions);
    writer.Write(Output::ColorLegacy, colors_legacy);
    writer.Write(Output::ColorRadius, colors_radius);
    return writer.Finish();
}

auto WPTextureUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    for (std::size_t index = 0; index < WE_GLTEX_NAMES.size(); ++index) {
        auto resolution =
            Bind(sink,
                 WPTextureResolutionOutput(index),
                 rstd::cppstd::as_str(std::string_view(WE_GLTEX_RESOLUTION_NAMES[index])).unwrap(),
                 UniformValueShape::Float(u32(4)));
        if (resolution.is_err()) return resolution;
        auto mipmap =
            Bind(sink,
                 WPTextureMipmapOutput(index),
                 rstd::cppstd::as_str(std::string_view(WE_GLTEX_MIPMAPINFO_NAMES[index])).unwrap(),
                 UniformValueShape::Float(u32(1)));
        if (mipmap.is_err()) return mipmap;
        auto rotation =
            Bind(sink,
                 WPTextureRotationOutput(index),
                 rstd::cppstd::as_str(std::string_view(WE_GLTEX_ROTATION_NAMES[index])).unwrap(),
                 UniformValueShape::Float(u32(4)));
        if (rotation.is_err()) return rotation;
        auto translation =
            Bind(sink,
                 WPTextureTranslationOutput(index),
                 rstd::cppstd::as_str(std::string_view(WE_GLTEX_TRANSLATION_NAMES[index])).unwrap(),
                 UniformValueShape::Float(u32(2)));
        if (translation.is_err()) return translation;
    }
    return Ok(empty {});
}

auto WPTextureUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto WPTextureUniformSource::Evaluate(ref<dyn<UniformUpdateContext>> context,
                                      mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    WPUniformWriter writer(sink);
    auto            resources = context->Resources();
    for (std::size_t index = 0; index < WE_GLTEX_NAMES.size(); ++index) {
        auto texture = resources->Texture(usize(index));
        if (texture.is_none()) continue;
        if (texture->has_extent) {
            writer.Write(WPTextureResolutionOutput(index),
                         rstd::array<float, 4> { texture->source_extent[usize(0)],
                                                 texture->source_extent[usize(1)],
                                                 texture->sample_extent[usize(0)],
                                                 texture->sample_extent[usize(1)] });
        }
        if (texture->has_mipmap) {
            writer.Write(WPTextureMipmapOutput(index), texture->mipmap_level);
        }
        if (texture->has_transform) {
            writer.Write(WPTextureRotationOutput(index), texture->rotation);
            writer.Write(WPTextureTranslationOutput(index), texture->translation);
        }
    }
    return writer.Finish();
}

auto WPParticleTrailUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    return Bind(sink,
                WPParticleTrailUniformOutput::RenderVar0,
                G_RENDERVAR0,
                UniformValueShape::Float(u32(4)));
}

auto WPParticleTrailUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto WPParticleTrailUniformSource::Evaluate(ref<dyn<UniformUpdateContext>>,
                                            mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    WPUniformWriter writer(sink);
    writer.Write(WPParticleTrailUniformOutput::RenderVar0, m_state->render_var);
    return writer.Finish();
}

auto WPPuppetUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    return Bind(sink,
                UniformOutputId { .value = u32() },
                G_BONES,
                UniformValueShape::MatrixArray(u32(4), u32(4), usize(1), usize(256)));
}

auto WPPuppetUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto WPPuppetUniformSource::Evaluate(ref<dyn<UniformUpdateContext>> context,
                                     mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    const auto output = UniformOutputId { .value = u32() };
    if (! sink->Wants(output)) return Ok(empty {});
    auto matrices = m_layer->genFrame(context->Frame()->elapsed.to_primitive());
    if (matrices.is_empty()) return Ok(empty {});
    auto value = UniformValue::fromMatrixArray(matrices[usize()].data(),
                                               u32(4),
                                               u32(4),
                                               matrices.len(),
                                               UniformMatrixStorage::ColumnMajor);
    return sink->Write(output, value.View());
}

} // namespace owe
