// waywallen-wescene-renderer — wescene (Wallpaper Engine scene) host
// subprocess.

#include "BridgeExSwapchain.hpp"

#include <rstd/macro.hpp>

#include <waywallen-bridge/bridge.h>
#include <waywallen-bridge/extent_resolve.h>
#include <waywallen-bridge/pool.h>
#include <waywallen-bridge/probe_vk.h>
#include <waywallen-bridge/protocol_bits.h>

#include <argparse/argparse.hpp>

#include <errno.h>
#include <signal.h>
#include <string.h>

#include <sys/prctl.h>
#include <sys/socket.h>
#include <vulkan/vulkan.h>

import rstd.cppstd;
import rstd.log;
import wescene.scene_wallpaper;

namespace
{

struct Options {
    std::string ipc_path;
    uint32_t    width { 1280 };
    uint32_t    height { 720 };
    std::string initial_scene;
    std::string initial_assets;
    std::string workshop_id;
    uint32_t    initial_fps { 30 };
    bool        test_pattern { false };
    float       initial_volume { 1.0f };
    std::string render_node;
};

[[noreturn]] void die(const std::string& msg) {
    rstd_error("waywallen-wescene-renderer: {}", msg);
    std::exit(1);
}

Options parse_args(int argc, char** argv) {
    argparse::ArgumentParser program("waywallen-wescene-renderer");

    program.add_argument("--ipc")
        .required()
        .help("Unix-domain socket path for daemon IPC");
    program.add_argument("--path")
        .default_value(std::string{})
        .help("Wallpaper Engine .pkg path (canonical resource)");
    program.add_argument("--assets")
        .default_value(std::string{})
        .help("Optional Wallpaper Engine assets directory");
    program.add_argument("--workshop_id")
        .default_value(std::string{})
        .help("Optional Steam workshop id (informational)");
    program.add_argument("--render-node")
        .default_value(std::string{})
        .help("DRM render-node path to pin Vulkan device selection to "
              "(empty ⇒ let Vulkan pick the default)");
    program.add_argument("remaining").remaining();

    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        rstd_error("{}", static_cast<const char*>(err.what()));
        std::cerr << program;
        std::exit(1);
    }

    Options o;
    o.ipc_path       = program.get<std::string>("--ipc");
    o.initial_scene  = program.get<std::string>("--path");
    o.initial_assets = program.get<std::string>("--assets");
    o.workshop_id    = program.get<std::string>("--workshop_id");
    o.render_node    = program.get<std::string>("--render-node");
    return o;
}

// Linear-scan lookup for ww_kv_list_t. Lists are tiny (manifest-driven
// settings have <10 entries today).
const char* kv_get(const ww_kv_list_t& kv, const char* key) {
    for (uint32_t i = 0; i < kv.count; ++i) {
        if (kv.data[i].key && std::strcmp(kv.data[i].key, key) == 0)
            return kv.data[i].value;
    }
    return nullptr;
}

// Parse a setting string as f32; falls back to `def` on parse error
// or a NULL pointer.
float parse_f32(const char* s, float def) {
    if (!s || !*s) return def;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(s, &end);
    if (errno != 0 || end == s) return def;
    return static_cast<float>(v);
}

bool resolve_render_node_to_uuid(const std::string& path,
                                 std::array<uint8_t, VK_UUID_SIZE>& out_uuid,
                                 std::string& err_msg) {
    static const char* k_inst_exts[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkApplicationInfo app {};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "wescene-render-node-probe";
    app.apiVersion       = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici {};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledExtensionCount   = sizeof(k_inst_exts) / sizeof(k_inst_exts[0]);
    ici.ppEnabledExtensionNames = k_inst_exts;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        err_msg = "vkCreateInstance failed";
        return false;
    }

    ww_bridge_vk_dt_t dt {};
    if (ww_bridge_vk_dt_load(&dt, vkGetInstanceProcAddr, inst) != 0) {
        vkDestroyInstance(inst, nullptr);
        err_msg = "ww_bridge_vk_dt_load failed";
        return false;
    }

    int rc = ww_bridge_vk_resolve_render_node(&dt, inst, path.c_str(),
                                              out_uuid.data());
    vkDestroyInstance(inst, nullptr);

    if (rc == 0) return true;
    if (rc == -ENOENT) {
        err_msg = "no Vulkan device with VK_EXT_physical_device_drm matches "
                  + path;
    } else if (rc == -ENOTSUP) {
        err_msg = "Vulkan instance lacks vkGetPhysicalDeviceProperties2 chain";
    } else if (rc < 0) {
        err_msg = std::string("ww_bridge_vk_resolve_render_node: ")
                  + ::strerror(-rc);
    } else {
        err_msg = "ww_bridge_vk_resolve_render_node returned "
                  + std::to_string(rc);
    }
    return false;
}


// ---------------------------------------------------------------------------
// Host state shared between reader thread and main thread.
// ---------------------------------------------------------------------------

struct HostState {
    int                            sock { -1 };
    ww_pool_t*                     pool { nullptr };
    // Non-owning pointer; the unique_ptr lives inside VulkanRender.
    ww_wescene::BridgeExSwapchain* swapchain { nullptr };
    // Non-owning pointer to the SceneWallpaper that lives in main's
    // stack frame; the reader thread uses it to dispatch ApplySettings
    // hot-reload (volume / fps) into the looper.
    owe::SceneWallpaper*     wp { nullptr };

    // Render-target extent. Pointer events arrive in pixel coords from
    // the consumer display; 
    uint32_t width { 0 };
    uint32_t height { 0 };

    std::atomic<bool> shutdown { false };
};

void signal_shutdown(HostState& s) {
    s.shutdown.store(true, std::memory_order_release);
}

// Apply a single fps change through the same path WW_EVT_IN_SET_FPS would
// have used. Centralised so ApplySettings can route through it.
void set_fps(HostState& s, uint32_t fps) {
    if (!s.wp || fps == 0) return;
    s.wp->setPropertyInt32(owe::PROPERTY_FPS,
                           static_cast<int32_t>(fps));
}

void apply_control(HostState& s, ww_bridge_control_t& msg) {
    switch (msg.op) {
    case WW_EVT_IN_INIT:
        // Init is consumed at the top of main before the reader thread
        // starts. A late Init is either a buggy daemon resending or a
        // protocol violation; log and ignore.
        rstd_warn("waywallen-wescene-renderer: unexpected late Init; ignoring");
        break;
    case WW_EVT_IN_SETTING_CHANGED: {
        // v5 hot-reload. Peel the typed view, dispatch known keys
        // (volume) through the SceneWallpaper looper; route fps through
        // the same path WW_EVT_IN_SET_FPS used to. Unknown keys warn.
        ww_bridge_setting_changed_t as {};
        if (ww_bridge_setting_changed_from_control(&msg, &as) != 0) break;
        for (uint32_t i = 0; i < as.settings.count; ++i) {
            const char* key = as.settings.data[i].key;
            const char* val = as.settings.data[i].value;
            if (!key || !val) continue;
            if (std::strcmp(key, "volume") == 0) {
                if (s.wp) {
                    s.wp->setPropertyFloat(owe::PROPERTY_VOLUME,
                                           parse_f32(val, 1.0f));
                }
            } else if (std::strcmp(key, "fps") == 0) {
                char* end = nullptr;
                unsigned long n = std::strtoul(val, &end, 10);
                if (end != val) set_fps(s, static_cast<uint32_t>(n));
            } else if (std::strcmp(key, "test_pattern") == 0) {
                // Wescene's test_pattern flag is set on initial spawn
                // through PROPERTY_TEST_PATTERN; runtime toggling not
                // wired (would require respawn). Log and ignore.
            } else {
                rstd_warn("waywallen-wescene-renderer: ApplySettings: "
                          "wescene has no setting '{}'; ignoring",
                          static_cast<const char*>(key));
            }
        }
        ww_bridge_setting_changed_free(&as);
        break;
    }
    case WW_EVT_IN_PLAY:
    case WW_EVT_IN_PAUSE:
        break;
    case WW_EVT_IN_POINTER_MOTION: {
        ww_bridge_pointer_motion_t pm {};
        if (ww_bridge_pointer_motion_from_control(&msg, &pm) == 0 && s.wp
            && s.width > 0 && s.height > 0) {
            s.wp->mouseInput(static_cast<double>(pm.x) / s.width,
                             static_cast<double>(pm.y) / s.height);
        }
        break;
    }
    case WW_EVT_IN_POINTER_BUTTON:
    case WW_EVT_IN_POINTER_AXIS:
        break;
    case WW_EVT_IN_SET_FPS:
        set_fps(s, msg.u.set_fps.fps);
        break;
    case WW_EVT_IN_SHUTDOWN:
        signal_shutdown(s);
        break;
    case WW_EVT_IN_NEGOTIATE_BUFFERS: {
        const auto& nb = msg.u.negotiate_buffers;
        ww_pool_directive_t d {};
        d.category    = nb.path;
        d.mem_source  = nb.mem_source;
        d.fourcc      = nb.fourcc;
        d.modifier    = nb.modifier;
        d.plane_count = nb.plane_count;
        d.sync_mode   = nb.sync_mode;
        d.color       = nb.color;
        d.mem_hint    = nb.mem_hint;
        d.count       = nb.count > 0 ? nb.count : 3;
        if (d.count > ww_wescene::BridgeExSwapchain::kMaxSlots)
            d.count = ww_wescene::BridgeExSwapchain::kMaxSlots;
        // Hand off to the swapchain directly. The render thread drains
        // the pending directive at the head of its next acquireRenderTarget,
        // so this thread does no Vk / bridge slot work.
        if (s.swapchain) s.swapchain->queueDirective(d);
        break;
    }
    default:
        rstd_warn("waywallen-wescene-renderer: unknown control op {}",
                  static_cast<int>(msg.op));
        break;
    }
}

void reader_loop(HostState& s) {
    while (!s.shutdown.load(std::memory_order_acquire)) {
        ww_bridge_control_t msg {};
        int rc = ww_bridge_recv_control(s.sock, &msg);
        if (rc != 0) {
            if (!s.shutdown.load(std::memory_order_acquire)) {
                rstd_error("waywallen-wescene-renderer: recv_control failed: {}", rc);
            }
            signal_shutdown(s);
            return;
        }
        apply_control(s, msg);
        ww_bridge_control_free(&msg);
    }
}

} // namespace


// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    static rstd::log::EnvLogger _logger;
    rstd::log::set_logger(_logger);
    rstd::log::set_max_level(_logger.filter());

    Options opts = parse_args(argc, argv);

    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    HostState host;
    host.sock = ww_bridge_connect(opts.ipc_path.c_str());
    if (host.sock < 0)
        die("ww_bridge_connect: " + std::string(::strerror(-host.sock)));

    {
        ww_bridge_init_t init {};
        if (int rc = ww_bridge_recv_init(host.sock, &init); rc != 0) {
            const char* reason = (rc == -EPROTO)
                ? "init: protocol error or unsupported spawn_version"
                : "init: recv failed";
            ww_bridge_send_init_nack(host.sock, init.spawn_version,
                                     WW_BRIDGE_SUPPORTED_SPAWN_VERSION,
                                     reason);
            ww_bridge_init_free(&init);
            die(std::string(reason) + " rc=" + std::to_string(rc));
        }

        // SPAWN_VERSION 3: scene .pkg path + assets + workshop_id all
        // arrive via CLI argv (already parsed into opts.{initial_scene,
        // initial_assets, workshop_id}). Init carries only extent +
        // settings kv. Wescene scenes don't ship a fixed native
        // resolution (every scene is designed to scale), so we treat
        // the host's compiled-in `opts.{width,height}` defaults as
        // the "native" size that `ww_resolve_extent` resolves against.
        {
            uint32_t native_w = opts.width;
            uint32_t native_h = opts.height;
            ww_resolve_extent(init.extent_w, init.extent_h, init.extent_mode,
                              native_w, native_h,
                              &opts.width, &opts.height);
        }
        if (const char* v = kv_get(init.settings, "fps"); v && *v) {
            char* end = nullptr;
            unsigned long n = std::strtoul(v, &end, 10);
            if (end != v) opts.initial_fps = static_cast<uint32_t>(n);
        }
        if (const char* v = kv_get(init.settings, "test_pattern"); v && *v) {
            opts.test_pattern = (std::strcmp(v, "0") != 0);
        }
        opts.initial_volume = parse_f32(kv_get(init.settings, "volume"), 1.0f);
        // CLI `--render-node` wins over Init kv (mirroring mpv/video).
        // Empty ⇒ let SceneWallpaper pick the default Vulkan device.
        if (opts.render_node.empty()) {
            if (const char* v = kv_get(init.settings, "render_node");
                v && *v) {
                opts.render_node = v;
            }
        }

        ww_bridge_init_free(&init);
    }

    owe::SceneWallpaper wp;
    if (!wp.init()) die("SceneWallpaper::init failed");

    host.wp = &wp;
    host.width  = opts.width;
    host.height = opts.height;

    if (!opts.initial_assets.empty())
        wp.setPropertyString(owe::PROPERTY_ASSETS, opts.initial_assets);
    if (!opts.initial_scene.empty())
        wp.setPropertyString(owe::PROPERTY_SOURCE, opts.initial_scene);
    if (opts.initial_fps)
        wp.setPropertyInt32(owe::PROPERTY_FPS,
                            static_cast<int32_t>(opts.initial_fps));
    wp.setPropertyFloat(owe::PROPERTY_VOLUME, opts.initial_volume);

    // The factory runs inside VulkanRender::init after the GPU is picked
    // and the VkDevice is created; that's when ww_bridge_pool_create can
    // succeed. The factory captures `host` by reference; after init both
    // host.pool and host.swapchain point at live objects.
    auto factory =
        [&host](const owe::RenderInitInfo::ExSwapchainHandles& h)
            -> std::unique_ptr<owe::ExSwapchain> {
        ww_pool_vulkan_init_t pi {};
        pi.instance              = h.instance;
        pi.physical_device       = h.physical_device;
        pi.device                = h.device;
        pi.queue                 = h.graphics_queue;
        pi.queue_family_index    = h.graphics_queue_family;
        pi.get_instance_proc_addr =
            reinterpret_cast<void* (*)(void*, const char*)>(vkGetInstanceProcAddr);
        pi.device_uuid           = nullptr; // bridge will zero
        pi.driver_uuid           = nullptr;

        ww_bridge_vk_dt_t dt {};
        ww_bridge_vk_dt_load(&dt, vkGetInstanceProcAddr, h.instance);
        if (int rc = ww_bridge_vk_query_render_node(
                &dt, h.physical_device,
                &pi.drm_render_major, &pi.drm_render_minor);
            rc != 0) {
            rstd_warn("waywallen-wescene-renderer: drm render-node query failed ({}); "
                      "topology will be unknown to daemon", rc);
        }
        pi.drm_render_fd         = -1; // bridge opens by minor
        // FinPass uses vkCmdBlitImage to write the slot. 
        pi.image_usage_flags     = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        // BLIT_DST is required by vkCmdBlitImage at the modifier-feature
        // level and has no usage-flag analogue; 
        // try fix NVIDIA blank screen
        pi.format_feature_flags  = VK_FORMAT_FEATURE_TRANSFER_DST_BIT
                                 | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if (int rc = ww_bridge_pool_create(WW_POOL_BACKEND_VULKAN, &pi, &host.pool);
            rc != 0) {
            rstd_error("waywallen-wescene-renderer: ww_bridge_pool_create failed: {}", rc);
            return nullptr;
        }
        (void)h; // Vulkan handles no longer needed by BridgeExSwapchain.
        auto sw = std::make_unique<ww_wescene::BridgeExSwapchain>(
            host.pool, host.sock);
        host.swapchain = sw.get();
        return sw;
    };

    std::array<uint8_t, VK_UUID_SIZE> chosen_uuid {};
    bool have_uuid = false;
    if (!opts.render_node.empty()) {
        std::string err;
        if (resolve_render_node_to_uuid(opts.render_node, chosen_uuid, err)) {
            have_uuid = true;
            rstd_info("waywallen-wescene-renderer: render_node={} pinning Vulkan device by UUID",
                      opts.render_node);
        } else {
            rstd_warn("waywallen-wescene-renderer: render_node={} not honored: {}; "
                      "falling back to default device",
                      opts.render_node, err);
        }
    }

    {
        owe::RenderInitInfo info;
        info.offscreen        = true;
        info.offscreen_tiling = owe::TexTiling::OPTIMAL;
        info.width            = static_cast<uint16_t>(opts.width);
        info.height           = static_cast<uint16_t>(opts.height);
        info.surface_info.createSurfaceOp =
            [](VkInstance, VkSurfaceKHR*) -> VkResult { return VK_SUCCESS; };
        info.ex_swapchain_factory = std::move(factory);
        if (have_uuid) {
            info.uuid = std::span<const std::uint8_t>(chosen_uuid.data(),
                                                     chosen_uuid.size());
        }
        wp.initVulkan(std::move(info));
    }

    if (!wp.waitVulkanInited(/*timeout_ms*/ 10000))
        die("VulkanRender did not finish init within 10s");
    if (!host.pool || !host.swapchain)
        die("ex_swapchain_factory did not produce a pool / swapchain");

    host.swapchain->setOnFirstNegotiated([&] {
        wp.play();
        rstd_info("waywallen-wescene-renderer: negotiated, scene playback started");
    });

    // Bridge sends ready + release_syncobj + format_caps in one go.
    if (int rc = ww_bridge_pool_advertise_caps(host.pool, host.sock,
                                               opts.width, opts.height,
                                               WW_MEM_HINT_DEVICE_LOCAL
                                                   | WW_MEM_HINT_HOST_VISIBLE);
        rc != 0)
        die("ww_bridge_pool_advertise_caps failed: " + std::to_string(rc));

    rstd_info("waywallen-wescene-renderer: ready, advertise sent to daemon");

    std::thread reader([&]() { reader_loop(host); });

    // Idle until shutdown. All real work is on the render and reader
    // threads.
    while (!host.shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (reader.joinable()) {
        ::shutdown(host.sock, SHUT_RD);
        reader.join();
    }
    if (host.pool) ww_bridge_pool_destroy(host.pool);
    ww_bridge_close(host.sock);

    return 0;
}
