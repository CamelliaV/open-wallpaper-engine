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
    // helper. Match `CefSettings.no_sandbox = true` at the cmdline level
    // so the switch propagates to all child procs.
    cmd->AppendSwitch("no-sandbox");

    cmd->AppendSwitchWithValue("ozone-platform", "x11");

    // Autoplay video / audio without user-gesture prompts. WE wallpapers
    // routinely auto-play media on load.
    cmd->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");

    // Wallpapers are decorative; suppress the chromium component updater.
    cmd->AppendSwitch("disable-component-update");

    cmd->AppendSwitch("disable-session-crashed-bubble");

    // Skip KDE/GNOME password-store probes (kwalletd6 / libsecret /
    // klauncher D-Bus calls). Wallpapers never store credentials.
    cmd->AppendSwitchWithValue("password-store", "basic");

    {
        std::string features;
        if (cmd->HasSwitch("disable-features")) {
            features = cmd->GetSwitchValue("disable-features").ToString();
            if (!features.empty()) features += ",";
        }
        features += "Crashpad,AutofillServerCommunication,HardwareMediaKeyHandling,WebBluetooth,WebUSB";
        cmd->AppendSwitchWithValue("disable-features", features);
    }

    cmd->AppendSwitch("no-first-run");                                                            
    cmd->AppendSwitch("no-default-browser-check");                                                
    cmd->AppendSwitch("disable-plugins");                                                
    cmd->AppendSwitch("disable-sync");                                                            
    cmd->AppendSwitch("disable-translate");                                                       
    cmd->AppendSwitch("disable-default-apps");                                                    
    cmd->AppendSwitch("disable-extensions");                                                      
    cmd->AppendSwitch("disable-client-side-phishing-detection");                                  
    cmd->AppendSwitch("disable-popup-blocking");       
    cmd->AppendSwitch("disable-pinch");       
    cmd->AppendSwitch("metrics-recording-only");       
}

void AppHandler::OnContextInitialized() {
    // Browser is created from BrowserHost::OpenWallpaper; nothing to do here.
}

}  // namespace weweb
