module;

#include <rstd/macro.hpp>

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <sys/prctl.h>
#include <sys/socket.h>

module waywallen.web_entry;

import rstd.argparse;
import rstd.cppstd;
import rstd.log;
import owe.user_property;
import wescene.cli;
import wescene.json;
import vulkan;
import weweb;
import waywallen.bridge;
import waywallen.bridge_producer_core;
import waywallen.web_producer_device;

namespace
{

struct Options {
    std::string           ipc_path;
    uint32_t              width { 1920 };
    uint32_t              height { 1080 };
    std::filesystem::path workshop_dir;
    std::string           workshop_id;
    uint32_t              initial_fps { 60 };
    float                 initial_volume { 1.0f };
    int                   remote_debugging_port { 0 };
    bool                  enable_audio { true };
    bool                  shared_texture_enabled { true };
    std::string           render_node;
    rstd::json::Map       initial_user_properties;
};

[[noreturn]] void die(const std::string& msg) {
    rstd_error("waywallen-weweb-renderer: {}", msg);
    std::exit(1);
}

std::string ToStdString(const rstd::string::String& value) {
    return { value.data(), value.size() };
}

template<typename T>
const T& ArgValue(const rstd::argparse::Matches& matches, const rstd::argparse::ArgKey<T>& key) {
    auto value = matches.get_one(key);
    if (value.is_err() || value->is_none()) rstd::unreachable();
    return ***value;
}

Options parse_args(int argc, char** argv) {
    using namespace rstd::argparse;

    auto command = Command::make("waywallen-weweb-renderer");
    auto ipc     = command.add_arg(Arg<rstd::string::String>::value("ipc", string_parser())
                                       .long_name("ipc")
                                       .help("Unix-domain socket path for daemon IPC")
                                       .required());
    auto path =
        command.add_arg(Arg<rstd::string::String>::value("path", string_parser())
                            .long_name("path")
                            .help("Workshop directory (containing project.json + index.html)")
                            .default_value(""));
    auto workshop_id =
        command.add_arg(Arg<rstd::string::String>::value("workshop_id", string_parser())
                            .long_name("workshop_id")
                            .help("Optional Steam workshop id (informational; used for cache dir)")
                            .default_value(""));
    auto render_node =
        command.add_arg(Arg<rstd::string::String>::value("render-node", string_parser())
                            .long_name("render-node")
                            .help("DRM render-node path to pin Vulkan/CEF GPU selection to "
                                  "(empty => let the renderer pick the default)")
                            .default_value(""));
    command.add_arg(Arg<rstd::string::String>::value("remaining", string_parser())
                        .num_args(NumArgs::any())
                        .allow_hyphen_values());

    auto parsed = owe::cli::ParseArgs(rstd::move(command), argc, argv);
    if (parsed.is_err()) std::exit(parsed.unwrap_err().code);
    auto matches = rstd::move(parsed).unwrap();

    Options options;
    options.ipc_path     = ToStdString(ArgValue(matches, ipc));
    options.workshop_dir = ToStdString(ArgValue(matches, path));
    options.workshop_id  = ToStdString(ArgValue(matches, workshop_id));
    options.render_node  = ToStdString(ArgValue(matches, render_node));
    return options;
}

const char* kv_get(const ww_kv_list_t& kv, const char* key) {
    for (uint32_t i = 0; i < kv.count; ++i) {
        if (kv.data[i].key && std::strcmp(kv.data[i].key, key) == 0) return kv.data[i].value;
    }
    return nullptr;
}

float parse_f32(const char* s, float def) {
    if (! s || ! *s) return def;
    char* end = nullptr;
    errno     = 0;
    double v  = std::strtod(s, &end);
    if (errno != 0 || end == s) return def;
    return static_cast<float>(v);
}

// Daemon serializes bool settings as the literal "true"/"false".
bool parse_bool(const char* s, bool def) {
    if (! s || ! *s) return def;
    if (std::strcmp(s, "true") == 0) return true;
    if (std::strcmp(s, "false") == 0) return false;
    return def;
}

uint32_t parse_u32(const char* s, uint32_t def) {
    if (! s || ! *s) return def;
    char*         end = nullptr;
    unsigned long v   = std::strtoul(s, &end, 10);
    if (end == s) return def;
    return static_cast<uint32_t>(v);
}

int32_t parse_i32(const char* s, int32_t def) {
    if (! s || ! *s) return def;
    char* end = nullptr;
    errno     = 0;
    long v    = std::strtol(s, &end, 10);
    if (errno != 0 || end == s) return def;
    return static_cast<int32_t>(v);
}

std::filesystem::path derive_cache_dir(const std::string& workshop_id) {
    namespace fs    = std::filesystem;
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    fs::path    base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (! home || ! *home) return {};
        base = fs::path(home) / ".cache";
    }
    fs::path dir = base / "waywallen-weweb-renderer";
    if (! workshop_id.empty()) dir /= workshop_id;
    std::error_code ec;
    fs::create_directories(dir, ec); // best-effort
    return dir;
}

std::filesystem::path executable_dir(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto            self = fs::read_symlink("/proc/self/exe", ec);
    if (! ec && ! self.empty()) return self.parent_path();
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

constexpr std::string_view kRuntimeMuteKey = "__waywallen_runtime_mute";

struct HostState {
    int                             sock { -1 };
    ww_wescene::BridgeProducerCore* core { nullptr };
    weweb::BrowserHost*             host { nullptr };
    owe::Json*                      user_properties { nullptr };

    std::mutex                settings_mu;
    std::vector<SettingDelta> pending_settings;

    std::atomic<bool>     shutdown { false };
    std::atomic<bool>     paused { false };
    std::atomic<bool>     submitted_since_negotiate { false };
    std::atomic<uint32_t> target_fps { 60 };
    bool                  audio_enabled { true };
    float                 base_volume { 1.0f };
    bool                  muted { false };

    std::mutex                                                subscription_mu;
    std::shared_ptr<ww_wescene::BridgeSubscriptionController> subscriptions;
    std::atomic<bool>                                         audio_response_demand { false };
    std::mutex                                                audio_mu;
    std::array<float, 64>                                     audio_left {};
    std::array<float, 64>                                     audio_right {};
    std::chrono::steady_clock::time_point                     audio_received {};
    bool                                                      audio_primed { false };
    uint64_t                                                  last_audio_generation { 0 };
    uint64_t                                                  last_audio_sequence { 0 };

    // Tracked locally on the reader thread so OnMouseMove can carry
    // the left-button-down flag CEF expects in modifiers (GLFW path
    // does the same — there's no protocol field for it).
    bool left_down { false };
};

void set_audio_response_demand(HostState& s, bool active) {
    s.audio_response_demand.store(active, std::memory_order_release);
    {
        std::scoped_lock lock(s.audio_mu);
        if (! active) {
            s.audio_primed = false;
            s.audio_left.fill(0.0f);
            s.audio_right.fill(0.0f);
        }
    }
    std::shared_ptr<ww_wescene::BridgeSubscriptionController> subscriptions;
    {
        std::scoped_lock lock(s.subscription_mu);
        subscriptions = s.subscriptions;
    }
    if (subscriptions && ! subscriptions->set("audio", active)) {
        rstd_warn("waywallen-weweb-renderer: failed to update audio subscription");
    }
}

// Linux input-event-code → CEF cef_mouse_button_type_t. -1 for codes
// CEF can't represent (BTN_SIDE, BTN_EXTRA, …).
int cef_button_from_linux(uint32_t btn) {
    switch (btn) {
    case 0x110: return 0; // BTN_LEFT  -> MBT_LEFT
    case 0x111: return 2; // BTN_RIGHT -> MBT_RIGHT
    case 0x112: return 1; // BTN_MIDDLE-> MBT_MIDDLE
    default: return -1;
    }
}

void enqueue_setting(HostState& s, std::string key, std::string val) {
    std::lock_guard<std::mutex> lk(s.settings_mu);
    s.pending_settings.push_back({ std::move(key), std::move(val) });
}

float effective_volume(const HostState& s) {
    if (! s.audio_enabled || s.muted) return 0.0f;
    return std::clamp(s.base_volume, 0.0f, 1.0f);
}

void apply_effective_volume(HostState& s) {
    if (s.host) s.host->ApplyVolume(effective_volume(s));
}

void merge_user_property_overrides(owe::Json& properties, const rstd::json::Map& overrides) {
    if (! properties.is_object()) properties = owe::Json::Object(rstd::json::Map::make());
    overrides.iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        auto key                      = rstd::cppstd::as_string_view(entry_key->as_str());
        auto current                  = properties.get(key);
        auto descriptor =
            current.is_some()
                ? owe::MergeUserPropertyDescriptor(**current, *entry_value)
                : owe::MergeUserPropertyDescriptor(owe::JsonFromStd(""), *entry_value);
        auto object = properties.as_object_mut();
        (*object)->insert(entry_key->clone(), std::move(descriptor));
    });
}

void drain_settings(HostState& s) {
    std::vector<SettingDelta> drained;
    {
        std::lock_guard<std::mutex> lk(s.settings_mu);
        drained.swap(s.pending_settings);
    }
    if (! s.host) return;
    for (auto& sd : drained) {
        if (sd.key == "volume") {
            // Wire format is u32 0..100; CEF host takes 0..1 ratio.
            s.base_volume = parse_f32(sd.value.c_str(), 100.0f) / 100.0f;
            apply_effective_volume(s);
        } else if (sd.key == kRuntimeMuteKey) {
            s.muted = parse_bool(sd.value.c_str(), false);
            apply_effective_volume(s);
        } else if (sd.key == "fps") {
            uint32_t fps = parse_u32(sd.value.c_str(), 0);
            if (fps > 0) {
                s.target_fps.store(fps, std::memory_order_release);
                s.host->SetFrameRate(static_cast<int>(fps));
            }
        } else {
            auto patch      = owe::MakeUserPropertyWirePatch(sd.value);
            auto descriptor = patch.clone();
            if (s.user_properties) {
                auto current = s.user_properties->get(sd.key);
                if (current.is_some())
                    descriptor = owe::MergeUserPropertyDescriptor(**current, patch);
                auto object = s.user_properties->as_object_mut();
                if (object.is_some()) {
                    (*object)->insert(::alloc::string::String::make(rstd::cppstd::as_str(sd.key)),
                                      descriptor.clone());
                }
            }
            s.host->ApplyUserProperty(sd.key, descriptor);
        }
    }
}

void sync_pause_visibility(HostState& s) {
    if (! s.host) return;
    const bool hide = s.paused.load(std::memory_order_acquire) &&
                      s.submitted_since_negotiate.load(std::memory_order_acquire);
    s.host->SetPaused(hide);
    if (! hide) s.host->Invalidate();
}

std::chrono::microseconds frame_delay(const HostState& s) {
    uint32_t fps = s.target_fps.load(std::memory_order_acquire);
    if (fps == 0) fps = 60;
    if (fps > 240) fps = 240;
    return std::chrono::microseconds(1000000u / fps);
}

template<typename RenderToSlot>
void submit_bridge_slot(HostState& s, ww_wescene::BridgeProducerCore& core,
                        RenderToSlot&& render_to_slot) {
    VkImage  slot_image = VK_NULL_HANDLE;
    uint32_t slot_w = 0, slot_h = 0;
    if (! core.acquireSlot(&slot_image, &slot_w, &slot_h)) return;

    int sync_fd = render_to_slot(slot_image, VkExtent2D { slot_w, slot_h }, core.format());
    core.submitSlot(sync_fd);
    if (sync_fd < 0) return;

    s.submitted_since_negotiate.store(true, std::memory_order_release);
    if (s.paused.load(std::memory_order_acquire) && s.host) s.host->SetPaused(true);
}

// --- Reader thread ----------------------------------------------------------

void apply_control(HostState& s, ww_bridge_control_t& msg) {
    switch (msg.op) {
    case WW_EVT_IN_INIT:
        rstd_warn("waywallen-weweb-renderer: unexpected late Init; ignoring");
        break;
    case WW_EVT_IN_SETTING_CHANGED: {
        ww_bridge_setting_changed_t as {};
        if (ww_bridge_setting_changed_from_control(&msg, &as) != 0) break;
        for (uint32_t i = 0; i < as.settings.count; ++i) {
            const char* key = as.settings.data[i].key;
            const char* val = as.settings.data[i].value;
            if (! key || ! val) continue;
            enqueue_setting(s, key, val);
        }
        ww_bridge_setting_changed_free(&as);
        break;
    }
    case WW_EVT_IN_PLAY:
        s.paused.store(false, std::memory_order_release);
        sync_pause_visibility(s);
        break;
    case WW_EVT_IN_PAUSE:
        s.paused.store(true, std::memory_order_release);
        sync_pause_visibility(s);
        break;
    case WW_EVT_IN_MUTE: enqueue_setting(s, std::string(kRuntimeMuteKey), "true"); break;
    case WW_EVT_IN_UNMUTE: enqueue_setting(s, std::string(kRuntimeMuteKey), "false"); break;
    case WW_EVT_IN_POINTER_MOTION: {
        // Daemon transforms display-local coords into renderer-tex
        // pixel space before sending; CEF view rect is opened at the
        // same pixel size, so the values map 1:1.
        ww_bridge_pointer_motion_t pm {};
        if (ww_bridge_pointer_motion_from_control(&msg, &pm) == 0 && s.host) {
            s.host->OnMouseMove(static_cast<int>(pm.x), static_cast<int>(pm.y), s.left_down);
        }
        break;
    }
    case WW_EVT_IN_POINTER_BUTTON: {
        ww_bridge_pointer_button_t pb {};
        if (ww_bridge_pointer_button_from_control(&msg, &pb) == 0 && s.host) {
            int cef_btn = cef_button_from_linux(pb.button);
            if (cef_btn >= 0) {
                bool down = pb.state != 0;
                if (cef_btn == 0) s.left_down = down;
                s.host->OnMouseButton(static_cast<int>(pb.x),
                                      static_cast<int>(pb.y),
                                      cef_btn,
                                      down,
                                      /*click_count=*/1);
            }
        }
        break;
    }
    case WW_EVT_IN_POINTER_AXIS: {
        ww_bridge_pointer_axis_t pa {};
        if (ww_bridge_pointer_axis_from_control(&msg, &pa) == 0 && s.host) {
            // delta_* arrives in "logical notches" (1.0 per wheel
            // click). CEF wants pixel-ish deltas; 40 px/notch matches
            // the GLFW WebViewer convention.
            constexpr float kPxPerNotch = 40.0f;
            s.host->OnMouseWheel(static_cast<int>(pa.x),
                                 static_cast<int>(pa.y),
                                 static_cast<int>(pa.delta_x * kPxPerNotch),
                                 static_cast<int>(pa.delta_y * kPxPerNotch));
        }
        break;
    }
    case WW_EVT_IN_EVENT_SUBSCRIPTIONS_APPLIED: {
        ww_bridge_event_subscriptions_applied_t applied {};
        if (ww_bridge_event_subscriptions_applied_from_control(&msg, &applied) == 0) {
            std::shared_ptr<ww_wescene::BridgeSubscriptionController> subscriptions;
            {
                std::scoped_lock lock(s.subscription_mu);
                subscriptions = s.subscriptions;
            }
            if (subscriptions) subscriptions->applied(applied);
        }
        ww_bridge_event_subscriptions_applied_free(&applied);
        break;
    }
    case WW_EVT_IN_AUDIO_SPECTRUM: {
        ww_bridge_audio_spectrum_t audio {};
        if (ww_bridge_audio_spectrum_from_control(&msg, &audio) != 0) break;
        if (! s.audio_response_demand.load(std::memory_order_acquire)) break;
        std::shared_ptr<ww_wescene::BridgeSubscriptionController> subscriptions;
        {
            std::scoped_lock lock(s.subscription_mu);
            subscriptions = s.subscriptions;
        }
        if (! subscriptions || ! subscriptions->acceptsAudio(audio.subscription_revision)) break;
        if (audio.generation < s.last_audio_generation ||
            (audio.generation == s.last_audio_generation &&
             audio.sequence <= s.last_audio_sequence))
            break;
        std::scoped_lock lock(s.audio_mu);
        std::copy_n(audio.left, s.audio_left.size(), s.audio_left.begin());
        std::copy_n(audio.right, s.audio_right.size(), s.audio_right.begin());
        s.audio_received        = std::chrono::steady_clock::now();
        s.audio_primed          = true;
        s.last_audio_generation = audio.generation;
        s.last_audio_sequence   = audio.sequence;
        break;
    }
    case WW_EVT_IN_SET_FPS: enqueue_setting(s, "fps", std::to_string(msg.u.set_fps.fps)); break;
    case WW_EVT_IN_SHUTDOWN: s.shutdown.store(true, std::memory_order_release); break;
    case WW_EVT_IN_NEGOTIATE_BUFFERS: {
        const auto&         nb = msg.u.negotiate_buffers;
        ww_pool_directive_t d {};
        d.category    = nb.path;
        d.mem_source  = nb.mem_source;
        d.fourcc      = nb.fourcc;
        d.modifier    = nb.modifier;
        d.plane_count = nb.plane_count;
        d.sync_mode   = nb.sync_mode;
        d.color       = nb.color;
        d.mem_hint    = nb.mem_hint;
        d.count       = nb.count;
        if (s.core) s.core->queueDirective(d);
        s.submitted_since_negotiate.store(false, std::memory_order_release);
        sync_pause_visibility(s);
        break;
    }
    default:
        rstd_warn("waywallen-weweb-renderer: unknown control op {}", static_cast<int>(msg.op));
        break;
    }
}

void reader_loop(HostState& s) {
    while (! s.shutdown.load(std::memory_order_acquire)) {
        ww_bridge_control_t msg {};
        int                 rc = ww_bridge_recv_control(s.sock, &msg);
        if (rc != 0) {
            if (! s.shutdown.load(std::memory_order_acquire)) {
                rstd_error("waywallen-weweb-renderer: recv_control failed: {}", rc);
            }
            s.shutdown.store(true, std::memory_order_release);
            return;
        }
        apply_control(s, msg);
        ww_bridge_control_free(&msg);
    }
}

} // namespace

namespace waywallen
{
int run(int argc, char** argv) {
    // CRITICAL: CEF re-execs this binary as helper procs. Must run
    // before argparse / logging / anything with side effects.
    weweb::BrowserHost host;
    if (int helper_exit = host.RunOrExitIfHelper(argc, argv); helper_exit >= 0) {
        return helper_exit;
    }

    static rstd::log::EnvLogger _logger;
    rstd::log::set_logger(_logger);
    rstd::log::set_max_level(_logger.filter());

    ww_bridge_set_log_callback(
        [](ww_bridge_log_level_t level, const char* msg, void*) {
            constexpr rstd::log::Level kMap[4] = {
                rstd::log::Level::Debug,
                rstd::log::Level::Info,
                rstd::log::Level::Warn,
                rstd::log::Level::Error,
            };
            auto              lvl  = kMap[(unsigned)level <= 3u ? (unsigned)level : 3u];
            auto              args = rstd::fmt::Arguments::make("{}", msg);
            rstd::log::Record rec {
                rstd::log::Metadata { lvl, {} },
                args,
            };
            rstd::log::log(rec);
        },
        nullptr);

    Options opts = parse_args(argc, argv);

    ::prctl(PR_SET_PDEATHSIG, SIGTERM);

    if (opts.workshop_dir.empty() || ! std::filesystem::is_directory(opts.workshop_dir)) {
        die("--path must be an existing workshop directory");
    }

    HostState state;
    state.sock = ww_bridge_connect(opts.ipc_path.c_str());
    if (state.sock < 0) die("ww_bridge_connect: " + std::string(::strerror(-state.sock)));

    {
        ww_bridge_init_t init {};
        if (int rc = ww_bridge_recv_init(state.sock, &init); rc != 0) {
            const char* reason = (rc == -EPROTO)
                                     ? "init: protocol error or unsupported spawn_version"
                                     : "init: recv failed";
            ww_bridge_send_init_nack(
                state.sock, init.spawn_version, WW_BRIDGE_SUPPORTED_SPAWN_VERSION, reason);
            ww_bridge_init_free(&init);
            die(std::string(reason) + " rc=" + std::to_string(rc));
        }

        // Web wallpapers don't have a fixed native resolution; the
        // `resolution` setting drives the actual extent against a
        // 16:9 baseline (compositor handles final letterbox / scale
        // on present). Schema disallows ORIGIN, so any invalid kv
        // falls back to 1080p.
        {
            int32_t resolution = ww_resolution_sanitize(parse_i32(
                kv_get(init.settings, "resolution"), static_cast<int32_t>(WW_RESOLUTION_1080P)));
            if (resolution == static_cast<int32_t>(WW_RESOLUTION_ORIGIN) ||
                resolution == static_cast<int32_t>(WW_RESOLUTION_CUSTOM)) {
                resolution = static_cast<int32_t>(WW_RESOLUTION_1080P);
            }
            opts.width  = 16;
            opts.height = 9;
            ww_resolution_apply_cap(
                resolution, WW_RESOLUTION_CAP_ALLOW_UPSCALE, &opts.width, &opts.height);
        }
        opts.initial_fps = parse_u32(kv_get(init.settings, "fps"), opts.initial_fps);
        // Wire format is u32 0..100; CEF host takes 0..1 ratio.
        opts.initial_volume = parse_f32(kv_get(init.settings, "volume"), 100.0f) / 100.0f;
        // identity=true: respawn-only. Translates to --mute-audio so
        // Chromium never opens an output device.
        opts.enable_audio = parse_bool(kv_get(init.settings, "enable_audio"), true);
        opts.shared_texture_enabled =
            parse_bool(kv_get(init.settings, "shared_texture_enabled"), true);
        opts.remote_debugging_port =
            static_cast<int>(parse_u32(kv_get(init.settings, "remote_debugging_port"), 0));
        // CLI `--render-node` wins over Init kv (mirroring scene/mpv/video).
        if (opts.render_node.empty()) {
            if (const char* v = kv_get(init.settings, "render_node"); v && *v) {
                opts.render_node = v;
            }
        }
        state.target_fps.store(opts.initial_fps > 0 ? opts.initial_fps : 60,
                               std::memory_order_release);
        state.audio_enabled = opts.enable_audio;
        state.base_volume   = opts.initial_volume;

        if (init.user_properties && *init.user_properties) {
            auto parsed = owe::ParseJson(init.user_properties, { .allow_comments = true });
            if (parsed.is_err()) {
                rstd_warn("init.user_properties is invalid JSON; ignored: {}", parsed.unwrap_err());
            } else {
                auto value  = parsed.unwrap();
                auto object = value.as_object();
                if (object.is_some()) {
                    (*object)->iter().for_each([&](auto entry) {
                        auto [key, value] = entry;
                        opts.initial_user_properties.insert(key->clone(), value->clone());
                    });
                } else {
                    rstd_warn("init.user_properties is not a JSON object; ignored");
                }
            }
        }

        ww_bridge_init_free(&init);
    }

    auto manifest_opt = weweb::LoadWebManifest(opts.workshop_dir);
    if (! manifest_opt) die("LoadWebManifest failed");
    auto& manifest = *manifest_opt;
    merge_user_property_overrides(manifest.user_props, opts.initial_user_properties);
    state.user_properties = &manifest.user_props;

    ww_wescene::WebProducerDevice producer;
    if (! opts.render_node.empty()) {
        rstd_info("waywallen-weweb-renderer: render_node={} pinning Vulkan/CEF device",
                  opts.render_node);
        producer.SetRenderNode(opts.render_node);
    }
    if (! producer.Init()) die("WebProducerDevice::Init failed");

    ww_pool_vulkan_init_t pi {};
    pi.instance           = producer.Instance();
    pi.physical_device    = producer.Physical();
    pi.device             = producer.Device();
    pi.queue              = producer.Queue();
    pi.queue_family_index = producer.QueueFamily();
    pi.get_instance_proc_addr =
        reinterpret_cast<void* (*)(void*, const char*)>(vkGetInstanceProcAddr);
    pi.device_uuid = producer.DeviceUuid();
    pi.driver_uuid = producer.DriverUuid();
    {
        ww_bridge_vk_dt_t dt {};
        ww_bridge_vk_dt_load(&dt, vkGetInstanceProcAddr, producer.Instance());
        if (int rc = ww_bridge_vk_query_render_node(
                &dt, producer.Physical(), &pi.drm_render_major, &pi.drm_render_minor);
            rc != 0) {
            rstd_warn("waywallen-weweb-renderer: drm render-node query failed ({}); "
                      "topology will be unknown to daemon",
                      rc);
        }
    }
    pi.drm_render_fd     = -1; // bridge opens by minor
    pi.image_usage_flags = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // TRANSFER_DST is the default contract for any producer slot.
    // weweb additionally writes via vkCmdBlitImage in
    // WebProducerDevice::BlitToSlot (CEF source extent/format never
    // matches the slot), so OR in BLIT_DST.
    pi.format_feature_flags = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;

    ww_pool_t* pool = nullptr;
    if (int rc = ww_bridge_pool_create(WW_POOL_BACKEND_VULKAN, &pi, &pool); rc != 0)
        die("ww_bridge_pool_create failed: " + std::to_string(rc));

    auto session = ww_wescene::BridgeSession::Adopt(pool, state.sock);
    if (! session) {
        int error = errno;
        ww_bridge_pool_destroy(pool);
        die("bridge session socket dup failed: " + std::to_string(error));
    }
    ww_wescene::BridgeProducerCore core(session);
    state.core = &core;
    state.host = &host;

    core.setOnFirstNegotiated([&]() {
        // Apply the daemon's initial fps once slots are live. Set the
        // initial volume too — we couldn't before OpenWallpaper because
        // there was no browser yet, but we can post a property update
        // now and the listener will pick it up.
        if (opts.initial_fps > 0) host.SetFrameRate(static_cast<int>(opts.initial_fps));
        apply_effective_volume(state);
        rstd_info("waywallen-weweb-renderer: negotiated, fps={} volume={}",
                  opts.initial_fps,
                  std::format("{:.2f}", static_cast<double>(effective_volume(state))));
    });

    // BrowserHost::Init wants resources / locales relative to argv[0].
    auto                            exe_dir = executable_dir(argv[0]);
    weweb::BrowserHost::InitOptions ho;
    ho.resources_dir = exe_dir;
    ho.locales_dir   = exe_dir / "locales";
    ho.cache_dir     = derive_cache_dir(opts.workshop_id);
    if (opts.remote_debugging_port > 0) {
        ho.enable_remote_debugging = true;
        ho.remote_debugging_port   = opts.remote_debugging_port;
    }
    ho.enable_audio           = opts.enable_audio;
    ho.shared_texture_enabled = opts.shared_texture_enabled;
    if (! opts.render_node.empty()) {
        ho.render_node_override = opts.render_node;
    }
    if (! host.Init(ho)) die("BrowserHost::Init failed");

    host.SetAudioResponseDemandCallback([&state](bool active) {
        set_audio_response_demand(state, active);
    });

    // OnAcceleratedPaint runs synchronously on the CEF UI thread (=
    // the thread that drives Pump, which is this main thread). Drain
    // any pending negotiate directive first so the slot pool reflects
    // the latest extent / fourcc before acquiring.
    host.SetAcceleratedPaintCallback([&state, &core, &producer](const weweb::DmaBufFrame& frame) {
        core.drainPendingDirective();
        if (! core.ready()) return;

        auto imp = producer.Import(frame);
        if (! imp.ok) return;

        submit_bridge_slot(state,
                           core,
                           [&producer, &imp](VkImage    slot_image,
                                             VkExtent2D slot_extent,
                                             VkFormat /*slot_format*/) {
                               return producer.BlitToSlot(imp, slot_image, slot_extent);
                           });
        producer.DestroyImported(imp);
    });

    host.SetCpuPaintCallback([&state, &core, &producer](const weweb::CpuPaintFrame& frame) {
        core.drainPendingDirective();
        if (! core.ready()) return;

        submit_bridge_slot(
            state,
            core,
            [&producer, &frame](VkImage slot_image, VkExtent2D slot_extent, VkFormat slot_format) {
                return producer.UploadToSlot(frame, slot_image, slot_extent, slot_format);
            });
    });

    weweb::BrowserHost::OpenOptions open_opts;
    open_opts.shared_texture_enabled = opts.shared_texture_enabled;
    open_opts.frame_rate             = static_cast<int>(opts.initial_fps);
    if (! host.OpenWallpaper(manifest,
                             opts.workshop_dir,
                             static_cast<int>(opts.width),
                             static_cast<int>(opts.height),
                             open_opts)) {
        die("BrowserHost::OpenWallpaper failed");
    }

    if (int rc = session->advertiseCaps(opts.width, opts.height, WW_MEM_HINT_DEVICE_LOCAL); rc != 0)
        die("ww_bridge_pool_advertise_caps failed: " + std::to_string(rc));

    rstd_info("waywallen-weweb-renderer: ready, advertised caps {}x{}", opts.width, opts.height);

    auto subscriptions = std::make_shared<ww_wescene::BridgeSubscriptionController>(session);
    {
        std::scoped_lock lock(state.subscription_mu);
        state.subscriptions = subscriptions;
    }
    std::vector<std::string> event_kinds { "pointer" };
    if (state.audio_response_demand.load(std::memory_order_acquire)) {
        event_kinds.emplace_back("audio");
    }
    if (! subscriptions->replace(std::move(event_kinds))) {
        die("failed to register renderer event subscriptions");
    }

    std::thread reader([&]() {
        reader_loop(state);
    });

    auto next_audio_push = std::chrono::steady_clock::now();

    while (! state.shutdown.load(std::memory_order_acquire) && ! host.ShouldExit()) {
        drain_settings(state);
        host.Pump();
        // CEF's OSR pacing goes idle without explicit invalidate kicks
        // (see project memory: CEF 147 OSR + DMA-BUF). Dedup happens
        // inside CEF at windowless_frame_rate.
        host.Invalidate();

        const auto now = std::chrono::steady_clock::now();
        if (state.audio_response_demand.load(std::memory_order_acquire) && now >= next_audio_push) {
            next_audio_push = now + std::chrono::milliseconds(33);
            std::array<float, 128> response {};
            {
                std::scoped_lock lock(state.audio_mu);
                if (state.audio_primed &&
                    now - state.audio_received <= std::chrono::milliseconds(250)) {
                    std::copy(state.audio_left.begin(), state.audio_left.end(), response.begin());
                    std::copy(
                        state.audio_right.begin(), state.audio_right.end(), response.begin() + 64);
                }
            }
            host.PushAudioData(response.data(), response.size());
        }

        std::this_thread::sleep_for(frame_delay(state));
    }

    state.shutdown.store(true, std::memory_order_release);
    if (reader.joinable()) {
        ::shutdown(state.sock, SHUT_RD);
        reader.join();
    }
    (void)subscriptions->replace({});
    {
        std::scoped_lock lock(state.subscription_mu);
        state.subscriptions.reset();
    }
    host.Shutdown();
    session.reset();
    ww_bridge_close(state.sock);
    return 0;
}
} // namespace waywallen
