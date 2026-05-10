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
    cmd->AppendSwitch("allow-chrome-scheme-url");

    // The `minimal` CEF Linux distribution does not ship the SUID sandbox
    // helper. Match `CefSettings.no_sandbox = true` at the cmdline level
    // so the switch propagates to all child procs.
    cmd->AppendSwitch("no-sandbox");
    // cmd->AppendSwitch("disable-gpu-sandbox");

    cmd->AppendSwitch("off-screen-rendering-enabled");
    cmd->AppendSwitch("shared-texture-enabled");

    std::string features { "AcceleratedVideoDecodeLinuxZeroCopyGL,AcceleratedVideoDecodeLinuxGL,"
                           "VaapiIgnoreDriverChecks,VaapiOnNvidiaGPUs,VaapiVideoDecodeLinuxGL" };
    std::string dis_features;
    if (cmd->HasSwitch("disable-features")) {
        dis_features = cmd->GetSwitchValue("disable-features").ToString();
        if (! dis_features.empty()) dis_features += ",";
        dis_features +=
            "Crashpad,AutofillServerCommunication,HardwareMediaKeyHandling,WebBluetooth,WebUSB";
    }

    if (0) {
        // Vulkan
        cmd->AppendSwitch("vulkan");
        cmd->AppendSwitchWithValue("use-vulkan", "native");
        cmd->AppendSwitchWithValue("use-angle", "vulkan");
        cmd->AppendSwitchWithValue("use-gl", "angle");
        cmd->AppendSwitch("disable-vulkan-surface");
        cmd->AppendSwitch("disable-software-rasterizer");
        cmd->AppendSwitch("enable-raw-draw");
        cmd->AppendSwitch("enable-unsafe-webgpu");
        features.append(",DefaultAngleVulkan,VulkanFromANGLE,Vulkan,SkiaGraphite");
    } else {
        // Gl-Egl
        cmd->AppendSwitchWithValue("use-gl", "angle");
        cmd->AppendSwitchWithValue("use-angle", "gl-egl");
        cmd->AppendSwitch("disable-vulkan-surface");
        dis_features += ",Vulkan,VulkanFromANGLE,DefaultAngleVulkan,SkiaGraphite";
    }

    cmd->AppendSwitchWithValue("ozone-platform", "wayland");
    // cmd->AppendSwitchWithValue("headless", "new");
    // cmd->AppendSwitchWithValue("ozone-platform-hint", "wayland");
    cmd->AppendSwitch("enable-gpu");
    cmd->AppendSwitch("ignore-gpu-blocklist");
    cmd->AppendSwitch("enable-gpu-rasterization");
    cmd->AppendSwitch("enable-gpu-compositing");
    cmd->AppendSwitch("enable-zero-copy");

    // hw decode
    cmd->AppendSwitch("enable-accelerated-video-decode");
    cmd->AppendSwitch("enable-native-gpu-memory-buffers");

    // cmd->AppendSwitch("disable-gpu-compositing");
    // cmd->AppendSwitch("disable-gpu-vsync");

    cmd->AppendSwitchWithValue("enable-features", features);
    cmd->AppendSwitchWithValue("disable-features", dis_features);

    // Autoplay video / audio without user-gesture prompts. WE wallpapers
    // routinely auto-play media on load.
    cmd->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");

    // Skip KDE/GNOME password-store probes (kwalletd6 / libsecret /
    // klauncher D-Bus calls). Wallpapers never store credentials.
    cmd->AppendSwitchWithValue("password-store", "basic");

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
    cmd->AppendSwitch("disable-search-engine-choice-screen");

    // Honour the host's enable_audio gate. Chromium never instantiates an
    // output stream when this switch is present, so no system audio device
    // (PulseAudio/PipeWire) gets opened.
    if (m_mute_audio) {
        cmd->AppendSwitch("mute-audio");
    }
}

void AppHandler::OnContextInitialized() {
    // Browser is created from BrowserHost::OpenWallpaper; nothing to do here.
}

} // namespace weweb
