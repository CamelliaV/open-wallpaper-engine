module;

#include <rstd/macro.hpp>


module wescene.scene;
import eigen;
import wescene.spec_texs;
import wescene.core;
import rstd.log;
import rstd.cppstd;

using namespace owe;
using namespace Eigen;

struct WPGOption {
    bool thick_format { false };
    bool geometry_shader { false };
};

namespace
{
inline void AssignVertexTimes(std::span<float> dst, std::span<const float> src, unsigned num) noexcept {
    const unsigned dst_one_size = dst.size() / num;
    for (unsigned i = 0; i < num; i++) {
        std::copy(src.begin(), src.end(), dst.begin() + i * dst_one_size);
    }
}

inline void AssignVertex(std::span<float> dst, std::span<const float> src, unsigned num) noexcept {
    const unsigned dst_one_size = dst.size() / num;
    const unsigned src_one_size = src.size() / num;
    for (unsigned i = 0; i < num; i++) {
        std::copy_n(src.begin() + i * src_one_size, src_one_size, dst.begin() + i * dst_one_size);
    }
}

inline usize GenParticleData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                             const ParticleRawGenSpecOp& specOp, WPGOption opt,
                             SceneVertexArray& sv) noexcept {
    std::array<float, 32 * 4> storage;

    float* data = storage.data();

    const auto one_size   = sv.OneSize();
    const auto totle_size = 4 * one_size;
    usize      i { 0 };
    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;

        for (const auto& p : inst->Particles()) {
            if (! ParticleModify::LifetimeOk(p)) {
                continue;
            }

            float lifetime = p.lifetime;
            specOp(p, { &lifetime });

            auto  pos  = inst->GetBoundedData().pos + p.position;
            float size = p.size / 2.0f;

            usize offset = 0;

            // pos
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { pos[0], pos[1], pos[2] }, 4);
            offset += 4;
            // TexCoordVec4
            float      rz = p.rotation[2];
            std::array t { 0.0f, 1.0f, rz, size, 1.0f, 1.0f, rz, size,
                           1.0f, 0.0f, rz, size, 0.0f, 0.0f, rz, size };
            AssignVertex({ data + offset, totle_size }, t, 4);
            offset += 4;

            // color
            AssignVertexTimes({ data + offset, totle_size },
                              std::array { p.color[0], p.color[1], p.color[2], p.alpha },
                              4);
            offset += 4;

            if (opt.thick_format) {
                AssignVertexTimes(
                    { data + offset, totle_size },
                    std::array { p.velocity[0], p.velocity[1], p.velocity[2], lifetime },
                    4);
                offset += 4;
            }
            // TexCoordC2
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { p.rotation[0], p.rotation[1] }, 4);

            sv.SetVertexs((i++) * 4, { data, totle_size });
        }
    }
    return i;
}

// Emit one VS-input vertex per consecutive pair of trail-history samples for a
// single live rope-head particle. Each emitted vertex carries the segment's
// endpoints + Catmull-Rom neighbour positions as splineCP0/CP1 (the vert shader
// derives the GS-side tangents from them). Returns the number of segment
// vertices emitted; vertices land at [base_index, base_index+ret).
inline size_t GenRopeParticleSegments(const Particle& p, const ParticleTrail& trail,
                                      const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                      SceneVertexArray& sv, size_t base_index) {
    const auto one_size = sv.OneSize();
    std::array<float, 32> v {};
    size_t emitted = 0;

    if (trail.len < 2) return 0;

    float size     = p.size / 2.0f;
    float lifetime = p.lifetime;
    specOp(p, { &lifetime });

    const float in_ParticleTrailLength = (float)trail.len;

    // trail.At(0) = oldest, At(len-1) = newest. Segments connect (j-1) -> j.
    for (uint16_t j = 1; j < trail.len; j++) {
        Vector3f pre_pos = trail.At((uint16_t)(j - 1));
        Vector3f cur_pos = trail.At(j);
        // Catmull-Rom neighbour samples for the cubic-Bezier subdivision; the
        // GS hardcoded 0.15 factor matches Catmull-Rom tension 0.5 (theoretical
        // 1/6 ≈ 0.167). At the trail endpoints fall back to the segment ends
        // so those boundary spans render flat.
        Vector3f scp = (j >= 2) ? trail.At((uint16_t)(j - 2)) : pre_pos;
        Vector3f ecp = (j + 1 < trail.len) ? trail.At((uint16_t)(j + 1)) : cur_pos;

        const float in_ParticleTrailPosition = (float)(j - 1);

        size_t off = 0;
        v[off++] = pre_pos[0];
        v[off++] = pre_pos[1];
        v[off++] = pre_pos[2];
        v[off++] = size;
        v[off++] = cur_pos[0];
        v[off++] = cur_pos[1];
        v[off++] = cur_pos[2];
        v[off++] = in_ParticleTrailLength;
        v[off++] = scp[0];
        v[off++] = scp[1];
        v[off++] = scp[2];
        v[off++] = in_ParticleTrailPosition;
        if (opt.thick_format) {
            v[off++] = ecp[0];
            v[off++] = ecp[1];
            v[off++] = ecp[2];
            v[off++] = size;
            v[off++] = p.color[0];
            v[off++] = p.color[1];
            v[off++] = p.color[2];
            v[off++] = p.alpha;
        } else {
            v[off++] = ecp[0];
            v[off++] = ecp[1];
            v[off++] = ecp[2];
            v[off++] = 0.0f;
        }
        v[off++] = p.color[0];
        v[off++] = p.color[1];
        v[off++] = p.color[2];
        v[off++] = p.alpha;

        rstd_assert(off == one_size);
        sv.SetVertexs(base_index + emitted, { v.data(), one_size });
        emitted++;
    }
    return emitted;
}

inline size_t GenRopeParticleData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                  const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                  SceneVertexArray& sv) {
    size_t total = 0;
    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;
        auto particles = inst->Particles();
        auto trails    = inst->Trails();
        const size_t n = std::min(particles.size(), trails.size());
        for (size_t si = 0; si < n; si++) {
            if (! ParticleModify::LifetimeOk(particles[si])) continue;
            total += GenRopeParticleSegments(
                particles[si], trails[si], specOp, opt, sv, total);
        }
    }
    return total;
}

inline void updateIndexArray(uint32_t index, size_t count, SceneIndexArray& iarray) noexcept {
    constexpr size_t single_size = 6;
    uint32_t         cv          = index * 4;

    std::array<uint32_t, single_size> single;
    // 0 1 3
    // 1 2 3
    single[0] = cv;
    single[1] = cv + 1;
    single[2] = cv + 3;
    single[3] = cv + 1;
    single[4] = cv + 2;
    single[5] = cv + 3;
    // every particle
    for (uint32_t i = index; i < count; i++) {
        iarray.Assign(i * single_size, single);
        for (auto& x : single) x += 4;
    }
}
} // namespace

void WPParticleRawGener::GenGLData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                   SceneMesh& mesh, ParticleRawGenSpecOp& specOp) {
    auto& sv = mesh.GetVertexArray(0);

    WPGOption opt;
    opt.thick_format = sv.GetOption(WE_CB_THICK_FORMAT);

    if (sv.GetOption(WE_PRENDER_ROPE)) {
        // Rope/spline: one vertex per segment, drawn as POINT_LIST, expanded
        // by the geometry shader. No index buffer (SetRopeParticleMesh skips
        // index allocation). Reset the active size before regen so the
        // segment count for this frame isn't masked by a previous frame's
        // high-water mark.
        sv.ResetSize();
        GenRopeParticleData(instances, specOp, opt, sv);
        return;
    }

    usize particle_num = GenParticleData(instances, specOp, opt, sv);

    auto& si       = mesh.GetIndexArray(0);
    u32   indexNum = (u32)(si.DataCount() / 6);
    if (particle_num > indexNum) {
        updateIndexArray(indexNum, particle_num, si);
    }
    si.SetRenderDataCount(particle_num * 6);
}
