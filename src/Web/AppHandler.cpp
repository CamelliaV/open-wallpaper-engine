#include "AppHandler.hpp"

#include <cstdlib>
#include <cstring>

#include "include/cef_command_line.h"

namespace weweb
{

AppHandler::AppHandler() = default;

/*
arg: --no-sandbox
arg: --lang=en-US
arg: --log-severity=warning
arg: --resources-dir-path=...
arg: --locales-dir-path=.../locales
arg: --disable-features=GlicActorUi,AutofillActorMode,LensOverlay
*/

void AppHandler::OnBeforeCommandLineProcessing(const CefString&          process_type,
                                               CefRefPtr<CefCommandLine> cmd) {
    // Only tweak the browser process command line. Renderer / utility
    // helpers inherit the relevant switches from the browser anyway.
    if (! process_type.empty()) return;

    // WE web wallpapers are loose directory trees loaded as `file://` URLs.
    cmd->AppendSwitch("allow-file-access-from-files");
    cmd->AppendSwitch("disable-web-security");

    // The `minimal` CEF Linux distribution does not ship the SUID sandbox
    // helper. Match `CefSettings.no_sandbox = true` at the cmdline level
    // so the switch propagates to all child procs.
    cmd->AppendSwitch("no-sandbox");

    std::string features { "AcceleratedVideoDecodeLinuxZeroCopyGL,AcceleratedVideoDecodeLinuxGL,"
                           "VaapiIgnoreDriverChecks,VaapiOnNvidiaGPUs,VaapiVideoDecodeLinuxGL" };

    cmd->AppendSwitchWithValue("headless", "new");
    if (0) {
        cmd->AppendSwitch("vulkan");
        cmd->AppendSwitchWithValue("use-vulkan", "native");
        cmd->AppendSwitchWithValue("use-angle", "vulkan");
        cmd->AppendSwitchWithValue("use-gl", "angle");
        cmd->AppendSwitch("disable-vulkan-surface");
        cmd->AppendSwitch("disable-search-engine-choice-screen");
        cmd->AppendSwitch("disable-software-rasterizer");
        cmd->AppendSwitch("enable-raw-draw");
        cmd->AppendSwitch("enable-native-gpu-memory-buffers");
        cmd->AppendSwitch("enable-unsafe-webgpu");
        features.append(",DefaultAngleVulkan,VulkanFromANGLE,Vulkan");
    }
    cmd->AppendSwitchWithValue("enable-features", features);

    cmd->AppendSwitchWithValue("ozone-platform", "wayland");
    cmd->AppendSwitch("enable-gpu");
    cmd->AppendSwitch("enable-gpu-rasterization");
    cmd->AppendSwitch("enable-zero-copy");
    cmd->AppendSwitch("enable-accelerated-video-decode");
    cmd->AppendSwitch("enable-gpu-compositing");
    cmd->AppendSwitch("ignore-gpu-blocklist");
    // cmd->AppendSwitch("disable-gpu-vsync");

    // Autoplay video / audio without user-gesture prompts. WE wallpapers
    // routinely auto-play media on load.
    cmd->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");

    // Skip KDE/GNOME password-store probes (kwalletd6 / libsecret /
    // klauncher D-Bus calls). Wallpapers never store credentials.
    cmd->AppendSwitchWithValue("password-store", "basic");

    {
        std::string features;
        if (cmd->HasSwitch("disable-features")) {
            features = cmd->GetSwitchValue("disable-features").ToString();
            if (! features.empty()) features += ",";
        }
        features +=
            "Crashpad,AutofillServerCommunication,HardwareMediaKeyHandling,WebBluetooth,WebUSB";
        cmd->AppendSwitchWithValue("disable-features", features);
    }

    // Misc
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
    cmd->AppendSwitch("disable-component-update");
    cmd->AppendSwitch("disable-session-crashed-bubble");
}

void AppHandler::OnContextInitialized() {
    // Browser is created from BrowserHost::OpenWallpaper; nothing to do here.
}

} // namespace weweb
