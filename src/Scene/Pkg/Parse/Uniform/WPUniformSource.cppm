export module wescene.pkg.parse:wp_uniform_source;
import rstd;
import rstd.cppstd;
import wescene.json;
import wescene.pkg.puppet;
import wescene.scene;

using namespace rstd::prelude;

export namespace owe
{

enum class WPTransformUniformOutput : u32
{
    ModelInverse,
    Model,
    AlternateModel,
    ModelViewProjection,
    ModelViewProjectionInverse,
    EyePosition,
    EffectModel,
    EffectModelViewProjection,
    EffectModelViewProjectionInverse,
    LayerModel,
    EffectTextureViewProjection,
    EffectTextureViewProjectionInverse,
    ViewProjection,
};

enum class WPFrameUniformOutput : u32
{
    Time,
    FrameTime,
    DayTime,
    PointerPosition,
    PointerPositionLast,
    ParallaxPosition,
    TexelSize,
    TexelSizeHalf,
    Screen,
};

enum class WPLightUniformOutput : u32
{
    Position,
    ColorLegacy,
    ColorRadius,
};

enum class WPColorUniformOutput : u32
{
    UserAlpha,
    Color4,
    Color,
    Alpha,
    Brightness,
};

enum class WPAudioUniformOutput : u32
{
    Spectrum16Left,
    Spectrum16Right,
    Spectrum32Left,
    Spectrum32Right,
    Spectrum64Left,
    Spectrum64Right,
};

enum class WPTextureUniformOutput : u32
{
    Resolution0  = 0,
    Mipmap0      = 16,
    Rotation0    = 32,
    Translation0 = 48,
};

template<typename Output>
inline auto ToUniformOutput(Output output) -> UniformOutputId {
    return { .value = static_cast<u32>(output) };
}

inline auto WPTextureResolutionOutput(usize index) -> UniformOutputId {
    return { .value =
                 static_cast<u32>(WPTextureUniformOutput::Resolution0) + static_cast<u32>(index) };
}

inline auto WPTextureMipmapOutput(usize index) -> UniformOutputId {
    return { .value = static_cast<u32>(WPTextureUniformOutput::Mipmap0) + static_cast<u32>(index) };
}

inline auto WPTextureRotationOutput(usize index) -> UniformOutputId {
    return { .value =
                 static_cast<u32>(WPTextureUniformOutput::Rotation0) + static_cast<u32>(index) };
}

inline auto WPTextureTranslationOutput(usize index) -> UniformOutputId {
    return { .value =
                 static_cast<u32>(WPTextureUniformOutput::Translation0) + static_cast<u32>(index) };
}

struct WPUniformCameraParallax {
    bool enable { false };
    f32  amount { 0.0f };
    f32  delay { 0.0f };
    f32  mouse_influence { 0.0f };
};

struct WPUniformCameraShake {
    bool enable { false };
    f32  amplitude { 0.0f };
    f32  speed { 0.0f };
    f32  roughness { 1.0f };
};

struct WPUniformNodeConfigDraft {
    bool                               configured { false };
    rstd::array<f32, 2>                parallax_depth { 0.0f, 0.0f };
    rstd::array<f32, 2>                propagated_parallax_depth { 0.0f, 0.0f };
    bool                               propagate_parallax_to_children { true };
    bool                               use_camera_eye_position { false };
    Option<rstd::sync::Arc<SceneNode>> effect_projection_node;
    rstd::array<f32, 2>                effect_projection_size { 0.0f, 0.0f };

    auto Clone() const -> WPUniformNodeConfigDraft;
};

class WPUniformCameraResolver {
public:
    explicit WPUniformCameraResolver(std::shared_ptr<SceneCamera> active_camera)
        : m_active_camera(rstd::move(active_camera)) {}

    void Add(std::string name, std::shared_ptr<SceneCamera> camera);
    void Reserve(usize count) { m_cameras.reserve(count); }

    auto Resolve(const SceneNode& node) const -> std::shared_ptr<SceneCamera>;
    auto Active() const -> std::shared_ptr<SceneCamera> { return m_active_camera; }

private:
    struct Entry {
        std::string                  name;
        std::shared_ptr<SceneCamera> camera;
    };

    Vec<Entry>                   m_cameras;
    std::shared_ptr<SceneCamera> m_active_camera;
};

struct WPUniformNodeState {
    rstd::sync::Arc<SceneNode>               node;
    std::shared_ptr<WPUniformCameraResolver> camera_resolver;
    rstd::array<f32, 2>                      propagated_parallax_depth { 0.0f, 0.0f };
    bool                                     propagate_parallax_to_children { true };
    bool                                     use_camera_eye_position { false };
    Option<rstd::sync::Arc<SceneNode>>       effect_projection_node;
    rstd::array<f32, 2>                      effect_projection_size { 0.0f, 0.0f };

    explicit WPUniformNodeState(rstd::sync::Arc<SceneNode> value): node(rstd::move(value)) {}
};

struct WPUniformFrameInputs {
    rstd::array<f32, 2>  pointer { 0.5f, 0.5f };
    rstd::array<f32, 2>  pointer_last { 0.5f, 0.5f };
    rstd::array<f32, 64> audio_left {};
    rstd::array<f32, 64> audio_right {};
};

class WPUniformSceneState {
public:
    explicit WPUniformSceneState(std::shared_ptr<AudioResponseDemand> demand = {})
        : m_audio_demand(std::move(demand)) {}

    auto SetNodeState(SceneNodeId, std::shared_ptr<WPUniformNodeState>)
        -> std::shared_ptr<WPUniformNodeState>;
    bool SetEffectProjectionSize(SceneNodeId, rstd::array<f32, 2>);
    auto ResolveParallaxState(const WPUniformNodeState&) const -> const WPUniformNodeState&;

    WPUniformCameraParallax&       CameraParallax() noexcept { return m_camera_parallax; }
    const WPUniformCameraParallax& CameraParallax() const noexcept { return m_camera_parallax; }
    WPUniformCameraShake&          CameraShake() noexcept { return m_camera_shake; }
    const WPUniformCameraShake&    CameraShake() const noexcept { return m_camera_shake; }
    const WPUniformFrameInputs&    Inputs() const noexcept { return m_inputs; }
    rstd::array<f32, 2>            Ortho() const noexcept { return m_ortho; }

    void SetOrtho(f32 width, f32 height) { m_ortho = { width, height }; }
    void SetPointerInput(f64, f64);
    void SetPointerDelay(f32);
    void SetAudioSpectrum(slice<f32>, slice<f32>);
    void Advance(const SceneFrame&);
    void ApplyUserProperty(std::string_view, const Json&);
    auto AcquireAudioResponse() const -> std::shared_ptr<void> {
        return m_audio_demand ? m_audio_demand->Acquire() : std::shared_ptr<void> {};
    }

private:
    static u64 Key(SceneNodeId node) {
        return (static_cast<u64>(node.generation) << 32) | static_cast<u64>(node.index);
    }

    rstd::collections::HashMap<u64, std::shared_ptr<WPUniformNodeState>> m_nodes;
    rstd::collections::HashMap<const SceneNode*, std::shared_ptr<WPUniformNodeState>>
                                         m_nodes_by_address;
    WPUniformFrameInputs                 m_inputs;
    WPUniformCameraParallax              m_camera_parallax;
    WPUniformCameraShake                 m_camera_shake;
    rstd::array<f32, 2>                  m_ortho { 1920.0f, 1080.0f };
    rstd::array<f32, 2>                  m_pointer_input { 0.5f, 0.5f };
    std::shared_ptr<AudioResponseDemand> m_audio_demand;
    f32                                  m_pointer_delay { 0.0f };
    f64                                  m_pointer_delayed_time { 0.0 };
    rstd::time::Instant                  m_last_pointer_input_time { rstd::time::Instant::now() };
};

class WPUniformRuntimeInput {
public:
    explicit WPUniformRuntimeInput(std::shared_ptr<WPUniformSceneState> state)
        : m_state(rstd::move(state)) {}

    void SetPointerInput(f64 x, f64 y) {
        if (m_state) m_state->SetPointerInput(x, y);
    }
    void SetAudioSpectrum(slice<f32> left, slice<f32> right) {
        if (m_state) m_state->SetAudioSpectrum(left, right);
    }

private:
    std::shared_ptr<WPUniformSceneState> m_state;
};

class WPUniformRuntimeSystem {
public:
    explicit WPUniformRuntimeSystem(std::shared_ptr<WPUniformSceneState> state)
        : m_state(rstd::move(state)) {}

    void Update(ref<SceneFrame> frame) {
        if (m_state) m_state->Advance(*frame);
    }

private:
    std::shared_ptr<WPUniformSceneState> m_state;
};

class WPTransformUniformSource {
public:
    WPTransformUniformSource(std::shared_ptr<WPUniformSceneState> state,
                             std::shared_ptr<WPUniformNodeState>  node)
        : m_state(rstd::move(state)), m_node(rstd::move(node)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> std::shared_ptr<void> { return {}; }

private:
    std::shared_ptr<WPUniformSceneState> m_state;
    std::shared_ptr<WPUniformNodeState>  m_node;
};

class WPFrameUniformSource {
public:
    explicit WPFrameUniformSource(std::shared_ptr<WPUniformSceneState> state)
        : m_state(rstd::move(state)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> std::shared_ptr<void> { return {}; }

private:
    std::shared_ptr<WPUniformSceneState> m_state;
};

class WPAudioUniformSource {
public:
    explicit WPAudioUniformSource(std::shared_ptr<WPUniformSceneState> state)
        : m_state(rstd::move(state)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> std::shared_ptr<void>;

private:
    std::shared_ptr<WPUniformSceneState> m_state;
};

class WPColorUniformSource {
public:
    explicit WPColorUniformSource(rstd::sync::Arc<SceneNode> node): m_node(rstd::move(node)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> std::shared_ptr<void> { return {}; }

private:
    rstd::sync::Arc<SceneNode> m_node;
};

class WPLightUniformSource {
public:
    explicit WPLightUniformSource(Vec<ref<SceneLight>> lights): m_lights(rstd::move(lights)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> std::shared_ptr<void> { return {}; }

private:
    Vec<ref<SceneLight>> m_lights;
};

class WPTextureUniformSource {
public:
    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> std::shared_ptr<void> { return {}; }
};

class WPPuppetUniformSource {
public:
    explicit WPPuppetUniformSource(std::shared_ptr<WPPuppetLayer> layer)
        : m_layer(rstd::move(layer)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> std::shared_ptr<void> { return {}; }

private:
    std::shared_ptr<WPPuppetLayer> m_layer;
};

} // namespace owe
