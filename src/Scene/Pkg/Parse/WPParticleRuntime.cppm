module;

#include <rstd/macro.hpp>

export module wescene.pkg.parse:wp_particle_runtime;

import eigen;
import rstd;
import rstd.cppstd;
import wescene.core;
import wescene.particle;
import wescene.particle.program;
import wescene.scene;

export import wescene.pkg.scene_obj;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace owe
{

enum class WPParticleAnimationMode
{
    SEQUENCE,
    RANDOMONE,
};

struct WPParticleControlpoint {
    bool                        link_mouse { false };
    bool                        worldspace { false };
    Eigen::Vector3d             base_offset { 0.0, 0.0, 0.0 };
    Eigen::Vector3d             offset { 0.0, 0.0, 0.0 };
    Eigen::Matrix3d             rotation { Eigen::Matrix3d::Identity() };
    Option<SceneAnimationCurve> angle_curve;
};

struct WPParticleAudioResponse {
    bool                  enable { false };
    float                 amount { 1.0f };
    float                 exponent { 1.0f };
    rstd::array<float, 2> frequency { 0.0f, 15.0f };
    rstd::array<float, 2> bounds { 0.0f, 1.0f };
};

struct WPParticleFollowAnchor {
    bool  trail_renderer { false };
    float length { 0.0f };
    float max_length { 0.0f };
    float texture_ratio { 1.0f };
};

struct WPParticleTrailUniformState {
    rstd::array<float, 4> render_var {};
};

struct WPParticleAttributes {
    particle::ParticleAttributeKey<particle::PositionAttribute>            position;
    particle::ParticleAttributeKey<particle::VelocityAttribute>            velocity;
    particle::ParticleAttributeKey<particle::AccelerationAttribute>        acceleration;
    particle::ParticleAttributeKey<particle::RotationAttribute>            rotation;
    particle::ParticleAttributeKey<particle::AngularVelocityAttribute>     angular_velocity;
    particle::ParticleAttributeKey<particle::AngularAccelerationAttribute> angular_acceleration;
    particle::ParticleAttributeKey<particle::ColorAttribute>               color;
    particle::ParticleAttributeKey<particle::AlphaAttribute>               alpha;
    particle::ParticleAttributeKey<particle::SizeAttribute>                size;
    particle::ParticleAttributeKey<particle::LifetimeAttribute>            lifetime;
    particle::ParticleAttributeKey<particle::RandomAttribute>              random;
    particle::ParticleAttributeKey<particle::InitialColorAttribute>        initial_color;
    particle::ParticleAttributeKey<particle::InitialAlphaAttribute>        initial_alpha;
    particle::ParticleAttributeKey<particle::InitialSizeAttribute>         initial_size;
    particle::ParticleAttributeKey<particle::InitialLifetimeAttribute>     initial_lifetime;

    static auto Register(particle::ParticleSchemaBuilder&) -> WPParticleAttributes;
};

#define OWE_WP_OSCILLATION_ATTRIBUTE(Name, Type)                                                   \
    struct Name {                                                                                  \
        using Value = Type;                                                                        \
                                                                                                   \
        Name(particle::ParticleAttributeDescriptor descriptor, Value default_value)                \
            : storage(rstd::move(descriptor), rstd::move(default_value)) {}                        \
                                                                                                   \
        auto Descriptor() const -> ref<particle::ParticleAttributeDescriptor> {                    \
            return storage.Descriptor();                                                           \
        }                                                                                          \
        auto ConcreteType() const noexcept -> rstd::any::TypeId { return storage.ConcreteType(); } \
        auto ValueType() const noexcept -> rstd::any::TypeId { return storage.ValueTypeId(); }     \
        auto Len() const noexcept -> usize { return storage.Len(); }                               \
        auto Capacity() const noexcept -> usize { return storage.Capacity(); }                     \
        void Reserve(usize total_slots) { storage.Reserve(total_slots); }                          \
        void AppendDefaults(usize count) { storage.AppendDefaults(count); }                        \
        void ResetSlots(slice<particle::ParticleSlot>) {}                                          \
        void Clear() { storage.Clear(); }                                                          \
        auto Values() const noexcept -> slice<Value> { return storage.Values(); }                  \
        auto ValuesMut() noexcept -> mut_ref<Value[]> { return storage.ValuesMut(); }              \
        auto CloneEmpty() const -> Name {                                                          \
            return Name(storage.CloneDescriptor(), storage.DefaultValue());                        \
        }                                                                                          \
                                                                                                   \
        particle::ParticleValueAttributeStorage<Value> storage;                                    \
    }

OWE_WP_OSCILLATION_ATTRIBUTE(WPOscillationResetAttribute, bool);
OWE_WP_OSCILLATION_ATTRIBUTE(WPOscillationFrequencyAttribute, float);
OWE_WP_OSCILLATION_ATTRIBUTE(WPOscillationScaleAttribute, float);
OWE_WP_OSCILLATION_ATTRIBUTE(WPOscillationPhaseAttribute, float);

#undef OWE_WP_OSCILLATION_ATTRIBUTE

struct WPOscillationStateRef {
    bool&  reset;
    float& frequency;
    float& scale;
    float& phase;
};

struct WPOscillationValues {
    mut_ref<bool[]>  reset;
    mut_ref<float[]> frequency;
    mut_ref<float[]> scale;
    mut_ref<float[]> phase;

    auto At(usize index) -> WPOscillationStateRef {
        return {
            .reset     = reset[index],
            .frequency = frequency[index],
            .scale     = scale[index],
            .phase     = phase[index],
        };
    }
};

struct WPOscillationAttributes {
    particle::ParticleAttributeKey<WPOscillationResetAttribute>     reset;
    particle::ParticleAttributeKey<WPOscillationFrequencyAttribute> frequency;
    particle::ParticleAttributeKey<WPOscillationScaleAttribute>     scale;
    particle::ParticleAttributeKey<WPOscillationPhaseAttribute>     phase;
};

struct WPTrailSlotState {
    usize           head {};
    usize           len {};
    usize           sample_count {};
    Eigen::Vector3f previous_position { Eigen::Vector3f::Zero() };
    bool            has_previous_position { false };
};

struct WPTrailHistoryAttribute {
    using Value = WPTrailSlotState;

    WPTrailHistoryAttribute(particle::ParticleAttributeDescriptor descriptor,
                            usize                                 sample_capacity);

    auto Descriptor() const -> ref<particle::ParticleAttributeDescriptor>;
    auto ConcreteType() const noexcept -> rstd::any::TypeId;
    auto ValueType() const noexcept -> rstd::any::TypeId;
    auto Len() const noexcept -> usize;
    auto Capacity() const noexcept -> usize;
    void Reserve(usize total_slots);
    void AppendDefaults(usize count);
    void ResetSlots(slice<particle::ParticleSlot> slots);
    void Clear();
    auto CloneEmpty() const -> WPTrailHistoryAttribute;

    void Push(particle::ParticleSlot slot, const Eigen::Vector3f& position);
    void Initialize(particle::ParticleSlot slot, const Eigen::Vector3f& position);
    auto At(particle::ParticleSlot slot, usize logical_index) const -> Eigen::Vector3f;
    auto State(particle::ParticleSlot slot) const -> WPTrailSlotState;
    void SetPreviousPosition(particle::ParticleSlot slot, const Eigen::Vector3f& position);
    auto SampleCapacity() const noexcept -> usize { return m_sample_capacity; }

private:
    particle::ParticleAttributeDescriptor m_descriptor;
    usize                                 m_sample_capacity {};
    rstd::vec::Vec<WPTrailSlotState>      m_states;
    rstd::vec::Vec<Eigen::Vector3f>       m_positions;
};

struct WPParticleFrame {
    class WPParticleSubSystem* subsystem { nullptr };
    usize                      instance_index {};
    rstd::array<float, 16>     audio_average {};
    Eigen::Vector3d            mouse_local { Eigen::Vector3d::Zero() };
    Eigen::Matrix3d            world_from_local_dir { Eigen::Matrix3d::Identity() };
    Eigen::Matrix3d            local_from_world_dir { Eigen::Matrix3d::Identity() };
    Eigen::Matrix4d            local_from_world { Eigen::Matrix4d::Identity() };
    f64                        time {};
    f64                        delta {};
    f64                        emitter_delta {};
    usize                      trail_sample_steps {};
    f64                        trail_sample_remainder {};
    bool                       world_space { false };
};

auto WPParticleFrameFrom(ref<dyn<rstd::any::Any>>) -> ref<WPParticleFrame>;

struct WPParticleSpawnColumns {
    mut_ref<particle::ParticleSlotState[]> states;
    mut_ref<Eigen::Vector3f[]>             positions;
    mut_ref<Eigen::Vector3f[]>             velocities;
    mut_ref<Eigen::Vector3f[]>             rotations;
    mut_ref<Eigen::Vector3f[]>             angular_velocities;
    mut_ref<Eigen::Vector3f[]>             colors;
    mut_ref<float[]>                       alphas;
    mut_ref<float[]>                       sizes;
    mut_ref<float[]>                       lifetimes;
    mut_ref<float[]>                       randoms;
    mut_ref<Eigen::Vector3f[]>             initial_colors;
    mut_ref<float[]>                       initial_alphas;
    mut_ref<float[]>                       initial_sizes;
    mut_ref<float[]>                       initial_lifetimes;
};

class WPParticleSpawnInstruction {
public:
    WPParticleSpawnInstruction(const WPParticleSpawnInstruction&)                    = delete;
    auto operator=(const WPParticleSpawnInstruction&) -> WPParticleSpawnInstruction& = delete;
    WPParticleSpawnInstruction(WPParticleSpawnInstruction&&) noexcept;
    auto operator=(WPParticleSpawnInstruction&&) noexcept -> WPParticleSpawnInstruction&;
    ~WPParticleSpawnInstruction();

    void Initialize(WPParticleSpawnColumns&, particle::ParticleSpawnRequest,
                    ref<dyn<rstd::any::Any>>);
    auto SequenceCount() const -> Option<u32>;

private:
    struct Impl;

    explicit WPParticleSpawnInstruction(Box<Impl>);
    template<typename T>
    static auto Make(T value) -> WPParticleSpawnInstruction;

    friend class WPParticleParser;

    Box<Impl> m_impl;
};

class WPParticleSpawnPipeline {
public:
    explicit WPParticleSpawnPipeline(WPParticleAttributes attributes): m_attributes(attributes) {}

    void Add(WPParticleSpawnInstruction instruction) {
        m_instructions.push(rstd::move(instruction));
    }
    void EnableWorldSpace() noexcept { m_world_space = true; }
    void Compile(particle::ParticleViewCompiler&);
    auto Bind(particle::ParticleWriteView) -> WPParticleSpawnColumns;
    void Initialize(WPParticleSpawnColumns&, particle::ParticleSpawnRequest,
                    ref<dyn<rstd::any::Any>>);

private:
    WPParticleAttributes                                             m_attributes;
    rstd::vec::Vec<WPParticleSpawnInstruction>                       m_instructions;
    particle::ParticleWriteIndex<particle::VelocityAttribute>        m_velocity;
    particle::ParticleWriteIndex<particle::RotationAttribute>        m_rotation;
    particle::ParticleWriteIndex<particle::AngularVelocityAttribute> m_angular_velocity;
    particle::ParticleWriteIndex<particle::ColorAttribute>           m_color;
    particle::ParticleWriteIndex<particle::AlphaAttribute>           m_alpha;
    particle::ParticleWriteIndex<particle::SizeAttribute>            m_size;
    particle::ParticleWriteIndex<particle::LifetimeAttribute>        m_lifetime;
    particle::ParticleWriteIndex<particle::RandomAttribute>          m_random;
    particle::ParticleWriteIndex<particle::InitialColorAttribute>    m_initial_color;
    particle::ParticleWriteIndex<particle::InitialAlphaAttribute>    m_initial_alpha;
    particle::ParticleWriteIndex<particle::InitialSizeAttribute>     m_initial_size;
    particle::ParticleWriteIndex<particle::InitialLifetimeAttribute> m_initial_lifetime;
    bool                                                             m_world_space { false };
    bool                                                             m_compiled { false };
};

struct WPParticleBoxEmitterArgs {
    rstd::array<float, 3>   directions;
    rstd::array<float, 3>   min_distance;
    rstd::array<float, 3>   max_distance;
    float                   emit_speed {};
    rstd::array<float, 3>   origin;
    bool                    one_per_frame { false };
    u32                     instantaneous {};
    float                   min_speed {};
    float                   max_speed {};
    float                   duration {};
    i32                     controlpoint {};
    WPParticleAudioResponse audio_response;
};

struct WPParticleSphereEmitterArgs {
    rstd::array<float, 3>   directions;
    float                   min_distance {};
    float                   max_distance {};
    float                   emit_speed {};
    rstd::array<float, 3>   origin;
    rstd::array<i32, 3>     sign;
    bool                    one_per_frame { false };
    u32                     instantaneous {};
    float                   min_speed {};
    float                   max_speed {};
    float                   duration {};
    i32                     controlpoint {};
    WPParticleAudioResponse audio_response;
};

class WPBoxEmitterProgram {
public:
    WPBoxEmitterProgram(WPParticleSpawnPipeline& pipeline, WPParticleBoxEmitterArgs args,
                        usize index)
        : m_pipeline(rstd::addressof(pipeline)), m_args(rstd::move(args)), m_index(index) {}

    void Compile(particle::ParticleViewCompiler&);
    void Emit(particle::ParticleEmitterContext&);

private:
    WPParticleSpawnPipeline* m_pipeline;
    WPParticleBoxEmitterArgs m_args;
    usize                    m_index {};
};

class WPSphereEmitterProgram {
public:
    WPSphereEmitterProgram(WPParticleSpawnPipeline& pipeline, WPParticleSphereEmitterArgs args,
                           usize index)
        : m_pipeline(rstd::addressof(pipeline)), m_args(rstd::move(args)), m_index(index) {}

    void Compile(particle::ParticleViewCompiler&);
    void Emit(particle::ParticleEmitterContext&);

private:
    WPParticleSpawnPipeline*    m_pipeline;
    WPParticleSphereEmitterArgs m_args;
    usize                       m_index {};
};

struct WPParticleAnimationSpec {
    WPParticleAnimationMode mode { WPParticleAnimationMode::SEQUENCE };
    float                   sequence_multiplier { 1.0f };
};

class WPParticleSubSystem;

struct WPParticleInstanceState {
    struct EmitterState {
        f64 timer {};
        f64 elapsed {};
    };

    struct BoundedData {
        particle::ParticleInstance* parent { nullptr };
        const WPParticleSubSystem*  parent_subsystem { nullptr };
        usize                       parent_instance_index {};
        isize                       particle_index { isize(-1) };
        bool                        previous_lifetime_ok { true };
        Eigen::Vector3f             position { 0.0f, 0.0f, 0.0f };
    } bounded;

    bool                         death { false };
    bool                         no_live_particle { false };
    bool                         warmup_pending { true };
    rstd::vec::Vec<EmitterState> emitters;

    auto Emitter(usize index) -> EmitterState& {
        while (emitters.len() <= index) emitters.emplace_back();
        return emitters[index];
    }

    void Reset() {
        bounded          = {};
        death            = false;
        no_live_particle = false;
        warmup_pending   = true;
        emitters.clear();
    }
};

struct WPParticleInstanceRef {
    particle::ParticleInstance* instance { nullptr };
    WPParticleInstanceState*    state { nullptr };
    usize                       index {};
};

class WPParticleSubSystem : NoCopy, NoMove {
public:
    enum class SpawnType
    {
        STATIC,
        STATIC_CONTROLPOINT,
        EVENT_FOLLOW,
        EVENT_SPAWN,
        EVENT_DEATH,
    };

    static auto EffectiveInstanceCapacity(u32 max_instance_count, SpawnType spawn_type) noexcept
        -> u32 {
        // Static subsystems own one persistent instance; the limit only bounds event pools.
        return spawn_type == SpawnType::STATIC ? u32(1) : max_instance_count;
    }

    static auto MaxParticleCapacity(u32 max_count, u32 max_instance_count,
                                    SpawnType spawn_type) noexcept -> Option<u32> {
        return max_count.checked_mul(EffectiveInstanceCapacity(max_instance_count, spawn_type));
    }

    WPParticleSubSystem(Scene&, std::shared_ptr<SceneMesh>, u32 max_count, f64 rate,
                        u32 max_instance_count, f64 probability, SpawnType, WPParticleAnimationSpec,
                        WPParticleFollowAnchor = {}, u32 trail_length = {}, f64 trail_duration = {},
                        f64 start_time = {}, bool world_space = false,
                        Option<Arc<WPParticleTrailUniformState>> trail_uniform_state = None());
    ~WPParticleSubSystem();

    void Finalize();
    void Tick(f64 frame_time, bool update_mesh = true);
    auto QueryNewInstance() -> Option<WPParticleInstanceRef>;

    void AddEmitter(Box<dyn<particle::ParticleEmitterProgram>>);
    void AddInitializer(WPParticleSpawnInstruction);
    void AddOperator(Box<dyn<particle::ParticleUpdateProgram>>);
    void AddChild(Box<WPParticleSubSystem>);

    auto SchemaBuilder() noexcept -> particle::ParticleSchemaBuilder& { return m_schema_builder; }
    auto Attributes() const noexcept -> const WPParticleAttributes& { return m_attributes; }
    auto SpawnPipeline() noexcept -> WPParticleSpawnPipeline& { return m_spawn_pipeline; }
    auto Controlpoints() const noexcept -> slice<WPParticleControlpoint> {
        return m_controlpoints.as_slice();
    }
    auto ControlpointsMut() noexcept -> mut_ref<WPParticleControlpoint[]> {
        return m_controlpoints.as_mut_slice();
    }
    void SetInstanceOverride(Arc<wpscene::ParticleInstanceoverride> value) {
        m_instance_override = Some(rstd::move(value));
    }
    void SetControlpointAngleCurve(usize index, SceneAnimationCurve curve) {
        m_controlpoints[index].angle_curve = Some(rstd::move(curve));
    }
    void SetOwnerNode(SceneNode* node) noexcept { m_owner_node = node; }
    void SetParentControlpointStartIndex(i32 value) {
        m_parent_controlpoint_start_index = Some(value);
    }

    auto Type() const noexcept -> SpawnType { return m_spawn_type; }
    auto MaxInstanceCount() const noexcept -> u32 { return m_max_instance_count; }
    auto MaxParticleCapacity() const noexcept -> Option<u32> {
        return m_max_count.checked_mul(m_max_instance_count);
    }
    void CompileRuntimeView(particle::ParticleViewCompiler&);
    auto FollowPosition(particle::ParticleInstance&, usize parent_instance_index,
                        particle::ParticleSlot) const -> Eigen::Vector3f;
    bool LifetimeAlive(particle::ParticleInstance&, particle::ParticleSlot) const;
    auto InstanceState(usize index) const -> const WPParticleInstanceState& {
        return m_instance_states[index];
    }
    auto InstanceStateMut(usize index) -> WPParticleInstanceState& {
        return m_instance_states[index];
    }
    auto TrailKey() const noexcept
        -> Option<particle::ParticleAttributeKey<WPTrailHistoryAttribute>> {
        return m_trail_key;
    }
    auto RopeSequenceCount() const noexcept -> Option<u32> { return m_rope_sequence_count; }
    void SetRopeSequenceCount(u32 value) noexcept { m_rope_sequence_count = Some(value); }
    auto AnimationSpec() const noexcept -> WPParticleAnimationSpec { return m_animation_spec; }
    auto RenderPosition(usize instance_index, const Eigen::Vector3f& position) const
        -> Eigen::Vector3f;
    auto Mesh() noexcept -> SceneMesh& { return *m_mesh; }
    auto System() noexcept -> particle::ParticleSystem&;

    void ProcessChildEvents(particle::ParticleEventContext&);

private:
    void Warmup(WPParticleInstanceRef, ref<dyn<rstd::any::Any>>);
    void Advance(f64 frame_time, f64 child_frame_time, bool update_mesh);
    void UpdateFrameInput(f64 frame_time);
    void UpdateControlpoints(WPParticleInstanceRef);
    void UpdateBoundedState(WPParticleInstanceRef);
    auto HasBoundInstance(particle::ParticleInstance*, usize, particle::ParticleSlot) const -> bool;
    void ReleaseBoundInstances(particle::ParticleInstance*, usize, particle::ParticleSlot);
    void SpawnChild(WPParticleInstanceRef, WPParticleSubSystem&, particle::ParticleSlot,
                    Eigen::Vector3f position = Eigen::Vector3f::Zero(), bool fixed = false);

    Scene&                                                          m_scene;
    std::shared_ptr<SceneMesh>                                      m_mesh;
    SceneNode*                                                      m_owner_node { nullptr };
    particle::ParticleSchemaBuilder                                 m_schema_builder;
    WPParticleAttributes                                            m_attributes;
    WPParticleSpawnPipeline                                         m_spawn_pipeline;
    Option<particle::ParticleAttributeKey<WPTrailHistoryAttribute>> m_trail_key;
    Option<u32>                                                     m_rope_sequence_count;
    particle::ParticleProgram                                       m_program;
    Option<Box<particle::ParticleSystem>>                           m_system;
    rstd::vec::Vec<WPParticleInstanceState>                         m_instance_states;
    rstd::vec::Vec<Box<WPParticleSubSystem>>                        m_children;
    rstd::array<WPParticleControlpoint, 8>                          m_controlpoints;
    Option<i32>                                              m_parent_controlpoint_start_index;
    Option<Arc<wpscene::ParticleInstanceoverride>>           m_instance_override;
    WPParticleFrame                                          m_frame;
    WPParticleAnimationSpec                                  m_animation_spec;
    WPParticleFollowAnchor                                   m_follow_anchor;
    u32                                                      m_max_count;
    f64                                                      m_rate;
    f64                                                      m_time {};
    f64                                                      m_start_time {};
    bool                                                     m_world_space { false };
    u32                                                      m_max_instance_count { 1 };
    f64                                                      m_probability { 1.0 };
    SpawnType                                                m_spawn_type { SpawnType::STATIC };
    u32                                                      m_trail_length {};
    f64                                                      m_trail_sample_interval {};
    f64                                                      m_trail_sample_accumulator {};
    Option<Arc<WPParticleTrailUniformState>>                 m_trail_uniform_state;
    rstd::vec::Vec<particle::ParticleSlot>                   m_pending_child_deaths;
    particle::ParticleReadIndex<particle::VelocityAttribute> m_follow_velocity;
    particle::ParticleReadIndex<particle::SizeAttribute>     m_follow_size;
    particle::ParticleReadIndex<particle::LifetimeAttribute> m_follow_lifetime;
};

class WPParticleRuntime {
public:
    WPParticleRuntime()                                        = default;
    WPParticleRuntime(const WPParticleRuntime&)                = delete;
    WPParticleRuntime& operator=(const WPParticleRuntime&)     = delete;
    WPParticleRuntime(WPParticleRuntime&&) noexcept            = default;
    WPParticleRuntime& operator=(WPParticleRuntime&&) noexcept = default;

    void Add(Box<WPParticleSubSystem> subsystem) { m_subsystems.push(rstd::move(subsystem)); }
    void Update(ref<SceneFrame> frame);

private:
    void Tick(f64 delta);

    rstd::vec::Vec<Box<WPParticleSubSystem>> m_subsystems;
};

class WPParticleRawGenerator {
public:
    WPParticleRawGenerator(WPParticleSubSystem& subsystem): m_subsystem(&subsystem) {}

    void Compile(particle::ParticleViewCompiler&);
    void Extract(particle::ParticleExtractContext&);

private:
    WPParticleSubSystem*                                            m_subsystem;
    particle::ParticleReadIndex<particle::VelocityAttribute>        m_velocity;
    particle::ParticleReadIndex<particle::RotationAttribute>        m_rotation;
    particle::ParticleReadIndex<particle::ColorAttribute>           m_color;
    particle::ParticleReadIndex<particle::AlphaAttribute>           m_alpha;
    particle::ParticleReadIndex<particle::SizeAttribute>            m_size;
    particle::ParticleReadIndex<particle::LifetimeAttribute>        m_lifetime;
    particle::ParticleReadIndex<particle::RandomAttribute>          m_random;
    particle::ParticleReadIndex<particle::InitialLifetimeAttribute> m_initial_lifetime;
    particle::ParticleReadObjectIndex<WPTrailHistoryAttribute>      m_trail;
};

} // namespace owe
