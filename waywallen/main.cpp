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

#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"

#include <waywallen-bridge/bridge.h>
#include <waywallen-bridge/pool.h>
#include <waywallen-bridge/protocol_bits.h>

#include <argparse/argparse.hpp>

#include <atomic>
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
};

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "waywallen-wescene-renderer: %s\n", msg.c_str());
    std::exit(1);
}

Options parse_args(int argc, char** argv) {
    argparse::ArgumentParser program("waywallen-wescene-renderer");

    program.add_argument("--ipc")
        .required()
        .help("Unix-domain socket path for daemon IPC");
    program.add_argument("--width")
        .default_value(1280u)
        .help("render width")
        .scan<'u', uint32_t>();
    program.add_argument("--height")
        .default_value(720u)
        .help("render height")
        .scan<'u', uint32_t>();
    program.add_argument("--scene")
        .default_value(std::string())
        .help("initial scene pkg path");
    program.add_argument("--assets")
        .default_value(std::string())
        .help("initial assets directory");
    program.add_argument("--workshop_id")
        .default_value(std::string())
        .help("Workshop item ID (forwarded from source plugin metadata)");
    program.add_argument("--fps")
        .default_value(30u)
        .help("target frames per second")
        .scan<'u', uint32_t>();
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
    o.width          = program.get<uint32_t>("--width");
    o.height         = program.get<uint32_t>("--height");
    o.initial_scene  = program.get<std::string>("--scene");
    o.initial_assets = program.get<std::string>("--assets");
    o.workshop_id    = program.get<std::string>("--workshop_id");
    o.initial_fps    = program.get<uint32_t>("--fps");
    return o;
}


// ---------------------------------------------------------------------------
// Host state shared between reader thread and main thread.
// ---------------------------------------------------------------------------

struct HostState {
    int                            sock { -1 };
    ww_pool_t*                     pool { nullptr };
    // Non-owning pointer; the unique_ptr lives inside VulkanRender.
    ww_wescene::BridgeExSwapchain* swapchain { nullptr };

    std::atomic<bool> shutdown { false };
};

void signal_shutdown(HostState& s) {
    s.shutdown.store(true, std::memory_order_release);
}

void apply_control(HostState& s, const ww_bridge_control_t& msg) {
    switch (msg.op) {
    case WW_REQ_HELLO:
    case WW_REQ_LOAD_SCENE:
    case WW_REQ_PLAY:
    case WW_REQ_PAUSE:
    case WW_REQ_MOUSE:
    case WW_REQ_SET_FPS:
        // Iter 1: routed through the daemon's higher-level control API
        // (DBus/WebSocket) rather than through this subprocess. The
        // initial scene/assets/fps are forwarded as launch flags.
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
        d.width       = nb.extent_w;
        d.height      = nb.extent_h;
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

    wallpaper::SceneWallpaper wp;
    if (!wp.init()) die("SceneWallpaper::init failed");

    if (!opts.initial_assets.empty())
        wp.setPropertyString(wallpaper::PROPERTY_ASSETS, opts.initial_assets);
    if (!opts.initial_scene.empty())
        wp.setPropertyString(wallpaper::PROPERTY_SOURCE, opts.initial_scene);
    if (opts.initial_fps)
        wp.setPropertyInt32(wallpaper::PROPERTY_FPS,
                            static_cast<int32_t>(opts.initial_fps));

    HostState host;
    host.sock = ww_bridge_connect(opts.ipc_path.c_str());
    if (host.sock < 0)
        die("ww_bridge_connect: " + std::string(std::strerror(-host.sock)));

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
        // FinPass uses vkCmdBlitImage to write the slot — needs
        // TRANSFER_DST. Keeping TRANSFER_SRC matches the consumer's
        // shadow-import usage so the modifier sub-layouts align. (Same
        // as bridge's default; passed explicitly to document intent.)
        pi.image_usage_flags     = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
