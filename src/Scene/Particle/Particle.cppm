export module wescene.particle;

import eigen;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe::particle
{

struct ParticleAttributeId {
    u64 value {};

    bool        Valid() const noexcept { return value != u64(); }
    friend bool operator==(ParticleAttributeId, ParticleAttributeId) = default;
};

struct ParticleSlot {
    usize index {};

    friend bool operator==(ParticleSlot, ParticleSlot) = default;
};

enum class ParticleAttributeResetPolicy : rstd::uint8_t
{
    DefaultValue,
    Custom,
};

struct ParticleAttributeDescriptor {
    ParticleAttributeId          id;
    String                       debug_name;
    String                       owner;
    rstd::any::TypeId            concrete_type;
    rstd::any::TypeId            value_type;
    usize                        value_size {};
    usize                        value_alignment {};
    ParticleAttributeResetPolicy reset_policy { ParticleAttributeResetPolicy::DefaultValue };

    auto Clone() const -> ParticleAttributeDescriptor {
        return {
            .id              = id,
            .debug_name      = debug_name.clone(),
            .owner           = owner.clone(),
            .concrete_type   = concrete_type,
            .value_type      = value_type,
            .value_size      = value_size,
            .value_alignment = value_alignment,
            .reset_policy    = reset_policy,
        };
    }
};

struct ParticleAttribute {
    using Trait                  = ParticleAttribute;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleAttribute;

        auto Descriptor() const -> ref<ParticleAttributeDescriptor> {
            return rstd::trait_call<0>(this);
        }
        auto ConcreteType() const noexcept -> rstd::any::TypeId {
            return rstd::trait_call<1>(this);
        }
        auto ValueType() const noexcept -> rstd::any::TypeId { return rstd::trait_call<2>(this); }
        auto Len() const noexcept -> usize { return rstd::trait_call<3>(this); }
        auto Capacity() const noexcept -> usize { return rstd::trait_call<4>(this); }
        void Reserve(usize total_slots) { rstd::trait_call<5>(this, total_slots); }
        void AppendDefault() { rstd::trait_call<6>(this); }
        void Reset(ParticleSlot slot) { rstd::trait_call<7>(this, slot); }
        void Clear() { rstd::trait_call<8>(this); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Descriptor, &T::ConcreteType, &T::ValueType, &T::Len, &T::Capacity,
                             &T::Reserve, &T::AppendDefault, &T::Reset, &T::Clear>;
};

struct ParticleAttributeFactory {
    using Trait                  = ParticleAttributeFactory;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = ParticleAttributeFactory;

        auto Descriptor() const -> ref<ParticleAttributeDescriptor> {
            return rstd::trait_call<0>(this);
        }
        auto Create() const -> Box<dyn<ParticleAttribute>> { return rstd::trait_call<1>(this); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Descriptor, &T::Create>;
};

template<typename Attribute>
struct ParticleAttributeKey {
    using AttributeType = Attribute;
    using Value         = typename Attribute::Value;

    ParticleAttributeId id;
    usize               schema_slot { usize::MAX };

    bool Valid() const noexcept { return id.Valid() && schema_slot != usize::MAX; }
};

struct ParticleAttributeRequirement {
    ParticleAttributeId id;
    usize               schema_slot { usize::MAX };
    rstd::any::TypeId   concrete_type;
};

template<typename Attribute>
auto RequireParticleAttribute(ParticleAttributeKey<Attribute> key) -> ParticleAttributeRequirement {
    return {
        .id            = key.id,
        .schema_slot   = key.schema_slot,
        .concrete_type = rstd::any::TypeId::of<Attribute>(),
    };
}

struct ParticleSchemaError {
    String message;
};

template<typename ValueType>
class ParticleValueAttributeStorage {
public:
    using Value = ValueType;

    ParticleValueAttributeStorage(ParticleAttributeDescriptor descriptor, Value default_value)
        : m_descriptor(rstd::move(descriptor)), m_default(rstd::move(default_value)) {}

    auto Descriptor() const -> ref<ParticleAttributeDescriptor> {
        return ref<ParticleAttributeDescriptor>::from_raw_parts(rstd::addressof(m_descriptor));
    }
    auto ConcreteType() const noexcept -> rstd::any::TypeId { return m_descriptor.concrete_type; }
    auto ValueTypeId() const noexcept -> rstd::any::TypeId { return m_descriptor.value_type; }
    auto Len() const noexcept -> usize { return m_values.len(); }
    auto Capacity() const noexcept -> usize { return m_values.capacity(); }
    void Reserve(usize total_slots) {
        if (total_slots > m_values.capacity()) m_values.reserve(total_slots - m_values.len());
    }
    void AppendDefault() { m_values.emplace_back(m_default); }
    void Reset(ParticleSlot slot) { m_values[slot.index] = m_default; }
    void Clear() { m_values.clear(); }

    auto Values() const noexcept -> slice<Value> { return m_values.as_slice(); }
    auto ValuesMut() noexcept -> mut_ref<Value[]> { return m_values.deref_mut(); }
    auto CloneDescriptor() const -> ParticleAttributeDescriptor { return m_descriptor.Clone(); }
    const Value& DefaultValue() const noexcept { return m_default; }

private:
    ParticleAttributeDescriptor m_descriptor;
    Value                       m_default;
    rstd::vec::Vec<Value>       m_values;
};

#define OWE_PARTICLE_VALUE_ATTRIBUTE(Name, Type)                                                   \
    struct Name {                                                                                  \
        using Value = Type;                                                                        \
                                                                                                   \
        Name(ParticleAttributeDescriptor descriptor, Value default_value)                          \
            : storage(rstd::move(descriptor), rstd::move(default_value)) {}                        \
                                                                                                   \
        auto Descriptor() const -> ref<ParticleAttributeDescriptor> {                              \
            return storage.Descriptor();                                                           \
        }                                                                                          \
        auto ConcreteType() const noexcept -> rstd::any::TypeId { return storage.ConcreteType(); } \
        auto ValueType() const noexcept -> rstd::any::TypeId { return storage.ValueTypeId(); }     \
        auto Len() const noexcept -> usize { return storage.Len(); }                               \
        auto Capacity() const noexcept -> usize { return storage.Capacity(); }                     \
        void Reserve(usize total_slots) { storage.Reserve(total_slots); }                          \
        void AppendDefault() { storage.AppendDefault(); }                                          \
        void Reset(ParticleSlot slot) { storage.Reset(slot); }                                     \
        void Clear() { storage.Clear(); }                                                          \
        auto Values() const noexcept -> slice<Value> { return storage.Values(); }                  \
        auto ValuesMut() noexcept -> mut_ref<Value[]> { return storage.ValuesMut(); }              \
        auto CloneEmpty() const -> Name {                                                          \
            return Name(storage.CloneDescriptor(), storage.DefaultValue());                        \
        }                                                                                          \
                                                                                                   \
        ParticleValueAttributeStorage<Value> storage;                                              \
    }

struct ParticleSlotState {
    bool active { false };
    bool fresh { false };
    u64  spawn_sequence {};
};

OWE_PARTICLE_VALUE_ATTRIBUTE(SlotStateAttribute, ParticleSlotState);
OWE_PARTICLE_VALUE_ATTRIBUTE(PositionAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(VelocityAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(AccelerationAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(RotationAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(AngularVelocityAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(AngularAccelerationAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(ColorAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(AlphaAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(SizeAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(LifetimeAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(RandomAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(InitialColorAttribute, Eigen::Vector3f);
OWE_PARTICLE_VALUE_ATTRIBUTE(InitialAlphaAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(InitialSizeAttribute, float);
OWE_PARTICLE_VALUE_ATTRIBUTE(InitialLifetimeAttribute, float);

#undef OWE_PARTICLE_VALUE_ATTRIBUTE

template<typename Attribute>
class ConcreteParticleAttributeFactory {
public:
    explicit ConcreteParticleAttributeFactory(Attribute prototype)
        : m_prototype(rstd::move(prototype)) {}

    auto Descriptor() const -> ref<ParticleAttributeDescriptor> { return m_prototype.Descriptor(); }
    auto Create() const -> Box<dyn<ParticleAttribute>> {
        return Box<dyn<ParticleAttribute>>::make(m_prototype.CloneEmpty());
    }

private:
    Attribute m_prototype;
};

class ParticleStorage;

class ParticleSchema {
public:
    ParticleSchema(const ParticleSchema&)                = delete;
    ParticleSchema& operator=(const ParticleSchema&)     = delete;
    ParticleSchema(ParticleSchema&&) noexcept            = default;
    ParticleSchema& operator=(ParticleSchema&&) noexcept = default;

    auto Len() const noexcept -> usize { return m_factories.len(); }
    auto Descriptor(usize slot) const -> ref<ParticleAttributeDescriptor> {
        return m_factories[slot]->Descriptor();
    }
    auto Find(ParticleAttributeId id) const -> Option<usize> {
        auto slot = m_id_slots.get(id.value);
        if (slot.is_none()) return None();
        auto value = usize((**slot).to_primitive());
        return Some(rstd::move(value));
    }
    auto SlotStateKey() const noexcept -> ParticleAttributeKey<SlotStateAttribute> {
        return m_slot_state_key;
    }

    auto Validate(ParticleAttributeRequirement requirement) const
        -> Result<bool, ParticleSchemaError> {
        if (! requirement.id.Valid() || requirement.schema_slot >= m_factories.len()) {
            return Err(ParticleSchemaError {
                .message = String::make("required particle attribute is missing"_str),
            });
        }
        auto descriptor = m_factories[requirement.schema_slot]->Descriptor();
        if (descriptor->id != requirement.id ||
            descriptor->concrete_type != requirement.concrete_type) {
            return Err(ParticleSchemaError {
                .message = String::make("required particle attribute does not match schema"_str),
            });
        }
        return Ok(true);
    }

    auto CreateStorage() const -> ParticleStorage;

private:
    friend class ParticleSchemaBuilder;
    friend class ParticleStorage;

    ParticleSchema(rstd::vec::Vec<Box<dyn<ParticleAttributeFactory>>> factories,
                   rstd::collections::HashMap<u64, usize>             id_slots,
                   ParticleAttributeKey<SlotStateAttribute>           slot_state_key)
        : m_factories(rstd::move(factories)),
          m_id_slots(rstd::move(id_slots)),
          m_slot_state_key(slot_state_key) {}

    rstd::vec::Vec<Box<dyn<ParticleAttributeFactory>>> m_factories;
    rstd::collections::HashMap<u64, usize>             m_id_slots;
    ParticleAttributeKey<SlotStateAttribute>           m_slot_state_key;
};

class ParticleSchemaBuilder {
public:
    ParticleSchemaBuilder();

    template<typename Attribute, typename... Args>
    auto Register(ref<str> name, ref<str> owner, ParticleAttributeResetPolicy reset_policy,
                  Args&&... args) -> Result<ParticleAttributeKey<Attribute>, ParticleSchemaError> {
        for (const auto& factory : m_factories) {
            if (factory->Descriptor()->debug_name == name) {
                return Err(ParticleSchemaError {
                    .message = String::make("duplicate particle attribute name"_str),
                });
            }
        }

        ParticleAttributeId         id { .value = m_next_id++ };
        ParticleAttributeDescriptor descriptor {
            .id              = id,
            .debug_name      = String::make(name),
            .owner           = String::make(owner),
            .concrete_type   = rstd::any::TypeId::of<Attribute>(),
            .value_type      = rstd::any::TypeId::of<typename Attribute::Value>(),
            .value_size      = usize(sizeof(typename Attribute::Value)),
            .value_alignment = usize(alignof(typename Attribute::Value)),
            .reset_policy    = reset_policy,
        };
        Attribute prototype(rstd::move(descriptor), rstd::forward<Args>(args)...);
        auto      slot    = m_factories.len();
        auto      factory = ConcreteParticleAttributeFactory<Attribute>(rstd::move(prototype));
        m_factories.push(Box<dyn<ParticleAttributeFactory>>::make(rstd::move(factory)));
        (void)m_id_slots.insert(id.value, slot);
        return Ok(ParticleAttributeKey<Attribute> { .id = id, .schema_slot = slot });
    }

    template<typename Attribute, typename... Args>
    auto Register(ref<str> name, ref<str> owner, Args&&... args)
        -> Result<ParticleAttributeKey<Attribute>, ParticleSchemaError> {
        return Register<Attribute>(
            name, owner, ParticleAttributeResetPolicy::DefaultValue, rstd::forward<Args>(args)...);
    }

    auto Build() && -> ParticleSchema {
        return ParticleSchema(rstd::move(m_factories), rstd::move(m_id_slots), m_slot_state_key);
    }

    auto SlotStateKey() const noexcept -> ParticleAttributeKey<SlotStateAttribute> {
        return m_slot_state_key;
    }

private:
    u64                                                m_next_id { 1 };
    rstd::vec::Vec<Box<dyn<ParticleAttributeFactory>>> m_factories;
    rstd::collections::HashMap<u64, usize>             m_id_slots;
    ParticleAttributeKey<SlotStateAttribute>           m_slot_state_key;
};

class ParticleStorage {
public:
    ParticleStorage(const ParticleStorage&)                = delete;
    ParticleStorage& operator=(const ParticleStorage&)     = delete;
    ParticleStorage(ParticleStorage&&) noexcept            = default;
    ParticleStorage& operator=(ParticleStorage&&) noexcept = default;

    auto Len() const noexcept -> usize { return m_len; }
    auto StructureVersion() const noexcept -> u64 { return m_structure_version; }
    auto ColumnVersion() const noexcept -> u64 { return m_column_version; }
    auto SlotStateKey() const noexcept -> ParticleAttributeKey<SlotStateAttribute> {
        return m_slot_state_key;
    }

    void Reserve(usize total_slots) {
        for (auto& attribute : m_attributes) attribute->Reserve(total_slots);
        BumpStructureVersion();
        BumpColumnVersion();
        CheckInvariant();
    }

    auto AppendSlot() -> ParticleSlot {
        Reserve(m_len + usize(1));
        for (auto& attribute : m_attributes) attribute->AppendDefault();
        ParticleSlot slot { .index = m_len++ };
        BumpStructureVersion();
        BumpColumnVersion();
        CheckInvariant();
        return slot;
    }

    void ResetSlot(ParticleSlot slot) {
        if (slot.index >= m_len) rstd::panic { "particle slot out of bounds" };
        for (auto& attribute : m_attributes) attribute->Reset(slot);
        BumpStructureVersion();
        CheckInvariant();
    }

    void Clear() {
        for (auto& attribute : m_attributes) attribute->Clear();
        m_len                 = usize();
        m_next_spawn_sequence = u64();
        BumpStructureVersion();
        BumpColumnVersion();
        CheckInvariant();
    }

    auto AcquireSlot(usize max_slots) -> Option<ParticleSlot> {
        auto states = Values(m_slot_state_key);
        for (usize index {}; index < states.len(); ++index) {
            if (states[index].active) continue;
            ParticleSlot slot { .index = index };
            ResetSlot(slot);
            auto current   = ValuesMut(m_slot_state_key);
            current[index] = {
                .active         = true,
                .fresh          = true,
                .spawn_sequence = m_next_spawn_sequence++,
            };
            return Some(slot);
        }
        if (m_len >= max_slots) return None();

        auto slot           = AppendSlot();
        auto current        = ValuesMut(m_slot_state_key);
        current[slot.index] = {
            .active         = true,
            .fresh          = true,
            .spawn_sequence = m_next_spawn_sequence++,
        };
        return Some(slot);
    }

    void ReleaseSlot(ParticleSlot slot) {
        auto states = ValuesMut(m_slot_state_key);
        if (slot.index >= states.len()) rstd::panic { "particle slot out of bounds" };
        states[slot.index] = {};
    }

    void MarkOld(ParticleSlot slot) {
        auto states = ValuesMut(m_slot_state_key);
        if (slot.index >= states.len()) rstd::panic { "particle slot out of bounds" };
        states[slot.index].fresh = false;
    }

    template<typename Attribute>
    auto AttributeRef(ParticleAttributeKey<Attribute> key) const -> ref<Attribute> {
        ValidateKey(key);
        auto erased = m_attributes[key.schema_slot].as_ref();
        return ref<Attribute>::from_raw_parts(static_cast<const Attribute*>(erased.as_raw_ptr()));
    }

    template<typename Attribute>
    auto AttributeMut(ParticleAttributeKey<Attribute> key) -> mut_ref<Attribute> {
        ValidateKey(key);
        auto erased = m_attributes[key.schema_slot].as_mut_ptr();
        return mut_ref<Attribute>::from_raw_parts(static_cast<Attribute*>(erased.as_raw_ptr()));
    }

    template<typename Attribute>
    auto Values(ParticleAttributeKey<Attribute> key) const -> slice<typename Attribute::Value> {
        return AttributeRef(key)->Values();
    }

    template<typename Attribute>
    auto ValuesMut(ParticleAttributeKey<Attribute> key) -> mut_ref<typename Attribute::Value[]> {
        return AttributeMut(key)->ValuesMut();
    }

private:
    friend class ParticleSchema;
    friend class ParticleColumnCache;

    ParticleStorage(rstd::vec::Vec<Box<dyn<ParticleAttribute>>> attributes,
                    ParticleAttributeKey<SlotStateAttribute>    slot_state_key)
        : m_attributes(rstd::move(attributes)), m_slot_state_key(slot_state_key) {
        CheckInvariant();
    }

    template<typename Attribute>
    void ValidateKey(ParticleAttributeKey<Attribute> key) const {
        if (! key.Valid() || key.schema_slot >= m_attributes.len()) {
            rstd::panic { "invalid particle attribute key" };
        }
        auto descriptor = m_attributes[key.schema_slot]->Descriptor();
        if (descriptor->id != key.id ||
            descriptor->concrete_type != rstd::any::TypeId::of<Attribute>()) {
            rstd::panic { "particle attribute key does not match storage" };
        }
    }

    auto AttributeCount() const noexcept -> usize { return m_attributes.len(); }

    void CheckInvariant() const {
        for (const auto& attribute : m_attributes) {
            if (attribute->Len() != m_len) rstd::panic { "particle attribute length mismatch" };
        }
    }

    void BumpStructureVersion() noexcept {
        ++m_structure_version;
        if (m_structure_version == u64()) m_structure_version = u64(1);
    }

    void BumpColumnVersion() noexcept {
        ++m_column_version;
        if (m_column_version == u64()) m_column_version = u64(1);
    }

    usize                                       m_len {};
    u64                                         m_next_spawn_sequence {};
    u64                                         m_structure_version { 1 };
    u64                                         m_column_version { 1 };
    rstd::vec::Vec<Box<dyn<ParticleAttribute>>> m_attributes;
    ParticleAttributeKey<SlotStateAttribute>    m_slot_state_key;
};

class ParticleColumnCache {
public:
    template<typename Attribute>
    auto ValuesMut(ParticleStorage& storage, ParticleAttributeKey<Attribute> key)
        -> mut_ref<typename Attribute::Value[]> {
        Refresh(storage);
        if (! key.Valid() || key.schema_slot >= m_columns.len()) {
            rstd::panic { "invalid particle attribute key" };
        }

        auto& entry         = m_columns[key.schema_slot];
        auto  concrete_type = rstd::any::TypeId::of<Attribute>();
        if (entry.concrete_type.is_some() &&
            (entry.id != key.id || *entry.concrete_type != concrete_type)) {
            rstd::panic { "particle attribute key does not match storage" };
        }
        if (! entry.resolved) {
            auto values         = storage.ValuesMut(key);
            entry.id            = key.id;
            entry.concrete_type = Some(concrete_type);
            entry.values        = values.as_raw_ptr();
            entry.len           = values.len();
            entry.resolved      = true;
        }
        return mut_ref<typename Attribute::Value[]>::from_raw_parts(
            static_cast<typename Attribute::Value*>(entry.values), entry.len);
    }

private:
    struct Entry {
        ParticleAttributeId       id;
        Option<rstd::any::TypeId> concrete_type;
        void*                     values { nullptr };
        usize                     len {};
        bool                      resolved { false };
    };

    void Refresh(ParticleStorage& storage) {
        auto storage_ptr = rstd::addressof(storage);
        auto version     = storage.ColumnVersion();
        if (m_storage == storage_ptr && m_version == version) return;

        if (m_storage != storage_ptr || m_columns.len() != storage.AttributeCount()) {
            m_columns.clear();
            for (usize index {}; index < storage.AttributeCount(); ++index) {
                m_columns.emplace_back();
            }
        } else {
            for (auto& entry : m_columns) {
                entry.values   = nullptr;
                entry.len      = usize();
                entry.resolved = false;
            }
        }
        m_storage = storage_ptr;
        m_version = version;
    }

    ParticleStorage*      m_storage { nullptr };
    u64                   m_version {};
    rstd::vec::Vec<Entry> m_columns;
};

class ParticleReadQuery {
public:
    explicit ParticleReadQuery(const ParticleStorage& storage)
        : m_storage(rstd::addressof(storage)), m_version(storage.StructureVersion()) {}

    template<typename Attribute>
    auto Read(ParticleAttributeKey<Attribute> key) const -> slice<typename Attribute::Value> {
        CheckVersion();
        return m_storage->Values(key);
    }

private:
    void CheckVersion() const {
        if (m_storage->StructureVersion() != m_version) {
            rstd::panic { "particle query used after structural mutation" };
        }
    }

    const ParticleStorage* m_storage;
    u64                    m_version;
};

class ParticleSlotReader {
public:
    ParticleSlotReader(const ParticleStorage& storage, ParticleSlot slot)
        : m_storage(rstd::addressof(storage)), m_slot(slot), m_version(storage.StructureVersion()) {
        if (slot.index >= storage.Len()) rstd::panic { "particle slot out of bounds" };
    }

    template<typename Attribute>
    auto Read(ParticleAttributeKey<Attribute> key) const -> const typename Attribute::Value& {
        CheckVersion();
        return m_storage->Values(key)[m_slot.index];
    }

    auto Slot() const noexcept -> ParticleSlot { return m_slot; }

private:
    void CheckVersion() const {
        if (m_storage->StructureVersion() != m_version) {
            rstd::panic { "particle slot reader used after structural mutation" };
        }
    }

    const ParticleStorage* m_storage;
    ParticleSlot           m_slot;
    u64                    m_version;
};

class ParticleSlotWriter {
public:
    ParticleSlotWriter(ParticleStorage& storage, ParticleSlot slot)
        : m_storage(rstd::addressof(storage)), m_slot(slot), m_version(storage.StructureVersion()) {
        if (slot.index >= storage.Len()) rstd::panic { "particle slot out of bounds" };
    }

    template<typename Attribute>
    auto Read(ParticleAttributeKey<Attribute> key) const -> const typename Attribute::Value& {
        CheckVersion();
        return m_storage->Values(key)[m_slot.index];
    }

    template<typename Attribute>
    auto Write(ParticleAttributeKey<Attribute> key) -> typename Attribute::Value& {
        CheckVersion();
        return m_storage->ValuesMut(key)[m_slot.index];
    }

    auto Slot() const noexcept -> ParticleSlot { return m_slot; }

private:
    void CheckVersion() const {
        if (m_storage->StructureVersion() != m_version) {
            rstd::panic { "particle slot writer used after structural mutation" };
        }
    }

    ParticleStorage* m_storage;
    ParticleSlot     m_slot;
    u64              m_version;
};

class ParticleWriteQuery {
public:
    explicit ParticleWriteQuery(ParticleStorage& storage)
        : m_storage(rstd::addressof(storage)), m_version(storage.StructureVersion()) {}

    template<typename Attribute>
    auto Read(ParticleAttributeKey<Attribute> key) const -> slice<typename Attribute::Value> {
        CheckVersion();
        return m_storage->Values(key);
    }

    template<typename Attribute>
    auto Write(ParticleAttributeKey<Attribute> key) -> mut_ref<typename Attribute::Value[]> {
        CheckVersion();
        return m_storage->ValuesMut(key);
    }

private:
    void CheckVersion() const {
        if (m_storage->StructureVersion() != m_version) {
            rstd::panic { "particle query used after structural mutation" };
        }
    }

    ParticleStorage* m_storage;
    u64              m_version;
};

inline ParticleSchemaBuilder::ParticleSchemaBuilder() {
    auto key =
        Register<SlotStateAttribute>("slot_state"_str, "framework"_str, ParticleSlotState {});
    if (key.is_err()) rstd::panic { "failed to register particle slot state" };
    m_slot_state_key = key.unwrap();
}

inline auto ParticleSchema::CreateStorage() const -> ParticleStorage {
    auto attributes = rstd::vec::Vec<Box<dyn<ParticleAttribute>>>::with_capacity(m_factories.len());
    for (const auto& factory : m_factories) attributes.push(factory->Create());
    return ParticleStorage(rstd::move(attributes), m_slot_state_key);
}

} // namespace owe::particle

export namespace rstd
{

template<>
struct Impl<fmt::Display, owe::particle::ParticleSchemaError>
    : ImplBase<owe::particle::ParticleSchemaError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(fmt::Arguments::make("{}", this->self().message));
    }
};

template<>
struct Impl<fmt::Debug, owe::particle::ParticleSchemaError>
    : ImplBase<owe::particle::ParticleSchemaError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(
            fmt::Arguments::make("ParticleSchemaError({})", this->self().message));
    }
};

template<>
struct Impl<error::Error, owe::particle::ParticleSchemaError>
    : DefaultInImpl<error::Error, owe::particle::ParticleSchemaError> {};

} // namespace rstd

static_assert(rstd::Impled<owe::particle::ParticleSchemaError, rstd::error::Error>);
