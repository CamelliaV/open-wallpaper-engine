#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <argparse/argparse.hpp>

import rstd.cppstd;
import rstd.log;
import wescene.scene_wallpaper;
import wescene.utils;
import viewer.common;
import nlohmann.json;

using namespace std;

atomic<bool> renderCall(false);

struct UserData {
    owe::SceneWallpaper* psw { nullptr };

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
    data->psw->mouseInput(xpos / data->width, ypos / data->height);
}

void cursor_enter_callback(GLFWwindow* win, int entered) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw) return;
    data->psw->mouseEnter(entered != 0);
}
}

void updateCallback() {
    renderCall = true;
    glfwPostEmptyEvent();
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
            return glfwCreateWindowSurface(inst, window, NULL, surface);
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
    psw->initVulkan(std::move(info));
    psw->setPropertyString(owe::PROPERTY_ASSETS, program.get<std::string>(viewer::ARG_ASSETS));
    psw->setPropertyString(owe::PROPERTY_SOURCE, program.get<std::string>(viewer::ARG_SCENE));
    psw->setPropertyBool(owe::PROPERTY_GRAPHIVZ, program.get<bool>(viewer::OPT_GRAPHVIZ));
    psw->setPropertyInt32(owe::PROPERTY_FPS, program.get<int32_t>(viewer::OPT_FPS));

    std::string cache_path = program.get<std::string>(viewer::OPT_CACHE_PATH);
    if (cache_path.empty()) cache_path = owe::platform::GetCachePath("wescene-renderer");
    psw->setPropertyString(owe::PROPERTY_CACHE_PATH, cache_path);

    // Apply --user-properties FILE before the scene loads so the first
    // frame already reflects the user's edits. Mirrors the daemon path
    // (Init.user_properties): JSON object whose values can be strings,
    // numbers, or booleans; setPropertyString already parses both.
    if (auto up_path = program.get<std::string>(viewer::OPT_USER_PROPS); ! up_path.empty()) {
        std::ifstream is(up_path);
        if (! is) {
            std::cerr << "--user-properties: cannot open '" << up_path << "'\n";
            return 1;
        }
        std::stringstream ss;
        ss << is.rdbuf();
        auto parsed = nlohmann::json::parse(ss.str(),
                                            /*cb*/ nullptr,
                                            /*allow_ex*/ false,
                                            /*ignore_comments*/ true);
        if (! parsed.is_object()) {
            std::cerr << "--user-properties: '" << up_path << "' is not a JSON object\n";
            return 1;
        }
        // Iterator form: `items()` structured-binding chases std::get
        // through ADL, which doesn't resolve under modules.
        for (auto it = parsed.begin(); it != parsed.end(); ++it) {
            const std::string& k = it.key();
            const auto&        v = it.value();
            std::string        sval;
            if (v.is_string())
                sval = v.get<std::string>();
            else if (v.is_boolean())
                sval = v.get<bool>() ? "true" : "false";
            else
                sval = v.dump();
            psw->setPropertyString(k, sval);
        }
    }

    glfwSetWindowUserPointer(window, &data);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);
    glfwSetCursorEnterCallback(window, cursor_enter_callback);

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
