export module wescene.particle.program;

import rstd;
import wescene.particle;

using namespace rstd::prelude;

export namespace owe::particle
{

class ParticleEmitterContext;
struct ParticleSpawnContext;
struct ParticleLifecycleContext;
struct ParticleEventContext;
struct ParticleUpdateContext;
struct ParticleExtractContext;

struct ParticleEmitterProgram {
    using Trait                  = ParticleEmitterProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleEmitterProgram;

        void Emit(ParticleEmitterContext& context) { rstd::trait_call<0>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Emit>;
};

struct ParticleSpawnProgram {
    using Trait                  = ParticleSpawnProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleSpawnProgram;

        void Initialize(ParticleSpawnContext& context) { rstd::trait_call<0>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Initialize>;
};

struct ParticleLifecycleProgram {
    using Trait                  = ParticleLifecycleProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleLifecycleProgram;

        void Update(ParticleLifecycleContext& context) { rstd::trait_call<0>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Update>;
};

struct ParticleEventProgram {
    using Trait                  = ParticleEventProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleEventProgram;

        void Process(ParticleEventContext& context) { rstd::trait_call<0>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Process>;
};

struct ParticleUpdateProgram {
    using Trait                  = ParticleUpdateProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleUpdateProgram;

        void Update(ParticleUpdateContext& context) { rstd::trait_call<0>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Update>;
};

struct ParticleExtractProgram {
    using Trait                  = ParticleExtractProgram;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleExtractProgram;

        void Extract(ParticleExtractContext& context) { rstd::trait_call<0>(this, context); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Extract>;
};

struct ParticleSlotTransition {
    ParticleSlot slot;
    bool         spawned { false };
    bool         died { false };
};

struct ParticleSlotEvents {
    rstd::vec::Vec<ParticleSlot>           spawned;
    rstd::vec::Vec<ParticleSlot>           died;
    rstd::vec::Vec<ParticleSlotTransition> transitions;

    void RecordSpawn(ParticleSlot slot) {
        EnsureTransition(slot).spawned = true;
        spawned.emplace_back(slot);
    }

    void RecordDeath(ParticleSlot slot) {
        EnsureTransition(slot).died = true;
        died.emplace_back(slot);
    }

    void Clear() {
        spawned.clear();
        died.clear();
        transitions.clear();
    }

private:
    auto EnsureTransition(ParticleSlot slot) -> ParticleSlotTransition& {
        while (transitions.len() <= slot.index) {
            transitions.emplace_back(ParticleSlotTransition {
                .slot = ParticleSlot { .index = transitions.len() },
            });
        }
        return transitions[slot.index];
    }
};

enum class ParticleEventPhase
{
    BeforeEmit,
    AfterEmit,
};

struct ParticleSpawnContext {
    ParticleStorage&         storage;
    ParticleSlot             slot;
    ref<dyn<rstd::any::Any>> frame;
    f64                      emitter_duration {};
};

struct ParticleLifecycleContext {
    ParticleStorage&         storage;
    ParticleColumnCache&     columns;
    ParticleSlotEvents&      events;
    ref<dyn<rstd::any::Any>> frame;
    f64                      delta {};
    f64                      elapsed {};

    void Kill(ParticleSlot slot) {
        auto states = storage.ValuesMut(storage.SlotStateKey());
        if (slot.index >= states.len()) rstd::panic { "particle slot out of bounds" };
        if (! states[slot.index].active) return;
        states[slot.index].active = false;
        states[slot.index].fresh  = false;
        events.RecordDeath(slot);
    }
};

struct ParticleEventContext {
    ParticleStorage&         storage;
    ref<ParticleSlotEvents>  events;
    ref<dyn<rstd::any::Any>> frame;
    ParticleEventPhase       phase;
    f64                      delta {};
    f64                      elapsed {};
};

struct ParticleUpdateContext {
    ParticleStorage&         storage;
    ParticleColumnCache&     columns;
    ref<dyn<rstd::any::Any>> frame;
    f64                      delta {};
    f64                      elapsed {};
};

class ParticleProgram {
public:
    template<typename Attribute>
    void Require(ParticleAttributeKey<Attribute> key) {
        auto requirement = RequireParticleAttribute(key);
        for (const auto& existing : m_requirements) {
            if (existing.id == requirement.id && existing.schema_slot == requirement.schema_slot &&
                existing.concrete_type == requirement.concrete_type) {
                return;
            }
        }
        m_requirements.push(rstd::move(requirement));
    }

    auto Requirements() const noexcept -> slice<ParticleAttributeRequirement> {
        return m_requirements.as_slice();
    }

    void AddEmitter(Box<dyn<ParticleEmitterProgram>> program) {
        m_emitters.push(rstd::move(program));
    }
    void AddSpawn(Box<dyn<ParticleSpawnProgram>> program) { m_spawn.push(rstd::move(program)); }
    void AddLifecycle(Box<dyn<ParticleLifecycleProgram>> program) {
        m_lifecycle.push(rstd::move(program));
    }
    void AddEvent(Box<dyn<ParticleEventProgram>> program) { m_events.push(rstd::move(program)); }
    void AddUpdate(Box<dyn<ParticleUpdateProgram>> program) { m_updates.push(rstd::move(program)); }
    void AddPostUpdate(Box<dyn<ParticleUpdateProgram>> program) {
        m_post_updates.push(rstd::move(program));
    }
    void AddExtractor(Box<dyn<ParticleExtractProgram>> program) {
        m_extractors.push(rstd::move(program));
    }

private:
    friend class ParticleEmitterContext;
    friend class ParticleSystem;

    rstd::vec::Vec<ParticleAttributeRequirement>       m_requirements;
    rstd::vec::Vec<Box<dyn<ParticleEmitterProgram>>>   m_emitters;
    rstd::vec::Vec<Box<dyn<ParticleSpawnProgram>>>     m_spawn;
    rstd::vec::Vec<Box<dyn<ParticleLifecycleProgram>>> m_lifecycle;
    rstd::vec::Vec<Box<dyn<ParticleEventProgram>>>     m_events;
    rstd::vec::Vec<Box<dyn<ParticleUpdateProgram>>>    m_updates;
    rstd::vec::Vec<Box<dyn<ParticleUpdateProgram>>>    m_post_updates;
    rstd::vec::Vec<Box<dyn<ParticleExtractProgram>>>   m_extractors;
};

struct ParticleDefinition {
    ParticleSchema  schema;
    ParticleProgram program;
    usize           max_slots {};

    static auto Prepare(ParticleSchema schema, ParticleProgram program, usize max_slots)
        -> Result<ParticleDefinition, ParticleSchemaError> {
        auto requirements = program.Requirements();
        for (usize index {}; index < requirements.len(); ++index) {
            auto result = schema.Validate(requirements[index]);
            if (result.is_err()) return Err(rstd::move(result).unwrap_err());
        }
        return Ok(ParticleDefinition {
            .schema    = rstd::move(schema),
            .program   = rstd::move(program),
            .max_slots = max_slots,
        });
    }
};

class ParticleInstance {
public:
    explicit ParticleInstance(const ParticleSchema& schema): m_storage(schema.CreateStorage()) {}

    ParticleStorage&       Storage() noexcept { return m_storage; }
    const ParticleStorage& Storage() const noexcept { return m_storage; }
    ParticleColumnCache&   Columns() noexcept { return m_columns; }
    ParticleSlotEvents&    Events() noexcept { return m_events; }
    bool                   Active() const noexcept { return m_active; }
    void                   SetActive(bool active) noexcept { m_active = active; }

    void Reset() {
        m_storage.Clear();
        m_events.Clear();
        m_active = true;
    }

private:
    ParticleStorage     m_storage;
    ParticleColumnCache m_columns;
    ParticleSlotEvents  m_events;
    bool                m_active { true };
};

struct ParticleExtractContext {
    slice<Box<ParticleInstance>> instances;
    ref<dyn<rstd::any::Any>>     frame;
};

class ParticleEmitterContext {
public:
    ParticleStorage& Storage() noexcept { return *m_storage; }
    auto             Frame() const noexcept -> ref<dyn<rstd::any::Any>> { return m_frame; }
    f64              Delta() const noexcept { return m_delta; }
    f64              Elapsed() const noexcept { return m_elapsed; }

    auto Acquire() -> Option<ParticleSlot> {
        auto slot = m_storage->AcquireSlot(m_max_slots);
        if (slot.is_some()) m_events->RecordSpawn(*slot);
        return slot;
    }

    void Initialize(ParticleSlot slot, f64 emitter_duration) {
        ParticleSpawnContext context {
            .storage          = *m_storage,
            .slot             = slot,
            .frame            = m_frame,
            .emitter_duration = emitter_duration,
        };
        for (auto& program : *m_spawn) program->Initialize(context);
    }

private:
    friend class ParticleSystem;

    ParticleEmitterContext(ParticleStorage& storage, ParticleSlotEvents& events,
                           rstd::vec::Vec<Box<dyn<ParticleSpawnProgram>>>& spawn,
                           ref<dyn<rstd::any::Any>> frame, usize max_slots, f64 delta, f64 elapsed)
        : m_storage(rstd::addressof(storage)),
          m_events(rstd::addressof(events)),
          m_spawn(rstd::addressof(spawn)),
          m_frame(frame),
          m_max_slots(max_slots),
          m_delta(delta),
          m_elapsed(elapsed) {}

    ParticleStorage*                                m_storage;
    ParticleSlotEvents*                             m_events;
    rstd::vec::Vec<Box<dyn<ParticleSpawnProgram>>>* m_spawn;
    ref<dyn<rstd::any::Any>>                        m_frame;
    usize                                           m_max_slots;
    f64                                             m_delta;
    f64                                             m_elapsed;
};

class ParticleSystem {
public:
    explicit ParticleSystem(ParticleDefinition definition): m_definition(rstd::move(definition)) {}

    auto CreateInstance() -> ParticleInstance& {
        m_instances.push(Box<ParticleInstance>::make(m_definition.schema));
        return *m_instances[m_instances.len() - usize(1)];
    }

    auto Instances() const noexcept -> slice<Box<ParticleInstance>> {
        return m_instances.as_slice();
    }

    auto InstancesMut() noexcept -> mut_ref<Box<ParticleInstance>[]> {
        return m_instances.deref_mut();
    }

    auto InstanceCount() const noexcept -> usize { return m_instances.len(); }

    ParticleInstance&       Instance(usize index) { return *m_instances[index]; }
    const ParticleInstance& Instance(usize index) const { return *m_instances[index]; }

    ParticleDefinition&       Definition() noexcept { return m_definition; }
    const ParticleDefinition& Definition() const noexcept { return m_definition; }

    void Advance(ParticleInstance& instance, ref<dyn<rstd::any::Any>> frame, f64 delta,
                 f64 elapsed) {
        if (! instance.Active()) return;

        auto& storage = instance.Storage();
        auto& columns = instance.Columns();
        auto& events  = instance.Events();
        events.Clear();

        ParticleLifecycleContext lifecycle_context {
            .storage = storage,
            .columns = columns,
            .events  = events,
            .frame   = frame,
            .delta   = delta,
            .elapsed = elapsed,
        };
        for (auto& lifecycle : m_definition.program.m_lifecycle) {
            lifecycle->Update(lifecycle_context);
        }
        bool had_lifecycle_events = ! events.spawned.is_empty() || ! events.died.is_empty();
        ProcessEvents(
            storage, events, frame, ParticleEventPhase::BeforeEmit, delta, elapsed, false);
        events.Clear();

        ParticleEmitterContext emitter_context(storage,
                                               events,
                                               m_definition.program.m_spawn,
                                               frame,
                                               m_definition.max_slots,
                                               delta,
                                               elapsed);
        for (auto& emitter : m_definition.program.m_emitters) emitter->Emit(emitter_context);
        ProcessEvents(storage,
                      events,
                      frame,
                      ParticleEventPhase::AfterEmit,
                      delta,
                      elapsed,
                      had_lifecycle_events);

        auto states = storage.ValuesMut(storage.SlotStateKey());
        for (usize index {}; index < states.len(); ++index) states[index].fresh = false;

        ParticleUpdateContext update_context {
            .storage = storage,
            .columns = columns,
            .frame   = frame,
            .delta   = delta,
            .elapsed = elapsed,
        };
        for (auto& update : m_definition.program.m_updates) update->Update(update_context);
        for (auto& update : m_definition.program.m_post_updates) update->Update(update_context);
    }

    void Extract(ref<dyn<rstd::any::Any>> frame) {
        ParticleExtractContext context {
            .instances = m_instances.as_slice(),
            .frame     = frame,
        };
        for (auto& extractor : m_definition.program.m_extractors) extractor->Extract(context);
    }

private:
    void ProcessEvents(ParticleStorage& storage, ParticleSlotEvents& events,
                       ref<dyn<rstd::any::Any>> frame, ParticleEventPhase phase, f64 delta,
                       f64 elapsed, bool force) {
        if (! force && events.spawned.is_empty() && events.died.is_empty()) return;
        ParticleEventContext context {
            .storage = storage,
            .events  = ref<ParticleSlotEvents>::from_raw_parts(rstd::addressof(events)),
            .frame   = frame,
            .phase   = phase,
            .delta   = delta,
            .elapsed = elapsed,
        };
        for (auto& event : m_definition.program.m_events) event->Process(context);
    }

    ParticleDefinition                    m_definition;
    rstd::vec::Vec<Box<ParticleInstance>> m_instances;
};

} // namespace owe::particle
