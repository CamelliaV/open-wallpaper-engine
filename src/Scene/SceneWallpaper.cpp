module;

#include <rstd/macro.hpp>

module wescene.scene_wallpaper;
import wescene.types;
import wescene.utils;
import wescene.scene;

import nlohmann.json;
import rstd.log;
import rstd.cppstd;
import wavsen.audio;
import wescene.fs;
import wescene.message_loop;
import wescene.timer;
import wescene.parse;
import wescene.pkg_fs;
import wescene.rgraph;
import wescene.script;
import wescene.vulkan_render;

using namespace owe;

namespace owe
{

// ---- Render-thread messages -------------------------------------------------

struct RenderInit {
    std::shared_ptr<RenderInitInfo> info;
};
struct RenderSetScene {
    std::shared_ptr<Scene> scene;
};
struct RenderSetFillMode {
    FillMode mode;
};
struct RenderSetSpeed {
    float speed;
};
struct RenderSetUserProperty {
    std::string   key;
    nlohmann::json property;
};
struct RenderStop {
    bool stop;
};
struct RenderDraw {};
struct RenderSwapchainReady {
    bool     ready;
    uint32_t width;
    uint32_t height;
};

// Wrapped in a non-std struct so the rstd channel's internal `addressof`
// calls don't fall into ADL ambiguity with std::addressof when the element
// type sits in namespace std.
struct RenderMsg {
    std::variant<RenderInit, RenderSetScene, RenderSetFillMode, RenderSetSpeed,
                 RenderSetUserProperty, RenderStop, RenderDraw, RenderSwapchainReady>
        v;
};

// ---- Main-thread messages ---------------------------------------------------

struct MainLoadScene {};
struct MainStop {
    bool stop;
};
struct MainFirstFrame {};

// Property values stay in a small variant so we don't need a separate
// message kind per property.
using PropertyValue =
    std::variant<bool, int32_t, float, std::string,
                 std::shared_ptr<FirstFrameCallback>>;

struct MainSetProperty {
    std::string   key;
    PropertyValue value;
};

struct MainMsg {
    std::variant<MainLoadScene, MainSetProperty, MainStop, MainFirstFrame> v;
};

namespace {

nlohmann::json MakeUserPropertyDescriptor(nlohmann::json value) {
    if (value.is_object() && value.contains("value")) return value;
    nlohmann::json out = nlohmann::json::object();
    out["value"] = std::move(value);
    return out;
}

nlohmann::json ParseSettingJsonValue(std::string_view raw) {
    auto parsed = nlohmann::json::parse(raw,
                                        /*callback=*/nullptr,
                                        /*allow_exceptions=*/false,
                                        /*ignore_comments=*/true);
    if (! parsed.is_discarded()) return parsed;
    return std::string(raw);
}

nlohmann::json PropertyValueToUserProperty(const PropertyValue& value) {
    nlohmann::json v;
    if (auto* p = std::get_if<bool>(&value)) {
        v = *p;
    } else if (auto* p = std::get_if<int32_t>(&value)) {
        v = *p;
    } else if (auto* p = std::get_if<float>(&value)) {
        v = *p;
    } else if (auto* p = std::get_if<std::string>(&value)) {
        v = ParseSettingJsonValue(*p);
    } else {
        v = nullptr;
    }
    return MakeUserPropertyDescriptor(std::move(v));
}

// Parse a "r g b" / "r g b a" / "x y z w ..." space-separated float string into
// a small float vector. Trailing / leading whitespace is tolerated. Returns
// false when no numbers parse — caller treats as coercion failure.
bool ParseFloatList(std::string_view s, std::vector<float>& out) {
    out.clear();
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        std::size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        std::string tok(s.substr(start, i - start));
        try {
            out.push_back(std::stof(tok));
        } catch (...) {
            return false;
        }
    }
    return ! out.empty();
}

// Coerce a project.json property entry into a ShaderValue. Returns ok=false
// (with skip_reason) for combo / texture / unsupported types — the handler
// logs and skips those.
struct UserPropertyCoerceResult {
    bool        ok { false };
    ShaderValue value;
    const char* skip_reason { nullptr };
};

UserPropertyCoerceResult CoerceUserPropertyValue(const nlohmann::json& prop) {
    UserPropertyCoerceResult r;

    // Pull the explicit type if present; project.json properties always have
    // one, but inline {"value": ...} descriptors don't.
    std::string type;
    if (prop.is_object() && prop.contains("type") && prop.at("type").is_string()) {
        type = prop.at("type").get<std::string>();
    }

    // Combo / texture / file paths can't write a uniform.
    if (type == "combo") {
        r.skip_reason = "combo type — live #define recompile not implemented";
        return r;
    }
    if (type == "texture" || type == "replacetexture" || type == "file" ||
        type == "textinput") {
        r.skip_reason = "non-uniform property type";
        return r;
    }

    // Find the raw value.
    const nlohmann::json* val_ptr = &prop;
    if (prop.is_object() && prop.contains("value")) val_ptr = &prop.at("value");
    const nlohmann::json& v = *val_ptr;

    if (type == "color") {
        std::vector<float> nums;
        if (v.is_string() && ParseFloatList(v.get<std::string>(), nums) && nums.size() >= 3) {
            r.ok    = true;
            r.value = ShaderValue(std::span<const float>(nums));
            return r;
        }
        r.skip_reason = "color value not a 'r g b[ a]' float string";
        return r;
    }

    // Fallback inference when type is missing.
    if (v.is_boolean()) {
        r.ok    = true;
        float f = v.get<bool>() ? 1.0f : 0.0f;
        r.value = ShaderValue(f);
        return r;
    }
    if (v.is_number()) {
        r.ok    = true;
        r.value = ShaderValue(v.get<float>());
        return r;
    }
    if (v.is_string()) {
        std::vector<float> nums;
        if (ParseFloatList(v.get<std::string>(), nums)) {
            if (nums.size() == 1) {
                r.ok    = true;
                r.value = ShaderValue(nums[0]);
                return r;
            }
            r.ok    = true;
            r.value = ShaderValue(std::span<const float>(nums));
            return r;
        }
        r.skip_reason = "string value isn't parseable as float list";
        return r;
    }
    r.skip_reason = "unsupported JSON value shape";
    return r;
}

// Push a user-property value to every material whose shader declared a
// `u_*` uniform with this material-key. Sets the per-material dirty flag so
// CustomShaderPass picks the new value up next frame.
void ApplyUserPropertyToShaders(Scene& scene, const std::string& key,
                                const nlohmann::json& prop) {
    auto it = scene.shader_user_var_index.find(key);
    if (it == scene.shader_user_var_index.end()) return;

    auto coerced = CoerceUserPropertyValue(prop);
    if (! coerced.ok) {
        rstd_warn("user property '{}' skipped: {}",
                  key, coerced.skip_reason ? coerced.skip_reason : "unknown");
        return;
    }
    for (auto& [material, uniform_name] : it->second) {
        if (! material) continue;
        material->customShader.constValues[uniform_name] = coerced.value;
        material->customShader.dirty = true;
    }
}

void MergeProjectUserProperties(
    const std::filesystem::path& project_dir,
    std::unordered_map<std::string, nlohmann::json>& out) {
    const auto project_path = project_dir / "project.json";
    std::ifstream is(project_path);
    if (! is) return;

    auto j = nlohmann::json::parse(is,
                                   /*callback=*/nullptr,
                                   /*allow_exceptions=*/false,
                                   /*ignore_comments=*/true);
    if (j.is_discarded()) return;
    auto gen = j.find("general");
    if (gen == j.end() || ! gen->is_object()) return;
    auto props = gen->find("properties");
    if (props == gen->end() || ! props->is_object()) return;

    for (auto it = props->begin(); it != props->end(); ++it) {
        if (out.contains(it.key())) continue;
        out.emplace(it.key(), MakeUserPropertyDescriptor(it.value()));
    }
}

} // namespace

using MainSender   = msgloop::MessageLoop<MainMsg>::Sender;
using RenderSender = msgloop::MessageLoop<RenderMsg>::Sender;

class RenderHandler;

class MainHandler {
public:
    MainHandler();
    ~MainHandler();

    bool init();
    auto renderHandler() const { return m_render_handler.get(); }
    bool inited() const { return m_inited; }

    MainSender   mainSender() { return m_main_loop.sender(); }
    RenderSender renderSender() { return m_render_loop.sender(); }

    void on(MainLoadScene&&);
    void on(MainSetProperty&&);
    void on(MainStop&&);
    void on(MainFirstFrame&&);

    bool isGenGraphviz() const { return m_gen_graphviz; }

    void setOnClearColor(ClearColorCallback cb) { m_clear_color_cb = std::move(cb); }

private:
    void loadScene();

    bool m_inited { false };

    std::string m_assets;
    std::string m_source;
    std::string m_cache_path;
    bool        m_gen_graphviz { false };
    std::unordered_map<std::string, nlohmann::json> m_user_properties;

    WPSceneParser                        m_scene_parser;
    std::unique_ptr<wavsen::audio::SoundManager> m_sound_manager;
    FirstFrameCallback                   m_first_frame_callback;
    ClearColorCallback                   m_clear_color_cb;

    msgloop::MessageLoop<MainMsg>   m_main_loop;
    msgloop::MessageLoop<RenderMsg> m_render_loop;
    std::unique_ptr<RenderHandler>  m_render_handler;
};

class RenderHandler {
public:
    explicit RenderHandler(MainHandler& main): m_main(main) {
        // Best-effort: a failing init just leaves snapshots returning false
        // and audio_average at zeros — wallpapers still render fine.
        (void)m_audio_capture.init();
    }
    ~RenderHandler() {
        m_render->destroy();
        rstd_info("render handler deleted");
    }

    void on(RenderInit&&);
    void on(RenderSetScene&&);
    void on(RenderSetFillMode&&);
    void on(RenderSetSpeed&&);
    void on(RenderSetUserProperty&&);
    void on(RenderStop&&);
    void on(RenderDraw&&);
    void on(RenderSwapchainReady&&);

    ExSwapchain* exSwapchain() const { return m_render->exSwapchain(); }
    int          takeLastFrameSyncFd() { return m_render->takeLastFrameSyncFd(); }
    bool getDrmRenderNode(uint32_t& major, uint32_t& minor) const {
        return m_render->getDrmRenderNode(major, minor);
    }
    vulkan::VulkanRender* render() const { return m_render.get(); }

    bool renderInited() const { return m_render->inited(); }

    void setMousePos(double x, double y) {
        m_mouse_pos.store(std::array { (float)x, (float)y });
    }

    // Edge-events for the cursor button stream. Each call from the input
    // thread sets/clears the held bit and records the edge so the next
    // TickSceneScripts can fire cursorDown/Up. fetch_or guards against
    // press-release-press coalescing between ticks (rare).
    void setMouseButton(int button, bool down) {
        if (button < 0 || button > 31) return;
        const uint32_t mask = 1u << button;
        if (down) {
            m_buttons_down.fetch_or(mask);
            m_buttons_pressed.fetch_or(mask);
        } else {
            m_buttons_down.fetch_and(~mask);
            m_buttons_released.fetch_or(mask);
        }
    }
    void setMouseInWindow(bool in) { m_cursor_in_window.store(in); }
    uint32_t buttonsDown() const { return m_buttons_down.load(); }
    uint32_t consumePressed() { return m_buttons_pressed.exchange(0); }
    uint32_t consumeReleased() { return m_buttons_released.exchange(0); }
    bool     cursorInWindow() const { return m_cursor_in_window.load(); }

    void setSenders(RenderSender render_tx, MainSender main_tx) {
        m_render_tx.emplace(std::move(render_tx));
        m_main_tx.emplace(std::move(main_tx));
    }

    // Drop every Sender clone owned by this handler so the render channel
    // can disconnect at shutdown. The swapchain callback's sender is held
    // through `m_swapchain_tx` (strong) + a weak_ptr in the lambda — clearing
    // the strong ref turns the lambda into a no-op.
    void clearSenders() {
        m_swapchain_tx.reset();
        m_render_tx.reset();
        m_main_tx.reset();
    }

    FrameTimer frame_timer { [] {} };
    FpsCounter fps_counter;

private:
    MainHandler& m_main;

    std::unique_ptr<vulkan::VulkanRender> m_render { std::make_unique<vulkan::VulkanRender>() };
    std::shared_ptr<Scene>                m_scene { nullptr };
    std::unique_ptr<rg::RenderGraph>      m_rg { nullptr };
    float                                 m_speed { 1.0f };
    FillMode                              m_fillmode { FillMode::ASPECTCROP };

    std::atomic<std::array<float, 2>> m_mouse_pos { std::array { 0.5f, 0.5f } };
    std::atomic<uint32_t>             m_buttons_down { 0 };
    std::atomic<uint32_t>             m_buttons_pressed { 0 };
    std::atomic<uint32_t>             m_buttons_released { 0 };
    std::atomic<bool>                 m_cursor_in_window { false };

    std::optional<RenderSender> m_render_tx;
    std::optional<MainSender>   m_main_tx;

    // Strong ref kept here, weak copy captured by the swapchain callback;
    // nulling this out at shutdown lets the callback short-circuit so the
    // render channel can actually reach Err on recv().
    std::shared_ptr<RenderSender> m_swapchain_tx;

    wavsen::audio::AudioCapture m_audio_capture;
};

// ---- RenderHandler message handlers ----------------------------------------

void RenderHandler::on(RenderStop&& m) {
    if (m.stop)
        frame_timer.Stop();
    else
        frame_timer.Run();
}

void RenderHandler::on(RenderDraw&&) {
    frame_timer.FrameBegin();
    if (m_rg) {
        m_scene->shaderValueUpdater->FrameBegin();
        {
            auto pos = m_mouse_pos.load();
            m_scene->pointerPosition = pos;
            m_scene->shaderValueUpdater->MouseInput(pos[0], pos[1]);
        }
        // Drive any per-Scene scenescripts before particle emission.
        // Scripts mutate SceneNode transforms (scale/origin/angles) so
        // they need to run before the matrix-derivation in the
        // shaderValueUpdater's per-frame uniform pass — that's already
        // what FrameBegin set up; UpdateUniforms runs inside drawFrame.
        // The runtime is a no-op when no ScriptScene is installed.
        {
            owe::script::FrameInputs fi;
            fi.frametime = static_cast<float>(m_scene->frameTime * m_speed);
            fi.runtime   = static_cast<float>(m_scene->elapsingTime);
            fi.canvas_w  = static_cast<float>(m_scene->ortho[0]);
            fi.canvas_h  = static_cast<float>(m_scene->ortho[1]);
            fi.screen_w  = fi.canvas_w;
            fi.screen_h  = fi.canvas_h;
            {
                auto pos = m_mouse_pos.load();
                fi.cursor_x = pos[0];
                fi.cursor_y = pos[1];
            }
            fi.cursor_in_window       = cursorInWindow();
            fi.mouse_buttons_down     = buttonsDown();
            fi.mouse_buttons_pressed  = consumePressed();
            fi.mouse_buttons_released = consumeReleased();
            wavsen::audio::AudioSpectrum spec;
            m_audio_capture.snapshot(spec);
            fi.audio_average = spec.bins;
            // Layer pkg-internal audio response (WPSoundParser-driven) on
            // top of the system monitor capture so wallpapers with embedded
            // music still react. Scene::audioAverage is sized 16; only the
            // first 16 bins overlay.
            for (std::size_t i = 0; i < m_scene->audioAverage.size(); ++i) {
                const float local = m_scene->audioAverage[i].load(
                    std::memory_order_relaxed);
                fi.audio_average[i] = std::max(fi.audio_average[i], local);
                m_scene->audioAverage[i].store(local * 0.94f,
                                                std::memory_order_relaxed);
            }
            // Push the merged spectrum to the shader updater so audio-bar
            // shaders (Simple_Audio_Bars and friends) see live data through
            // g_AudioSpectrum{16,32,64}{Left,Right}.
            m_scene->shaderValueUpdater->SetAudioSpectrum(
                std::span<const float, 64>(fi.audio_average));
            owe::script::TickSceneScripts(*m_scene, fi);
        }
        m_scene->paritileSys->Emitt();

        /* Advance video textures (no-op if none) before drawFrame so
         * the new RGBA frame is sampled by the same render pass. */
        m_render->pumpVideoTextures(frame_timer.IdeaTime() * m_speed);

        /* Upload any glyph rects the actuators added this tick. Runs after
         * TickSceneScripts (which calls FontFace::Populate) and before
         * drawFrame so newly-rasterised glyphs are visible the same frame. */
        m_render->pumpFontAtlases(*m_scene);

        m_render->drawFrame(*m_scene);

        m_scene->PassFrameTime(frame_timer.IdeaTime() * m_speed);

        m_scene->shaderValueUpdater->FrameEnd();

        if (! m_scene->first_frame_ok) {
            m_scene->first_frame_ok = true;
            if (m_main_tx) (void)m_main_tx->send(MainMsg { MainFirstFrame {} });
        }
    }
    frame_timer.FrameEnd();
}

void RenderHandler::on(RenderSetFillMode&& m) {
    m_fillmode = m.mode;
    if (m_scene && renderInited()) {
        m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
    }
}

void RenderHandler::on(RenderSetScene&& m) {
    m_scene = std::move(m.scene);
    if (m_rg) m_render->clearLastRenderGraph();
    // Drop cached mesh buffers from the previous scene before building the
    // new graph. Swapchain-resize rebuilds (RenderSwapchainReady) reuse the
    // same SceneMesh set, so evict is intentionally not called there.
    m_render->evictUnusedMeshes();
    m_rg = sceneToRenderGraph(*m_scene);

    if (m_main.isGenGraphviz()) m_rg->ToGraphviz("graph.dot");
    m_render->compileRenderGraph(*m_scene, *m_rg);
    m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
}

void RenderHandler::on(RenderSetSpeed&& m) { m_speed = m.speed; }

void RenderHandler::on(RenderSetUserProperty&& m) {
    if (! m_scene) return;
    owe::script::SetSceneUserProperty(*m_scene, m.key, m.property);
    ApplyUserPropertyToShaders(*m_scene, m.key, m.property);
}

void RenderHandler::on(RenderInit&& m) {
    m_render->init(std::move(*m.info));

    // Subscribe to ExSwapchain ready/extent/format changes. The
    // callback runs on the render thread (sync for Local, from
    // drainPendingDirective for Bridge); we just relay it as a
    // RenderSwapchainReady message back to ourselves so the actual
    // handling happens through the normal loop path. Format reaches
    // VulkanRender via ExSwapchain::format() directly; no need to
    // round-trip it through this message.
    if (auto* sw = m_render->exSwapchain()) {
        if (m_render_tx) {
            m_swapchain_tx = std::make_shared<RenderSender>(*m_render_tx);
            std::weak_ptr<RenderSender> weak = m_swapchain_tx;
            sw->setOnReadyChanged([weak](const ExSwapchainReadyEvent& e) {
                if (auto tx = weak.lock()) {
                    (void)tx->send(RenderMsg { RenderSwapchainReady {
                        e.ready, e.width, e.height } });
                }
            });
        }
    }

    // inited, callback to load scene
    if (m_main_tx) (void)m_main_tx->send(MainMsg { MainLoadScene {} });
}

void RenderHandler::on(RenderSwapchainReady&& m) {
    if (! m.ready) {
        frame_timer.Stop();
        return;
    }
    bool extent_changed = m_render->onSwapchainReady(m.width, m.height);
    if (extent_changed && m_scene && m_rg) {
        m_render->clearLastRenderGraph();
        m_render->compileRenderGraph(*m_scene, *m_rg);
        m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
    }
    frame_timer.Run();
}

// ---- MainHandler message handlers ------------------------------------------

void MainHandler::on(MainLoadScene&&) {
    if (m_render_handler->renderInited()) {
        loadScene();
    }
}

void MainHandler::on(MainSetProperty&& m) {
    const auto& property = m.key;
    const auto& value    = m.value;

    if (property == PROPERTY_SOURCE) {
        if (auto* p = std::get_if<std::string>(&value)) {
            m_source = *p;
            rstd_info("source: {}", m_source);
            on(MainLoadScene {});
        }
    } else if (property == PROPERTY_ASSETS) {
        if (auto* p = std::get_if<std::string>(&value)) {
            m_assets = *p;
            on(MainLoadScene {});
        }
    } else if (property == PROPERTY_FPS) {
        if (auto* p = std::get_if<int32_t>(&value)) {
            int32_t fps = *p;
            if (fps >= 5) {
                m_render_handler->frame_timer.SetRequiredFps((uint8_t)fps);
            }
        }
    } else if (property == PROPERTY_FILLMODE) {
        if (auto* p = std::get_if<int32_t>(&value)) {
            (void)m_render_loop.sender().send(
                RenderMsg { RenderSetFillMode { (FillMode)*p } });
        }
    } else if (property == PROPERTY_GRAPHIVZ) {
        if (auto* p = std::get_if<bool>(&value)) m_gen_graphviz = *p;
    } else if (property == PROPERTY_MUTED) {
        if (auto* p = std::get_if<bool>(&value)) m_sound_manager->set_muted(*p);
    } else if (property == PROPERTY_VOLUME) {
        if (auto* p = std::get_if<float>(&value)) m_sound_manager->set_volume(*p);
    } else if (property == PROPERTY_CACHE_PATH) {
        if (auto* p = std::get_if<std::string>(&value)) m_cache_path = *p;
    } else if (property == PROPERTY_FIRST_FRAME_CALLBACK) {
        if (auto* p = std::get_if<std::shared_ptr<FirstFrameCallback>>(&value)) {
            m_first_frame_callback = **p;
        }
    } else if (property == PROPERTY_SPEED) {
        if (auto* p = std::get_if<float>(&value)) {
            (void)m_render_loop.sender().send(RenderMsg { RenderSetSpeed { *p } });
        }
    } else {
        nlohmann::json prop = PropertyValueToUserProperty(value);
        m_user_properties[property] = prop;
        (void)m_render_loop.sender().send(RenderMsg { RenderSetUserProperty {
            property, std::move(prop) } });
    }
}

void MainHandler::on(MainStop&& m) {
    if (m.stop) {
        m_sound_manager->pause();
    } else {
        m_sound_manager->play();
    }
    (void)m_render_loop.sender().send(RenderMsg { RenderStop { m.stop } });
}

void MainHandler::on(MainFirstFrame&&) {
    if (m_first_frame_callback) m_first_frame_callback();
}

void MainHandler::loadScene() {
    if (m_source.empty() || m_assets.empty()) return;

    rstd_info("loading scene: {}", m_source);

    if (! m_sound_manager->is_inited()) {
        m_sound_manager->init();
        m_sound_manager->play();
    } else {
        m_sound_manager->unmount_all();
    }

    std::shared_ptr<Scene> scene { nullptr };

    // mount assets dir
    std::unique_ptr<fs::VFS> pVfs = std::make_unique<fs::VFS>();
    auto&                    vfs  = *pVfs;
    if (! vfs.IsMounted("assets")) {
        bool sus = vfs.Mount("/assets", fs::CreatePhysicalFs(m_assets), "assets");
        if (! sus) {
            rstd_error("Mount assets dir failed");
            return;
        }
    }
    std::filesystem::path pkgPath_fs { m_source };
    pkgPath_fs.replace_extension("pkg");
    std::string pkgPath  = pkgPath_fs.native();
    std::string pkgEntry = pkgPath_fs.filename().replace_extension("json").native();
    std::string pkgDir   = pkgPath_fs.parent_path().native();
    std::string scene_id = pkgPath_fs.parent_path().filename().native();
    MergeProjectUserProperties(pkgPath_fs.parent_path(), m_user_properties);

    // load pkgfile. Read pkg version stamp before move-mounting so we can
    // pass it to the scene parser; on fallback (loose dir) we have no
    // version info and use kSceneVersionUnknown.
    wpscene::SceneVersion pkg_v = wpscene::kSceneVersionUnknown;
    auto                  wfs   = fs::WPPkgFs::CreatePkgFs(pkgPath);
    if (wfs) pkg_v = wpscene::ParsePkgVersionStamp(wfs->pkg_version_stamp());
    if (! wfs || ! vfs.Mount("/assets", std::move(wfs))) {
        rstd_info("load pkg file {} failed, fallback to use dir", pkgPath);
        pkg_v = wpscene::kSceneVersionUnknown;
        // load pkg dir
        if (! vfs.Mount("/assets", fs::CreatePhysicalFs(pkgDir))) {
            rstd_error("can't load pkg directory: {}", pkgDir);
            return;
        }
    }
    if (! m_cache_path.empty()) {
        if (! vfs.Mount("/cache", fs::CreatePhysicalFs(m_cache_path, true), "cache")) {
            rstd_error("can't load cache folder: {}", m_cache_path);
        } else {
            rstd_info("cache folder: {}", m_cache_path);
        }
    }

    {
        std::string       scene_src;
        const std::string base { "/assets/" };
        {
            std::string scenePath = base + pkgEntry;
            if (vfs.Contains(scenePath)) {
                auto f = vfs.Open(scenePath);
                if (f) scene_src = f->ReadAllStr();
            }
        }
        if (scene_src.empty()) {
            rstd_error("Not supported scene type");
            return;
        }
        scene = m_scene_parser.Parse(scene_id, scene_src, vfs, *m_sound_manager, pkg_v);
        for (const auto& [key, prop] : m_user_properties) {
            owe::script::SetSceneUserProperty(*scene, key, prop);
        }
        if (! m_cache_path.empty() && scene) {
            std::filesystem::path ls_dir = std::filesystem::path(m_cache_path) /
                                            "script_localstorage";
            std::error_code ec;
            std::filesystem::create_directories(ls_dir, ec);
            std::string ls_file = (ls_dir / (scene_id + ".json")).native();
            owe::script::SetScenePersistence(*scene, std::move(ls_file));
        }
        scene->vfs.reset(pVfs.release());

        // Surface the parsed clear color before the scene is shipped
        // off to the render thread; downstream callers (the daemon
        // host) need the value to feed `set_config.clear_*`.
        if (m_clear_color_cb) {
            const auto& c = scene->clearColor;
            m_clear_color_cb(c[0], c[1], c[2]);
        }

    }

    auto rtx = m_render_loop.sender();
    (void)rtx.send(RenderMsg { RenderSetScene { scene } });
    // First-frame default push: now that the render thread owns the scene,
    // replay every collected user property (project.json defaults + any
    // mutations the host already pushed during scene load) so the shader
    // cbuffer matches what the host UI displays.
    for (const auto& [key, prop] : m_user_properties) {
        (void)rtx.send(RenderMsg { RenderSetUserProperty { key, prop } });
    }
    // draw first frame
    (void)rtx.send(RenderMsg { RenderDraw {} });
}

bool MainHandler::init() {
    if (m_inited) return true;

    // Wire render handler senders before starting the loops; otherwise an
    // early RenderInit could fire before they're set.
    m_render_handler->setSenders(m_render_loop.sender(), m_main_loop.sender());

    m_main_loop.start([this](MainMsg&& m) {
        std::visit([this](auto&& v) { on(std::move(v)); }, std::move(m.v));
    });
    m_render_loop.start([this](RenderMsg&& m) {
        std::visit([this](auto&& v) { m_render_handler->on(std::move(v)); },
                   std::move(m.v));
    });

    {
        auto& frameTimer = m_render_handler->frame_timer;
        auto  rtx        = m_render_loop.sender();
        frameTimer.SetCallback([rtx]() mutable {
            (void)rtx.send(RenderMsg { RenderDraw {} });
        });
        frameTimer.SetRequiredFps(15);
        frameTimer.Run();
    }

    m_inited = true;
    return true;
}

MainHandler::MainHandler()
    : m_sound_manager(std::make_unique<wavsen::audio::SoundManager>()),
      m_main_loop("main"),
      m_render_loop("render"),
      m_render_handler(std::make_unique<RenderHandler>(*this)) {}

MainHandler::~MainHandler() {
    // Orderly shutdown: drain both loops *before* RenderHandler dies, so
    // m_render->destroy() doesn't race with an in-flight RenderDraw.
    //
    //   1. Stop the frame timer (joins its thread → no more Draw posts).
    //   2. Replace the timer callback with a no-op so the captured render
    //      Sender clone is released.
    //   3. Drop every Sender clone the render handler holds, including the
    //      strong ref the swapchain callback weak-captures.
    //   4. Stop the render loop — drops engine sender, recv() returns Err
    //      after the in-flight handler returns, thread joins.
    //   5. Same for the main loop.
    //   6. Default member destruction then runs RenderHandler's dtor with
    //      the render thread already gone, so destroy() is single-threaded.
    if (m_render_handler) {
        m_render_handler->frame_timer.Stop();
        m_render_handler->frame_timer.SetCallback([] {});
        m_render_handler->clearSenders();
    }
    m_render_loop.stop();
    m_main_loop.stop();
}

} // namespace owe

SceneWallpaper::SceneWallpaper(): m_main_handler(std::make_unique<MainHandler>()) {}

SceneWallpaper::~SceneWallpaper() = default;

bool SceneWallpaper::inited() const { return m_main_handler->inited(); }

bool SceneWallpaper::init() { return m_main_handler->init(); }

void SceneWallpaper::initVulkan(RenderInitInfo info) {
    m_offscreen = info.offscreen;
    auto sp     = std::make_shared<RenderInitInfo>(std::move(info));
    (void)m_main_handler->renderSender().send(
        RenderMsg { RenderInit { std::move(sp) } });
}

void SceneWallpaper::play() {
    (void)m_main_handler->mainSender().send(MainMsg { MainStop { false } });
}
void SceneWallpaper::pause() {
    (void)m_main_handler->mainSender().send(MainMsg { MainStop { true } });
}

void SceneWallpaper::mouseInput(double x, double y) {
    m_main_handler->renderHandler()->setMousePos(x, y);
}

void SceneWallpaper::mouseButton(int button, bool down) {
    m_main_handler->renderHandler()->setMouseButton(button, down);
}

void SceneWallpaper::mouseEnter(bool in_window) {
    m_main_handler->renderHandler()->setMouseInWindow(in_window);
}

void SceneWallpaper::setPropertyBool(std::string_view name, bool value) {
    (void)m_main_handler->mainSender().send(MainMsg {
        MainSetProperty { std::string(name), PropertyValue { value } } });
}
void SceneWallpaper::setPropertyInt32(std::string_view name, int32_t value) {
    (void)m_main_handler->mainSender().send(MainMsg {
        MainSetProperty { std::string(name), PropertyValue { value } } });
}
void SceneWallpaper::setPropertyFloat(std::string_view name, float value) {
    (void)m_main_handler->mainSender().send(MainMsg {
        MainSetProperty { std::string(name), PropertyValue { value } } });
}
void SceneWallpaper::setPropertyString(std::string_view name, std::string value) {
    (void)m_main_handler->mainSender().send(MainMsg { MainSetProperty {
        std::string(name), PropertyValue { std::move(value) } } });
}
void SceneWallpaper::setOnClearColor(ClearColorCallback cb) {
    m_main_handler->setOnClearColor(std::move(cb));
}

void SceneWallpaper::setPropertyObject(std::string_view name, std::shared_ptr<void> value) {
    // Currently the only object property is the first-frame callback. Cast at
    // the API boundary so the typed message stays self-describing.
    if (name == PROPERTY_FIRST_FRAME_CALLBACK) {
        std::shared_ptr<FirstFrameCallback> cb {
            value, reinterpret_cast<FirstFrameCallback*>(value.get()) };
        (void)m_main_handler->mainSender().send(MainMsg { MainSetProperty {
            std::string(name), PropertyValue { std::move(cb) } } });
    }
}

int SceneWallpaper::takeLastFrameSyncFd() {
    return m_main_handler->renderHandler()->takeLastFrameSyncFd();
}

ExSwapchain* SceneWallpaper::exSwapchain() const {
    return m_main_handler->renderHandler()->exSwapchain();
}

bool SceneWallpaper::getDrmRenderNode(uint32_t& out_major,
                                      uint32_t& out_minor) const {
    return m_main_handler->renderHandler()->getDrmRenderNode(out_major,
                                                              out_minor);
}

bool SceneWallpaper::waitVulkanInited(uint32_t timeout_ms) {
    using clock   = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    auto rh       = m_main_handler->renderHandler();
    while (clock::now() < deadline) {
        if (rh->renderInited()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return rh->renderInited();
}

VkInstance SceneWallpaper::vkInstance() const {
    return m_main_handler->renderHandler()->render()->vkInstance();
}
VkPhysicalDevice SceneWallpaper::vkPhysicalDevice() const {
    return m_main_handler->renderHandler()->render()->vkPhysicalDevice();
}
VkDevice SceneWallpaper::vkDevice() const {
    return m_main_handler->renderHandler()->render()->vkDevice();
}
VkQueue SceneWallpaper::vkGraphicsQueue() const {
    return m_main_handler->renderHandler()->render()->vkGraphicsQueue();
}
uint32_t SceneWallpaper::vkGraphicsQueueFamily() const {
    return m_main_handler->renderHandler()->render()->vkGraphicsQueueFamily();
}
void SceneWallpaper::deviceUuid(uint8_t out[16]) const {
    m_main_handler->renderHandler()->render()->deviceUuid(out);
}
void SceneWallpaper::driverUuid(uint8_t out[16]) const {
    m_main_handler->renderHandler()->render()->driverUuid(out);
}
