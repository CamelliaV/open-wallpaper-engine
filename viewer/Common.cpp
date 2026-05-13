module;

#include <filesystem>

#include <GLFW/glfw3.h>

module viewer.common;

import rstd.cppstd;

namespace viewer
{

std::filesystem::path ExecutableDir(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto            self = fs::read_symlink("/proc/self/exe", ec);
    if (! ec) return self.parent_path();
    return fs::path(argv0 ? argv0 : "").parent_path();
}

void InitGlfwPlatformHint(bool force_x11) {
    if (force_x11) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        return;
    }
    // RenderDoc's Vulkan capture only hooks xcb/xlib WSI; under a Wayland
    // session GLFW would request VK_KHR_wayland_surface and instance
    // creation fails with VK_ERROR_EXTENSION_NOT_PRESENT.
    const char* x11_env          = std::getenv("WP_GLFW_X11");
    const bool  renderdoc_active = std::getenv("RENDERDOC_CAPTUREOPTS")
                                 || std::getenv("RENDERDOC_HOOK_VK");
    const bool select_x11 = (x11_env && x11_env[0] == '1')
                            || (renderdoc_active && (x11_env == nullptr || x11_env[0] != '0'));
    if (select_x11) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
    }
}

} // namespace viewer
