#pragma once

#include "include/cef_app.h"
#include "include/cef_browser_process_handler.h"
#include "include/cef_render_process_handler.h"

#include <string>

namespace weweb
{

class AppHandler : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler {
public:
    AppHandler();

    // Must be set BEFORE CefInitialize. true ⇒ append `--mute-audio` so
    // Chromium never opens an output device.
    void SetMuteAudio(bool m) { m_mute_audio = m; }
    void SetSharedTextureEnabled(bool enabled) { m_shared_texture_enabled = enabled; }
    void SetRenderNodeOverride(const std::string& path) { m_render_node_override = path; }

    // CefApp.
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler>  GetRenderProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString&          process_type,
                                       CefRefPtr<CefCommandLine> cmd) override;

    // CefBrowserProcessHandler.
    void OnContextInitialized() override;

    // CefRenderProcessHandler. Runs in the render process before any page
    // script — installs the WE web audio API (wallpaperRegisterAudioListener
    // + the __weweb_pushAudio dispatcher the browser process feeds).
    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override;

private:
    bool m_mute_audio { false };
    bool m_shared_texture_enabled { true };
    std::string m_render_node_override;

    IMPLEMENT_REFCOUNTING(AppHandler);
    DISALLOW_COPY_AND_ASSIGN(AppHandler);
};

} // namespace weweb
