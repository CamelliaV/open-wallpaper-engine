// waywallen-weweb-renderer — CEF (Wallpaper Engine *web*) host
// subprocess.
//
// CEF spawns helper processes by re-execing the same binary with
// `--type=zygote` / `--type=renderer` / `--type=utility` switches. The
// `BrowserHost::RunOrExitIfHelper` call MUST run before any other
// initialisation so helpers early-exit without booting the bridge,
// argparse, or the producer Vulkan device.
//
// Frame path:
//   1. CEF's OnAcceleratedPaint hands us a DMA-BUF from the browser
//      compositor. The fd is borrowed; CEF reclaims when the callback
//      returns.
//   2. WebProducerDevice imports the dma-buf as a temp VkImage,
//      acquires the next bridge slot, blits into it, signals an
//      exportable sync_file semaphore, CPU-waits the blit fence, and
//      returns the sync_fd.
//   3. BridgeProducerCore::submitSlot forwards the sync_fd to
//      `ww_bridge_pool_submit_slot` — bridge emits frame_ready.
//   4. The temp VkImage + memory are destroyed. CEF's callback returns
//      and the dma-buf can be reclaimed safely.
//
// Settings hot-reload: WW_REQ_APPLY_SETTINGS arrives on the reader
// thread; the action is enqueued and drained on the main loop so all
// CEF API calls happen on the CEF UI thread.

#include "BridgeProducerCore.hpp"
#include "WebProducerDevice.hpp"
#include "BrowserHost.hpp"
#include "DmaBufFrame.hpp"
#include "Manifest.hpp"

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
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>

#include <nlohmann/json.hpp>

import rstd.log;

namespace
{

struct Options {
    std::string ipc_path;
    uint32_t    width  { 1280 };
    uint32_t    height { 720 };
    std::filesystem::path workshop_dir;
    std::string workshop_id;
    uint32_t    initial_fps { 60 };
    float       initial_volume { 1.0f };
    int         remote_debugging_port { 0 };
};

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "waywallen-weweb-renderer: %s\n", msg.c_str());
    std::exit(1);
}

Options parse_args(int argc, char** argv) {
    argparse::ArgumentParser program("waywallen-weweb-renderer");
    program.add_argument("--ipc")
        .required()
        .help("Unix-domain socket path for daemon IPC");
    program.add_argument("--path")
        .default_value(std::string {})
        .help("Workshop directory (containing project.json + index.html)");
    program.add_argument("--workshop_id")
        .default_value(std::string {})
        .help("Optional Steam workshop id (informational; used for cache dir)");
    program.add_argument("remaining").remaining();

    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::fprintf(stderr, "%s\n", err.what());
        std::exit(1);
    }

    Options o;
    o.ipc_path     = program.get<std::string>("--ipc");
    o.workshop_dir = program.get<std::string>("--path");
    o.workshop_id  = program.get<std::string>("--workshop_id");
    return o;
}

const char* kv_get(const ww_kv_list_t& kv, const char* key) {
    for (uint32_t i = 0; i < kv.count; ++i) {
        if (kv.data[i].key && std::strcmp(kv.data[i].key, key) == 0)
            return kv.data[i].value;
    }
    return nullptr;
}

float parse_f32(const char* s, float def) {
    if (!s || !*s) return def;
    char* end = nullptr;
    errno = 0;
    float v = std::strtof(s, &end);
    if (errno != 0 || end == s) return def;
    return v;
}

uint32_t parse_u32(const char* s, uint32_t def) {
    if (!s || !*s) return def;
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 10);
    if (end == s) return def;
    return static_cast<uint32_t>(v);
}

std::filesystem::path derive_cache_dir(const std::string& workshop_id) {
    namespace fs = std::filesystem;
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    fs::path base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !*home) return {};
        base = fs::path(home) / ".cache";
    }
    fs::path dir = base / "waywallen-weweb-renderer";
    if (!workshop_id.empty()) dir /= workshop_id;
    std::error_code ec;
    fs::create_directories(dir, ec); // best-effort
    return dir;
}

std::filesystem::path executable_dir(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !self.empty()) return self.parent_path();
    if (argv0) return fs::path(argv0).parent_path();
    return fs::current_path();
}

// --- Settings queue ---------------------------------------------------------
// Reader thread enqueues entries; main loop drains and applies onto the
// host (CEF UI thread). Keeps every CEF API call single-threaded.
struct SettingDelta {
    std::string key;
    std::string value;
};

struct HostState {
    int                                   sock { -1 };
    ww_pool_t*                            pool { nullptr };
    ww_wescene::BridgeProducerCore*       core { nullptr };
    weweb::BrowserHost*                   host { nullptr };

    std::mutex                            settings_mu;
    std::vector<SettingDelta>             pending_settings;

    std::atomic<bool> shutdown { false };
};

void enqueue_setting(HostState& s, std::string key, std::string val) {
    std::lock_guard<std::mutex> lk(s.settings_mu);
    s.pending_settings.push_back({ std::move(key), std::move(val) });
}

void drain_settings(HostState& s) {
    std::vector<SettingDelta> drained;
    {
        std::lock_guard<std::mutex> lk(s.settings_mu);
        drained.swap(s.pending_settings);
    }
    if (!s.host) return;
    for (auto& sd : drained) {
        if (sd.key == "volume") {
            s.host->ApplyVolume(parse_f32(sd.value.c_str(), 1.0f));
        } else if (sd.key == "fps") {
            uint32_t fps = parse_u32(sd.value.c_str(), 0);
            if (fps > 0) s.host->SetFrameRate(static_cast<int>(fps));
        } else {
            // Forward unknown keys to the page as a user-property patch.
            // Try parse as JSON first (so numbers / booleans / objects
            // round-trip); fall back to string.
            nlohmann::json v;
            auto parsed = nlohmann::json::parse(sd.value,
                                                /*cb*/nullptr,
                                                /*allow_exceptions*/false,
                                                /*ignore_comments*/true);
            if (parsed.is_discarded()) {
                v["value"] = sd.value;
            } else {
                v["value"] = parsed;
            }
            s.host->ApplyUserProperty(sd.key, v);
        }
    }
}

// --- Reader thread ----------------------------------------------------------

void apply_control(HostState& s, ww_bridge_control_t& msg) {
    switch (msg.op) {
    case WW_REQ_INIT:
        std::fprintf(stderr,
                     "waywallen-weweb-renderer: unexpected late Init; ignoring\n");
        break;
    case WW_REQ_APPLY_SETTINGS: {
        ww_bridge_apply_settings_t as {};
        if (ww_bridge_apply_settings_from_control(&msg, &as) != 0) break;
        for (uint32_t i = 0; i < as.settings.count; ++i) {
            const char* key = as.settings.data[i].key;
            const char* val = as.settings.data[i].value;
            if (!key || !val) continue;
            enqueue_setting(s, key, val);
        }
        ww_bridge_apply_settings_free(&as);
        break;
    }
    case WW_REQ_PLAY:
    case WW_REQ_PAUSE:
    case WW_REQ_MOUSE:
        break;
    case WW_REQ_SET_FPS:
        enqueue_setting(s, "fps", std::to_string(msg.u.set_fps.fps));
        break;
    case WW_REQ_SHUTDOWN:
        s.shutdown.store(true, std::memory_order_release);
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
        if (d.count > ww_wescene::BridgeProducerCore::kMaxSlots)
            d.count = ww_wescene::BridgeProducerCore::kMaxSlots;
        if (s.core) s.core->queueDirective(d);
        break;
    }
    default:
        std::fprintf(stderr,
                     "waywallen-weweb-renderer: unknown control op %d\n",
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
                             "waywallen-weweb-renderer: recv_control failed: %d\n",
                             rc);
            }
            s.shutdown.store(true, std::memory_order_release);
            return;
        }
        apply_control(s, msg);
        ww_bridge_control_free(&msg);
    }
}

} // namespace


int main(int argc, char** argv) {
    // CRITICAL: CEF re-execs this binary as helper procs. Must run
    // before argparse / logging / anything with side effects.
    weweb::BrowserHost host;
    if (int helper_exit = host.RunOrExitIfHelper(argc, argv);
        helper_exit >= 0) {
        return helper_exit;
    }

    static rstd::log::EnvLogger _logger;
    rstd::log::set_logger(_logger);
    rstd::log::set_max_level(_logger.filter());

    Options opts = parse_args(argc, argv);

    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    if (opts.workshop_dir.empty() ||
        !std::filesystem::is_directory(opts.workshop_dir)) {
        die("--path must be an existing workshop directory");
    }

    HostState state;
    state.sock = ww_bridge_connect(opts.ipc_path.c_str());
    if (state.sock < 0)
        die("ww_bridge_connect: " + std::string(std::strerror(-state.sock)));

    {
        ww_bridge_init_t init {};
        if (int rc = ww_bridge_recv_init(state.sock, &init); rc != 0) {
            const char* reason = (rc == -EPROTO)
                ? "init: protocol error or unsupported spawn_version"
                : "init: recv failed";
            ww_bridge_send_init_nack(state.sock, init.spawn_version,
                                     WW_BRIDGE_SUPPORTED_SPAWN_VERSION,
                                     reason);
            ww_bridge_init_free(&init);
            die(std::string(reason) + " rc=" + std::to_string(rc));
        }

        // Web wallpapers don't have a fixed native resolution — use the
        // hardcoded 1280×720 default as the "native" extent_resolve
        // argument so AUTO modes pick the host's preference.
        {
            uint32_t native_w = opts.width;
            uint32_t native_h = opts.height;
            ww_resolve_extent(init.extent_w, init.extent_h, init.extent_mode,
                              native_w, native_h,
                              &opts.width, &opts.height);
        }
        opts.initial_fps    = parse_u32(kv_get(init.settings, "fps"),
                                        opts.initial_fps);
        opts.initial_volume = parse_f32(kv_get(init.settings, "volume"),
                                        opts.initial_volume);
        opts.remote_debugging_port = static_cast<int>(parse_u32(
            kv_get(init.settings, "remote_debugging_port"), 0));

        ww_bridge_init_free(&init);
    }

    auto manifest_opt = weweb::LoadWebManifest(opts.workshop_dir);
    if (!manifest_opt) die("LoadWebManifest failed");
    auto& manifest = *manifest_opt;

    ww_wescene::WebProducerDevice producer;
    if (!producer.Init()) die("WebProducerDevice::Init failed");

    ww_pool_vulkan_init_t pi {};
    pi.instance               = producer.Instance();
    pi.physical_device        = producer.Physical();
    pi.device                 = producer.Device();
    pi.queue                  = producer.Queue();
    pi.queue_family_index     = producer.QueueFamily();
    pi.get_instance_proc_addr =
        reinterpret_cast<void* (*)(void*, const char*)>(vkGetInstanceProcAddr);
    pi.device_uuid            = producer.DeviceUuid();
    pi.driver_uuid            = producer.DriverUuid();
    pi.drm_render_major       = 0;
    pi.drm_render_minor       = 0;
    pi.drm_render_fd          = -1; // bridge opens its own
    pi.image_usage_flags      = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (int rc = ww_bridge_pool_create(WW_POOL_BACKEND_VULKAN, &pi, &state.pool);
        rc != 0)
        die("ww_bridge_pool_create failed: " + std::to_string(rc));

    ww_wescene::BridgeProducerCore core(state.pool, state.sock);
    state.core = &core;
    state.host = &host;

    core.setOnFirstNegotiated([&]() {
        // Apply the daemon's initial fps once slots are live. Set the
        // initial volume too — we couldn't before OpenWallpaper because
        // there was no browser yet, but we can post a property update
        // now and the listener will pick it up.
        if (opts.initial_fps > 0)
            host.SetFrameRate(static_cast<int>(opts.initial_fps));
        host.ApplyVolume(opts.initial_volume);
        std::fprintf(stderr,
                     "waywallen-weweb-renderer: negotiated, fps=%u volume=%.2f\n",
                     opts.initial_fps,
                     static_cast<double>(opts.initial_volume));
    });

    // BrowserHost::Init wants resources / locales relative to argv[0].
    auto exe_dir = executable_dir(argv[0]);
    weweb::BrowserHost::InitOptions ho;
    ho.resources_dir = exe_dir;
    ho.locales_dir   = exe_dir / "locales";
    ho.cache_dir     = derive_cache_dir(opts.workshop_id);
    if (opts.remote_debugging_port > 0) {
        ho.enable_remote_debugging = true;
        ho.remote_debugging_port   = opts.remote_debugging_port;
    }
    if (!host.Init(ho)) die("BrowserHost::Init failed");

    // OnAcceleratedPaint runs synchronously on the CEF UI thread (=
    // the thread that drives Pump, which is this main thread). Drain
    // any pending negotiate directive first so the slot pool reflects
    // the latest extent / fourcc before acquiring.
    host.SetAcceleratedPaintCallback(
        [&core, &producer](const weweb::DmaBufFrame& frame) {
            core.drainPendingDirective();
            if (!core.ready()) return;

            auto imp = producer.Import(frame);
            if (!imp.ok) return;

            VkImage  slot_image = VK_NULL_HANDLE;
            uint32_t slot_w = 0, slot_h = 0;
            if (!core.acquireSlot(&slot_image, &slot_w, &slot_h)) {
                producer.DestroyImported(imp);
                return;
            }
            int sync_fd = producer.BlitToSlot(imp, slot_image,
                                              { slot_w, slot_h });
            core.submitSlot(sync_fd);
            producer.DestroyImported(imp);
        });

    if (!host.OpenWallpaper(manifest, opts.workshop_dir,
                            static_cast<int>(opts.width),
                            static_cast<int>(opts.height))) {
        die("BrowserHost::OpenWallpaper failed");
    }

    if (int rc = ww_bridge_pool_advertise_caps(state.pool, state.sock,
                                               opts.width, opts.height,
                                               WW_MEM_HINT_DEVICE_LOCAL);
        rc != 0)
        die("ww_bridge_pool_advertise_caps failed: " + std::to_string(rc));

    std::fprintf(stderr,
                 "waywallen-weweb-renderer: ready, advertised caps %ux%u\n",
                 opts.width, opts.height);

    std::thread reader([&]() { reader_loop(state); });

    while (!state.shutdown.load(std::memory_order_acquire) &&
           !host.ShouldExit()) {
        drain_settings(state);
        host.Pump();
        // CEF's OSR pacing goes idle without explicit invalidate kicks
        // (see project memory: CEF 147 OSR + DMA-BUF). Dedup happens
        // inside CEF at windowless_frame_rate.
        host.Invalidate();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    state.shutdown.store(true, std::memory_order_release);
    if (reader.joinable()) {
        ::shutdown(state.sock, SHUT_RD);
        reader.join();
    }
    host.Shutdown();
    if (state.pool) ww_bridge_pool_destroy(state.pool);
    ww_bridge_close(state.sock);
    return 0;
}
