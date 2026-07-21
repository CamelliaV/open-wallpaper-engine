export module wescene.scene:runtime;
import rstd;

using namespace rstd::prelude;

export namespace owe
{

struct SceneFrame {
    u64 index { 0 };
    f64 elapsed { 0.0 };
    f64 delta { 0.0 };
    u64 revision { 1 };
};

struct SceneRuntimeSystem {
    using Trait                  = SceneRuntimeSystem;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = SceneRuntimeSystem;

        void Update(ref<SceneFrame> frame) { rstd::trait_call<0>(this, frame); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Update>;
};

class SceneRuntime {
public:
    const SceneFrame& Frame() const noexcept { return m_frame; }

    void Register(Box<dyn<SceneRuntimeSystem>> system) { m_systems.push(rstd::move(system)); }

    template<typename T>
    void RegisterSystem(T system) {
        Register(Box<dyn<SceneRuntimeSystem>>::make(rstd::move(system)));
    }

    void Advance(f64 delta) {
        m_frame.delta = delta;
        m_frame.elapsed += delta;
        ++m_frame.index;
        ++m_frame.revision;
        if (m_frame.revision == u64()) m_frame.revision = u64(1);

        auto frame = ref<SceneFrame>::from_raw_parts(rstd::addressof(m_frame));
        for (auto& system : m_systems) system->Update(frame);
    }

private:
    SceneFrame                                   m_frame;
    rstd::vec::Vec<Box<dyn<SceneRuntimeSystem>>> m_systems;
};

} // namespace owe
