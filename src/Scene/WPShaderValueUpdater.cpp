module;

#include <rstd/macro.hpp>

#include <cmath>

module wescene.shader_value_updater;
import eigen;
import wescene.spec_texs;
import wescene.core;
import rstd.cppstd;
import wescene.utils;
import wescene.scene;

using namespace owe;
using namespace Eigen;

namespace
{

// Music energy is concentrated in low frequencies (bass/kick fundamentals).
// Without compensation the leftmost bars dominate and the rest barely move.
// Apply a per-wavsen-band gain so a flat pink-spectrum input lands roughly
// equal across bands. Layout must match wavsen's audio_capture.cpp::kBandEdges
// (FFT bins 1..512 over a 1024-pt FFT @ 48 kHz, with +1 collision floor).
constexpr float kAudioSampleRate  = 48000.0f;
constexpr float kAudioFftSize     = 1024.0f;
constexpr float kAudioHalfFft     = 512.0f;
constexpr float kAudioTiltPivotHz = 200.0f;
constexpr float kAudioTiltExp     = 0.30f; // ~ +1.8 dB/oct, slightly past pink

const std::array<float, 64> kAudioBandGain = [] {
    std::array<std::size_t, 65> edges {};
    edges[0] = 1;
    for (std::size_t k = 1; k <= 64; ++k) {
        const double t =
            std::pow(static_cast<double>(kAudioHalfFft), static_cast<double>(k) / 64.0);
        auto next = static_cast<std::size_t>(std::ceil(t));
        if (next <= edges[k - 1]) next = edges[k - 1] + 1;
        const auto cap = static_cast<std::size_t>(kAudioHalfFft);
        if (next > cap) next = cap;
        edges[k] = next;
    }
    std::array<float, 64> gain {};
    for (std::size_t k = 0; k < 64; ++k) {
        const float center_bin = 0.5f * (float(edges[k]) + float(edges[k + 1]));
        const float center_hz  = center_bin * kAudioSampleRate / kAudioFftSize;
        gain[k]                = std::pow(center_hz / kAudioTiltPivotHz, kAudioTiltExp);
    }
    return gain;
}();

// Asymmetric EMA in the visual domain: fast attack so transients pop,
// slow release so bars decay smoothly instead of flickering. Time constants
// (not per-frame alphas) so the feel is the same at 30 / 60 / 144 fps.
constexpr float kAudioAttackTauSec  = 0.020f;
constexpr float kAudioReleaseTauSec = 0.180f;

void UpdateVisualBands(std::span<const float, 64> bins, unsigned out_n, float dt_sec,
                       std::span<float> state) {
    const unsigned ratio = 64u / out_n;
    for (unsigned i = 0; i < out_n; ++i) {
        // Peak (not mean) preserves transients across the consolidated bands.
        float peak = 0.0f;
        for (unsigned k = 0; k < ratio; ++k) {
            const unsigned src = i * ratio + k;
            const float    v   = bins[src] * kAudioBandGain[src];
            if (v > peak) peak = v;
        }
        // Tanh re-compresses past-1 values that the high-freq boost can produce.
        peak             = std::tanh(peak);
        const float prev = state[i];
        if (dt_sec <= 0.0f) {
            // First frame (no valid frametime yet) — adopt raw immediately.
            state[i] = peak;
        } else {
            const float tau = peak > prev ? kAudioAttackTauSec : kAudioReleaseTauSec;
            const float a   = 1.0f - std::exp(-dt_sec / tau);
            state[i]        = prev + a * (peak - prev);
        }
    }
}

} // namespace

void WPShaderValueUpdater::FrameBegin() {
    /*
        using namespace std::chrono;
        auto nowTime = system_clock::to_time_t(system_clock::now());
        auto cTime   = std::localtime(&nowTime);
        m_dayTime =
            (((cTime->tm_hour * 60) + cTime->tm_min) * 60 + cTime->tm_sec) / (24.0f * 60.0f
       * 60.0f);
    */
    double new_time    = m_mouseDelayedTime + m_scene->frameTime;
    new_time           = new_time > m_parallax.delay ? m_parallax.delay : new_time;
    m_mouseDelayedTime = new_time;
    // Guard against parallax.delay == 0: scenes with cameraparallaxdelay=0
    // would otherwise produce 0/0 = NaN here, propagating through the MVP
    // and disappearing the wallpaper entirely (issue: gray-screen render).
    double t   = m_parallax.delay > 0.0 ? new_time / m_parallax.delay : 1.0;
    m_mousePos = std::array { (float)algorism::lerp(t, m_mousePos[0], m_mousePosInput[0]),
                              (float)algorism::lerp(t, m_mousePos[1], m_mousePosInput[1]) };
}

void WPShaderValueUpdater::FrameEnd() {}

void WPShaderValueUpdater::MouseInput(double x, double y) {
    using namespace std::chrono;

    auto   now_time = steady_clock::now();
    double new_time = m_mouseDelayedTime -
                      duration_cast<duration<double>>(now_time - m_last_mouse_input_time).count();
    m_mouseDelayedTime = new_time < 0.0f ? 0.0f : new_time;

    m_mousePosInput[0] = (float)x;
    m_mousePosInput[1] = (float)y;

    m_last_mouse_input_time = now_time;
}

void WPShaderValueUpdater::InitUniforms(SceneNode* pNode, const ExistsUniformOp& existsOp) {
    m_nodeUniformInfoMap[pNode] = WPUniformInfo();
    auto& info                  = m_nodeUniformInfoMap[pNode];
    info.has_MI                 = existsOp(G_MI);
    info.has_M                  = existsOp(G_M);
    info.has_AM                 = existsOp(G_AM);
    info.has_MVP                = existsOp(G_MVP);
    info.has_MVPI               = existsOp(G_MVPI);
    info.has_ETVP               = existsOp(G_ETVP);
    info.has_ETVPI              = existsOp(G_ETVPI);

    info.has_VP = existsOp(G_VP);

    info.has_BONES            = existsOp(G_BONES);
    info.has_TIME             = existsOp(G_TIME);
    info.has_DAYTIME          = existsOp(G_DAYTIME);
    info.has_POINTERPOSITION  = existsOp(G_POINTERPOSITION);
    info.has_PARALLAXPOSITION = existsOp(G_PARALLAXPOSITION);
    info.has_TEXELSIZE        = existsOp(G_TEXELSIZE);
    info.has_TEXELSIZEHALF    = existsOp(G_TEXELSIZEHALF);
    info.has_SCREEN           = existsOp(G_SCREEN);
    info.has_LP               = existsOp(G_LP);
    info.has_audio_16_l       = existsOp(G_AUDIO_SPEC_16_L);
    info.has_audio_16_r       = existsOp(G_AUDIO_SPEC_16_R);
    info.has_audio_32_l       = existsOp(G_AUDIO_SPEC_32_L);
    info.has_audio_32_r       = existsOp(G_AUDIO_SPEC_32_R);
    info.has_audio_64_l       = existsOp(G_AUDIO_SPEC_64_L);
    info.has_audio_64_r       = existsOp(G_AUDIO_SPEC_64_R);

    std::accumulate(begin(info.texs), end(info.texs), 0, [&existsOp](unsigned index, auto& value) {
        value.has_resolution = existsOp(WE_GLTEX_RESOLUTION_NAMES[index]);
        value.has_mipmap     = existsOp(WE_GLTEX_MIPMAPINFO_NAMES[index]);
        return index + 1;
    });
}

void WPShaderValueUpdater::UpdateUniforms(SceneNode* pNode, sprite_map_t& sprites,
                                          const UpdateUniformOp& updateOp) {
    if (! pNode->Mesh()) return;

    pNode->UpdateTrans();

    SceneCamera*     camera;
    std::string_view cam_name = pNode->Camera();
    if (! pNode->Camera().empty()) {
        camera = m_scene->cameras.at(cam_name.data()).get();
    } else
        camera = m_scene->activeCamera;

    if (! camera) return;

    auto* material = pNode->Mesh()->Material();
    if (! material) return;
    // auto& shadervs = material->customShader.updateValueList;
    // const auto& valueSet = material->customShader.valueSet;

    rstd_assert(exists(m_nodeUniformInfoMap, pNode));
    const auto& info = m_nodeUniformInfoMap[pNode];

    bool hasNodeData = exists(m_nodeDataMap, pNode);
    if (hasNodeData) {
        auto& nodeData = m_nodeDataMap.at(pNode);
        for (const auto& el : nodeData.renderTargets) {
            if (m_scene->renderTargets.count(el.second) == 0) continue;
            const auto& rt = m_scene->renderTargets[el.second];

            const auto& unifrom_tex = info.texs[el.first];

            if (unifrom_tex.has_resolution) {
                std::array<i32, 4> resolution_uint({ rt.width, rt.height, rt.width, rt.height });
                updateOp(WE_GLTEX_RESOLUTION_NAMES[el.first],
                         ShaderValue(array_cast<float>(resolution_uint)));
            }
            if (unifrom_tex.has_mipmap) {
                updateOp(WE_GLTEX_MIPMAPINFO_NAMES[el.first], (float)rt.mipmap_level);
            }
        }
        if (nodeData.puppet_layer.hasPuppet() && info.has_BONES) {
            auto data = nodeData.puppet_layer.genFrame(m_scene->elapsingTime);
            updateOp(G_BONES, std::span<const float> { data[0].data(), data.size() * 16 });
        }
    }

    bool reqMI    = info.has_MI;
    bool reqM     = info.has_M;
    bool reqAM    = info.has_AM;
    bool reqMVP   = info.has_MVP;
    bool reqMVPI  = info.has_MVPI;
    bool reqETVP  = info.has_ETVP;
    bool reqETVPI = info.has_ETVPI;

    Matrix4d viewProTrans = camera->GetViewProjectionMatrix();

    if (info.has_VP) {
        updateOp(G_VP, ShaderValue::fromMatrix(viewProTrans));
    }
    if (reqM || reqMVP || reqMI || reqMVPI) {
        Matrix4d modelTrans = pNode->ModelTrans();
        if (hasNodeData && cam_name != "effect") {
            const auto& nodeData = m_nodeDataMap.at(pNode);
            if (m_parallax.enable) {
                // World position, not local. Image-effect composite nodes
                // inherit transform via SetParentAnchor and keep identity
                // local trans — Translate() would return (0,0) and put the
                // parallax shift around canvas origin instead of the layer's
                // actual world position.
                Vector3f nodePos = modelTrans.block<3, 1>(0, 3).cast<float>();
                Vector2f depth(&nodeData.parallaxDepth[0]);
                Vector2f ortho { (float)m_scene->ortho[0], (float)m_scene->ortho[1] };
                // flip mouse y axis
                Vector2f mouseVec =
                    Scaling(1.0f, -1.0f) * (Vector2f { 0.5f, 0.5f } - Vector2f(&m_mousePos[0]));
                mouseVec        = mouseVec.cwiseProduct(ortho) * m_parallax.mouseinfluence;
                Vector3f camPos = camera->GetPosition().cast<float>();
                Vector2f paraVec =
                    (nodePos.head<2>() - camPos.head<2>() + mouseVec).cwiseProduct(depth) *
                    m_parallax.amount;
                modelTrans =
                    Affine3d(Translation3d(Vector3d(paraVec.x(), paraVec.y(), 0.0f))).matrix() *
                    modelTrans;
            }
        }

        if (reqM) updateOp(G_M, ShaderValue::fromMatrix(modelTrans));
        if (reqAM) updateOp(G_AM, ShaderValue::fromMatrix(modelTrans));
        if (reqMI) updateOp(G_MI, ShaderValue::fromMatrix(modelTrans.inverse()));
        if (reqMVP) {
            Matrix4d mvpTrans = viewProTrans * modelTrans;
            updateOp(G_MVP, ShaderValue::fromMatrix(mvpTrans));
            if (reqMVPI) updateOp(G_MVPI, ShaderValue::fromMatrix(mvpTrans.inverse()));
        }
        if (reqETVP || reqETVPI) {
            /*
            Vector3d nodePos = pNode->Translate().cast<double>();
            nodePos.z()      = 1.0f;
            Matrix4d etvpTrans =
                viewProTrans * modelTrans * Affine3d(Eigen::Scaling(nodePos)).matrix();
            if (reqETVPI) updateOp(G_ETVP, ShaderValue::fromMatrix(etvpTrans));
            if (reqETVPI) updateOp(G_ETVPI, ShaderValue::fromMatrix(etvpTrans.inverse()));
            */
        }
    }

    //	g_EffectTextureProjectionMatrix
    // shadervs.push_back({"g_EffectTextureProjectionMatrixInverse",
    // ShaderValue::ValueOf(Eigen::Matrix4f::Identity())});
    if (info.has_TIME) updateOp(G_TIME, (float)m_scene->elapsingTime);

    if (info.has_DAYTIME) updateOp(G_DAYTIME, (float)m_dayTime);

    if (info.has_POINTERPOSITION) updateOp(G_POINTERPOSITION, m_mousePos);

    if (info.has_TEXELSIZE) updateOp(G_TEXELSIZE, m_texelSize);

    if (info.has_TEXELSIZEHALF)
        updateOp(G_TEXELSIZEHALF, std::array { m_texelSize[0] / 2.0f, m_texelSize[1] / 2.0f });

    if (info.has_SCREEN)
        updateOp(G_SCREEN,
                 std::array<float, 3> {
                     m_screen_size[0], m_screen_size[1], m_screen_size[0] / m_screen_size[1] });

    if (info.has_PARALLAXPOSITION) {
        Vector2f para { 0.5f, 0.5f };
        if (m_parallax.enable) {
            const Vector2f mouseCentered = Vector2f(&m_mousePos[0]) - Vector2f { 0.5f, 0.5f };
            para = Vector2f { 0.5f, 0.5f } +
                   (Scaling(1.0f, -1.0f) * mouseCentered) * m_parallax.mouseinfluence;
        }
        updateOp(G_PARALLAXPOSITION, std::array { para[0], para[1] });
    }

    const auto& anim_override = pNode->TexAnim();
    for (auto& [i, sp] : sprites) {
        // Script-driven override:
        //   current_frame >= 0  → pin to that frame
        //   playing == false    → freeze on current auto-advance frame
        //   else                → normal time-driven advance
        const SpriteFrame* fp = nullptr;
        if (anim_override.current_frame >= 0 && sp.numFrames() > 0) {
            const i32 idx = i32(anim_override.current_frame) % i32(sp.numFrames());
            fp            = &sp.GetFrame(idx);
        } else if (! anim_override.playing && sp.numFrames() > 0) {
            fp = &sp.GetCurFrame();
        } else {
            fp = &sp.GetAnimateFrame(m_scene->frameTime);
        }
        const auto& f      = *fp;
        auto        grot   = WE_GLTEX_ROTATION_NAMES[i];
        auto        gtrans = WE_GLTEX_TRANSLATION_NAMES[i];
        updateOp(grot, std::array { f.xAxis[0], f.xAxis[1], f.yAxis[0], f.yAxis[1] });
        updateOp(gtrans, std::array { f.x, f.y });
    }

    // WE's `g_LightsPosition[4]` is a vec3 array; std140 pads to vec4 stride
    // (16B) so the trailing scalar slot stays at 0.
    if (info.has_LP) {
        constexpr unsigned                kMaxLights = 4;
        std::array<float, kMaxLights * 4> lights_pos { 0 };
        std::array<float, 12>             lights_color_legacy { 0 };
        unsigned                          i = 0;
        for (auto& l : m_scene->lights) {
            if (i == kMaxLights) break;
            if (! l->runtimeVisible()) {
                i++;
                continue;
            }
            rstd_assert(l->node() != nullptr);
            const auto& trans     = l->node()->Translate();
            lights_pos[i * 4 + 0] = trans.x();
            lights_pos[i * 4 + 1] = trans.y();
            lights_pos[i * 4 + 2] = trans.z();
            lights_pos[i * 4 + 3] = 0.0f;
            if (i < 3) {
                const auto& cp = l->premultipliedColor();
                std::copy(cp.begin(), cp.end(), lights_color_legacy.begin() + i * 4);
            }
            i++;
        }
        updateOp(G_LP, lights_pos);
        updateOp(G_LCP, lights_color_legacy);
    }

    // Script-driven per-frame overrides. updateOp overlays the material's
    // baked constValue for this draw; we only push when the script has
    // actually written to avoid clobbering the bake.
    if (pNode->IsAlphaOverridden()) {
        const float eff_alpha = pNode->EffectiveAlpha();
        updateOp("g_UserAlpha", eff_alpha);
        if (pNode->IsColorOverridden()) {
            const auto& c = pNode->Color();
            updateOp("g_Color4", std::array<float, 4> { c.x(), c.y(), c.z(), eff_alpha });
        }
    } else if (pNode->IsColorOverridden()) {
        const auto& c = pNode->Color();
        updateOp("g_Color4", std::array<float, 4> { c.x(), c.y(), c.z(), 1.0f });
    }
    if (pNode->IsBrightnessOverridden()) {
        updateOp("g_Brightness", pNode->Brightness());
    }

    // WE audio-bar shaders. std140 array stride is 16 bytes per element, so
    // pack each amplitude into the .x slot of a vec4 and leave .yzw at zero.
    // Visual values are pre-smoothed by SetAudioSpectrum. wavsen is mono today
    // so Left and Right read the same source.
    auto push_audio = [&](std::string_view name, std::span<const float> visual) {
        std::vector<float> packed(visual.size() * 4, 0.0f);
        for (std::size_t i = 0; i < visual.size(); ++i) packed[i * 4] = visual[i];
        updateOp(name, std::span<const float>(packed));
    };
    if (info.has_audio_16_l) push_audio(G_AUDIO_SPEC_16_L, m_audio_visual_16);
    if (info.has_audio_16_r) push_audio(G_AUDIO_SPEC_16_R, m_audio_visual_16);
    if (info.has_audio_32_l) push_audio(G_AUDIO_SPEC_32_L, m_audio_visual_32);
    if (info.has_audio_32_r) push_audio(G_AUDIO_SPEC_32_R, m_audio_visual_32);
    if (info.has_audio_64_l) push_audio(G_AUDIO_SPEC_64_L, m_audio_visual_64);
    if (info.has_audio_64_r) push_audio(G_AUDIO_SPEC_64_R, m_audio_visual_64);
}

void WPShaderValueUpdater::SetNodeData(void* nodeAddr, const WPShaderValueData& data) {
    m_nodeDataMap[nodeAddr] = data;
}

void WPShaderValueUpdater::CopyNodeData(void* src, void* dst) {
    auto it = m_nodeDataMap.find(src);
    if (it == m_nodeDataMap.end()) return;
    m_nodeDataMap[dst] = it->second;
}

void WPShaderValueUpdater::SetTexelSize(float x, float y) { m_texelSize = { x, y }; }

void WPShaderValueUpdater::SetAudioSpectrum(std::span<const float, 64> bins) {
    std::copy(bins.begin(), bins.end(), m_audio_bins.begin());
    // Run smoothing once per frame here, not per SceneNode in UpdateUniforms,
    // so multiple audio-responsive nodes share one EMA tick. m_scene->frameTime
    // holds the previous frame's dt (PassFrameTime runs at end of frame); on
    // frame 0 it's still zero, which UpdateVisualBands handles by adopting raw.
    const float dt = static_cast<float>(m_scene->frameTime);
    UpdateVisualBands(m_audio_bins, 16, dt, m_audio_visual_16);
    UpdateVisualBands(m_audio_bins, 32, dt, m_audio_visual_32);
    UpdateVisualBands(m_audio_bins, 64, dt, m_audio_visual_64);
}
