#pragma once

#include <filesystem>
#include <memory>

#include "Manifest.hpp"

namespace weweb {

// Owns the CEF browser lifecycle for one webviewer process. Standard
// same-binary multi-process model: helper procs early-exit via
// `RunOrExitIfHelper`; the main browser proc continues into Init +
// OpenWallpaper + Pump loop + Shutdown.
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

    // Open the given wallpaper in a child window of `parent_x11_window`.
    // The entry HTML is loaded from `file://<workshop_dir>/<entry_html>`
    // and the manifest's user properties are injected on first load.
    // Returns false if Init has not been called.
    bool OpenWallpaper(const WebManifest& manifest,
                       const std::filesystem::path& workshop_dir,
                       unsigned long parent_x11_window,
                       int width, int height);

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
