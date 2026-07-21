module wescene.scene;
import eigen;
import wescene.core;
import rstd;
import rstd.cppstd;
import wescene.utils;

using namespace rstd::prelude;
using namespace owe;

typedef std::function<Particle()> GenParticleOp;
typedef std::function<Particle()> SpwanOp;

namespace
{

inline std::tuple<std::size_t, bool> FindLastParticle(std::span<const Particle> ps,
                                                      std::size_t               last) {
    for (std::size_t i = last; i < ps.size(); ++i) {
        if (! ParticleModify::LifetimeOk(ps[i])) return { i, true };
    }
    return { std::size_t(), false };
}

inline u32 GetEmitNum(double& timer, float speed) {
    if (speed <= 0.0f) return u32();
    double emitDur = 1.0f / speed;
    if (emitDur > timer) return u32();
    u32 num(static_cast<rstd::uint32_t>(timer / emitDur));
    while (emitDur < timer) timer -= emitDur;
    if (timer < 0) timer = 0;
    return num;
}

inline double EmitDuration(float speed) noexcept { return speed > 0.0f ? 1.0 / speed : 0.0; }

inline u32 ResolveEmitNum(double& timer, float speed, u32 instantaneous, bool one_per_frame,
                          bool empty) {
    if (instantaneous > u32() && empty) return instantaneous;
    if (speed <= 0.0f) return u32();
    u32 emit_num = GetEmitNum(timer, speed);
    return one_per_frame ? u32(1) : emit_num;
}

inline u32 Emitt(std::vector<Particle>& particles, u32 num, u32 maxcount, bool sort,
                 SpwanOp Spwan) {
    std::size_t lastPartcle = 0;
    bool        has_dead    = true;
    u32         i {};

    for (i = u32(); i < num; ++i) {
        if (has_dead) {
            auto [r1, r2] = FindLastParticle(particles, lastPartcle);
            lastPartcle   = r1;
            has_dead      = r2;
        }
        if (has_dead) {
            particles[lastPartcle] = Spwan();

        } else {
            if (maxcount.to_primitive() == particles.size()) break;
            particles.push_back(Spwan());
        }
    }

    if (sort) {
        // old << new << dead
        std::stable_sort(particles.begin(), particles.end(), [](const auto& a, const auto& b) {
            bool l_a = ParticleModify::LifetimeOk(a);
            bool l_b = ParticleModify::LifetimeOk(b);

            return (l_a && ! l_b) ||
                   (l_a && l_b && ! ParticleModify::IsNew(a) && ParticleModify::IsNew(b));
        });
    }

    return i + u32(1);
}

inline float AudioResponseScale(std::span<const float> audio, const ParticleAudioResponse& ar) {
    if (! ar.enable || audio.empty()) return 1.0f;

    const auto clamp_idx = [audio](float v) {
        auto idx = static_cast<int>(std::round(v));
        idx      = std::max(0, std::min(idx, static_cast<int>(audio.size()) - 1));
        return static_cast<std::size_t>(idx);
    };
    auto first = clamp_idx(ar.frequency[usize()]);
    auto last  = clamp_idx(ar.frequency[usize(1)]);
    if (last < first) std::swap(first, last);

    float sum = 0.0f;
    for (std::size_t i = first; i <= last; ++i) sum += std::max(0.0f, audio[i]);
    float level = sum / static_cast<float>(last - first + 1);

    const float lo = std::min(ar.bounds[usize()], ar.bounds[usize(1)]);
    const float hi = std::max(ar.bounds[usize()], ar.bounds[usize(1)]);
    if (hi > lo) level = (level - lo) / (hi - lo);
    level = std::clamp(level, 0.0f, 1.0f);
    level = std::pow(level, std::max(0.001f, ar.exponent));
    return std::max(0.0f, 1.0f + level * ar.amount);
}

inline Particle Spwan(GenParticleOp gen, std::vector<ParticleInitOp>& inis, double duration) {
    auto particle   = gen();
    particle.random = Random::get(0.0f, 1.0f);
    for (auto& el : inis) el(particle, duration);
    return particle;
}

inline void ApplySign(Eigen::Vector3d& p, int32_t x, int32_t y, int32_t z) noexcept {
    if (x != 0) {
        p.x() = std::abs(p.x()) * (float)x;
    }
    if (y != 0) {
        p.y() = std::abs(p.y()) * (float)y;
    }
    if (z != 0) {
        p.z() = std::abs(p.z()) * (float)z;
    }
}

inline u32 ActiveAxisCount(const Eigen::Vector3d& directions) noexcept {
    rstd::uint32_t count = 0;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(directions[i]) > 1e-6) count++;
    }
    return u32(std::max(1u, count));
}

inline double RandomRadius(double min_distance, double max_distance, u32 dimensions) {
    min_distance = std::max(0.0, min_distance);
    max_distance = std::max(min_distance, max_distance);
    if (dimensions <= u32(1))
        return algorism::lerp(Random::get(0.0, 1.0), min_distance, max_distance);

    double dim = static_cast<double>(dimensions.to_primitive());
    double lo  = std::pow(min_distance, dim);
    double hi  = std::pow(max_distance, dim);
    return std::pow(algorism::lerp(Random::get(0.0, 1.0), lo, hi), 1.0 / dim);
}

inline Eigen::Vector3d RandomDirectedUnit(const Eigen::Vector3d& directions) {
    Eigen::Vector3d unit { 0.0, 0.0, 0.0 };
    for (int retry = 0; retry < 8; ++retry) {
        for (int i = 0; i < 3; ++i) {
            unit[i] = std::abs(directions[i]) > 1e-6
                          ? Random::get<std::normal_distribution<>>(0.0, 1.0)
                          : 0.0;
        }
        double norm = unit.norm();
        if (norm > 1e-6) return unit / norm;
    }
    return { 1.0, 0.0, 0.0 };
}
} // namespace

// Resolve emitter spawn origin in particle-local space. wpe.origin is the
// authored offset; controlpoints[N].offset adds the runtime cp delta (mouse
// follow for link_mouse cps, etc).
inline Eigen::Vector3d ResolveEmitterOrigin(std::span<const ParticleControlpoint> cps, i32 cp_index,
                                            const rstd::array<float, 3>& authored) {
    Eigen::Vector3d o {
        static_cast<double>(authored[usize()]),
        static_cast<double>(authored[usize(1)]),
        static_cast<double>(authored[usize(2)]),
    };
    if (cp_index >= i32() && static_cast<std::size_t>(cp_index.to_primitive()) < cps.size()) {
        o += cps[static_cast<std::size_t>(cp_index.to_primitive())].offset;
    }
    return o;
}

ParticleEmittOp ParticleBoxEmitterArgs::MakeEmittOp(ParticleBoxEmitterArgs a) {
    double timer { 0.0f };
    double elapsed { 0.0f };
    return [a, timer, elapsed](std::vector<Particle>&       ps,
                               std::vector<ParticleInitOp>& inis,
                               u32                          maxcount,
                               double                       timepass,
                               std::span<const float>
                                   audio_average,
                               std::span<const ParticleControlpoint>
                                   cps) mutable {
        elapsed += timepass;
        if (a.duration > 0.0f && elapsed > a.duration) return;

        timer += timepass;
        Eigen::Vector3d origin = ResolveEmitterOrigin(cps, a.controlpoint, a.orgin);
        auto            GenBox = [&]() {
            Eigen::Vector3d pos;
            for (int32_t i = 0; i < 3; ++i)
                pos[i] = algorism::lerp(Random::get(-1.0, 1.0),
                                        a.minDistance[usize(static_cast<std::size_t>(i))],
                                        a.maxDistance[usize(static_cast<std::size_t>(i))]);
            auto p = Particle();
            pos    = pos.cwiseProduct(Eigen::Vector3f { a.directions.data() }.cast<double>());
            ParticleModify::MoveTo(p, pos);
            double speed = Random::get(a.minSpeed, a.maxSpeed);
            if (speed != 0.0 && pos.squaredNorm() > 1e-12)
                ParticleModify::ChangeVelocity(p, speed * pos.normalized());

            ParticleModify::Move(p, origin);
            return p;
        };
        float emit_speed = a.emitSpeed * AudioResponseScale(audio_average, a.audio_response);
        u32   emit_num =
            ResolveEmitNum(timer, emit_speed, a.instantaneous, a.one_per_frame, ps.empty());
        if (emit_num == u32()) return;
        Emitt(ps, emit_num, maxcount, a.sort, [&]() {
            return Spwan(GenBox, inis, EmitDuration(emit_speed));
        });
    };
}

ParticleEmittOp ParticleSphereEmitterArgs::MakeEmittOp(ParticleSphereEmitterArgs a) {
    using namespace Eigen;
    double timer { 0.0f };
    double elapsed { 0.0f };
    return [a, timer, elapsed](std::vector<Particle>&       ps,
                               std::vector<ParticleInitOp>& inis,
                               u32                          maxcount,
                               double                       timepass,
                               std::span<const float>
                                   audio_average,
                               std::span<const ParticleControlpoint>
                                   cps) mutable {
        elapsed += timepass;
        if (a.duration > 0.0f && elapsed > a.duration) return;

        timer += timepass;
        Eigen::Vector3d origin     = ResolveEmitterOrigin(cps, a.controlpoint, a.orgin);
        Eigen::Vector3d directions = Eigen::Vector3f { a.directions.data() }.cast<double>();
        u32             dimensions = ActiveAxisCount(directions);
        auto            GenSphere  = [&]() {
            auto            p    = Particle();
            double          r    = RandomRadius(a.minDistance, a.maxDistance, dimensions);
            Eigen::Vector3d unit = RandomDirectedUnit(directions);
            Eigen::Vector3d sp   = r * unit.cwiseProduct(directions.cwiseAbs());
            ApplySign(sp,
                      a.sign[usize()].to_primitive(),
                      a.sign[usize(1)].to_primitive(),
                      a.sign[usize(2)].to_primitive());

            ParticleModify::MoveTo(p, sp);
            double speed = Random::get(a.minSpeed, a.maxSpeed);
            if (speed != 0.0 && sp.squaredNorm() > 1e-12)
                ParticleModify::ChangeVelocity(p, speed * sp.normalized());

            ParticleModify::Move(p, origin);
            return p;
        };
        float emit_speed = a.emitSpeed * AudioResponseScale(audio_average, a.audio_response);
        u32   emit_num =
            ResolveEmitNum(timer, emit_speed, a.instantaneous, a.one_per_frame, ps.empty());
        if (emit_num == u32()) return;
        Emitt(ps, emit_num, maxcount, a.sort, [&]() {
            return Spwan(GenSphere, inis, EmitDuration(emit_speed));
        });
    };
}
