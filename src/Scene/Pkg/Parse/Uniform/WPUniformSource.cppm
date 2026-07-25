export module wescene.pkg.parse:wp_uniform_source;
import rstd;
import rstd.cppstd;
import wescene.json;
import wescene.pkg.puppet;
import wescene.scene;
import :wp_particle_runtime;

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::sync::Arc;

export namespace owe
{

enum class WPTransformUniformOutput : rstd::uint32_t
{
    ModelInverse,
    Model,
    NormalModel,
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

enum class WPFrameUniformOutput : rstd::uint32_t
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

enum class WPLightUniformOutput : rstd::uint32_t
{
    Position,
    ColorLegacy,
    ColorRadius,
};

enum class WPColorUniformOutput : rstd::uint32_t
{
    UserAlpha,
    Color4,
    Color,
    Alpha,
    Brightness,
};

enum class WPAudioUniformOutput : rstd::uint32_t
{
    Spectrum16Left,
    Spectrum16Right,
    Spectrum32Left,
    Spectrum32Right,
    Spectrum64Left,
    Spectrum64Right,
};

enum class WPTextureUniformOutput : rstd::uint32_t
{
    Resolution0  = 0,
    Mipmap0      = 16,
    Rotation0    = 32,
    Translation0 = 48,
};

enum class WPParticleTrailUniformOutput : rstd::uint32_t
{
    RenderVar0,
};

template<typename Output>
inline auto ToUniformOutput(Output output) -> UniformOutputId {
    return { .value = u32(static_cast<rstd::uint32_t>(output)) };
}

inline auto WPTextureResolutionOutput(std::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(WPTextureUniformOutput::Resolution0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto WPTextureMipmapOutput(std::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(WPTextureUniformOutput::Mipmap0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto WPTextureRotationOutput(std::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(WPTextureUniformOutput::Rotation0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto WPTextureTranslationOutput(std::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(WPTextureUniformOutput::Translation0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

struct WPUniformCameraParallax {
    bool  enable { false };
    float amount { 0.0f };
    float delay { 0.0f };
    float mouse_influence { 0.0f };
};

struct WPUniformCameraShake {
    bool  enable { false };
    float amplitude { 0.0f };
    float speed { 0.0f };
    float roughness { 1.0f };
};

struct WPUniformNodeConfigDraft {
    bool                   configured { false };
    array<float, 2>        parallax_depth { 0.0f, 0.0f };
    array<float, 2>        propagated_parallax_depth { 0.0f, 0.0f };
    bool                   propagate_parallax_to_children { true };
    bool                   use_camera_eye_position { false };
    bool                   vertices_in_world_space { false };
    Option<Arc<SceneNode>> effect_projection_node;
    array<float, 2>        effect_projection_size { 0.0f, 0.0f };

    auto Clone() const -> WPUniformNodeConfigDraft;
};

class WPUniformCameraResolver {
public:
    explicit WPUniformCameraResolver(Arc<SceneCamera> active_camera)
        : m_active_camera(rstd::move(active_camera)) {}

    void Add(String name, Arc<SceneCamera> camera);
    void Reserve(usize count) { m_cameras.reserve(count); }

    auto Resolve(const SceneNode& node) const -> Option<mut_ref<SceneCamera>>;
    auto Active() const -> mut_ref<SceneCamera> { return m_active_camera.deref_mut(); }

private:
    HashMap<String, Arc<SceneCamera>> m_cameras;
    Arc<SceneCamera>                  m_active_camera;
};

struct WPUniformNodeState {
    Arc<SceneNode>               node;
    Arc<WPUniformCameraResolver> camera_resolver;
    array<float, 2>              propagated_parallax_depth { 0.0f, 0.0f };
    bool                         propagate_parallax_to_children { true };
    bool                         use_camera_eye_position { false };
    bool                         vertices_in_world_space { false };
    Option<Arc<SceneNode>>       effect_projection_node;
    array<float, 2>              effect_projection_size { 0.0f, 0.0f };

    WPUniformNodeState(Arc<SceneNode> value, Arc<WPUniformCameraResolver> resolver)
        : node(rstd::move(value)), camera_resolver(rstd::move(resolver)) {}
};

struct WPUniformFrameInputs {
    array<float, 2>  pointer { 0.5f, 0.5f };
    array<float, 2>  pointer_last { 0.5f, 0.5f };
    array<float, 64> audio_left {};
    array<float, 64> audio_right {};
};

class WPUniformSceneState {
public:
    explicit WPUniformSceneState(Arc<AudioResponseDemand> demand)
        : m_audio_demand(rstd::move(demand)) {}

    void SetNodeState(SceneNodeId, Arc<WPUniformNodeState>);
    bool SetEffectProjectionSize(SceneNodeId, array<float, 2>);
    auto ResolveParallaxState(const WPUniformNodeState&) const -> const WPUniformNodeState&;

    WPUniformCameraParallax&       CameraParallax() noexcept { return m_camera_parallax; }
    const WPUniformCameraParallax& CameraParallax() const noexcept { return m_camera_parallax; }
    WPUniformCameraShake&          CameraShake() noexcept { return m_camera_shake; }
    const WPUniformCameraShake&    CameraShake() const noexcept { return m_camera_shake; }
    const WPUniformFrameInputs&    Inputs() const noexcept { return m_inputs; }
    array<float, 2>                Ortho() const noexcept { return m_ortho; }

    void SetOrtho(float width, float height) { m_ortho = { width, height }; }
    void SetPointerInput(double, double);
    void SetPointerDelay(float);
    void SetAudioSpectrum(slice<float>, slice<float>);
    void Advance(const SceneFrame&);
    void ApplyUserProperty(std::string_view, const Json&);
    auto AcquireAudioResponse() const -> Box<dyn<UniformBindingLease>> {
        return m_audio_demand->Acquire();
    }

private:
    static u64 Key(SceneNodeId node) {
        const auto generation = static_cast<rstd::uint64_t>(node.generation.to_primitive());
        const auto index      = static_cast<rstd::uint64_t>(node.index.to_primitive());
        return u64((generation << 32U) | index);
    }

    HashMap<u64, Arc<WPUniformNodeState>>              m_nodes;
    HashMap<const SceneNode*, Arc<WPUniformNodeState>> m_nodes_by_address;
    WPUniformFrameInputs                               m_inputs;
    WPUniformCameraParallax                            m_camera_parallax;
    WPUniformCameraShake                               m_camera_shake;
    array<float, 2>                                    m_ortho { 1920.0f, 1080.0f };
    array<float, 2>                                    m_pointer_input { 0.5f, 0.5f };
    Arc<AudioResponseDemand>                           m_audio_demand;
    float                                              m_pointer_delay { 0.0f };
    double                                             m_pointer_delayed_time { 0.0 };
    rstd::time::Instant m_last_pointer_input_time { rstd::time::Instant::now() };
};

class WPUniformRuntimeInput {
public:
    explicit WPUniformRuntimeInput(Arc<WPUniformSceneState> state): m_state(rstd::move(state)) {}

    void SetPointerInput(double x, double y) { m_state->SetPointerInput(x, y); }
    void SetAudioSpectrum(slice<float> left, slice<float> right) {
        m_state->SetAudioSpectrum(left, right);
    }

private:
    Arc<WPUniformSceneState> m_state;
};

class WPUniformRuntimeSystem {
public:
    explicit WPUniformRuntimeSystem(Arc<WPUniformSceneState> state): m_state(rstd::move(state)) {}

    void Update(ref<SceneFrame> frame) { m_state->Advance(*frame); }

private:
    Arc<WPUniformSceneState> m_state;
};

class WPTransformUniformSource {
public:
    WPTransformUniformSource(Arc<WPUniformSceneState> state, Arc<WPUniformNodeState> node)
        : m_state(rstd::move(state)), m_node(rstd::move(node)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Arc<WPUniformSceneState> m_state;
    Arc<WPUniformNodeState>  m_node;
};

class WPFrameUniformSource {
public:
    explicit WPFrameUniformSource(Arc<WPUniformSceneState> state): m_state(rstd::move(state)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Arc<WPUniformSceneState> m_state;
};

class WPAudioUniformSource {
public:
    explicit WPAudioUniformSource(Arc<WPUniformSceneState> state): m_state(rstd::move(state)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>>;

private:
    Arc<WPUniformSceneState> m_state;
};

class WPColorUniformSource {
public:
    explicit WPColorUniformSource(Arc<SceneNode> node): m_node(rstd::move(node)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Arc<SceneNode> m_node;
};

class WPLightUniformSource {
public:
    explicit WPLightUniformSource(Vec<ref<SceneLight>> lights): m_lights(rstd::move(lights)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Vec<ref<SceneLight>> m_lights;
};

class WPTextureUniformSource {
public:
    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }
};

class WPParticleTrailUniformSource {
public:
    explicit WPParticleTrailUniformSource(Arc<WPParticleTrailUniformState> state)
        : m_state(rstd::move(state)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>>;

private:
    Arc<WPParticleTrailUniformState> m_state;
};

class WPPuppetUniformSource {
public:
    explicit WPPuppetUniformSource(Arc<WPPuppetLayer> layer): m_layer(rstd::move(layer)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Arc<WPPuppetLayer> m_layer;
};

} // namespace owe
