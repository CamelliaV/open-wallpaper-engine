#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <argparse/argparse.hpp>

import rstd.cppstd;
import rstd.log;
import wescene.json;
import wescene.scene_wallpaper;
import wescene.utils;
import viewer.common;

using namespace std;

atomic<bool> renderCall(false);

struct UserData {
    owe::SceneWallpaper* psw { nullptr };
    bool                 mouse_position_locked { false };

    uint16_t width;
    uint16_t height;
};

extern "C" {
void framebuffer_size_callback(GLFWwindow*, int width, int height) {}

void mouse_button_callback(GLFWwindow* win, int button, int action, int /*mods*/) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw) return;
    // GLFW button numbering (0=left, 1=right, 2=middle) matches WE.
    if (action == GLFW_PRESS) data->psw->mouseButton(button, true);
    if (action == GLFW_RELEASE) data->psw->mouseButton(button, false);
}

void cursor_position_callback(GLFWwindow* win, double xpos, double ypos) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw || data->mouse_position_locked) return;
    data->psw->mouseInput(xpos / data->width, ypos / data->height);
}

void cursor_enter_callback(GLFWwindow* win, int entered) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw || data->mouse_position_locked) return;
    data->psw->mouseEnter(entered != 0);
}
}

void updateCallback() {
    renderCall = true;
    glfwPostEmptyEvent();
}

std::optional<std::array<double, 2>> parseMousePosition(const std::string& value) {
    if (value.empty()) return std::nullopt;
    const auto comma = value.find(',');
    if (comma == std::string::npos) return std::nullopt;
    double x  = 0.0;
    double y  = 0.0;
    auto   xs = value.substr(0, comma);
    auto   ys = value.substr(comma + 1);
    auto   xr = std::from_chars(xs.data(), xs.data() + xs.size(), x);
    auto   yr = std::from_chars(ys.data(), ys.data() + ys.size(), y);
    if (xr.ec != std::errc {} || yr.ec != std::errc {}) return std::nullopt;
    return std::array { std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0) };
}

int main(int argc, char** argv) {
    static rstd::log::EnvLogger _logger;
    rstd::log::set_logger(_logger);
    rstd::log::set_max_level(_logger.filter());

    argparse::ArgumentParser program("scene-viewer");
    viewer::setAndParseArg(program, argc, argv);
    auto [w_width, w_height] = program.get<viewer::Resolution>(viewer::OPT_RESOLUTION);

    viewer::InitGlfwPlatformHint(/*force_x11=*/false);
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

    owe::RenderInitInfo info;
    info.enable_valid_layer = program.get<bool>(viewer::OPT_VALID_LAYER);
    info.width              = w_width;
    info.height             = w_height;
    info.msaa_samples       = program.get<uint32_t>(viewer::OPT_MSAA);

    auto& sf_info = info.surface_info;
    {
        uint32_t glfwExtCount = 0;
        auto     exts         = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        for (int i = 0; i < glfwExtCount; i++) {
            sf_info.instanceExts.emplace_back(exts[i]);
        }

        sf_info.createSurfaceOp = [window](VkInstance inst, VkSurfaceKHR* surface) {
            return glfwCreateWindowSurface(inst, window, nullptr, surface);
        };
    }

    if (window == nullptr) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    auto* psw = new owe::SceneWallpaper();
    data.psw  = psw;

    psw->init();

    owe::SceneWallpaperConfig config;
    config.assets_dir      = program.get<std::string>(viewer::ARG_ASSETS);
    config.source_pkg_path = program.get<std::string>(viewer::ARG_SCENE);
    config.graphviz        = program.get<bool>(viewer::OPT_GRAPHVIZ);
    config.fps             = static_cast<uint32_t>(program.get<int32_t>(viewer::OPT_FPS));

    std::string cache_path = program.get<std::string>(viewer::OPT_CACHE_PATH);
    if (cache_path.empty()) cache_path = owe::platform::GetCachePath("wescene-renderer");
    config.cache_dir = std::move(cache_path);

    // Apply --user-properties FILE before the scene loads so the first
    // frame already reflects the user's edits. Mirrors the daemon path
    // (Init.user_properties): JSON object whose values can be strings,
    // numbers, or booleans.
    if (auto up_path = program.get<std::string>(viewer::OPT_USER_PROPS); ! up_path.empty()) {
        std::ifstream is(up_path);
        if (! is) {
            std::cerr << "--user-properties: cannot open '" << up_path << "'\n";
            return 1;
        }
        std::stringstream ss;
        ss << is.rdbuf();
        auto parsed_result = owe::ParseJson(ss.str(), { .allow_comments = true });
        if (parsed_result.is_err()) {
            auto error = parsed_result.unwrap_err();
            std::cerr << "--user-properties: '" << up_path << "' is invalid JSON at line "
                      << error.line() << " column " << error.column() << '\n';
            return 1;
        }
        auto parsed = parsed_result.unwrap();
        if (! parsed.is_object()) {
            std::cerr << "--user-properties: '" << up_path << "' is not a JSON object\n";
            return 1;
        }
        auto object = parsed.as_object();
        (*object)->iter().for_each([&](auto entry) {
            auto [entry_key, entry_value] = entry;
            config.user_properties.insert(entry_key->clone(), entry_value->clone());
        });
    }

    psw->configure(std::move(config));
    psw->initVulkan(std::move(info));

    glfwSetWindowUserPointer(window, &data);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);
    glfwSetCursorEnterCallback(window, cursor_enter_callback);

    auto locked_mouse = parseMousePosition(program.get<std::string>(viewer::OPT_MOUSE_POS));
    data.mouse_position_locked = locked_mouse.has_value();
    auto apply_locked_mouse    = [&]() {
        if (! locked_mouse) return;
        psw->mouseEnter(true);
        psw->mouseInput((*locked_mouse)[0], (*locked_mouse)[1]);
        glfwSetCursorPos(window, (*locked_mouse)[0] * w_width, (*locked_mouse)[1] * w_height);
    };
    apply_locked_mouse();

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
            apply_locked_mouse();
        }
    }
    delete psw;
    // wgl.Clear();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
