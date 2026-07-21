#include <gtest/gtest.h>

import rstd;
import rstd.cppstd;
import eigen;
import wescene.particle;
import wescene.particle.program;

using namespace rstd::prelude;

namespace
{

namespace particle = owe::particle;

template<typename Attribute, typename... Args>
auto Register(particle::ParticleSchemaBuilder& builder, const char* name, const char* owner,
              Args&&... args) -> particle::ParticleAttributeKey<Attribute> {
    auto result =
        builder.Register<Attribute>(ref<str>(name), ref<str>(owner), rstd::forward<Args>(args)...);
    if (result.is_err()) rstd::panic { "particle test attribute registration failed" };
    return result.unwrap();
}

struct TemperatureAttribute {
    using Value = float;

    TemperatureAttribute(particle::ParticleAttributeDescriptor descriptor, Value default_value)
        : storage(rstd::move(descriptor), default_value) {}

    auto Descriptor() const -> ref<particle::ParticleAttributeDescriptor> {
        return storage.Descriptor();
    }
    auto ConcreteType() const noexcept -> rstd::any::TypeId { return storage.ConcreteType(); }
    auto ValueType() const noexcept -> rstd::any::TypeId { return storage.ValueTypeId(); }
    auto Len() const noexcept -> usize { return storage.Len(); }
    auto Capacity() const noexcept -> usize { return storage.Capacity(); }
    void Reserve(usize total_slots) { storage.Reserve(total_slots); }
    void AppendDefault() { storage.AppendDefault(); }
    void Reset(particle::ParticleSlot slot) { storage.Reset(slot); }
    void Clear() { storage.Clear(); }
    auto Values() const noexcept -> slice<Value> { return storage.Values(); }
    auto ValuesMut() noexcept -> mut_ref<Value[]> { return storage.ValuesMut(); }
    auto CloneEmpty() const -> TemperatureAttribute {
        return TemperatureAttribute(storage.CloneDescriptor(), storage.DefaultValue());
    }

    particle::ParticleValueAttributeStorage<Value> storage;
};

struct TraceEmitter {
    rstd::vec::Vec<i32>* trace;

    void Emit(particle::ParticleEmitterContext& context) {
        trace->push(i32(1));
        auto slot = context.Acquire();
        if (slot.is_none()) rstd::panic { "particle test could not acquire slot" };
        context.Initialize(*slot, f64());
    }
};

struct TraceSpawn {
    rstd::vec::Vec<i32>*                                 trace;
    particle::ParticleAttributeKey<TemperatureAttribute> temperature;

    void Initialize(particle::ParticleSpawnContext& context) {
        trace->push(i32(2));
        particle::ParticleSlotWriter slot(context.storage, context.slot);
        slot.Write(temperature) = 42.0f;
    }
};

struct TraceLifecycle {
    rstd::vec::Vec<i32>* trace;

    void Update(particle::ParticleLifecycleContext&) { trace->push(i32(3)); }
};

struct TraceEvent {
    rstd::vec::Vec<i32>* trace;
    usize*               spawned;

    void Process(particle::ParticleEventContext& context) {
        trace->push(i32(4));
        *spawned = context.events->spawned.len();
    }
};

struct TraceUpdate {
    rstd::vec::Vec<i32>* trace;
    i32                  value;

    void Update(particle::ParticleUpdateContext&) { trace->emplace_back(value); }
};

struct TraceExtract {
    rstd::vec::Vec<i32>* trace;

    void Extract(particle::ParticleExtractContext&) { trace->push(i32(7)); }
};

struct EmptyFrame {};

} // namespace

TEST(ParticleStorage, OwnsIndependentAttributesAndReusesStableSlots) {
    particle::ParticleSchemaBuilder builder;
    auto                            position =
        Register<particle::PositionAttribute>(builder, "position", "test", Eigen::Vector3f::Zero());
    auto temperature = Register<TemperatureAttribute>(builder, "temperature", "test", 18.0f);
    auto schema      = rstd::move(builder).Build();
    auto storage     = schema.CreateStorage();

    auto first  = storage.AcquireSlot(usize(4));
    auto second = storage.AcquireSlot(usize(4));
    ASSERT_TRUE(first.is_some());
    ASSERT_TRUE(second.is_some());
    EXPECT_EQ(first->index, usize());
    EXPECT_EQ(second->index, usize(1));
    EXPECT_EQ(storage.Len(), usize(2));
    EXPECT_EQ(storage.AttributeRef(position)->Len(), storage.Len());
    EXPECT_EQ(storage.AttributeRef(temperature)->Len(), storage.Len());

    particle::ParticleSlotWriter first_value(storage, *first);
    first_value.Write(position)    = Eigen::Vector3f { 1.0f, 2.0f, 3.0f };
    first_value.Write(temperature) = 90.0f;
    storage.ReleaseSlot(*first);

    auto reused = storage.AcquireSlot(usize(4));
    ASSERT_TRUE(reused.is_some());
    EXPECT_EQ(*reused, *first);
    particle::ParticleSlotReader reused_value(storage, *reused);
    EXPECT_EQ(reused_value.Read(position), Eigen::Vector3f::Zero());
    EXPECT_FLOAT_EQ(reused_value.Read(temperature), 18.0f);
    EXPECT_EQ(rstd::cppstd::as_string_view(
                  storage.AttributeRef(temperature)->Descriptor()->owner.as_str()),
              "test");
}

TEST(ParticleSchema, RejectsDuplicateNamesAndMismatchedTypedKeys) {
    particle::ParticleSchemaBuilder duplicate_builder;
    auto                            position = Register<particle::PositionAttribute>(
        duplicate_builder, "value", "test", Eigen::Vector3f::Zero());
    auto duplicate = duplicate_builder.Register<particle::ColorAttribute>(
        ref<str>("value"), ref<str>("test"), Eigen::Vector3f::Ones());
    EXPECT_TRUE(duplicate.is_err());

    auto schema  = rstd::move(duplicate_builder).Build();
    auto storage = schema.CreateStorage();
    (void)storage.AppendSlot();
    particle::ParticleAttributeKey<particle::ColorAttribute> wrong {
        .id          = position.id,
        .schema_slot = position.schema_slot,
    };
    EXPECT_DEATH((void)storage.Values(wrong), "particle attribute key does not match storage");
}

TEST(ParticleSchema, RejectsMissingProgramRequirementsDuringPrepare) {
    particle::ParticleSchemaBuilder builder;
    auto                            position =
        Register<particle::PositionAttribute>(builder, "position", "test", Eigen::Vector3f::Zero());
    particle::ParticleProgram program;
    program.Require(particle::ParticleAttributeKey<particle::ColorAttribute> {
        .id          = position.id,
        .schema_slot = position.schema_slot,
    });

    auto definition = particle::ParticleDefinition::Prepare(
        rstd::move(builder).Build(), rstd::move(program), usize(4));
    EXPECT_TRUE(definition.is_err());
}

TEST(ParticleQuery, RejectsUseAfterStructuralMutation) {
    particle::ParticleSchemaBuilder builder;
    auto temperature = Register<TemperatureAttribute>(builder, "temperature", "test", 1.0f);
    auto schema      = rstd::move(builder).Build();
    auto storage     = schema.CreateStorage();
    (void)storage.AppendSlot();
    particle::ParticleReadQuery query(storage);
    EXPECT_EQ(query.Read(temperature).len(), usize(1));

    (void)storage.AppendSlot();
    EXPECT_DEATH((void)query.Read(temperature), "particle query used after structural mutation");
}

TEST(ParticleSlotEvents, CombinesTransitionsInStableSlotOrder) {
    particle::ParticleSlotEvents events;
    events.RecordDeath(particle::ParticleSlot { .index = usize(2) });
    events.RecordSpawn(particle::ParticleSlot { .index = usize() });
    events.RecordSpawn(particle::ParticleSlot { .index = usize(2) });

    ASSERT_EQ(events.transitions.len(), usize(3));
    EXPECT_EQ(events.transitions[usize()].slot.index, usize());
    EXPECT_TRUE(events.transitions[usize()].spawned);
    EXPECT_FALSE(events.transitions[usize()].died);
    EXPECT_EQ(events.transitions[usize(1)].slot.index, usize(1));
    EXPECT_FALSE(events.transitions[usize(1)].spawned);
    EXPECT_FALSE(events.transitions[usize(1)].died);
    EXPECT_EQ(events.transitions[usize(2)].slot.index, usize(2));
    EXPECT_TRUE(events.transitions[usize(2)].spawned);
    EXPECT_TRUE(events.transitions[usize(2)].died);
}

TEST(ParticleProgram, RunsPreparedProgramsInContractOrder) {
    particle::ParticleSchemaBuilder builder;
    auto temperature = Register<TemperatureAttribute>(builder, "temperature", "test", 0.0f);
    rstd::vec::Vec<i32> trace;
    usize               spawned {};

    particle::ParticleProgram program;
    program.Require(temperature);
    program.AddEmitter(Box<dyn<particle::ParticleEmitterProgram>>::make(
        TraceEmitter { .trace = rstd::addressof(trace) }));
    program.AddSpawn(Box<dyn<particle::ParticleSpawnProgram>>::make(
        TraceSpawn { .trace = rstd::addressof(trace), .temperature = temperature }));
    program.AddLifecycle(Box<dyn<particle::ParticleLifecycleProgram>>::make(
        TraceLifecycle { .trace = rstd::addressof(trace) }));
    program.AddEvent(Box<dyn<particle::ParticleEventProgram>>::make(
        TraceEvent { .trace = rstd::addressof(trace), .spawned = rstd::addressof(spawned) }));
    program.AddUpdate(Box<dyn<particle::ParticleUpdateProgram>>::make(
        TraceUpdate { .trace = rstd::addressof(trace), .value = i32(5) }));
    program.AddPostUpdate(Box<dyn<particle::ParticleUpdateProgram>>::make(
        TraceUpdate { .trace = rstd::addressof(trace), .value = i32(6) }));
    program.AddExtractor(Box<dyn<particle::ParticleExtractProgram>>::make(
        TraceExtract { .trace = rstd::addressof(trace) }));

    auto definition = particle::ParticleDefinition::Prepare(
        rstd::move(builder).Build(), rstd::move(program), usize(4));
    ASSERT_TRUE(definition.is_ok());
    particle::ParticleSystem system(rstd::move(definition).unwrap());
    auto&                    instance = system.CreateInstance();
    EmptyFrame               frame;
    auto                     frame_ref = rstd::dyn<rstd::any::Any>::from_ref(frame).as_ref();
    system.Advance(instance, frame_ref, f64(1.0 / 60.0), f64(1.0 / 60.0));
    system.Extract(frame_ref);

    ASSERT_EQ(trace.len(), usize(7));
    for (usize index {}; index < trace.len(); ++index) {
        EXPECT_EQ(trace[index], i32(static_cast<rstd::int32_t>(index.to_primitive() + 1)));
    }
    EXPECT_EQ(spawned, usize(1));
    particle::ParticleSlotReader value(instance.Storage(), particle::ParticleSlot {});
    EXPECT_FLOAT_EQ(value.Read(temperature), 42.0f);
    EXPECT_FALSE(value.Read(instance.Storage().SlotStateKey()).fresh);
}
