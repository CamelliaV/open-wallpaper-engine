module;

export module wescene.shader_value_updater;
import eigen;
import wescene.core;
import rstd.cppstd;
import wescene.scene;

import wescene.puppet; // WPPuppetLayer

export namespace owe
{

struct WPUniformInfo {
    bool has_MI { false };
    bool has_M { false };
    bool has_AM { false };
    bool has_MVP { false };
    bool has_MVPI { false };
    bool has_ETVP { false };
    bool has_ETVPI { false };
    bool has_VP { false };

    bool has_BONES { false };
    bool has_TIME { false };
    bool has_DAYTIME { false };
    bool has_POINTERPOSITION { false };
    bool has_PARALLAXPOSITION { false };
    bool has_TEXELSIZE { false };
    bool has_TEXELSIZEHALF { false };
    bool has_SCREEN { false };
    bool has_LP { false };

    // WE audio-bar shaders. Each pair is a (Left, Right) float[N] array
    // selected by the shader's RESOLUTION combo at compile time.
    bool has_audio_16_l { false }, has_audio_16_r { false };
    bool has_audio_32_l { false }, has_audio_32_r { false };
    bool has_audio_64_l { false }, has_audio_64_r { false };

    struct Tex {
        bool has_resolution { false };
        bool has_mipmap { false };
    };
    std::array<Tex, 12> texs;
};

struct WPShaderValueData {
    std::array<float, 2>                       parallaxDepth { 0.0f, 0.0f };
    std::vector<std::pair<usize, std::string>> renderTargets;
    WPPuppetLayer                              puppet_layer;
};

struct WPCameraParallax {
    bool  enable { false };
    float amount;
    float delay;
    float mouseinfluence;
};

class WPShaderValueUpdater : public IShaderValueUpdater {
public:
    WPShaderValueUpdater(Scene* scene): m_scene(scene) {}
    virtual ~WPShaderValueUpdater() {}

    void FrameBegin() override;

    void InitUniforms(SceneNode*, const ExistsUniformOp&) override;
    void UpdateUniforms(SceneNode*, sprite_map_t&, const UpdateUniformOp&) override;
    void FrameEnd() override;
    void MouseInput(double, double) override;
    void SetTexelSize(float x, float y) override;

    void SetNodeData(void*, const WPShaderValueData&);
    // Replicate the shader-value record from src to dst. Used when scripts
    // clone a SceneNode at parse time (audio-bar fanout) so the clones
    // pick up the template's parallaxDepth / puppet binding / RT links.
    void CopyNodeData(void* src, void* dst);
    void SetCameraParallax(const WPCameraParallax& value) { m_parallax = value; }

    // Push the current 64-bin spectrum snapshot. Renderer calls this once
    // per frame before drawFrame. Used to fill `g_AudioSpectrum{16,32,64}{Left,Right}`
    // shader uniforms in UpdateUniforms.
    void SetAudioSpectrum(std::span<const float, 64> left,
                          std::span<const float, 64> right) override;

    void SetScreenSize(i32 w, i32 h) override { m_screen_size = { (float)w, (float)h }; }

private:
    Scene*               m_scene;
    WPCameraParallax     m_parallax;
    double               m_dayTime { 0.0f };
    std::array<float, 2> m_texelSize { 1.0f / 1920.0f, 1.0f / 1080.0f };

    std::array<float, 2> m_mousePos { 0.5f, 0.5f };
    std::array<float, 2> m_mousePosInput { 0.5f, 0.5f };
    double               m_mouseDelayedTime { 0.0f };
    unsigned             m_mouseInputCount { 0 };

    std::chrono::time_point<std::chrono::steady_clock> m_last_mouse_input_time;

    std::array<float, 2> m_screen_size { 1920, 1080 };

    // Per-frame visual bands ready for std140 packing in UpdateUniforms.
    // AudioCapture already applies smoothing and amplitude mapping.
    std::array<float, 16> m_audio_16_l {};
    std::array<float, 16> m_audio_16_r {};
    std::array<float, 32> m_audio_32_l {};
    std::array<float, 32> m_audio_32_r {};
    std::array<float, 64> m_audio_64_l {};
    std::array<float, 64> m_audio_64_r {};

    Map<void*, WPShaderValueData> m_nodeDataMap;
    Map<void*, WPUniformInfo>     m_nodeUniformInfoMap;
};

} // namespace owe
