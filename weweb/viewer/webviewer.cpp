// weweb standalone GLFW + CEF viewer.

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <argparse/argparse.hpp>

#include "BrowserHost.hpp"
#include "Manifest.hpp"

namespace {

// Resolve where CEF's Resources/ + Release/ files live at runtime. We stage
// them next to the executable in the build tree (see weweb_stage_cef_runtime
// in cef.cmake) — so they sit alongside argv[0] both in build/ and after a
// hypothetical install.
std::filesystem::path ExecutableDir(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return self.parent_path();
    return fs::path(argv0).parent_path();
}

GLFWwindow* g_window = nullptr;

void OnFramebufferResize(GLFWwindow*, int /*w*/, int /*h*/) {
    // CEF's child window resizes itself to match its parent automatically
    // via the X11 ResizeRedirectMask CEF sets up. Nothing to do here.
}

}  // namespace

int main(int argc, char** argv) {
    weweb::BrowserHost host;

    // CRITICAL: must run before any of our own arg parsing — CEF re-execs
    // this binary with `--type=zygote/--type=renderer/...` switches; we
    // must short-circuit those helper invocations immediately.
    if (int helper_exit = host.RunOrExitIfHelper(argc, argv);
        helper_exit >= 0) {
        return helper_exit;
    }

    argparse::ArgumentParser p("webviewer");
    p.add_argument("workshop")
        .help("path to a workshop/<id>/ directory containing project.json + index.html");
    p.add_argument("--width")
        .help("initial window width in pixels")
        .default_value(1280)
        .scan<'i', int>();
    p.add_argument("--height")
        .help("initial window height in pixels")
        .default_value(720)
        .scan<'i', int>();
    p.add_argument("--remote-debugging-port")
        .help("if non-zero, expose chrome devtools on this localhost port")
        .default_value(0)
        .scan<'i', int>();

    try {
        p.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "webviewer: " << e.what() << "\n" << p;
        return 2;
    }

    auto workshop_dir = std::filesystem::path(p.get<std::string>("workshop"));
    if (!std::filesystem::is_directory(workshop_dir)) {
        std::cerr << "webviewer: not a directory: " << workshop_dir << "\n";
        return 2;
    }

    auto manifest_opt = weweb::LoadWebManifest(workshop_dir);
    if (!manifest_opt) return 2;  // LoadWebManifest already logged.
    auto& manifest = *manifest_opt;

    // Force GLFW to use the X11 backend. CEF Linux's child-window mode
    // wants an X11 Window handle; on Wayland sessions GLFW will route via
    // XWayland once we set this hint.
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    if (!glfwInit()) {
        std::cerr << "webviewer: glfwInit failed\n";
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    int w_width  = p.get<int>("--width");
    int w_height = p.get<int>("--height");
    g_window = glfwCreateWindow(w_width, w_height,
                                manifest.title.c_str(), nullptr, nullptr);
    if (!g_window) {
        std::cerr << "webviewer: glfwCreateWindow failed (X11 unavailable?)\n";
        glfwTerminate();
        return 1;
    }
    glfwSetFramebufferSizeCallback(g_window, OnFramebufferResize);

    auto exe_dir = ExecutableDir(argv[0]);

    weweb::BrowserHost::InitOptions opts;
    opts.resources_dir = exe_dir;
    opts.locales_dir   = exe_dir / "locales";
    // Use CEF's default per-user cache; setting a path here is optional.
    if (int port = p.get<int>("--remote-debugging-port"); port > 0) {
        opts.enable_remote_debugging = true;
        opts.remote_debugging_port   = port;
    }

    if (!host.Init(opts)) {
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }

    Window x11_window = glfwGetX11Window(g_window);
    if (!host.OpenWallpaper(manifest, workshop_dir,
                            static_cast<unsigned long>(x11_window),
                            w_width, w_height)) {
        host.Shutdown();
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }

    // Pump GLFW + CEF together. ~60Hz nominal — CefDoMessageLoopWork is
    // cheap, and glfwPollEvents returns immediately when no events queue.
    while (!glfwWindowShouldClose(g_window) && !host.ShouldExit()) {
        glfwPollEvents();
        host.Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    host.Shutdown();
    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
}
