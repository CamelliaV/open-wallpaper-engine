module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;
import eigen;
import wescene.core;
import rstd.log;
import rstd.cppstd;
import wescene.utils;
import wescene.scene;
import wescene.particle;
import wescene.particle.program;

using namespace owe;
using namespace Eigen;

namespace WPParticleModify
{

void Move(WPParticleRef value, const Eigen::Vector3d& offset) {
    value.position = (value.position.cast<double>() + offset).cast<float>();
}

void ChangeVelocity(WPParticleRef value, const Eigen::Vector3d& velocity) {
    value.velocity = (value.velocity.cast<double>() + velocity).cast<float>();
}

void ChangeVelocity(WPParticleRef value, double x, double y, double z) {
    ChangeVelocity(value, { x, y, z });
}

void Accelerate(WPParticleRef value, const Eigen::Vector3d& acceleration, f64 time) {
    ChangeVelocity(value, acceleration * time.to_primitive());
}

void ChangeAngularVelocity(WPParticleRef value, const Eigen::Vector3d& velocity) {
    value.angular_velocity = (value.angular_velocity.cast<double>() + velocity).cast<float>();
}

void ChangeAngularVelocity(WPParticleRef value, double x, double y, double z) {
    ChangeAngularVelocity(value, { x, y, z });
}

void AngularAccelerate(WPParticleRef value, const Eigen::Vector3d& acceleration, f64 time) {
    ChangeAngularVelocity(value, acceleration * time.to_primitive());
}

void ChangeRotation(WPParticleRef value, const Eigen::Vector3d& rotation) {
    value.rotation = (value.rotation.cast<double>() + rotation).cast<float>();
}

void ChangeRotation(WPParticleRef value, double x, double y, double z) {
    ChangeRotation(value, { x, y, z });
}

void InitLifetime(WPParticleRef value, float lifetime) {
    value.lifetime         = lifetime;
    value.initial.lifetime = lifetime;
}

void InitSize(WPParticleRef value, double size) {
    value.size         = static_cast<float>(size);
    value.initial.size = static_cast<float>(size);
}

void InitAlpha(WPParticleRef value, double alpha) {
    value.alpha         = static_cast<float>(alpha);
    value.initial.alpha = static_cast<float>(alpha);
}

void InitColor(WPParticleRef value, double red, double green, double blue) {
    value.color         = Eigen::Vector3d(red, green, blue).cast<float>();
    value.initial.color = value.color;
}

void MutiplyInitLifeTime(WPParticleRef value, double multiplier) {
    value.lifetime *= multiplier;
    value.initial.lifetime = value.lifetime;
}

void MutiplyInitAlpha(WPParticleRef value, double multiplier) {
    value.alpha *= multiplier;
    value.initial.alpha = value.alpha;
}

void MutiplyInitSize(WPParticleRef value, double multiplier) {
    value.size *= multiplier;
    value.initial.size = value.size;
}

void MutiplyVelocity(WPParticleRef value, double multiplier) { value.velocity *= multiplier; }

void MutiplyAlpha(WPParticleRef value, double multiplier) { value.alpha *= multiplier; }

void MutiplySize(WPParticleRef value, double multiplier) { value.size *= multiplier; }

void MutiplyColor(WPParticleRef value, double red, double green, double blue) {
    value.color =
        Eigen::Vector3d(red, green, blue).cwiseProduct(value.color.cast<double>()).cast<float>();
}

auto LifetimePos(WPParticleRef value) -> double {
    if (value.lifetime < 0.0f) return 1.0;
    return 1.0 - static_cast<double>(value.lifetime / value.initial.lifetime);
}

auto LifetimePassed(WPParticleRef value) noexcept -> double {
    return static_cast<double>(value.initial.lifetime - value.lifetime);
}

auto LifetimeOk(WPParticleRef value) noexcept -> bool { return value.lifetime > 0.0f; }
auto GetPos(WPParticleRef value) -> const Eigen::Vector3f& { return value.position; }
auto GetVelocity(WPParticleRef value) -> const Eigen::Vector3f& { return value.velocity; }
auto GetAngular(WPParticleRef value) -> const Eigen::Vector3f& { return value.rotation; }

} // namespace WPParticleModify

namespace PM = WPParticleModify;

namespace
{

constexpr float  kPi    = rstd::f32::consts::PI.to_primitive();
constexpr float  kTau   = rstd::f32::consts::TAU.to_primitive();
constexpr double kTau64 = rstd::f64::consts::TAU.to_primitive();

inline void Color(WPParticleRef p, const std::array<float, 3> min, const std::array<float, 3> max) {
    double               random = Random::get(0.0, 1.0);
    std::array<float, 3> result;
    for (int32_t i = 0; i < 3; i++) {
        result[i] = (float)algorism::lerp(random, min[i], max[i]);
    }
    PM::InitColor(p, result[0], result[1], result[2]);
}

inline Vector3d GenRandomVec3(const std::array<float, 3>& min, const std::array<float, 3>& max) {
    Vector3d result(3);
    for (int32_t i = 0; i < 3; i++) {
        result[i] = Random::get(min[i], max[i]);
    }
    return result;
}

inline float UiColorToLinear(float value) { return value * value; }

inline float UiScalarToLinear(float value) { return value; }

} // namespace

struct SingleRandom {
    float       min { 0.0f };
    float       max { 0.0f };
    float       exponent { 1.0f };
    static void ReadFromJson(const Json& j, SingleRandom& r) {
        owe::GetJsonValue(j, "min", r.min, false);
        owe::GetJsonValue(j, "max", r.max, false);
    };
};
struct VecRandom {
    std::array<float, 3> min { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> max { 0.0f, 0.0f, 0.0f };
    float                exponent { 1.0f };

    static void ReadFromJson(const Json& j, VecRandom& r) {
        owe::GetJsonValue(j, "min", r.min, false);
        owe::GetJsonValue(j, "max", r.max, false);
    };
};
struct TurbulentRandom {
    float  scale { 1.0f };
    double timescale { 1.0f };
    float  offset { 0.0f };
    float  speedmin { 100.0f };
    float  speedmax { 250.0f };
    float  phasemin { 0.0f };
    float  phasemax { 0.1f };

    std::array<float, 3> forward { 0.0f, 1.0f, 0.0f }; // x y z
    std::array<float, 3> right { 0.0f, 0.0f, 1.0f };
    std::array<float, 3> up { 1.0f, 0.0f, 0.0f };

    static void ReadFromJson(const Json& j, TurbulentRandom& r) {
        owe::GetJsonValue(j, "scale", r.scale, false);
        owe::GetJsonValue(j, "timescale", r.timescale, false);
        owe::GetJsonValue(j, "offset", r.offset, false);
        owe::GetJsonValue(j, "speedmin", r.speedmin, false);
        owe::GetJsonValue(j, "speedmax", r.speedmax, false);
        owe::GetJsonValue(j, "phasemin", r.phasemin, false);
        owe::GetJsonValue(j, "phasemax", r.phasemax, false);
        owe::GetJsonValue(j, "forward", r.forward, false);
        owe::GetJsonValue(j, "right", r.right, false);
        owe::GetJsonValue(j, "up", r.up, false);
    };
};
template<rstd::size_t N>
std::array<float, N> mapVertex(const std::array<float, N>& v, float (*oper)(float)) {
    std::array<float, N> result;
    std::transform(v.begin(), v.end(), result.begin(), oper);
    return result;
};

Box<dyn<particle::ParticleSpawnProgram>>
WPParticleParser::GenInitializer(const Json& wpj, WPParticleAttributes attributes) {
    using namespace std::placeholders;
    do {
        if (wpj.get("name").is_none()) break;
        std::string name;
        owe::GetJsonValue(wpj, "name", name);

        if (name == "colorrandom") {
            VecRandom r;
            r.min = { 0.0f, 0.0f, 0.0f };
            r.max = { 255.0f, 255.0f, 255.0f };
            VecRandom::ReadFromJson(wpj, r);

            auto function = [=](WPParticleRef p, f64) {
                Color(p,
                      mapVertex(r.min,
                                [](float x) {
                                    return x / 255.0f;
                                }),
                      mapVertex(r.max, [](float x) {
                          return x / 255.0f;
                      }));
            };
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPParticleInitProgram(attributes, rstd::move(function)));
        } else if (name == "lifetimerandom") {
            SingleRandom r = { 0.0f, 1.0f };
            SingleRandom::ReadFromJson(wpj, r);
            auto function = [=](WPParticleRef p, f64) {
                PM::InitLifetime(p, Random::get(r.min, r.max));
            };
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPParticleInitProgram(attributes, rstd::move(function)));
        } else if (name == "sizerandom") {
            SingleRandom r = { 0.0f, 20.0f };
            SingleRandom::ReadFromJson(wpj, r);
            auto function = [=](WPParticleRef p, f64) {
                PM::InitSize(p, Random::get(r.min, r.max));
            };
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPParticleInitProgram(attributes, rstd::move(function)));
        } else if (name == "alpharandom") {
            SingleRandom r = { 0.05f, 1.0f };
            SingleRandom::ReadFromJson(wpj, r);
            auto function = [=](WPParticleRef p, f64) {
                PM::InitAlpha(p, Random::get(r.min, r.max));
            };
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPParticleInitProgram(attributes, rstd::move(function)));
        } else if (name == "velocityrandom") {
            VecRandom r;
            r.min[0] = r.min[1] = -32.0f;
            r.max[0] = r.max[1] = 32.0f;
            VecRandom::ReadFromJson(wpj, r);
            auto function = [=](WPParticleRef p, f64) {
                auto result = GenRandomVec3(r.min, r.max);
                PM::ChangeVelocity(p, result[0], result[1], result[2]);
            };
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPParticleInitProgram(attributes, rstd::move(function)));
        } else if (name == "rotationrandom") {
            VecRandom r;
            r.max[2] = kTau;
            VecRandom::ReadFromJson(wpj, r);
            auto function = [=](WPParticleRef p, f64) {
                auto result = GenRandomVec3(r.min, r.max);
                PM::ChangeRotation(p, result[0], result[1], result[2]);
            };
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPParticleInitProgram(attributes, rstd::move(function)));
        } else if (name == "angularvelocityrandom") {
            VecRandom r;
            r.min[2] = -5.0f;
            r.max[2] = 5.0f;
            VecRandom::ReadFromJson(wpj, r);
            auto function = [=](WPParticleRef p, f64) {
                auto result = GenRandomVec3(r.min, r.max);
                PM::ChangeAngularVelocity(p, result[0], result[1], result[2]);
            };
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPParticleInitProgram(attributes, rstd::move(function)));
        } else if (name == "turbulentvelocityrandom") {
            // to do
            TurbulentRandom r;
            TurbulentRandom::ReadFromJson(wpj, r);
            Vector3f forward(r.forward.data());
            Vector3f right(r.right.data());
            Vector3f pos      = GenRandomVec3({ 0, 0, 0 }, { 10.0f, 10.0f, 10.0f }).cast<float>();
            auto     function = [=](WPParticleRef p, f64 duration) mutable {
                float speed = Random::get(r.speedmin, r.speedmax);
                if (duration > f64(10.0)) {
                    pos[0] += speed;
                    duration = f64();
                }
                Vector3f result;
                do {
                    result = algorism::CurlNoise(pos.cast<double>()).cast<float>().normalized();
                    pos += result * 0.005f / r.timescale;
                    duration -= f64(0.01);
                } while (duration > f64(0.01));
                // limit direction
                {
                    double c     = result.dot(forward) / (result.norm() * forward.norm());
                    float  a     = static_cast<float>(std::acos(c)) / kPi;
                    float  scale = r.scale / 2.0f;
                    if (a > scale) {
                        auto axis = result.cross(forward).normalized();
                        result    = AngleAxisf((a - a * scale) * kPi, axis) * result;
                    }
                }
                // offset
                result = AngleAxisf(r.offset, right) * result;
                result *= speed;
                PM::ChangeVelocity(p, result[0], result[1], result[2]);
            };
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPParticleInitProgram(attributes, rstd::move(function)));
        }
    } while (false);
    return Box<dyn<particle::ParticleSpawnProgram>>::make(
        WPParticleInitProgram(attributes, [](WPParticleRef, f64) {
        }));
}

Box<dyn<particle::ParticleSpawnProgram>>
WPParticleParser::GenOverride(std::shared_ptr<const wpscene::ParticleInstanceoverride> over,
                              WPParticleAttributes                                     attributes) {
    auto function = [over = std::move(over)](WPParticleRef p, f64) {
        PM::MutiplyInitLifeTime(p, over->lifetime);
        PM::MutiplyInitAlpha(p, UiScalarToLinear(over->alpha));
        PM::MutiplyInitSize(p, over->size);
        PM::MutiplyVelocity(p, over->speed);
        if (over->overColor) {
            PM::InitColor(p,
                          UiColorToLinear(over->color[0] / 255.0f),
                          UiColorToLinear(over->color[1] / 255.0f),
                          UiColorToLinear(over->color[2] / 255.0f));
        } else if (over->overColorn) {
            // `colorn` = "color (normalized)" -> absolute 0..1 RGB override
            // (matches WE editor behaviour: picking red in the UI yields a
            // red trail regardless of the base colorrandom initializer).
            PM::InitColor(p,
                          UiColorToLinear(over->colorn[0]),
                          UiColorToLinear(over->colorn[1]),
                          UiColorToLinear(over->colorn[2]));
        }
    };
    return Box<dyn<particle::ParticleSpawnProgram>>::make(
        WPParticleInitProgram(attributes, rstd::move(function)));
}
double FadeValueChange(float life, float start, float end, float startValue,
                       float endValue) noexcept {
    if (life <= start)
        return startValue;
    else if (life > end)
        return endValue;
    else {
        double pass = (life - start) / (end - start);
        return algorism::lerp(pass, startValue, endValue);
    }
}

struct ValueChange {
    float starttime { 0 };
    float endtime { 1.0f };
    float startvalue { 1.0f };
    float endvalue { 0.0f };

    static auto ReadFromJson(const Json& j) {
        ValueChange v;
        owe::GetJsonValue(j, "starttime", v.starttime, false);
        owe::GetJsonValue(j, "endtime", v.endtime, false);
        owe::GetJsonValue(j, "startvalue", v.startvalue, false);
        owe::GetJsonValue(j, "endvalue", v.endvalue, false);
        return v;
    }
};
double FadeValueChange(float life, const ValueChange& v) noexcept {
    return FadeValueChange(life, v.starttime, v.endtime, v.startvalue, v.endvalue);
}

struct VecChange {
    float                starttime { 0 };
    float                endtime { 1.0f };
    std::array<float, 3> startvalue { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> endvalue { 0.0f, 0.0f, 0.0f };

    static auto ReadFromJson(const Json& j) {
        VecChange v;
        owe::GetJsonValue(j, "starttime", v.starttime, false);
        owe::GetJsonValue(j, "endtime", v.endtime, false);
        owe::GetJsonValue(j, "startvalue", v.startvalue, false);
        owe::GetJsonValue(j, "endvalue", v.endvalue, false);
        return v;
    }
};

struct FrequencyValue {
    std::array<float, 3> mask { 1.0f, 1.0f, 0.0f };

    float frequencymin { 0.0f };
    float frequencymax { 10.0f };
    float scalemin { 0.0f };
    float scalemax { 1.0f };
    float phasemin { 0.0f };
    float phasemax { kTau };

    static auto ReadFromJson(const Json& j, std::string_view name) {
        FrequencyValue v;
        if (name == "oscillatesize") {
            v.scalemin = 0.8f;
            v.scalemax = 1.2f;
        } else if (name == "oscillateposition") {
            v.frequencymax = 5.0f;
        }
        owe::GetJsonValue(j, "frequencymin", v.frequencymin, false);
        owe::GetJsonValue(j, "frequencymax", v.frequencymax, false);
        if (v.frequencymax == 0.0f) v.frequencymax = v.frequencymin;
        owe::GetJsonValue(j, "scalemin", v.scalemin, false);
        owe::GetJsonValue(j, "scalemax", v.scalemax, false);
        owe::GetJsonValue(j, "phasemin", v.phasemin, false);
        owe::GetJsonValue(j, "phasemax", v.phasemax, false);
        owe::GetJsonValue(j, "mask", v.mask, false);
        return v;
    };
    inline void GenFrequency(WPParticleRef p, WPOscillationStateRef st) {
        if (! PM::LifetimeOk(p)) st.reset = true;
        if (st.reset) {
            st.frequency = Random::get(frequencymin, frequencymax);
            st.scale     = Random::get(scalemin, scalemax);
            st.phase =
                static_cast<float>(Random::get(static_cast<double>(phasemin), phasemax + kTau64));
            st.reset = false;
        }
    }
    inline double GetScale(WPOscillationStateRef st, double time) {
        double f = st.frequency / kTau;
        double w = kTau * f;
        return algorism::lerp((std::cos(w * time + st.phase) + 1.0f) * 0.5f, scalemin, scalemax);
    }
    inline double GetMove(WPOscillationStateRef st, double time, f64 time_pass) {
        double f = st.frequency / kTau;
        double w = kTau * f;
        return -1.0f * st.scale * w * std::sin(w * time + st.phase) * time_pass.to_primitive();
    }
};

struct Turbulence {
    // the minimum time offset of the noise field for a particle.
    float phasemin { 0 };
    // the maximum time offset of the noise field for a particle.
    float phasemax { 0 };
    // the minimum velocity applied to particles.
    float speedmin { 500.0f };
    // the maximum velocity applied to particles.
    float speedmax { 1000.0f };
    // how fast the noise field changes shape.
    float timescale { 20.0f };

    float scale { 0.01f };

    std::array<int32_t, 3> mask { 1, 1, 0 };

    static auto ReadFromJson(const Json& j) {
        Turbulence v;
        owe::GetJsonValue(j, "phasemin", v.phasemin, false);
        owe::GetJsonValue(j, "phasemax", v.phasemax, false);
        owe::GetJsonValue(j, "speedmin", v.speedmin, false);
        owe::GetJsonValue(j, "speedmax", v.speedmax, false);
        owe::GetJsonValue(j, "timescale", v.timescale, false);
        owe::GetJsonValue(j, "mask", v.mask, false);
        owe::GetJsonValue(j, "scale", v.scale, false);
        return v;
    };
};

struct Vortex {
    enum class FlagEnum
    {
        infinit_axis = 0, // 1
    };
    using EFlags = BitFlags<FlagEnum>;

    i32 controlpoint { 0 };

    // anything below this distance receives force multiplied with speed inner.
    float distanceinner { 500.0f };
    // anything above this distance receives force multiplied with speed outer.
    float distanceouter { 650.0f };
    // amount of force applied to inner ring.
    float speedinner { 2500.0f };
    // amount of force applied to outer ring.
    float speedouter { 0 };

    EFlags flags { 0 };

    // positional offset from the center of the control point.
    std::array<float, 3> offset { 0.0f, 0.0f, 0.0f };

    // the axis to rotate around.
    std::array<float, 3> axis { 0.0f, 0.0f, 1.0f };

    static auto ReadFromJson(const Json& j) {
        Vortex v;
        owe::GetJsonValue(j, "controlpoint", v.controlpoint, false);
        if (v.controlpoint >= i32(8)) rstd_error("wrong contropoint index {}", v.controlpoint);
        v.controlpoint %= i32(8);

        owe::GetJsonValue(j, "distanceinner", v.distanceinner, false);
        owe::GetJsonValue(j, "distanceouter", v.distanceouter, false);
        owe::GetJsonValue(j, "speedinner", v.speedinner, false);
        owe::GetJsonValue(j, "speedouter", v.speedouter, false);

        i32 _flags { 0 };
        owe::GetJsonValue(j, "flags", _flags, false);
        v.flags = EFlags(static_cast<rstd::uint32_t>(_flags.to_primitive()));

        owe::GetJsonValue(j, "offset", v.offset, false);
        owe::GetJsonValue(j, "axis", v.axis, false);

        return v;
    };
};

struct ControlPointForce {
    i32 controlpoint { 0 };

    // how strongly the control point attracts or repels.
    float scale { 512.0f };
    // the maximum distance between particle and control point where the force takes effect.
    float threshold { 512.0f };

    // positional offset from the center of the control point.
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };

    static auto ReadFromJson(const Json& j) {
        ControlPointForce v;
        owe::GetJsonValue(j, "controlpoint", v.controlpoint, false);
        if (v.controlpoint >= i32(8)) rstd_error("wrong contropoint index {}", v.controlpoint);
        v.controlpoint %= i32(8);

        owe::GetJsonValue(j, "scale", v.scale, false);
        owe::GetJsonValue(j, "threshold", v.threshold, false);

        owe::GetJsonValue(j, "origin", v.origin, false);
        return v;
    };
};

template<typename F>
auto MakeUpdateProgram(WPParticleAttributes attributes, F&& function)
    -> Box<dyn<particle::ParticleUpdateProgram>> {
    return Box<dyn<particle::ParticleUpdateProgram>>::make(
        WPParticleUpdateProgram(attributes, rstd::forward<F>(function)));
}

template<typename Attribute, typename Value>
auto RegisterOscillationAttribute(particle::ParticleSchemaBuilder& builder, const std::string& name,
                                  Value default_value)
    -> particle::ParticleAttributeKey<Attribute> {
    auto result = builder.Register<Attribute>(ref<str>(name.c_str()),
                                              ref<str>("we.operator.oscillation"),
                                              particle::ParticleAttributeResetPolicy::Custom,
                                              default_value);
    if (result.is_err()) rstd::panic { "failed to register oscillation attribute" };
    return result.unwrap();
}

auto RegisterOscillationAttributes(WPParticleSubSystem& subsystem, usize operator_index,
                                   std::string_view suffix) -> WPOscillationAttributes {
    auto  prefix  = std::string("oscillation_") + std::to_string(operator_index.to_primitive()) +
                    "_" + std::string(suffix);
    auto& builder = subsystem.SchemaBuilder();
    WPOscillationAttributes attributes {
        .reset = RegisterOscillationAttribute<WPOscillationResetAttribute>(
            builder, prefix + "_reset", true),
        .frequency = RegisterOscillationAttribute<WPOscillationFrequencyAttribute>(
            builder, prefix + "_frequency", 0.0f),
        .scale = RegisterOscillationAttribute<WPOscillationScaleAttribute>(
            builder, prefix + "_scale", 1.0f),
        .phase = RegisterOscillationAttribute<WPOscillationPhaseAttribute>(
            builder, prefix + "_phase", 0.0f),
    };
    subsystem.RequireAttribute(attributes.reset);
    subsystem.RequireAttribute(attributes.frequency);
    subsystem.RequireAttribute(attributes.scale);
    subsystem.RequireAttribute(attributes.phase);
    return attributes;
}

Box<dyn<particle::ParticleUpdateProgram>>
WPParticleParser::GenOperator(const Json&                                              wpj,
                              std::shared_ptr<const wpscene::ParticleInstanceoverride> over_state,
                              WPParticleSubSystem& subsystem, usize operator_index) {
    auto attributes = subsystem.Attributes();
    do {
        if (wpj.get("name").is_none()) break;
        std::string name;
        owe::GetJsonValue(wpj, "name", name);
        if (name == "movement") {
            float drag { 0.0f };

            std::array<float, 3> gravity { 0, 0, 0 };
            owe::GetJsonValue(wpj, "drag", drag, false);
            owe::GetJsonValue(wpj, "gravity", gravity, false);
            Vector3d vecG     = Vector3f(gravity.data()).cast<double>();
            auto     function = [drag, vecG, over_state](WPParticleBatch& info) {
                auto speed = over_state->speed;
                for (usize index {}; index < info.Len(); ++index) {
                    auto     p = info.Particle(index);
                    Vector3d world_velocity =
                        info.world_from_local_dir * PM::GetVelocity(p).cast<double>();
                    Vector3d acc =
                        info.local_from_world_dir * algorism::DragForce(world_velocity, drag);
                    if (info.world_space)
                        acc += info.local_from_world_dir * vecG;
                    else
                        acc += vecG;
                    PM::Accelerate(p, speed * acc, info.time_pass);
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "angularmovement") {
            float                drag { 0.0f };
            std::array<float, 3> force { 0, 0, 0 };
            owe::GetJsonValue(wpj, "drag", drag, false);
            owe::GetJsonValue(wpj, "force", force, false);
            Vector3d vecF     = Vector3f(force.data()).cast<double>();
            auto     function = [=](WPParticleBatch& info) {
                for (usize index {}; index < info.Len(); ++index) {
                    auto     p = info.Particle(index);
                    Vector3d acc =
                        algorism::DragForce(PM::GetAngular(p).cast<double>(), drag) + vecF;
                    PM::AngularAccelerate(p, acc, info.time_pass);
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "sizechange") {
            auto vc       = ValueChange::ReadFromJson(wpj);
            auto function = [vc, over_state](WPParticleBatch& info) {
                auto size_over = over_state->size;
                for (usize index {}; index < info.Len(); ++index) {
                    auto p = info.Particle(index);
                    PM::MutiplySize(p, size_over * FadeValueChange(PM::LifetimePos(p), vc));
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));

        } else if (name == "alphafade") {
            float fadeintime { 0.5f }, fadeouttime { 0.5f };
            owe::GetJsonValue(wpj, "fadeintime", fadeintime, false);
            owe::GetJsonValue(wpj, "fadeouttime", fadeouttime, false);
            auto function = [fadeintime, fadeouttime](WPParticleBatch& info) {
                for (usize index {}; index < info.Len(); ++index) {
                    auto p    = info.Particle(index);
                    auto life = PM::LifetimePos(p);
                    if (life <= fadeintime)
                        PM::MutiplyAlpha(p, FadeValueChange(life, 0, fadeintime, 0, 1.0f));
                    else if (life > fadeouttime)
                        PM::MutiplyAlpha(p,
                                         1.0f - FadeValueChange(life, fadeouttime, 1.0f, 0, 1.0f));
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "alphachange") {
            auto vc       = ValueChange::ReadFromJson(wpj);
            auto function = [vc](WPParticleBatch& info) {
                for (usize index {}; index < info.Len(); ++index) {
                    auto p = info.Particle(index);
                    PM::MutiplyAlpha(p, FadeValueChange(PM::LifetimePos(p), vc));
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "colorchange") {
            auto vc       = VecChange::ReadFromJson(wpj);
            auto function = [vc](WPParticleBatch& info) {
                for (usize index {}; index < info.Len(); ++index) {
                    auto     p    = info.Particle(index);
                    auto     life = PM::LifetimePos(p);
                    Vector3f result;
                    for (unsigned i = 0; i < 3; i++)
                        result[i] = FadeValueChange(
                            life, vc.starttime, vc.endtime, vc.startvalue[i], vc.endvalue[i]);
                    PM::MutiplyColor(p, result[0], result[1], result[2]);
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "oscillatealpha") {
            FrequencyValue fv = FrequencyValue::ReadFromJson(wpj, name);
            auto state_keys   = RegisterOscillationAttributes(subsystem, operator_index, "alpha");
            auto function     = [fv, state_keys](WPParticleBatch& info) mutable {
                auto states = state_keys.ValuesMut(info.storage);
                for (usize index {}; index < info.Len(); ++index) {
                    auto p     = info.Particle(index);
                    auto state = states.At(index);
                    fv.GenFrequency(p, state);
                    PM::MutiplyAlpha(p, fv.GetScale(state, PM::LifetimePassed(p)));
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "oscillatesize") {
            FrequencyValue fv = FrequencyValue::ReadFromJson(wpj, name);
            auto state_keys   = RegisterOscillationAttributes(subsystem, operator_index, "size");
            auto function     = [fv, state_keys](WPParticleBatch& info) mutable {
                auto states = state_keys.ValuesMut(info.storage);
                for (usize index {}; index < info.Len(); ++index) {
                    auto p     = info.Particle(index);
                    auto state = states.At(index);
                    fv.GenFrequency(p, state);
                    PM::MutiplySize(p, fv.GetScale(state, PM::LifetimePassed(p)));
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));

        } else if (name == "oscillateposition") {
            FrequencyValue                         fvx = FrequencyValue::ReadFromJson(wpj, name);
            std::array<FrequencyValue, 3>          fxp = { fvx, fvx, fvx };
            std::array<WPOscillationAttributes, 3> state_keys {
                RegisterOscillationAttributes(subsystem, operator_index, "position_x"),
                RegisterOscillationAttributes(subsystem, operator_index, "position_y"),
                RegisterOscillationAttributes(subsystem, operator_index, "position_z"),
            };
            auto function = [fxp, state_keys](WPParticleBatch& info) mutable {
                std::array<WPOscillationValues, 3> states {
                    state_keys[0].ValuesMut(info.storage),
                    state_keys[1].ValuesMut(info.storage),
                    state_keys[2].ValuesMut(info.storage),
                };
                for (usize index {}; index < info.Len(); ++index) {
                    auto     p = info.Particle(index);
                    Vector3d del { Vector3d::Zero() };
                    auto     time = PM::LifetimePassed(p);
                    for (usize d {}; d < usize(3); ++d) {
                        auto component = d.to_primitive();
                        if (fxp[0].mask[component] < 0.01) continue;
                        auto state = states[component].At(index);
                        fxp[component].GenFrequency(p, state);
                        del[component] = fxp[component].GetMove(state, time, info.time_pass);
                    }
                    PM::Move(p, del);
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "turbulence") {
            Turbulence tur   = Turbulence::ReadFromJson(wpj);
            double     phase = Random::get(tur.phasemin, tur.phasemax);
            double     speed = Random::get(tur.speedmin, tur.speedmax);

            auto function = [=](WPParticleBatch& info) {
                for (usize index {}; index < info.Len(); ++index) {
                    auto     p   = info.Particle(index);
                    Vector3d pos = PM::GetPos(p).cast<double>();
                    pos.x() += phase + tur.timescale * info.time.to_primitive();
                    Vector3d result = speed * algorism::CurlNoise(pos * tur.scale * 2).normalized();
                    for (usize index {}; index < usize(3); ++index) {
                        auto component = index.to_primitive();
                        if (tur.mask[component] == 0) result[component] = 0;
                    }
                    PM::Accelerate(p, result, info.time_pass);
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "vortex") {
            Vortex v        = Vortex::ReadFromJson(wpj);
            auto   function = [=](WPParticleBatch& info) {
                Vector3d offset  = info.controlpoints[rstd::as_cast<usize>(v.controlpoint)].offset +
                                   (Vector3f { v.offset.data() }).cast<double>();
                Vector3d axis    = (Vector3f { v.axis.data() }).cast<double>();
                double   dis_mid = v.distanceouter - v.distanceinner + 0.1f;

                for (usize index {}; index < info.Len(); ++index) {
                    auto     p        = info.Particle(index);
                    Vector3d pos      = p.position.cast<double>();
                    Vector3d direct   = -axis.cross(pos).normalized();
                    double   distance = (pos - offset).norm();
                    if (dis_mid < 0 || distance < v.distanceinner) {
                        PM::Accelerate(p, direct * v.speedinner, info.time_pass);
                    }
                    if (distance > v.distanceouter) {
                        PM::Accelerate(p, direct * v.speedouter, info.time_pass);
                    } else if (distance > v.distanceinner) {
                        double t = (distance - v.distanceinner) / dis_mid;
                        PM::Accelerate(p,
                                       direct * algorism::lerp(t, v.speedinner, v.speedouter),
                                       info.time_pass);
                    }
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "controlpointattract") {
            ControlPointForce c        = ControlPointForce::ReadFromJson(wpj);
            auto              function = [=](WPParticleBatch& info) {
                Vector3d offset = info.controlpoints[rstd::as_cast<usize>(c.controlpoint)].offset +
                                  Vector3f { c.origin.data() }.cast<double>();
                for (usize index {}; index < info.Len(); ++index) {
                    auto     p        = info.Particle(index);
                    Vector3d diff     = offset - PM::GetPos(p).cast<double>();
                    double   distance = diff.norm();
                    if (distance < c.threshold) {
                        PM::Accelerate(p, diff.normalized() * c.scale, info.time_pass);
                    }
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        }
    } while (false);
    return MakeUpdateProgram(attributes, [](WPParticleBatch&) {
    });
}

Box<dyn<particle::ParticleEmitterProgram>>
WPParticleParser::GenEmitter(const wpscene::Emitter& wpe, WPParticleAttributes attributes) {
    WPParticleAudioResponse audio_response {
        .enable    = wpe.audioprocessingmode != u32(),
        .amount    = wpe.audioamount,
        .exponent  = wpe.audioexponent,
        .frequency = array_cast<float>(wpe.audiofrequency),
        .bounds    = array_cast<float>(wpe.audiobounds),
    };
    if (wpe.name == "boxrandom") {
        WPParticleBoxEmitterArgs box;
        box.emit_speed     = wpe.rate;
        box.min_distance   = array_cast<float>(wpe.distancemin);
        box.max_distance   = array_cast<float>(wpe.distancemax);
        box.directions     = array_cast<float>(wpe.directions);
        box.origin         = array_cast<float>(wpe.origin);
        box.one_per_frame  = wpe.flags[wpscene::Emitter::FlagEnum::one_per_frame];
        box.instantaneous  = wpe.instantaneous;
        box.min_speed      = wpe.speedmin;
        box.max_speed      = wpe.speedmax;
        box.duration       = wpe.duration;
        box.controlpoint   = wpe.controlpoint;
        box.audio_response = audio_response;
        return Box<dyn<particle::ParticleEmitterProgram>>::make(
            WPBoxEmitterProgram(attributes, rstd::move(box)));
    } else if (wpe.name == "sphererandom") {
        WPParticleSphereEmitterArgs sphere;
        sphere.emit_speed     = wpe.rate;
        sphere.min_distance   = wpe.distancemin[0];
        sphere.max_distance   = wpe.distancemax[0];
        sphere.directions     = array_cast<float>(wpe.directions);
        sphere.origin         = array_cast<float>(wpe.origin);
        sphere.sign           = array_cast<i32>(wpe.sign);
        sphere.one_per_frame  = wpe.flags[wpscene::Emitter::FlagEnum::one_per_frame];
        sphere.instantaneous  = wpe.instantaneous;
        sphere.min_speed      = wpe.speedmin;
        sphere.max_speed      = wpe.speedmax;
        sphere.duration       = wpe.duration;
        sphere.controlpoint   = wpe.controlpoint;
        sphere.audio_response = audio_response;
        return Box<dyn<particle::ParticleEmitterProgram>>::make(
            WPSphereEmitterProgram(attributes, rstd::move(sphere)));
    }

    struct NoopEmitter {
        void Emit(particle::ParticleEmitterContext&) {}
    };
    return Box<dyn<particle::ParticleEmitterProgram>>::make(NoopEmitter {});
}
