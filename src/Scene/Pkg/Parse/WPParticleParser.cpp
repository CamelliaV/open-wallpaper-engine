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
using namespace rstd::literals;
using rstd::sync::Arc;

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

enum class SequenceLimitBehavior
{
    Repeat,
    Mirror,
    Clamp,
};

auto ParseSequenceLimitBehavior(const Json& json) -> SequenceLimitBehavior {
    std::string value { "repeat" };
    owe::GetJsonValue(json, "limitbehavior", value, false);
    if (value == "mirror") return SequenceLimitBehavior::Mirror;
    if (value == "clamp") return SequenceLimitBehavior::Clamp;
    return SequenceLimitBehavior::Repeat;
}

auto SequenceIndex(u64 sequence, u64 count, SequenceLimitBehavior behavior) -> u64 {
    if (count <= u64(1)) return u64();
    if (behavior == SequenceLimitBehavior::Clamp) return rstd::cmp::min(sequence, count - u64(1));
    if (behavior == SequenceLimitBehavior::Mirror) {
        auto period = (count - u64(1)) * u64(2);
        auto value  = sequence % period;
        return value < count ? value : period - value;
    }
    return sequence % count;
}

auto SpawnSequence(const particle::ParticleSpawnContext& context) -> u64 {
    return context.storage.Values(context.storage.SlotStateKey())[context.slot.index]
        .spawn_sequence;
}

auto SequenceBasis(const Eigen::Vector3d& axis) -> Eigen::Vector3d {
    if (std::abs(axis.z()) > 0.5) return Eigen::Vector3d { 0.0, 1.0, 0.0 };
    return Eigen::Vector3d { 1.0, 0.0, 0.0 };
}

struct MapSequenceAroundControlPoint {
    i32                   controlpoint {};
    float                 count { 1.0f };
    std::array<float, 2>  bounds { 0.0f, 1.0f };
    std::array<float, 3>  axis { 0.0f, 0.0f, 1.0f };
    std::array<float, 3>  speed_min { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>  speed_max { 0.0f, 0.0f, 0.0f };
    SequenceLimitBehavior limit_behavior { SequenceLimitBehavior::Repeat };

    static auto ReadFromJson(const Json& json) -> MapSequenceAroundControlPoint {
        MapSequenceAroundControlPoint value;
        owe::GetJsonValue(json, "controlpoint", value.controlpoint, false);
        owe::GetJsonValue(json, "count", value.count, false);
        owe::GetJsonValue(json, "bounds", value.bounds, false);
        owe::GetJsonValue(json, "axis", value.axis, false);
        owe::GetJsonValue(json, "speedmin", value.speed_min, false);
        owe::GetJsonValue(json, "speedmax", value.speed_max, false);
        value.limit_behavior = ParseSequenceLimitBehavior(json);
        return value;
    }
};

class WPMapSequenceAroundControlPointProgram {
public:
    WPMapSequenceAroundControlPointProgram(WPParticleAttributes          attributes,
                                           MapSequenceAroundControlPoint config)
        : m_attributes(attributes), m_config(rstd::move(config)) {}

    void Initialize(particle::ParticleSpawnContext& context) {
        auto frame         = WPParticleFrameFrom(context.frame);
        auto value         = MakeWPParticleRef(context.storage, m_attributes, context.slot);
        auto sequence      = SpawnSequence(context);
        auto controlpoints = frame->subsystem->Controlpoints();
        auto controlpoint  = rstd::as_cast<usize>(m_config.controlpoint % i32(8));
        auto center        = controlpoints[controlpoint].offset;

        Eigen::Vector3d axis { Eigen::Vector3f { m_config.axis.data() }.cast<double>() };
        if (axis.squaredNorm() <= 1e-12) axis = Eigen::Vector3d { 0.0, 0.0, 1.0 };
        axis.normalize();
        auto relative = value.position.cast<double>() - center;
        auto parallel = axis * relative.dot(axis);
        auto radius   = (relative - parallel).norm();
        auto basis    = SequenceBasis(axis);
        basis         = (basis - axis * basis.dot(axis)).normalized();
        auto tangent  = basis.cross(axis).normalized();
        auto angle    = kTau64 * static_cast<double>(sequence.to_primitive()) /
                        std::max(1e-6, static_cast<double>(m_config.count));
        angle *= static_cast<double>(m_config.bounds[1] - m_config.bounds[0]);
        angle += kTau64 * static_cast<double>(m_config.bounds[0]);
        value.position =
            (center + parallel + radius * (std::cos(angle) * basis + std::sin(angle) * tangent))
                .cast<float>();

        auto velocity = GenRandomVec3(m_config.speed_min, m_config.speed_max);
        if (velocity.squaredNorm() > 1e-12) {
            value.velocity =
                (value.velocity.cast<double>() + Eigen::AngleAxisd(-angle, axis) * velocity)
                    .cast<float>();
        }
    }

private:
    WPParticleAttributes          m_attributes;
    MapSequenceAroundControlPoint m_config;
};

struct MapSequenceBetweenControlPoints {
    i32                   controlpoint_start {};
    i32                   controlpoint_end { 1 };
    u32                   count { 2 };
    SequenceLimitBehavior limit_behavior { SequenceLimitBehavior::Repeat };

    static auto ReadFromJson(const Json& json) -> MapSequenceBetweenControlPoints {
        MapSequenceBetweenControlPoints value;
        owe::GetJsonValue(json, "controlpointstart", value.controlpoint_start, false);
        owe::GetJsonValue(json, "controlpointend", value.controlpoint_end, false);
        owe::GetJsonValue(json, "count", value.count, false);
        value.count          = rstd::cmp::max(value.count, u32(2));
        value.limit_behavior = ParseSequenceLimitBehavior(json);
        return value;
    }
};

class WPMapSequenceBetweenControlPointsProgram {
public:
    WPMapSequenceBetweenControlPointsProgram(WPParticleAttributes            attributes,
                                             MapSequenceBetweenControlPoints config)
        : m_attributes(attributes), m_config(rstd::move(config)) {}

    void Initialize(particle::ParticleSpawnContext& context) {
        auto frame         = WPParticleFrameFrom(context.frame);
        auto value         = MakeWPParticleRef(context.storage, m_attributes, context.slot);
        auto sequence      = SpawnSequence(context);
        auto controlpoints = frame->subsystem->Controlpoints();
        auto start_index   = rstd::as_cast<usize>(m_config.controlpoint_start % i32(8));
        auto end_index     = rstd::as_cast<usize>(m_config.controlpoint_end % i32(8));
        auto start         = controlpoints[start_index].offset;
        auto end           = controlpoints[end_index].offset;
        auto path          = end - start;
        auto count         = rstd::as_cast<u64>(m_config.count);
        auto index         = SequenceIndex(sequence, count, m_config.limit_behavior);
        auto amount        = static_cast<double>(index.to_primitive()) /
                             static_cast<double>((count - u64(1)).to_primitive());

        Eigen::Vector3d relative = value.position.cast<double>() - start;
        if (path.squaredNorm() > 1e-12) {
            auto direction = path.normalized();
            relative -= direction * relative.dot(direction);
        }
        value.position = (start + amount * path + relative).cast<float>();
    }

private:
    WPParticleAttributes            m_attributes;
    MapSequenceBetweenControlPoints m_config;
};

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

    std::array<float, 3> forward { 0.0f, 1.0f, 0.0f };
    std::array<float, 3> normal { 0.0f, 0.0f, 1.0f };

    static void ReadFromJson(const Json& j, TurbulentRandom& r) {
        owe::GetJsonValue(j, "scale", r.scale, false);
        owe::GetJsonValue(j, "timescale", r.timescale, false);
        owe::GetJsonValue(j, "offset", r.offset, false);
        owe::GetJsonValue(j, "speedmin", r.speedmin, false);
        owe::GetJsonValue(j, "speedmax", r.speedmax, false);
        owe::GetJsonValue(j, "phasemin", r.phasemin, false);
        owe::GetJsonValue(j, "phasemax", r.phasemax, false);
        owe::GetJsonValue(j, "forward", r.forward, false);
        owe::GetJsonValue(j, "right", r.normal, false);
        owe::GetJsonValue(j, "normal", r.normal, false);
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
        if (wpj.get("name"_str).is_none()) break;
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
            TurbulentRandom r;
            TurbulentRandom::ReadFromJson(wpj, r);
            Vector3f normal(r.normal.data());
            if (normal.squaredNorm() <= 1e-8f) normal = Vector3f::UnitZ();
            normal.normalize();

            Vector3f forward(r.forward.data());
            forward -= normal * forward.dot(normal);
            if (forward.squaredNorm() <= 1e-8f) forward = normal.unitOrthogonal();
            forward.normalize();

            Vector3f pos      = GenRandomVec3({ 0, 0, 0 }, { 10.0f, 10.0f, 10.0f }).cast<float>();
            auto     function = [=](WPParticleRef p, f64 duration) mutable {
                float speed = Random::get(r.speedmin, r.speedmax);
                float phase = Random::get(r.phasemin, r.phasemax);
                if (duration > f64(10.0)) {
                    pos[0] += speed;
                    duration = f64();
                }
                Vector3f result;
                do {
                    result =
                        algorism::CurlNoise((pos + normal * phase).cast<double>()).cast<float>();
                    result -= normal * result.dot(normal);
                    if (result.squaredNorm() <= 1e-8f) result = forward;
                    result.normalize();
                    pos += result * 0.005f * static_cast<float>(r.timescale);
                    duration -= f64(0.01);
                } while (duration > f64(0.01));

                auto cosine = std::clamp(result.dot(forward), -1.0f, 1.0f);
                auto angle =
                    static_cast<float>(std::atan2(normal.dot(forward.cross(result)), cosine));
                auto scale = std::max(0.0f, r.scale * 0.5f);
                result     = AngleAxisf(angle * scale + r.offset, normal) * forward;
                result *= speed;
                PM::ChangeVelocity(p, result[0], result[1], result[2]);
            };
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPParticleInitProgram(attributes, rstd::move(function)));
        } else if (name == "mapsequencearoundcontrolpoint") {
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPMapSequenceAroundControlPointProgram(
                    attributes, MapSequenceAroundControlPoint::ReadFromJson(wpj)));
        } else if (name == "mapsequencebetweencontrolpoints") {
            return Box<dyn<particle::ParticleSpawnProgram>>::make(
                WPMapSequenceBetweenControlPointsProgram(
                    attributes, MapSequenceBetweenControlPoints::ReadFromJson(wpj)));
        }
    } while (false);
    return Box<dyn<particle::ParticleSpawnProgram>>::make(
        WPParticleInitProgram(attributes, [](WPParticleRef, f64) {
        }));
}

Box<dyn<particle::ParticleSpawnProgram>>
WPParticleParser::GenOverride(Arc<wpscene::ParticleInstanceoverride> over,
                              WPParticleAttributes                   attributes) {
    auto function = [over = rstd::move(over)](WPParticleRef p, f64) {
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
    std::array<float, 3> startvalue { 1.0f, 1.0f, 1.0f };
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
        infinite_axis               = 0, // 1
        maintain_distance_to_center = 1, // 2
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

    float ringradius {};
    float ringwidth {};
    float ringpulldistance {};

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
        owe::GetJsonValue(j, "ringradius", v.ringradius, false);
        owe::GetJsonValue(j, "ringwidth", v.ringwidth, false);
        owe::GetJsonValue(j, "ringpulldistance", v.ringpulldistance, false);

        return v;
    };
};

struct VortexFrame {
    Vector3d center { Vector3d::Zero() };
    Vector3d axis { Vector3d::UnitZ() };
};

auto ResolveVortexFrame(const Vortex& vortex, slice<WPParticleControlpoint> controlpoints)
    -> VortexFrame {
    const auto& controlpoint = controlpoints[rstd::as_cast<usize>(vortex.controlpoint)];
    Vector3d    local_offset = Vector3f { vortex.offset.data() }.cast<double>();
    Vector3d    axis         = Vector3f { vortex.axis.data() }.cast<double>();
    local_offset             = controlpoint.rotation * local_offset;
    axis                     = controlpoint.rotation * axis;
    if (axis.squaredNorm() <= 1e-12) axis = Vector3d::UnitZ();
    return {
        .center = controlpoint.offset + local_offset,
        .axis   = axis.normalized(),
    };
}

auto VortexSpeedAtDistance(const Vortex& vortex, double distance) -> double {
    auto distance_range = static_cast<double>(vortex.distanceouter - vortex.distanceinner);
    if (distance_range < 0.0 || distance < vortex.distanceinner) return vortex.speedinner;
    if (distance > vortex.distanceouter) return vortex.speedouter;
    auto amount = (distance - vortex.distanceinner) / (distance_range + 0.1);
    return algorism::lerp(amount, vortex.speedinner, vortex.speedouter);
}

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

struct MaintainDistanceState {
    float distance {};
    bool  initialized { false };
};

class MaintainDistanceAttribute {
public:
    using Value = MaintainDistanceState;

    MaintainDistanceAttribute(particle::ParticleAttributeDescriptor descriptor, Value default_value)
        : m_storage(rstd::move(descriptor), rstd::move(default_value)) {}

    auto Descriptor() const -> ref<particle::ParticleAttributeDescriptor> {
        return m_storage.Descriptor();
    }
    auto ConcreteType() const noexcept -> rstd::any::TypeId { return m_storage.ConcreteType(); }
    auto ValueType() const noexcept -> rstd::any::TypeId { return m_storage.ValueTypeId(); }
    auto Len() const noexcept -> usize { return m_storage.Len(); }
    auto Capacity() const noexcept -> usize { return m_storage.Capacity(); }
    void Reserve(usize total_slots) { m_storage.Reserve(total_slots); }
    void AppendDefault() { m_storage.AppendDefault(); }
    void Reset(particle::ParticleSlot slot) { m_storage.Reset(slot); }
    void Clear() { m_storage.Clear(); }
    auto Values() const noexcept -> slice<Value> { return m_storage.Values(); }
    auto ValuesMut() noexcept -> mut_ref<Value[]> { return m_storage.ValuesMut(); }
    auto CloneEmpty() const -> MaintainDistanceAttribute {
        return MaintainDistanceAttribute(m_storage.CloneDescriptor(), m_storage.DefaultValue());
    }

private:
    particle::ParticleValueAttributeStorage<Value> m_storage;
};

struct MaintainDistance {
    i32   controlpoint {};
    float variable_strength { 5.0f };

    static auto ReadFromJson(const Json& json) -> MaintainDistance {
        MaintainDistance value;
        owe::GetJsonValue(json, "controlpoint", value.controlpoint, false);
        owe::GetJsonValue(json, "variablestrength", value.variable_strength, false);
        value.controlpoint %= i32(8);
        return value;
    }
};

auto RegisterMaintainDistanceAttribute(WPParticleSubSystem& subsystem, usize operator_index)
    -> particle::ParticleAttributeKey<MaintainDistanceAttribute> {
    auto name   = std::string("maintain_distance_") + std::to_string(operator_index.to_primitive());
    auto result = subsystem.SchemaBuilder().Register<MaintainDistanceAttribute>(
        rstd::cppstd::as_str(name).unwrap(),
        "we.operator.maintain_distance"_str,
        MaintainDistanceState {});
    if (result.is_err()) rstd::panic { "failed to register maintain distance attribute" };
    auto key = result.unwrap();
    subsystem.RequireAttribute(key);
    return key;
}

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
    auto result = builder.Register<Attribute>(rstd::cppstd::as_str(name).unwrap(),
                                              "we.operator.oscillation"_str,
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
WPParticleParser::GenOperator(const Json& wpj, Arc<wpscene::ParticleInstanceoverride> over_state,
                              WPParticleSubSystem& subsystem, usize operator_index) {
    auto attributes = subsystem.Attributes();
    do {
        if (wpj.get("name"_str).is_none()) break;
        std::string name;
        owe::GetJsonValue(wpj, "name", name);
        if (name == "movement") {
            float drag { 0.0f };

            std::array<float, 3> gravity { 0, 0, 0 };
            owe::GetJsonValue(wpj, "drag", drag, false);
            owe::GetJsonValue(wpj, "gravity", gravity, false);
            Vector3d vecG = Vector3f(gravity.data()).cast<double>();
            auto function = [drag, vecG, over_state = over_state.clone()](WPParticleBatch& info) {
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
                    PM::Move(p, p.velocity.cast<double>() * info.time_pass.to_primitive());
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
                    PM::ChangeRotation(
                        p, p.angular_velocity.cast<double>() * info.time_pass.to_primitive());
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "sizechange") {
            auto vc       = ValueChange::ReadFromJson(wpj);
            auto function = [vc, over_state = over_state.clone()](WPParticleBatch& info) {
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
                auto states = state_keys.ValuesMut(info);
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
                auto states = state_keys.ValuesMut(info);
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
                    state_keys[0].ValuesMut(info),
                    state_keys[1].ValuesMut(info),
                    state_keys[2].ValuesMut(info),
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
                auto  frame  = ResolveVortexFrame(v, info.controlpoints);
                auto& offset = frame.center;
                auto& axis   = frame.axis;

                for (usize index {}; index < info.Len(); ++index) {
                    auto     p        = info.Particle(index);
                    Vector3d relative = p.position.cast<double>() - offset;
                    Vector3d radial   = relative - axis * relative.dot(axis);
                    double   distance = radial.norm();
                    if (distance <= 1e-9) continue;
                    Vector3d direct = -axis.cross(radial).normalized();
                    PM::Accelerate(
                        p, direct * VortexSpeedAtDistance(v, distance) * 0.5, info.time_pass);
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "vortex_v2") {
            Vortex v        = Vortex::ReadFromJson(wpj);
            auto   function = [=](WPParticleBatch& info) {
                auto  frame  = ResolveVortexFrame(v, info.controlpoints);
                auto& offset = frame.center;
                auto& axis   = frame.axis;

                for (usize index {}; index < info.Len(); ++index) {
                    auto     p               = info.Particle(index);
                    Vector3d relative        = p.position.cast<double>() - offset;
                    Vector3d radial          = relative - axis * relative.dot(axis);
                    double   radial_distance = radial.norm();
                    if (radial_distance <= 1e-9) continue;

                    auto distance = v.flags[Vortex::FlagEnum::infinite_axis] ? radial_distance
                                                                             : relative.norm();

                    bool ring_shape =
                        v.ringradius > 0.0f && v.ringwidth > 0.0f && v.ringpulldistance > 0.0f;
                    if (ring_shape) {
                        Vector3d ring_position =
                            radial.normalized() * static_cast<double>(v.ringradius);
                        Vector3d ring_delta    = ring_position - relative;
                        auto     ring_distance = ring_delta.norm();
                        if (ring_distance >= static_cast<double>(v.ringpulldistance)) continue;

                        auto ring_width = static_cast<double>(v.ringwidth);
                        auto pull_range =
                            std::max(static_cast<double>(v.ringpulldistance) - ring_width, 1e-9);
                        double ring_influence {};
                        double pull_influence {};
                        double ring_speed {};
                        if (ring_distance <= ring_width) {
                            auto amount    = ring_distance / ring_width;
                            ring_speed     = algorism::lerp(amount, v.speedinner, v.speedouter);
                            ring_influence = 1.0;
                            pull_influence = amount;
                        } else {
                            pull_influence = (ring_distance - ring_width) / pull_range;
                            ring_influence = std::sqrt(1.0 - pull_influence);
                            ring_speed     = v.speedouter;
                        }

                        auto     ring_strength = ring_speed * ring_influence * 0.5;
                        Vector3d tangent       = -axis.cross(radial).normalized();
                        if (! v.flags[Vortex::FlagEnum::maintain_distance_to_center]) {
                            PM::Accelerate(p, tangent * ring_strength, info.time_pass);
                        } else {
                            Vector3d velocity      = p.velocity.cast<double>();
                            auto     axis_velocity = axis * velocity.dot(axis);
                            auto     tangent_speed = velocity.dot(tangent) +
                                                     ring_strength * info.time_pass.to_primitive();
                            p.velocity = (axis_velocity + tangent * tangent_speed).cast<float>();
                        }
                        auto pull_strength = std::abs(VortexSpeedAtDistance(v, distance)) * 0.5;
                        PM::Accelerate(p,
                                       ring_delta.normalized() * pull_strength * pull_influence,
                                       info.time_pass);
                        continue;
                    }

                    auto strength = VortexSpeedAtDistance(v, distance) * 0.5;
                    if (std::abs(strength) <= 1e-9) continue;

                    Vector3d tangent = -axis.cross(radial).normalized();
                    if (! v.flags[Vortex::FlagEnum::maintain_distance_to_center]) {
                        PM::Accelerate(p, tangent * strength, info.time_pass);
                        continue;
                    }

                    Vector3d velocity      = p.velocity.cast<double>();
                    auto     axis_velocity = axis * velocity.dot(axis);
                    auto     tangent_speed =
                        velocity.dot(tangent) + strength * info.time_pass.to_primitive();
                    p.velocity = (axis_velocity + tangent * tangent_speed).cast<float>();
                }
            };
            return MakeUpdateProgram(attributes, rstd::move(function));
        } else if (name == "maintaindistancetocontrolpoint") {
            auto config    = MaintainDistance::ReadFromJson(wpj);
            auto state_key = RegisterMaintainDistanceAttribute(subsystem, operator_index);
            auto function  = [=](WPParticleBatch& info) {
                auto states = info.ValuesMut(state_key);
                auto center = info.controlpoints[rstd::as_cast<usize>(config.controlpoint)].offset;
                for (usize index {}; index < info.Len(); ++index) {
                    auto     p        = info.Particle(index);
                    auto&    state    = states[index];
                    Vector3d relative = p.position.cast<double>() - center;
                    double   distance = relative.norm();
                    if (distance <= 1e-9) continue;
                    if (! state.initialized) {
                        state.distance    = static_cast<float>(distance);
                        state.initialized = true;
                    }
                    auto direction       = relative / distance;
                    auto radial_velocity = p.velocity.cast<double>().dot(direction);
                    auto target_velocity = (static_cast<double>(state.distance) - distance) *
                                           static_cast<double>(config.variable_strength);
                    PM::ChangeVelocity(p, direction * (target_velocity - radial_velocity));
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
