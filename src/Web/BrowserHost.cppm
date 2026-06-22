module;

#include <nlohmann/json.hpp>

export module weweb:browser_host;

import rstd.cppstd;

import :frame;
import :manifest;

export namespace weweb
{

class OsrRenderHandler;
class ClientHandler;

class BrowserHost {
public:
    struct InitOptions {
        std::filesystem::path resources_dir;
        std::filesystem::path locales_dir;
        std::filesystem::path cache_dir;
        bool                  enable_remote_debugging { false };
        int                   remote_debugging_port { 0 };
        bool                  enable_audio { true };
        bool                  shared_texture_enabled { true };
        std::string           render_node_override;
    };

    struct OpenOptions {
        bool shared_texture_enabled { true };
        int  frame_rate { 60 };
    };

    BrowserHost();
    ~BrowserHost();

    BrowserHost(const BrowserHost&)            = delete;
    BrowserHost& operator=(const BrowserHost&) = delete;

    int  RunOrExitIfHelper(int argc, char** argv);
    bool Init(const InitOptions& opts);

    void SetAcceleratedPaintCallback(AcceleratedPaintCallback cb);
    void SetCpuPaintCallback(CpuPaintCallback cb);

    bool OpenWallpaper(const WebManifest& manifest, const std::filesystem::path& workshop_dir,
                       int width, int height);
    bool OpenWallpaper(const WebManifest& manifest, const std::filesystem::path& workshop_dir,
                       int width, int height, OpenOptions opts);

    void OnResize(int width, int height);
    void Invalidate();

    void OnMouseMove(int x, int y, bool left_down);
    void OnMouseButton(int x, int y, int cef_button, bool down, int click_count);
    void OnMouseWheel(int x, int y, int delta_x, int delta_y);
    void OnKey(int cef_key_event_type, int native_key_code, int windows_key_code, int modifiers,
               unsigned int unicode_char);
    void OnFocus(bool gained);

    void Pump();

    void ApplyVolume(float volume);
    void SetFrameRate(int fps);
    void SetPaused(bool paused);
    void ApplyUserProperty(std::string_view key, const nlohmann::json& value);
    void PushAudioData(const float* data, std::size_t count);

    bool ShouldExit() const;
    void RequestClose();
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace weweb
