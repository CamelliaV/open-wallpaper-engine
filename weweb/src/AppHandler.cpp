#include "AppHandler.hpp"

#include "include/cef_command_line.h"

namespace weweb {

AppHandler::AppHandler() = default;

void AppHandler::OnBeforeCommandLineProcessing(const CefString& process_type,
                                               CefRefPtr<CefCommandLine> cmd) {
    // Only tweak the browser process command line. Renderer / utility
    // helpers inherit the relevant switches from the browser anyway.
    if (!process_type.empty()) return;

    // WE web wallpapers are loose directory trees loaded as `file://` URLs.
    // Without `allow-file-access-from-files` Blink's same-origin policy
    // blocks adjacent JS module / CSS / image fetches in many wallpapers
    // (notably ES-module Vue apps such as workshop 1404861377).
    cmd->AppendSwitch("allow-file-access-from-files");
    cmd->AppendSwitch("disable-web-security");

    // The `minimal` CEF Linux distribution does not ship the SUID sandbox
    // helper. Match `CefSettings.no_sandbox = true` at the cmdline level so
    // the switch propagates to all child procs.
    cmd->AppendSwitch("no-sandbox");

    // Wallpapers are decorative; suppress the chromium component updater.
    cmd->AppendSwitch("disable-component-update");
}

void AppHandler::OnContextInitialized() {
    // Browser is created from BrowserHost::OpenWallpaper; nothing to do here.
}

}  // namespace weweb
