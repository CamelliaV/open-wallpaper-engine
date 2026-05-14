module;

#include <algorithm>
#include <cmath>

module wescene.scene;
import eigen;
import wescene.core;
import rstd.cppstd;
import wescene.utils;

using namespace owe;

typedef std::function<Particle()> GenParticleOp;
typedef std::function<Particle()> SpwanOp;

namespace
{

inline std::tuple<u32, bool> FindLastParticle(std::span<const Particle> ps, u32 last) {
    for (u32 i = last; i < ps.size(); i++) {
        if (! ParticleModify::LifetimeOk(ps[i])) return { i, true };
    }
    return { 0, false };
}

inline u32 GetEmitNum(double& timer, float speed) {
    double emitDur = 1.0f / speed;
    if (emitDur > timer) return 0;
    u32 num = timer / emitDur;
    while (emitDur < timer) timer -= emitDur;
    if (timer < 0) timer = 0;
    return num;
}

inline u32 Emitt(std::vector<Particle>& particles, u32 num, u32 maxcount, bool sort,
                 SpwanOp Spwan) {
    u32  lastPartcle = 0;
    bool has_dead    = true;
    u32  i           = 0;

    for (i = 0; i < num; i++) {
        if (has_dead) {
            auto [r1, r2] = FindLastParticle(particles, lastPartcle);
            lastPartcle   = r1;
            has_dead      = r2;
        }
        if (has_dead) {
            particles[lastPartcle] = Spwan();

        } else {
            if (maxcount == particles.size()) break;
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

    return i + 1;
}

inline float AudioResponseScale(std::span<const float> audio, const ParticleAudioResponse& ar) {
    if (! ar.enable || audio.empty()) return 1.0f;

    const auto clamp_idx = [audio](float v) {
        auto idx = static_cast<int>(std::round(v));
        idx      = std::max(0, std::min(idx, static_cast<int>(audio.size()) - 1));
        return static_cast<std::size_t>(idx);
    };
    auto first = clamp_idx(ar.frequency[0]);
    auto last  = clamp_idx(ar.frequency[1]);
    if (last < first) std::swap(first, last);

    float sum = 0.0f;
    for (std::size_t i = first; i <= last; ++i) sum += std::max(0.0f, audio[i]);
    float level = sum / static_cast<float>(last - first + 1);

    const float lo = std::min(ar.bounds[0], ar.bounds[1]);
    const float hi = std::max(ar.bounds[0], ar.bounds[1]);
    if (hi > lo) level = (level - lo) / (hi - lo);
    level = std::clamp(level, 0.0f, 1.0f);
    level = std::pow(level, std::max(0.001f, ar.exponent));
    return std::max(0.0f, 1.0f + level * ar.amount);
}

inline Particle Spwan(GenParticleOp gen, std::vector<ParticleInitOp>& inis, double duration) {
    auto particle = gen();
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
} // namespace

ParticleEmittOp ParticleBoxEmitterArgs::MakeEmittOp(ParticleBoxEmitterArgs a) {
    double timer { 0.0f };
    double elapsed { 0.0f };
    return [a, timer, elapsed](std::vector<Particle>&       ps,
                               std::vector<ParticleInitOp>& inis,
                               u32                          maxcount,
                               double                       timepass,
                               std::span<const float>       audio_average) mutable {
        elapsed += timepass;
        if (a.duration > 0.0f && elapsed > a.duration) return;

        timer += timepass;
        auto GenBox = [&]() {
            Eigen::Vector3d pos;
            for (int32_t i = 0; i < 3; i++)
                pos[i] = algorism::lerp(Random::get(-1.0, 1.0), a.minDistance[i], a.maxDistance[i]);
            auto p = Particle();
            pos    = pos.cwiseProduct(Eigen::Vector3f { a.directions.data() }.cast<double>());
            ParticleModify::MoveTo(p, pos);
            ParticleModify::ChangeVelocity(p,
                                           Random::get(a.minSpeed, a.maxSpeed) * pos.normalized());

            ParticleModify::Move(p, a.orgin[0], a.orgin[1], a.orgin[2]);
            return p;
        };
        float emit_speed = a.emitSpeed * AudioResponseScale(audio_average, a.audio_response);
        if (emit_speed <= 0.0f) return;
        u32 emit_num = GetEmitNum(timer, emit_speed);
        emit_num     = a.one_per_frame ? 1 : emit_num;
        emit_num     = a.instantaneous > 0 && ps.empty() ? a.instantaneous : emit_num;
        Emitt(ps, emit_num, maxcount, a.sort, [&]() {
            return Spwan(GenBox, inis, 1.0f / emit_speed);
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
                               std::span<const float>       audio_average) mutable {
        elapsed += timepass;
        if (a.duration > 0.0f && elapsed > a.duration) return;

        timer += timepass;
        auto GenSphere = [&]() {
            auto   p = Particle();
            double r = algorism::lerp(
                std::pow(Random::get(0.0, 1.0), 1.0 / 3.0), a.minDistance, a.maxDistance);
            Eigen::Vector3d sp = r * algorism::GenSphereSurfaceNormal(
                                         [](double u, double o) {
                                             return Random::get<std::normal_distribution<>>(u, o);
                                         },
                                         Eigen::Vector3f { a.directions.data() }.cast<double>());
            ApplySign(sp, a.sign[0], a.sign[1], a.sign[2]);

            ParticleModify::MoveTo(p, sp);
            ParticleModify::ChangeVelocity(p,
                                           Random::get(a.minSpeed, a.maxSpeed) * sp.normalized());

            ParticleModify::Move(p, Eigen::Vector3f { a.orgin.data() }.cast<double>());
            return p;
        };
        float emit_speed = a.emitSpeed * AudioResponseScale(audio_average, a.audio_response);
        if (emit_speed <= 0.0f) return;
        u32 emit_num = GetEmitNum(timer, emit_speed);
        emit_num     = a.one_per_frame ? 1 : emit_num;
        emit_num     = a.instantaneous > 0 && ps.empty() ? a.instantaneous : emit_num;
        Emitt(ps, emit_num, maxcount, a.sort, [&]() {
            return Spwan(GenSphere, inis, 1.0f / emit_speed);
        });
    };
}
