#include "BrowserHost.hpp"

#include <atomic>
#include <cstdio>

#include "include/cef_app.h"
#include "include/cef_browser.h"

#include "AppHandler.hpp"
#include "ClientHandler.hpp"

namespace weweb {

struct BrowserHost::Impl {
    CefRefPtr<AppHandler>    app;
    CefRefPtr<ClientHandler> client;
    std::atomic<bool>        should_exit{false};
    bool                     initialised{false};
    // Stash the original argv from RunOrExitIfHelper; CefInitialize needs
    // the real argv to derive the per-child --type=… / --icu-data-file=…
    // switches it forwards to subprocesses.
    int    saved_argc{0};
    char** saved_argv{nullptr};
};

BrowserHost::BrowserHost() : impl_(std::make_unique<Impl>()) {
    impl_->app = new AppHandler();
}

BrowserHost::~BrowserHost() {
    Shutdown();
}

int BrowserHost::RunOrExitIfHelper(int argc, char** argv) {
    impl_->saved_argc = argc;
    impl_->saved_argv = argv;
    CefMainArgs main_args(argc, argv);
    return CefExecuteProcess(main_args, impl_->app.get(), nullptr);
}

bool BrowserHost::Init(const InitOptions& opts) {
    if (impl_->initialised) {
        std::fprintf(stderr, "weweb: BrowserHost::Init called twice\n");
        return false;
    }

    // Browser process Init. Hand CEF the original argv (saved during
    // RunOrExitIfHelper) — without the real argv, CEF can't materialise
    // the internal command line it forwards to child processes, and
    // helpers SIGABRT in ICU init with "Invalid file descriptor to ICU
    // data received." argparse has already consumed argv but the array
    // is still valid memory.
    CefMainArgs main_args(impl_->saved_argc, impl_->saved_argv);

    CefSettings settings;
    settings.no_sandbox                   = true;
    settings.windowless_rendering_enabled = false;
    settings.multi_threaded_message_loop  = false;
    // Leave command_line_args_disabled at its default (false). Setting it
    // true blocks CEF from materialising the *internal* argv that gets
    // passed to child processes, which then SIGABRT in ICU init with
    // "Invalid file descriptor to ICU data received." The user-supplied
    // positional `workshop` arg only reaches the *browser* process here
    // (helpers re-enter through RunOrExitIfHelper before argparse), so
    // adopting argv is harmless — Chromium ignores unknown args.
    settings.log_severity                 = LOGSEVERITY_WARNING;

    auto set_cef_path = [](cef_string_t* dest,
                           const std::filesystem::path& p) {
        if (p.empty()) return;
        // Wrap dest in a temp CefString and assign — using brace-init to
        // dodge the C++ vexing parse where `CefString(dest)` would be
        // read as a redundant-parens declaration of `dest`.
        CefString cef_str{dest};
        cef_str = p.string();
    };
    set_cef_path(&settings.resources_dir_path, opts.resources_dir);
    set_cef_path(&settings.locales_dir_path,   opts.locales_dir);
    set_cef_path(&settings.root_cache_path,    opts.cache_dir);

    if (opts.enable_remote_debugging && opts.remote_debugging_port > 0) {
        settings.remote_debugging_port = opts.remote_debugging_port;
    }

    if (!CefInitialize(main_args, settings, impl_->app.get(), nullptr)) {
        std::fprintf(stderr, "weweb: CefInitialize failed\n");
        return false;
    }
    impl_->initialised = true;
    return true;
}

bool BrowserHost::OpenWallpaper(const WebManifest& manifest,
                                const std::filesystem::path& workshop_dir,
                                unsigned long parent_x11_window,
                                int width, int height) {
    if (!impl_->initialised) {
        std::fprintf(stderr, "weweb: OpenWallpaper before Init\n");
        return false;
    }

    impl_->client = new ClientHandler(manifest.user_props);
    impl_->client->SetCloseCallback([this] {
        impl_->should_exit.store(true);
    });

    auto entry = workshop_dir / manifest.entry_html;
    std::string url = "file://" + entry.string();

    CefWindowInfo info;
    CefRect rect{0, 0, width, height};
    info.SetAsChild(static_cast<CefWindowHandle>(parent_x11_window), rect);

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 60;

    CefBrowserHost::CreateBrowser(info, impl_->client.get(), url,
                                  browser_settings, nullptr, nullptr);
    return true;
}

void BrowserHost::Pump() {
    if (impl_->initialised) CefDoMessageLoopWork();
}

bool BrowserHost::ShouldExit() const {
    return impl_->should_exit.load();
}

void BrowserHost::RequestClose() {
    impl_->should_exit.store(true);
}

void BrowserHost::Shutdown() {
    if (!impl_->initialised) return;
    CefShutdown();
    impl_->initialised = false;
}

}  // namespace weweb
