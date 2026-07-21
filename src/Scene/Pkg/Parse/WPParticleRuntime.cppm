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
    void        Require(particle::ParticleProgram&) const;
};

struct WPParticleInitialRef {
    Eigen::Vector3f& color;
    float&           alpha;
    float&           size;
    float&           lifetime;
};

struct WPParticleRef {
    particle::ParticleSlot       slot;
    particle::ParticleSlotState& state;
    Eigen::Vector3f&             position;
    Eigen::Vector3f&             color;
    float&                       alpha;
    float&                       size;
    float&                       lifetime;
    Eigen::Vector3f&             rotation;
    Eigen::Vector3f&             velocity;
    Eigen::Vector3f&             acceleration;
    Eigen::Vector3f&             angular_velocity;
    Eigen::Vector3f&             angular_acceleration;
    float&                       random;
    WPParticleInitialRef         initial;
};

struct WPParticleInitialConstRef {
    const Eigen::Vector3f& color;
    const float&           alpha;
    const float&           size;
    const float&           lifetime;
};

struct WPParticleConstRef {
    particle::ParticleSlot             slot;
    const particle::ParticleSlotState& state;
    const Eigen::Vector3f&             position;
    const Eigen::Vector3f&             color;
    const float&                       alpha;
    const float&                       size;
    const float&                       lifetime;
    const Eigen::Vector3f&             rotation;
    const Eigen::Vector3f&             velocity;
    const Eigen::Vector3f&             acceleration;
    const Eigen::Vector3f&             angular_velocity;
    const Eigen::Vector3f&             angular_acceleration;
    const float&                       random;
    WPParticleInitialConstRef          initial;
};

auto MakeWPParticleRef(particle::ParticleStorage&, const WPParticleAttributes&,
                       particle::ParticleSlot) -> WPParticleRef;
auto MakeWPParticleConstRef(const particle::ParticleStorage&, const WPParticleAttributes&,
                            particle::ParticleSlot) -> WPParticleConstRef;

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
        void AppendDefault() { storage.AppendDefault(); }                                          \
        void Reset(particle::ParticleSlot) {}                                                      \
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

    auto ValuesMut(particle::ParticleStorage& storage) const -> WPOscillationValues {
        return {
            .reset     = storage.ValuesMut(reset),
            .frequency = storage.ValuesMut(frequency),
            .scale     = storage.ValuesMut(scale),
            .phase     = storage.ValuesMut(phase),
        };
    }
};

struct WPTrailSlotState {
    usize           head {};
    usize           len {};
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
    void AppendDefault();
    void Reset(particle::ParticleSlot slot);
    void Clear();
    auto CloneEmpty() const -> WPTrailHistoryAttribute;

    void Push(particle::ParticleSlot slot, const Eigen::Vector3f& position);
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
    Eigen::Matrix3d            world_from_local_dir { Eigen::Matrix3d::Identity() };
    Eigen::Matrix3d            local_from_world_dir { Eigen::Matrix3d::Identity() };
    f64                        time {};
    f64                        delta {};
    f64                        emitter_delta {};
    bool                       world_space { false };
};

auto WPParticleFrameFrom(ref<dyn<rstd::any::Any>>) -> ref<WPParticleFrame>;

struct WPParticleBatch {
    particle::ParticleStorage&    storage;
    const WPParticleAttributes&   attributes;
    slice<WPParticleControlpoint> controlpoints;
    Eigen::Matrix3d               world_from_local_dir { Eigen::Matrix3d::Identity() };
    Eigen::Matrix3d               local_from_world_dir { Eigen::Matrix3d::Identity() };
    bool                          world_space { false };
    f64                           time {};
    f64                           time_pass {};

    auto Len() const noexcept -> usize { return storage.Len(); }
    auto Particle(usize index) -> WPParticleRef {
        return MakeWPParticleRef(storage, attributes, particle::ParticleSlot { .index = index });
    }
};

class WPParticleInitProgram {
public:
    template<typename F>
    WPParticleInitProgram(WPParticleAttributes attributes, F&& function)
        : m_attributes(attributes),
          m_function(Box<dyn<FnMut<void(WPParticleRef, f64)>>>::make(rstd::forward<F>(function))) {}

    void Initialize(particle::ParticleSpawnContext& context) {
        auto value = MakeWPParticleRef(context.storage, m_attributes, context.slot);
        m_function->operator()(value, context.emitter_duration);
    }

private:
    WPParticleAttributes                      m_attributes;
    Box<dyn<FnMut<void(WPParticleRef, f64)>>> m_function;
};

class WPParticleUpdateProgram {
public:
    template<typename F>
    WPParticleUpdateProgram(WPParticleAttributes attributes, F&& function)
        : m_attributes(attributes),
          m_function(Box<dyn<FnMut<void(WPParticleBatch&)>>>::make(rstd::forward<F>(function))) {}

    void Update(particle::ParticleUpdateContext& context);

private:
    WPParticleAttributes                    m_attributes;
    Box<dyn<FnMut<void(WPParticleBatch&)>>> m_function;
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
    WPBoxEmitterProgram(WPParticleAttributes attributes, WPParticleBoxEmitterArgs args)
        : m_attributes(attributes), m_args(rstd::move(args)) {}

    void Emit(particle::ParticleEmitterContext&);

private:
    WPParticleAttributes     m_attributes;
    WPParticleBoxEmitterArgs m_args;
    f64                      m_timer {};
    f64                      m_elapsed {};
};

class WPSphereEmitterProgram {
public:
    WPSphereEmitterProgram(WPParticleAttributes attributes, WPParticleSphereEmitterArgs args)
        : m_attributes(attributes), m_args(rstd::move(args)) {}

    void Emit(particle::ParticleEmitterContext&);

private:
    WPParticleAttributes        m_attributes;
    WPParticleSphereEmitterArgs m_args;
    f64                         m_timer {};
    f64                         m_elapsed {};
};

struct WPParticleAnimationSpec {
    WPParticleAnimationMode mode { WPParticleAnimationMode::SEQUENCE };
    float                   sequence_multiplier { 1.0f };
};

class WPParticleSubSystem;

struct WPParticleInstanceState {
    struct BoundedData {
        particle::ParticleInstance* parent { nullptr };
        const WPParticleSubSystem*  parent_subsystem { nullptr };
        usize                       parent_instance_index {};
        isize                       particle_index { isize(-1) };
        bool                        previous_lifetime_ok { true };
        Eigen::Vector3f             position { 0.0f, 0.0f, 0.0f };
    } bounded;

    bool death { false };
    bool no_live_particle { false };
    f64  trail_sample_accumulator {};

    void Reset() {
        bounded                  = {};
        death                    = false;
        no_live_particle         = false;
        trail_sample_accumulator = f64();
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
        EVENT_FOLLOW,
        EVENT_SPAWN,
        EVENT_DEATH,
    };

    WPParticleSubSystem(Scene&, std::shared_ptr<SceneMesh>, u32 max_count, f64 rate,
                        u32 max_instance_count, f64 probability, SpawnType, WPParticleAnimationSpec,
                        WPParticleFollowAnchor = {}, u32 trail_length = {}, f64 trail_duration = {},
                        f64 start_time = {}, bool world_space = false);
    ~WPParticleSubSystem();

    void Finalize();
    void Tick(f64 frame_time, bool update_mesh = true);
    auto QueryNewInstance() -> Option<WPParticleInstanceRef>;

    void AddEmitter(Box<dyn<particle::ParticleEmitterProgram>>);
    void AddInitializer(Box<dyn<particle::ParticleSpawnProgram>>);
    void AddOperator(Box<dyn<particle::ParticleUpdateProgram>>);
    void AddChild(Box<WPParticleSubSystem>);

    template<typename Attribute>
    void RequireAttribute(particle::ParticleAttributeKey<Attribute> key) {
        m_program.Require(key);
    }

    auto SchemaBuilder() noexcept -> particle::ParticleSchemaBuilder& { return m_schema_builder; }
    auto Attributes() const noexcept -> const WPParticleAttributes& { return m_attributes; }
    auto Controlpoints() const noexcept -> slice<WPParticleControlpoint> {
        return m_controlpoints.as_slice();
    }
    auto ControlpointsMut() noexcept -> mut_ref<WPParticleControlpoint[]> {
        return m_controlpoints.as_mut_slice();
    }
    void SetInstanceOverride(std::shared_ptr<const wpscene::ParticleInstanceoverride> value) {
        m_instance_override = rstd::move(value);
    }
    void SetControlpointAngleCurve(usize index, SceneAnimationCurve curve) {
        m_controlpoints[index].angle_curve = Some(rstd::move(curve));
    }
    void SetOwnerNode(SceneNode* node) noexcept { m_owner_node = node; }

    auto Type() const noexcept -> SpawnType { return m_spawn_type; }
    auto MaxInstanceCount() const noexcept -> u32 { return m_max_instance_count; }
    auto FollowPosition(const particle::ParticleStorage&, particle::ParticleSlot) const
        -> Eigen::Vector3f;
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
    auto AnimationSpec() const noexcept -> WPParticleAnimationSpec { return m_animation_spec; }
    auto Mesh() noexcept -> SceneMesh& { return *m_mesh; }
    auto System() noexcept -> particle::ParticleSystem&;

    void ProcessChildEvents(particle::ParticleEventContext&);

private:
    void Warmup();
    void Advance(f64 frame_time, bool update_mesh);
    void UpdateFrameInput(f64 frame_time);
    void UpdateBoundedState(WPParticleInstanceRef);
    void SpawnChild(WPParticleInstanceRef, WPParticleSubSystem&, particle::ParticleSlot,
                    Eigen::Vector3f position = Eigen::Vector3f::Zero(), bool fixed = false);

    Scene&                                                          m_scene;
    std::shared_ptr<SceneMesh>                                      m_mesh;
    SceneNode*                                                      m_owner_node { nullptr };
    particle::ParticleSchemaBuilder                                 m_schema_builder;
    WPParticleAttributes                                            m_attributes;
    Option<particle::ParticleAttributeKey<WPTrailHistoryAttribute>> m_trail_key;
    particle::ParticleProgram                                       m_program;
    Option<Box<particle::ParticleSystem>>                           m_system;
    rstd::vec::Vec<WPParticleInstanceState>                         m_instance_states;
    rstd::vec::Vec<Box<WPParticleSubSystem>>                        m_children;
    rstd::array<WPParticleControlpoint, 8>                          m_controlpoints;
    std::shared_ptr<const wpscene::ParticleInstanceoverride>        m_instance_override;
    WPParticleFrame                                                 m_frame;
    WPParticleAnimationSpec                                         m_animation_spec;
    WPParticleFollowAnchor                                          m_follow_anchor;
    u32                                                             m_max_count;
    f64                                                             m_rate;
    f64                                                             m_time {};
    f64                                                             m_start_time {};
    bool                                                            m_world_space { false };
    bool                                                            m_started { false };
    u32                                                             m_max_instance_count { 1 };
    f64                                                             m_probability { 1.0 };
    SpawnType m_spawn_type { SpawnType::STATIC };
    u32       m_trail_length {};
    f64       m_trail_sample_interval {};
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

    void Extract(particle::ParticleExtractContext&);

private:
    WPParticleSubSystem* m_subsystem;
};

} // namespace owe
