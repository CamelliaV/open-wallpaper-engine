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

// Emit one vertex per consecutive trail segment for a single particle history
// (one ParticleInstance worth). The geometry shader expands each point into a
// strip via cubic Bezier (see genericropeparticle.geom). Returns the number of
// segment vertices emitted; vertices land at [base_index, base_index+ret).
inline size_t GenRopeParticleDataOne(std::span<const Particle>   particles,
                                     const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                     SceneVertexArray& sv, size_t base_index) {
    const auto one_size = sv.OneSize();
    std::array<float, 32> v {};
    size_t emitted = 0;

    for (size_t i = 1; i < particles.size(); i++) {
        const auto& p     = particles[i];
        const auto& pre_p = particles[i - 1];
        if (! ParticleModify::LifetimeOk(p)) break;
        if (! ParticleModify::LifetimeOk(pre_p)) continue;

        float size     = p.size / 2.0f;
        float lifetime = p.lifetime;
        specOp(p, { &lifetime });

        const float in_ParticleTrailLength   = (float)particles.size();
        const float in_ParticleTrailPosition = (float)(i - 1);

        Vector3f cp_vec  = AngleAxisf(p.rotation[2] + rstd::f32_::consts::FRAC_PI_2,
                                      Vector3f::UnitZ()) *
                          Vector3f { 0.0f, size / 2.0f, 0.0f };
        Vector3f pos_vec = Vector3f { p.position } - Vector3f { pre_p.position };
        cp_vec = pos_vec.normalized().dot(cp_vec) > 0 ? cp_vec : -1.0f * cp_vec;

        Vector3f scp = Vector3f { pre_p.position } + cp_vec;
        Vector3f ecp = Vector3f { p.position } - cp_vec;

        size_t off = 0;
        // a_PositionVec4: (startPos, sizeStart)
        v[off++] = pre_p.position[0];
        v[off++] = pre_p.position[1];
        v[off++] = pre_p.position[2];
        v[off++] = size;
        // a_TexCoordVec4: (endPos, trailLength)
        v[off++] = p.position[0];
        v[off++] = p.position[1];
        v[off++] = p.position[2];
        v[off++] = in_ParticleTrailLength;
        // a_TexCoordVec4C1: (CPStart, trailPosition)
        v[off++] = scp[0];
        v[off++] = scp[1];
        v[off++] = scp[2];
        v[off++] = in_ParticleTrailPosition;
        if (opt.thick_format) {
            // a_TexCoordVec4C2: (CPEnd, sizeEnd)
            v[off++] = ecp[0];
            v[off++] = ecp[1];
            v[off++] = ecp[2];
            v[off++] = size;
            // a_TexCoordVec4C3: end color
            v[off++] = p.color[0];
            v[off++] = p.color[1];
            v[off++] = p.color[2];
            v[off++] = p.alpha;
        } else {
            // a_TexCoordVec3C2: CPEnd
            v[off++] = ecp[0];
            v[off++] = ecp[1];
            v[off++] = ecp[2];
        }
        // a_Color: start color
        v[off++] = pre_p.color[0];
        v[off++] = pre_p.color[1];
        v[off++] = pre_p.color[2];
        v[off++] = pre_p.alpha;

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
        total +=
            GenRopeParticleDataOne(inst->Particles(), specOp, opt, sv, total);
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
