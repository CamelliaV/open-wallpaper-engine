// waywallen-wescene-renderer — wescene (Wallpaper Engine scene) host
// subprocess.
//
// Speaks ipc-v3 through the waywallen-bridge pool API:
//   1. Connect to the daemon's Unix-domain socket (`--ipc <path>`).
//   2. wp.init() spins up SceneWallpaper's main + render loopers.
//   3. wp.initVulkan(...) is called with `ex_swapchain_factory` set.
//      VulkanRender picks the GPU, creates the VkDevice, then invokes
//      the factory: the factory creates the bridge pool and a
//      BridgeExSwapchain that VulkanRender adopts as its offscreen
//      target.
//   4. ww_bridge_pool_advertise_caps. Bridge sends ready,
//      release_syncobj, and format_caps to the daemon.
//   5. A reader thread decodes daemon → host messages. NEGOTIATE_BUFFERS
//      goes to the main thread, which calls
//      `BridgeExSwapchain::applyDirective`. Bridge then emits
//      bind_buffers; first directive triggers wp.play().
//   6. Each frame the SceneWallpaper render loop calls drawFrameOffscreen,
//      which pulls a slot from the BridgeExSwapchain, records, exports
//      a sync_fd, and submits — bridge emits frame_ready under the hood.
//   7. prctl(PR_SET_PDEATHSIG) so the renderer dies with the daemon.

#include "BridgeExSwapchain.hpp"

#include <waywallen-bridge/bridge.h>
#include <waywallen-bridge/extent_resolve.h>
#include <waywallen-bridge/pool.h>
#include <waywallen-bridge/protocol_bits.h>

#include <argparse/argparse.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <thread>
#include <vulkan/vulkan.h>

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
};

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "waywallen-wescene-renderer: %s\n", msg.c_str());
    std::exit(1);
}

// Step 3 of the renderer-Init refactor: spawn-time params come from
// the daemon's typed `Init` message; the only CLI flag the renderer
// honours is `--ipc <socket>`. The argparse `remaining()` catch-all
// is kept (but ignored) so any straggling daemon argv during the
// transition window is silently consumed instead of failing the parse.
// SPAWN_VERSION 3: scene .pkg path arrives as `--path`, with
// plugin-specific extras `--assets <dir>` and `--workshop_id <id>` from
// the wescene manifest's whitelist. argparse's `remaining()` catches
// any other unknown flags and silently ignores them.
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
    program.add_argument("remaining").remaining();

    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        std::exit(1);
    }

    Options o;
    o.ipc_path       = program.get<std::string>("--ipc");
    o.initial_scene  = program.get<std::string>("--path");
    o.initial_assets = program.get<std::string>("--assets");
    o.workshop_id    = program.get<std::string>("--workshop_id");
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
    float v = std::strtof(s, &end);
    if (errno != 0 || end == s) return def;
    return v;
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
    wallpaper::SceneWallpaper*     wp { nullptr };

    std::atomic<bool> shutdown { false };
};

void signal_shutdown(HostState& s) {
    s.shutdown.store(true, std::memory_order_release);
}

// Apply a single fps change through the same path WW_REQ_SET_FPS would
// have used. Centralised so ApplySettings can route through it.
void set_fps(HostState& s, uint32_t fps) {
    if (!s.wp || fps == 0) return;
    s.wp->setPropertyInt32(wallpaper::PROPERTY_FPS,
                           static_cast<int32_t>(fps));
}

void apply_control(HostState& s, ww_bridge_control_t& msg) {
    switch (msg.op) {
    case WW_REQ_INIT:
        // Init is consumed at the top of main before the reader thread
        // starts. A late Init is either a buggy daemon resending or a
        // protocol violation; log and ignore.
        std::fprintf(stderr,
                     "waywallen-wescene-renderer: unexpected late Init; ignoring\n");
        break;
    case WW_REQ_APPLY_SETTINGS: {
        // v5 hot-reload. Peel the typed view, dispatch known keys
        // (volume) through the SceneWallpaper looper; route fps through
        // the same path WW_REQ_SET_FPS used to. Unknown keys warn.
        ww_bridge_apply_settings_t as {};
        if (ww_bridge_apply_settings_from_control(&msg, &as) != 0) break;
        for (uint32_t i = 0; i < as.settings.count; ++i) {
            const char* key = as.settings.data[i].key;
            const char* val = as.settings.data[i].value;
            if (!key || !val) continue;
            if (std::strcmp(key, "volume") == 0) {
                if (s.wp) {
                    s.wp->setPropertyFloat(wallpaper::PROPERTY_VOLUME,
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
                std::fprintf(stderr,
                             "waywallen-wescene-renderer: ApplySettings: "
                             "wescene has no setting '%s'; ignoring\n",
                             key);
            }
        }
        ww_bridge_apply_settings_free(&as);
        break;
    }
    case WW_REQ_PLAY:
    case WW_REQ_PAUSE:
    case WW_REQ_MOUSE:
        // Iter 1: routed through the daemon's higher-level control API
        // (DBus/WebSocket) rather than through this subprocess.
        break;
    case WW_REQ_SET_FPS:
        set_fps(s, msg.u.set_fps.fps);
        break;
    case WW_REQ_SHUTDOWN:
        signal_shutdown(s);
        break;
    case WW_REQ_NEGOTIATE_BUFFERS: {
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
        std::fprintf(stderr,
                     "waywallen-wescene-renderer: unknown control op %d\n",
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
                std::fprintf(stderr,
                             "waywallen-wescene-renderer: recv_control failed: %d\n",
                             rc);
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
    Options opts = parse_args(argc, argv);

    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    // Step 3 of the renderer-Init refactor: connect first, then
    // recv Init and use its typed fields to populate Options before
    // any GPU init or scene-property writes. The wescene renderer
    // already had connect() before initVulkan(); we just slot Init
    // recv right after the connect.
    HostState host;
    host.sock = ww_bridge_connect(opts.ipc_path.c_str());
    if (host.sock < 0)
        die("ww_bridge_connect: " + std::string(std::strerror(-host.sock)));

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

        ww_bridge_init_free(&init);
    }

    wallpaper::SceneWallpaper wp;
    if (!wp.init()) die("SceneWallpaper::init failed");

    host.wp = &wp;

    if (!opts.initial_assets.empty())
        wp.setPropertyString(wallpaper::PROPERTY_ASSETS, opts.initial_assets);
    if (!opts.initial_scene.empty())
        wp.setPropertyString(wallpaper::PROPERTY_SOURCE, opts.initial_scene);
    if (opts.initial_fps)
        wp.setPropertyInt32(wallpaper::PROPERTY_FPS,
                            static_cast<int32_t>(opts.initial_fps));
    wp.setPropertyFloat(wallpaper::PROPERTY_VOLUME, opts.initial_volume);

    // The factory runs inside VulkanRender::init after the GPU is picked
    // and the VkDevice is created; that's when ww_bridge_pool_create can
    // succeed. The factory captures `host` by reference; after init both
    // host.pool and host.swapchain point at live objects.
    auto factory =
        [&host](const wallpaper::RenderInitInfo::ExSwapchainHandles& h)
            -> std::unique_ptr<wallpaper::ExSwapchain> {
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
        pi.drm_render_major      = 0;
        pi.drm_render_minor      = 0;
        pi.drm_render_fd         = -1; // bridge opens its own
        // FinPass uses vkCmdBlitImage to write the slot — needs only
        // TRANSFER_DST. Bridge OR-s in TRANSFER_SRC unconditionally so
        // the consumer-side shadow-import sub-layout matches.
        pi.image_usage_flags     = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (int rc = ww_bridge_pool_create(WW_POOL_BACKEND_VULKAN, &pi, &host.pool);
            rc != 0) {
            std::fprintf(stderr,
                         "waywallen-wescene-renderer: ww_bridge_pool_create failed: %d\n", rc);
            return nullptr;
        }
        (void)h; // Vulkan handles no longer needed by BridgeExSwapchain.
        auto sw = std::make_unique<ww_wescene::BridgeExSwapchain>(
            host.pool, host.sock);
        host.swapchain = sw.get();
        return sw;
    };

    {
        wallpaper::RenderInitInfo info;
        info.offscreen        = true;
        info.offscreen_tiling = wallpaper::TexTiling::OPTIMAL;
        info.width            = static_cast<uint16_t>(opts.width);
        info.height           = static_cast<uint16_t>(opts.height);
        info.surface_info.createSurfaceOp =
            [](VkInstance, VkSurfaceKHR*) -> VkResult { return VK_SUCCESS; };
        info.ex_swapchain_factory = std::move(factory);
        wp.initVulkan(std::move(info));
    }

    if (!wp.waitVulkanInited(/*timeout_ms*/ 10000))
        die("VulkanRender did not finish init within 10s");
    if (!host.pool || !host.swapchain)
        die("ex_swapchain_factory did not produce a pool / swapchain");

    // Fired from the render thread the first time the swapchain
    // successfully applies a directive — at that point slot views are
    // live and drawFrameOffscreen will start emitting frame_ready. wp.play
    // is itself a looper post, so re-entering from the render thread is
    // fine.
    host.swapchain->setOnFirstNegotiated([&] {
        wp.play();
        std::fprintf(stderr,
                     "waywallen-wescene-renderer: negotiated, scene playback started\n");
    });

    // Bridge sends ready + release_syncobj + format_caps in one go.
    if (int rc = ww_bridge_pool_advertise_caps(host.pool, host.sock,
                                               opts.width, opts.height,
                                               WW_MEM_HINT_DEVICE_LOCAL
                                                   | WW_MEM_HINT_HOST_VISIBLE);
        rc != 0)
        die("ww_bridge_pool_advertise_caps failed: " + std::to_string(rc));

    uint32_t drm_render_major = 0, drm_render_minor = 0;
    (void)wp.getDrmRenderNode(drm_render_major, drm_render_minor);
    std::fprintf(stderr,
                 "waywallen-wescene-renderer: ready, advertised caps drm_render=%u:%u\n",
                 drm_render_major, drm_render_minor);

    // Reader thread receives daemon → host messages and pushes any
    // NEGOTIATE_BUFFERS directly onto the swapchain. The render thread
    // (RenderHandler's looper) drains pending directives at the head of
    // each acquireRenderTarget, so all slot/view lifetime is single-
    // threaded.
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
