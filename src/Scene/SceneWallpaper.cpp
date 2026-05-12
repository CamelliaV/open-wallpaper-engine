module;

#include <rstd/macro.hpp>
#include "Swapchain/ExSwapchain.hpp"

module wescene.scene_wallpaper;
import wescene.types;
import wescene.utils;
import wescene.scene;

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
                 RenderStop, RenderDraw, RenderSwapchainReady>
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
    explicit RenderHandler(MainHandler& main): m_main(main) {}
    ~RenderHandler() {
        m_render->destroy();
        rstd_info("render handler deleted");
    }

    void on(RenderInit&&);
    void on(RenderSetScene&&);
    void on(RenderSetFillMode&&);
    void on(RenderSetSpeed&&);
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

    std::optional<RenderSender> m_render_tx;
    std::optional<MainSender>   m_main_tx;

    // Strong ref kept here, weak copy captured by the swapchain callback;
    // nulling this out at shutdown lets the callback short-circuit so the
    // render channel can actually reach Err on recv().
    std::shared_ptr<RenderSender> m_swapchain_tx;
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
            owe::script::TickSceneScripts(*m_scene, fi);
        }
        m_scene->paritileSys->Emitt();

        /* Advance video textures (no-op if none) before drawFrame so
         * the new RGBA frame is sampled by the same render pass. */
        m_render->pumpVideoTextures(frame_timer.IdeaTime() * m_speed);

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
    m_rg = sceneToRenderGraph(*m_scene);

    if (m_main.isGenGraphviz()) m_rg->ToGraphviz("graph.dot");
    m_render->compileRenderGraph(*m_scene, *m_rg);
    m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
}

void RenderHandler::on(RenderSetSpeed&& m) { m_speed = m.speed; }

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
