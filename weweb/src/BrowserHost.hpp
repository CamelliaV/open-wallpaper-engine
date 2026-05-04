#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "Manifest.hpp"

namespace weweb {

class OsrRenderHandler;
class ClientHandler;

// Snapshot of the latest OSR frame. While alive, holds the OSR
// handler's mutex; release with BrowserHost::UnlockFrame.
struct FrameLock {
    const std::uint8_t* pixels{nullptr};
    int                 width{0};
    int                 height{0};
    bool                fresh{false};
};

// Owns the CEF browser lifecycle for one webviewer process. Standard
// same-binary multi-process model: helper procs early-exit via
// `RunOrExitIfHelper`; the main browser proc continues into Init +
// OpenWallpaper + Pump loop + Shutdown.
//
// v2 OSR: the browser renders into a CPU bitmap delivered by
// OsrRenderHandler::OnPaint. The viewer owns the host window (Vulkan
// surface) and uploads each frame to the GPU.
class BrowserHost {
public:
    struct InitOptions {
        std::filesystem::path resources_dir;  // CEF Resources/
        std::filesystem::path locales_dir;    // CEF Resources/locales/
        std::filesystem::path cache_dir;      // optional CEF disk cache
        bool enable_remote_debugging{false};
        int  remote_debugging_port{0};
    };

    BrowserHost();
    ~BrowserHost();

    BrowserHost(const BrowserHost&) = delete;
    BrowserHost& operator=(const BrowserHost&) = delete;

    // Returns >= 0 if this process is a CEF helper (renderer / utility /
    // zygote) — caller MUST `return` that as the process exit code without
    // doing anything else. Returns -1 if this is the main browser process
    // and initialisation should continue.
    int RunOrExitIfHelper(int argc, char** argv);

    // Initialise CEF in the main browser process. Must be called exactly
    // once after RunOrExitIfHelper returns -1. Returns false on failure
    // and prints a diagnostic to stderr — weweb-host is built with
    // -fno-exceptions so we can't throw.
    bool Init(const InitOptions& opts);

    // Spawn a windowless (OSR) browser for the wallpaper. The entry HTML
    // is loaded from `file://<workshop_dir>/<entry_html>` and the
    // manifest's user properties are injected on first load. The caller
    // accesses pixels via the returned OsrRenderHandler. Initial logical
    // size is `width` x `height`; resize via OnResize.
    bool OpenWallpaper(const WebManifest& manifest,
                       const std::filesystem::path& workshop_dir,
                       int width, int height);

    // Lock the latest CEF-painted frame. The returned pointer is valid
    // until UnlockFrame() is called. The bool `fresh` indicates whether
    // the frame changed since the last lock. While the frame is locked,
    // CEF's UI thread cannot deliver a new OnPaint, so do the GPU upload
    // and unlock as quickly as possible.
    FrameLock LockFrame();
    void      UnlockFrame();

    // Notify CEF that the host window changed size. Updates GetViewRect's
    // returned rect so the next OnPaint matches `width` x `height`.
    void OnResize(int width, int height);

    // Forward GLFW input events into the live browser. Coordinates are in
    // logical pixels matching the GetViewRect rect.
    void OnMouseMove(int x, int y, bool left_down);
    void OnMouseButton(int x, int y, int cef_button, bool down, int click_count);
    void OnMouseWheel(int x, int y, int delta_x, int delta_y);
    void OnKey(int cef_key_event_type, int native_key_code,
               int windows_key_code, int modifiers,
               unsigned int unicode_char);
    void OnFocus(bool gained);

    // Pump the CEF message loop once. Caller drives this from their main
    // event loop alongside whatever windowing-system polling they do.
    void Pump();

    // True once the browser has been closed (close button, JS-driven
    // close, etc.).
    bool ShouldExit() const;

    // Flag the host for graceful exit.
    void RequestClose();

    // Tear down CEF. Safe to call multiple times.
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace weweb
