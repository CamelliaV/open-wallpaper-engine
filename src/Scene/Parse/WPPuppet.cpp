module;

#include <rstd/macro.hpp>

module wescene.puppet;
import eigen;
import wescene.core;
import rstd.cppstd;

using namespace owe;
using namespace Eigen;

static Quaterniond ToQuaternion(Vector3f euler) {
    const std::array<Vector3d, 3> axis { Vector3d::UnitX(), Vector3d::UnitY(), Vector3d::UnitZ() };
    return AngleAxis<double>(euler.z(), axis[2]) * AngleAxis<double>(euler.y(), axis[1]) *
           AngleAxis<double>(euler.x(), axis[0]);
};

void WPPuppet::prepared() {
    for (unsigned i = 0; i < bones.size(); i++) {
        auto& b = bones[i];
        rstd_assert(b.bind_parent < i || b.noBindParent());
        b.world_bind =
            (b.noBindParent() ? Affine3f::Identity() : bones[b.bind_parent].world_bind) *
            b.local_bind;
        b.inv_bind = b.world_bind.inverse();
    }
    for (auto& anim : anims) {
        anim.frame_time = 1.0f / anim.fps;
        anim.max_time   = anim.length / anim.fps;
        for (auto& t : anim.bone_tracks) {
            for (auto& f : t.frames) {
                f.quaternion = ToQuaternion(f.angle);
            }
        }
    }

    m_final_affines.resize(bones.size());
}

std::span<const Eigen::Affine3f> WPPuppet::genFrame(WPPuppetLayer& puppet_layer,
                                                    double         time) noexcept {
    double global_blend = puppet_layer.m_global_blend;

    puppet_layer.updateInterpolation(time);

    // Re-enable TRS skinning. WE's official DXBC capture shows pure-translation
    // g_Bones, but that's a snapshot — at blink frames (frame.scale.y → 0.02)
    // WE must temporarily upload TRS matrices to produce the visible squash
    // effect. Pure-translation can only shift a bone's whole sprite as a unit
    // (no shape change); compression requires non-identity row1 so vertices
    // within the sprite get differential treatment. LBS triangle stretching at
    // bone boundaries is the unavoidable cost.
    for (unsigned i = 0; i < m_final_affines.size(); i++) {
        const auto& bone   = bones[i];
        auto&       affine = m_final_affines[i];

        affine = Affine3f::Identity();
        rstd_assert(bone.anim_parent < i || bone.noAnimParent());
        const Affine3f parent =
            bone.noAnimParent() ? Affine3f::Identity() : m_final_affines[bone.anim_parent];

        Vector3f    trans { bone.local_bind.translation() * global_blend };
        Vector3f    scale { Vector3f::Ones() * global_blend };
        Quaterniond quat { Quaterniond::Identity() };
        Quaterniond ident { Quaterniond::Identity() };

        for (auto& layer : puppet_layer.m_layers) {
            auto& alayer = layer.anim_layer;
            if (layer.anim == nullptr || ! alayer.visible) continue;
            if (i >= layer.anim->bone_tracks.size()) continue;

            auto& info       = layer.interp_info;
            auto& track      = layer.anim->bone_tracks[i];
            auto& frame_base = track.frames[(usize)0];
            auto& frame_a    = track.frames[(usize)info.frame_a];
            auto& frame_b    = track.frames[(usize)info.frame_b];

            double t     = info.t;
            double one_t = 1.0f - info.t;

            auto frame_a_quat_delta = frame_a.quaternion * frame_base.quaternion.conjugate();
            auto frame_b_quat_delta = frame_b.quaternion * frame_base.quaternion.conjugate();
            auto pos_a_delta   = frame_a.position - frame_base.position;
            auto pos_b_delta   = frame_b.position - frame_base.position;
            auto scale_a_delta = frame_a.scale - frame_base.scale;
            auto scale_b_delta = frame_b.scale - frame_base.scale;

            if (alayer.additive) {
                // Additive: only contribute the per-frame delta from the
                // anim's own neutral pose (frame[0]). The replace-layer
                // base translation/scale/rotation is untouched.
                trans += alayer.blend * (pos_a_delta * one_t + pos_b_delta * t);
                scale += alayer.blend * (scale_a_delta * one_t + scale_b_delta * t);
                quat *= frame_a_quat_delta.slerp(t, frame_b_quat_delta)
                            .slerp(1.0 - alayer.blend, ident);
            } else {
                quat *= frame_a_quat_delta.slerp(t, frame_b_quat_delta).slerp(
                            1.0 - alayer.blend, ident) *
                        frame_base.quaternion.slerp(1.0 - layer.blend, ident);
                trans += (layer.blend * frame_base.position) +
                         (alayer.blend * (pos_a_delta * one_t + pos_b_delta * t));
                scale += (layer.blend * frame_base.scale) +
                         (alayer.blend * (scale_a_delta * one_t + scale_b_delta * t));
            }
        }
        // V21 sprites scale around their vertex centroid (puppet-world), not
        // around bone.local_bind.t. The file stores bone.local_bind.t at an
        // anchor offset from the actual sprite; vertices weighted to bone i
        // are clustered at bind.t + centroid_offset. Compose T(centroid) * R*S
        // * T(-centroid) by adding centroid_offset on the pretranslate side
        // and subtracting it after inv_bind. For older MDL versions the offset
        // is zero, no-op.
        //
        // Also conjugate the anim transform by the bone's bind rotation: WE
        // applies scale/angle in the sprite's bind-local frame. We extract R_bind
        // from local_bind.linear() and insert it before R_anim; inv_bind already
        // contains R_bind^-1 on the right side, so the form becomes
        // T(eff) * R_bind * R_anim * S_anim * R_bind^-1 * T(-bind.t - c).
        // For bones with no bind rotation this is a no-op.
        const Matrix3f R_bind = bones[i].local_bind.linear();
        Vector3f effective_trans = trans + bones[i].vertex_centroid_offset;
        affine.pretranslate(effective_trans);
        affine.rotate(R_bind);
        affine.rotate(quat.slerp(global_blend, ident).cast<float>());
        affine.scale(scale);
        affine = parent * affine;
    }

    for (unsigned i = 0; i < m_final_affines.size(); i++) {
        m_final_affines[i] *= bones[i].inv_bind.matrix();
        m_final_affines[i].translate(-bones[i].vertex_centroid_offset);
    }
    return m_final_affines;
}

static constexpr void genInterpolationInfo(WPPuppet::Animation::InterpolationInfo& info,
                                           double& cur, u32 length, double frame_time,
                                           double max_time) {
    cur          = std::fmod(cur, max_time);
    double _rate = cur / frame_time;

    info.frame_a = ((unsigned)_rate) % length;
    info.frame_b = (info.frame_a + 1) % length;
    info.t       = _rate - (double)info.frame_a;
}

WPPuppet::Animation::InterpolationInfo
WPPuppet::Animation::getInterpolationInfo(double* cur_time) const {
    InterpolationInfo _info;
    auto&             _cur_time = *cur_time;

    if (mode == PlayMode::Loop || mode == PlayMode::Single) {
        genInterpolationInfo(_info, _cur_time, (u32)length, frame_time, max_time);
    } else if (mode == PlayMode::Mirror) {
        const auto _get_frame = [this](auto f) {
            return f >= length ? (length - 1) - (f - length) : f;
        };
        genInterpolationInfo(_info, _cur_time, (u32)length * 2, frame_time, max_time * 2.0f);
        _info.frame_a = _get_frame(_info.frame_a);
        _info.frame_b = _get_frame(_info.frame_b);
    }

    return _info;
}

void WPPuppetLayer::prepared(std::span<AnimationLayer> alayers) {
    m_layers.resize(alayers.size());
    double& blend = m_global_blend;
    double& total_blend = m_total_blend;

    // Only REPLACE layers (additive=false) contribute to the total_blend
    // normalization — additive layers don't compete for the replace slot.
    // Skip layers whose animation isn't actually in the puppet so missing
    // editor-residue refs don't dilute the survivors.
    const auto& anims = m_puppet->anims;
    total_blend = 0.0;
    for (int i = 0; i < alayers.size(); i++) {
        if (! alayers[i].visible || alayers[i].additive) continue;
        bool exists = std::any_of(anims.begin(), anims.end(),
                                  [&](const auto& a) { return a.id == alayers[i].id; });
        if (exists) total_blend += alayers[i].blend;
    }

    std::transform(
        alayers.rbegin(), alayers.rend(), m_layers.rbegin(), [&blend, this](const auto& layer) {
            double      cur_blend { 0.0f };
            const auto& anims = m_puppet->anims;

            auto it = std::find_if(anims.begin(), anims.end(), [&layer](auto& a) {
                return layer.id == a.id;
            });
            bool ok = it != anims.end() && layer.visible;

            double &total_blend = m_total_blend;

            if (ok) {
                if (layer.additive) {
                    // Additive layers carry their scene.json blend unchanged;
                    // genFrame consumes it as a delta scale, not a normalized
                    // replace weight.
                    cur_blend = layer.blend;
                }
                else if (total_blend > 1.0)
                {
                    cur_blend = layer.blend / total_blend;
                    blend = 0.0;
                }
                else
                {
                    cur_blend = blend * layer.blend;
                    blend *= 1.0f - layer.blend;
                    blend = blend < 0.0f ? 0.0f : blend;
                }
            }

            return Layer {
                .anim_layer = layer,
                .blend      = cur_blend,
                .anim       = ok ? std::addressof(*it) : nullptr,
            };
        });
}

std::span<const Eigen::Affine3f> WPPuppetLayer::genFrame(double time) noexcept {
    return m_puppet->genFrame(*this, time);
}

void WPPuppetLayer::updateInterpolation(double time) noexcept {
    for (auto& layer : m_layers) {
        if (layer) {
            layer.anim_layer.cur_time += time * layer.anim_layer.rate;
            layer.interp_info = layer.anim->getInterpolationInfo(&(layer.anim_layer.cur_time));
        }
    }
}

WPPuppetLayer::WPPuppetLayer(std::shared_ptr<WPPuppet> pup): m_puppet(pup) {}
WPPuppetLayer::WPPuppetLayer()  = default;
WPPuppetLayer::~WPPuppetLayer() = default;
