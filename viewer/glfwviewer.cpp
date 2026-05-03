#include <iostream>
#include <set>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <thread>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <atomic>
#include "arg.hpp"


import wescene.scene_wallpaper;
import wescene.utils;

using namespace std;

atomic<bool> renderCall(false);

struct UserData {
    wallpaper::SceneWallpaper* psw { nullptr };

    uint16_t width;
    uint16_t height;
};

extern "C" {
void framebuffer_size_callback(GLFWwindow*, int width, int height) {}

void mouse_button_callback(GLFWwindow* win, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
        // data->psw->setPropertyString(wallpaper::PROPERTY_SOURCE,
    }
}

void cursor_position_callback(GLFWwindow* win, double xpos, double ypos) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    data->psw->mouseInput(xpos / data->width, ypos / data->height);
}
}

void updateCallback() {
    renderCall = true;
    glfwPostEmptyEvent();
}

int main(int argc, char** argv) {
    argparse::ArgumentParser program("scene-viewer");
    setAndParseArg(program, argc, argv);
    auto [w_width, w_height] = program.get<Resolution>(OPT_RESOLUTION);

    // RenderDoc's Vulkan capture only hooks xcb/xlib WSI; under a Wayland
    // session GLFW will request VK_KHR_wayland_surface and the instance
    // creation fails with VK_ERROR_EXTENSION_NOT_PRESENT. Force the X11
    // backend (via XWayland) when WP_GLFW_X11=1, or when RenderDoc is
    // detected by env. Set WP_GLFW_X11=0 to keep native Wayland.
    {
        const char* x11_env = std::getenv("WP_GLFW_X11");
        const bool  renderdoc_active =
            std::getenv("RENDERDOC_CAPTUREOPTS") || std::getenv("RENDERDOC_HOOK_VK");
        const bool force_x11 = (x11_env && x11_env[0] == '1') ||
                               (renderdoc_active && (x11_env == nullptr || x11_env[0] != '0'));
        if (force_x11) {
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        }
    }
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Bulk-scan path: WP_HEADLESS=1 hides the window so a scan loop over
    // hundreds of pkgs doesn't spam the desktop. Compile/render still
    // runs against the offscreen surface — stderr captures shader errors.
    if (const char* hl = std::getenv("WP_HEADLESS"); hl && hl[0] == '1') {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    GLFWwindow* window = glfwCreateWindow(w_width, w_height, "WP", nullptr, nullptr);

    UserData data;
    data.width  = w_width;
    data.height = w_height;

    wallpaper::RenderInitInfo info;
    info.enable_valid_layer = program.get<bool>(OPT_VALID_LAYER);
    info.width              = w_width;
    info.height             = w_height;

    auto& sf_info = info.surface_info;
    {
        uint32_t glfwExtCount = 0;
        auto     exts         = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        for (int i = 0; i < glfwExtCount; i++) {
            sf_info.instanceExts.emplace_back(exts[i]);
        }

        sf_info.createSurfaceOp = [window](VkInstance inst, VkSurfaceKHR* surface) {
            return glfwCreateWindowSurface(inst, window, NULL, surface);
        };
    }

    if (window == nullptr) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    auto* psw = new wallpaper::SceneWallpaper();
    data.psw  = psw;

    psw->init();
    psw->initVulkan(std::move(info));
    psw->setPropertyString(wallpaper::PROPERTY_ASSETS, program.get<std::string>(ARG_ASSETS));
    psw->setPropertyString(wallpaper::PROPERTY_SOURCE, program.get<std::string>(ARG_SCENE));
    psw->setPropertyBool(wallpaper::PROPERTY_GRAPHIVZ, program.get<bool>(OPT_GRAPHVIZ));
    psw->setPropertyInt32(wallpaper::PROPERTY_FPS, program.get<int32_t>(OPT_FPS));

    std::string cache_path = program.get<std::string>(OPT_CACHE_PATH);
    if (cache_path.empty()) cache_path = wallpaper::platform::GetCachePath("wescene-renderer");
    psw->setPropertyString(wallpaper::PROPERTY_CACHE_PATH, cache_path);

    glfwSetWindowUserPointer(window, &data);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);

    // Bulk-scan path: WP_COMPILE_ONLY=N waits N seconds after scene load
    // to let the async shader-compile pass drain, then exits. Skips the
    // render loop so no swapchain present is required (which would deadlock
    // with a hidden window). Use together with WP_HEADLESS=1.
    if (const char* co = std::getenv("WP_COMPILE_ONLY"); co && co[0] != '\0') {
        int seconds = std::atoi(co);
        if (seconds <= 0) seconds = 2;
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
    } else {
        while (! glfwWindowShouldClose(window)) {
            glfwPollEvents();
        }
    }
    delete psw;
    // wgl.Clear();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
