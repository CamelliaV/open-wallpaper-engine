#include <gtest/gtest.h>

import rstd.cppstd;
import rstd;
import eigen;
import wescene.json;
import wescene.scene;
import wescene.script;
import wescene.testing.json_builder;

using namespace owe::script;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{

// Build a one-shot FieldScript whose update() returns a host-visible counter.
// The module body schedules timers/intervals that mutate that counter, so we
// can observe the JsRuntime's deferred-callback sweep through FieldScript's
// last_value().
FieldScript* MakeProbe(JsRuntime& rt, const char* sha, const char* src) {
    return rt.MakeFieldScript(src,
                              sha,
                              FieldKind::Scalar,
                              /*properties_config=*/owe::MakeObject(),
                              /*initial_value=*/owe::IntoJson(0),
                              /*node=*/nullptr);
}

double Tick(JsRuntime& rt, double runtime) {
    FrameInputs fi {};
    fi.runtime = float(runtime);
    rt.SetFrameInputs(fi);
    rt.TickAll();
    // last_value() of the last MakeFieldScript'd script — caller must own a
    // pointer, but for this test the call site walks via ForEach.
    return 0.0;
}

double LastScalar(FieldScript* fs) {
    EXPECT_TRUE(std::holds_alternative<ScalarValue>(fs->last_value()));
    if (! std::holds_alternative<ScalarValue>(fs->last_value())) return 0.0;
    return std::get<ScalarValue>(fs->last_value()).v;
}

} // namespace

TEST(ScriptInitialization, UsesSceneOwnerOrderInsteadOfRegistrationOrder) {
    auto root = Arc<owe::SceneNode>::make();

    JsRuntime rt;
    auto*     consumer = rt.MakeFieldScript(
        R"JS(
            let seen = -1;
            export function init() { seen = shared.ready; }
            export function update() { return seen; }
        )JS",
        "test/init_order_consumer",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0));
    auto* producer = rt.MakeFieldScript(
        R"JS(
            export function init() { shared.ready = 7; }
            export function update() { return 0; }
        )JS",
        "test/init_order_producer",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0));
    ASSERT_NE(consumer, nullptr);
    ASSERT_NE(producer, nullptr);

    rt.SetInitializationOrder(*consumer, 1);
    rt.SetInitializationOrder(*producer, 0);
    rt.SetSceneRoot(root.as_ptr());
    rt.TickAll();

    EXPECT_EQ(LastScalar(consumer), 7.0);
}

TEST(ScriptTimer, SetTimeoutFiresAfterDelay) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/timer_fires",
                         R"JS(
        let fired = 0;
        setTimeout(() => { fired++; }, 100);
        export function update() { return fired; }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.05);
    ASSERT_TRUE(std::holds_alternative<ScalarValue>(fs->last_value()));
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 0.0);

    Tick(rt, 0.15);
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);

    Tick(rt, 0.30);
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);
}

TEST(ScriptTimer, SetIntervalRepeats) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/interval_repeats",
                         R"JS(
        let n = 0;
        setInterval(() => { n++; }, 100);
        export function update() { return n; }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.25);
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 2.0);

    Tick(rt, 0.55);
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 5.0);
}

TEST(ScriptTimer, ClearTimeoutCancels) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/clear_cancels",
                         R"JS(
        let n = 0;
        let h = setTimeout(() => { n++; }, 100);
        clearTimeout(h);
        export function update() { return n; }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.50);
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 0.0);
}

TEST(ScriptTimer, HandleSelfCallCancels) {
    // Corpus also calls the return value as a function to cancel (e.g.
    // `if (stopTimeout) stopTimeout()`). Both shapes must work.
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/handle_self_call",
                         R"JS(
        let n = 0;
        let h = setTimeout(() => { n++; }, 100);
        h();  // cancel by invoking handle
        export function update() { return n; }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.50);
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 0.0);
}

TEST(ScriptCompat, RegExpLegacyCapturesSurviveTimerCallbacks) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/regexp_legacy_captures",
                         R"JS(
        let n = 0;
        setInterval(() => {
            if (/(H+)/.test('HH:mm')) n = RegExp.$1.length;
        }, 100);
        export function update() { return n; }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.15);
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 2.0);
}

TEST(ScriptAudio, RegisterAudioBuffersResamplesRequestedResolution) {
    JsRuntime   rt;
    FrameInputs fi {};
    for (std::size_t i = 0; i < fi.audio_average.size(); ++i) {
        fi.audio_left[i]    = static_cast<float>(i);
        fi.audio_right[i]   = static_cast<float>(200 + i);
        fi.audio_average[i] = static_cast<float>(100 + i);
    }
    rt.SetFrameInputs(fi);

    auto* fs = MakeProbe(rt,
                         "test/audio_buffers_resample",
                         R"JS(
        let audio = engine.registerAudioBuffers(16);
        export function update() {
            return audio.left.length * 100000000
                + audio.right.length * 1000000
                + audio.average[15] * 10000
                + audio.left[15] * 100
                + audio.right[15];
        }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_DOUBLE_EQ(LastScalar(fs),
                     16.0 * 100000000.0 + 16.0 * 1000000.0 + 161.5 * 10000.0 + 61.5 * 100.0 +
                         261.5);

    for (std::size_t i = 0; i < fi.audio_average.size(); ++i) {
        fi.audio_left[i]    = static_cast<float>(100 + i);
        fi.audio_right[i]   = static_cast<float>(300 + i);
        fi.audio_average[i] = static_cast<float>(200 + i);
    }
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_DOUBLE_EQ(LastScalar(fs),
                     16.0 * 100000000.0 + 16.0 * 1000000.0 + 261.5 * 10000.0 + 161.5 * 100.0 +
                         361.5);
}

TEST(ScriptAudio, RegisterAudioBuffersAcceptsResolution64Constant) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);

    auto* fs = MakeProbe(rt,
                         "test/audio_buffers_resolution_64_constant",
                         R"JS(
        const audio = engine.registerAudioBuffers(engine.AUDIO_RESOLUTION_64);
        export function update() { return audio.average.length; }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_DOUBLE_EQ(LastScalar(fs), 64.0);
}

TEST(ScriptAudio, RegisterAudioBuffersHoldsOneRuntimeDemandLease) {
    auto              demand = rstd::sync::Arc<owe::AudioResponseDemand>::make();
    std::vector<bool> changes;
    demand->SetCallback([&changes](bool active) {
        changes.push_back(active);
    });
    {
        JsRuntime rt;
        rt.SetAudioResponseDemand(rstd::Some(demand.clone()));
        auto* fs = MakeProbe(rt,
                             "test/audio_demand",
                             R"JS(
            engine.registerAudioBuffers(64);
            engine.registerAudioBuffers(32);
            export function update() { return 1; }
        )JS");
        ASSERT_NE(fs, nullptr);
        EXPECT_TRUE(demand->Active());
    }
    EXPECT_FALSE(demand->Active());
    EXPECT_EQ(changes, (std::vector<bool> { false, true, false }));
}

// ---------------------------------------------------------------------------
// SceneNode wrapper surface

TEST(ScriptNodeSize, ParserSetSizeFlowsToScript) {
    owe::SceneNode node;
    node.SetSize({ 320.0f, 240.0f });

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() { return thisLayer.size.x + thisLayer.size.y * 1000; }
        )JS",
        "test/node_size_real",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 320.0 + 240.0 * 1000);
}

TEST(ScriptNodeParent, CursorCallbackParentChainTerminatesAtUnparentedNode) {
    auto root  = Arc<owe::SceneNode>::make();
    auto child = Arc<owe::SceneNode>::make();
    root->AppendChild(child.clone());
    child->SetTranslate({ 500.0f, 500.0f, 0.0f });
    child->SetSize({ 200.0f, 200.0f });

    JsRuntime   rt;
    FrameInputs fi {};
    fi.canvas_w               = 1920.0f;
    fi.canvas_h               = 1080.0f;
    fi.cursor_in_window       = true;
    fi.cursor_x               = 500.0f / fi.canvas_w;
    fi.cursor_y               = 1.0f - 500.0f / fi.canvas_h;
    fi.mouse_buttons_released = 1u << 0;
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let result = 0;
            export function cursorUp() {
                let layer = thisLayer;
                let depth = 0;
                while (typeof layer !== 'undefined' && layer != null && depth < 8) {
                    depth++;
                    layer = layer.getParent();
                }
                result = typeof layer === 'undefined' ? depth : -depth;
            }
            export function update() { return result; }
        )JS",
        "test/parent_chain_terminates",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        child.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 2.0);
}

TEST(ScriptNodeParent, DefaultLayerParentIsUndefined) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                return typeof thisLayer.getParent() === 'undefined' ? 1 : 0;
            }
        )JS",
        "test/default_layer_parent",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0));
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 1.0);
}

TEST(ScriptNodeSoftMutation, VisibleAndAlphaWrites) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            // Toggle alpha and visibility from script.
            thisLayer.alpha = 0.25;
            thisLayer.visible = false;
            export function update() {}
        )JS",
        "test/visible_alpha_writes",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_TRUE(node.IsAlphaOverridden());
    EXPECT_EQ(node.UserAlpha(), 0.25f);
    EXPECT_FALSE(node.Visible());
    EXPECT_EQ(node.EffectiveAlpha(), 0.0f); // hidden wins
}

TEST(ScriptNodeSoftMutation, VisibleWritesUseSceneVisibilityOwner) {
    owe::Scene scene;
    auto       node = Arc<owe::SceneNode>::make();
    node->ID()      = rstd::i32(17);
    scene.RootMut()->AppendChild(node.clone());

    JsRuntime rt;
    rt.SetScene(&scene);
    auto* script = rt.MakeFieldScript(
        R"JS(
            export function init() { thisLayer.visible = false; }
            export function update() { return thisLayer.visible ? 1 : 0; }
        )JS",
        "test/scene_owned_visibility",
        FieldKind::Bool,
        owe::MakeObject(),
        owe::IntoJson(true),
        node.as_ptr());
    ASSERT_NE(script, nullptr);

    rt.SetSceneRoot(scene.RootMut().as_raw_ptr());
    rt.TickAll();

    EXPECT_FALSE(node->Visible());
    EXPECT_TRUE(scene.IsLayerVisibilityElidable(owe::WallpaperLayerId { .value = rstd::i32(17) }));
    EXPECT_TRUE(scene.ConsumeRenderGraphDirty());
}

TEST(ScriptNodeSoftMutation, VisibleTrueRestoresUserAlpha) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.alpha = 0.4;
            thisLayer.visible = false;
            thisLayer.visible = true;
            export function update() {}
        )JS",
        "test/visible_restore",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_TRUE(node.Visible());
    EXPECT_FLOAT_EQ(node.EffectiveAlpha(), 0.4f);
}

TEST(ScriptNodeSoftMutation, PerspectiveWritesNodeFlag) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.perspective = true;
            export function update() { return thisLayer.perspective ? 1 : 0; }
        )JS",
        "test/perspective_write",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_TRUE(node.Perspective());
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);
}

TEST(ScriptNodeSoftMutation, ImageAlignmentDispatchesRegisteredSetter) {
    owe::SceneNode node;
    JsRuntime      rt;
    String         alignment;
    rt.RegisterImageAlignmentSetter(
        &node, "center"_str, JsRuntime::ImageAlignmentSetter::make([&](ref<str> value) {
            alignment = String::make(value);
        }));

    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.alignment = 'bottom';
            export function update() { return thisLayer.alignment === 'bottom' ? 1 : 0; }
        )JS",
        "test/image_alignment_write",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_EQ(alignment.as_str(), "bottom"_str);
    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 1.0);
}

TEST(ScriptNodeActuator, AlphaFieldReturnWritesNodeAlpha) {
    auto node = rstd::sync::Arc<owe::SceneNode>::make();

    ScriptScene ss;
    auto*       fs = ss.runtime().MakeFieldScript(
        R"JS(
            export function update() { return 0.125; }
        )JS",
        "test/alpha_field_return",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(1.0),
        node.as_ptr());
    ASSERT_NE(fs, nullptr);
    ss.AddActuator({ fs, MakeNodeAlphaApply(node.clone()) });

    FrameInputs fi {};
    ss.Tick(fi);

    EXPECT_TRUE(node->IsAlphaOverridden());
    EXPECT_FLOAT_EQ(node->UserAlpha(), 0.125f);
    EXPECT_FLOAT_EQ(node->EffectiveAlpha(), 0.125f);
}

TEST(ScriptNodeActuator, ColorFieldReturnWritesNodeColor) {
    auto node = Arc<owe::SceneNode>::make();

    ScriptScene ss;
    auto*       fs = ss.runtime().MakeFieldScript(
        R"JS(
            export function update() { return new Vec3(0.2, 0.4, 0.6); }
        )JS",
        "test/color_field_return",
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("1 1 1"),
        node.as_ptr());
    ASSERT_NE(fs, nullptr);
    ss.AddActuator({ fs, MakeNodeColorApply(node.clone()) });

    FrameInputs fi {};
    ss.Tick(fi);

    EXPECT_FLOAT_EQ(node->Color().x(), 0.2f);
    EXPECT_FLOAT_EQ(node->Color().y(), 0.4f);
    EXPECT_FLOAT_EQ(node->Color().z(), 0.6f);
}

TEST(SceneNodeRuntimeAlpha, AlphaSourceContributesOverride) {
    owe::SceneNode source;
    owe::SceneNode composite;
    composite.SetAlphaSource(&source);

    EXPECT_FALSE(composite.IsAlphaOverridden());
    EXPECT_FLOAT_EQ(composite.EffectiveAlpha(), 1.0f);

    source.SetUserAlpha(0.25f);
    EXPECT_TRUE(composite.IsAlphaOverridden());
    EXPECT_FLOAT_EQ(composite.EffectiveAlpha(), 0.25f);

    composite.SetUserAlpha(0.5f);
    EXPECT_FLOAT_EQ(composite.EffectiveAlpha(), 0.125f);

    source.SetVisible(false);
    EXPECT_FLOAT_EQ(composite.EffectiveAlpha(), 0.0f);
}

TEST(ScriptNodeSoftMutation, BrightnessAndColorWrites) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.brightness = 1.5;
            thisLayer.color = new Vec3(1, 0.5, 0);
            export function update() {}
        )JS",
        "test/brightness_color",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_TRUE(node.IsBrightnessOverridden());
    EXPECT_FLOAT_EQ(node.Brightness(), 1.5f);
    EXPECT_TRUE(node.IsColorOverridden());
    EXPECT_FLOAT_EQ(node.Color().x(), 1.0f);
    EXPECT_FLOAT_EQ(node.Color().y(), 0.5f);
    EXPECT_FLOAT_EQ(node.Color().z(), 0.0f);
}

TEST(ScriptNodeSoftMutation, NoWritesLeaveOverridesUnset) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            // Reads only; baked material values should stand.
            let a = thisLayer.alpha;
            let v = thisLayer.visible;
            export function update() {}
        )JS",
        "test/no_writes",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_FALSE(node.IsAlphaOverridden());
    EXPECT_FALSE(node.IsBrightnessOverridden());
    EXPECT_FALSE(node.IsColorOverridden());
}

// ---------------------------------------------------------------------------
// Cursor event dispatch

namespace
{
FrameInputs MakeFi(float canvas_w = 1920.0f, float canvas_h = 1080.0f) {
    FrameInputs fi {};
    fi.canvas_w = canvas_w;
    fi.canvas_h = canvas_h;
    return fi;
}
} // namespace

TEST(ScriptCursor, EnterLeaveAndMove) {
    // A 200×200 layer centered at (500, 500). Cursor at (500/1920, 500/1080)
    // sits inside; (100/1920, 100/1080) sits outside.
    owe::SceneNode node;
    node.SetTranslate({ 500.0f, 500.0f, 0.0f });
    node.SetSize({ 200.0f, 200.0f });

    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let enters = 0, leaves = 0, moves = 0;
            export function cursorEnter() { enters++; }
            export function cursorLeave() { leaves++; }
            export function cursorMove()  { moves++;  }
            export function update() { return enters * 1000000 + leaves * 1000 + moves; }
        )JS",
        "test/cursor_enter_leave_move",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    // Outside: no enter, no move.
    auto fi             = MakeFi();
    fi.cursor_in_window = true;
    fi.cursor_x         = 100.0f / 1920.0f;
    fi.cursor_y         = 100.0f / 1080.0f;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 0.0);

    // Move inside: 1 enter + 1 move.
    fi.cursor_x = 500.0f / 1920.0f;
    fi.cursor_y = 500.0f / 1080.0f;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1'000'001);

    // Still inside (no edge): +1 move.
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1'000'002);

    // Move outside: +1 leave (no move when outside).
    fi.cursor_x = 100.0f / 1920.0f;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1'001'002);
}

TEST(ScriptCursor, ClickAndDownUpInside) {
    owe::SceneNode node;
    node.SetTranslate({ 500.0f, 500.0f, 0.0f });
    node.SetSize({ 200.0f, 200.0f });

    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let down = 0, up = 0, click = 0, last_btn = -9;
            export function cursorDown(e)  { down++;  last_btn = e.button; }
            export function cursorUp(e)    { up++;    last_btn = e.button; }
            export function cursorClick(e) { click++; last_btn = e.button; }
            export function update() {
                return down * 10000 + up * 100 + click + (last_btn + 1) * 1000000;
            }
        )JS",
        "test/cursor_click",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    auto fi             = MakeFi();
    fi.cursor_in_window = true;
    fi.cursor_x         = 500.0f / 1920.0f;
    fi.cursor_y         = 500.0f / 1080.0f;
    // Press left button (bit 0) this frame.
    fi.mouse_buttons_pressed = 1u << 0;
    fi.mouse_buttons_down    = 1u << 0;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    // 1 down, 1 click, last_btn = 0 → 1*1000000 + 1*10000 + 0*100 + 1 = 1010001
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1'010'001);

    // Release this frame (no press): 1 up, last_btn=0.
    fi.mouse_buttons_pressed  = 0;
    fi.mouse_buttons_released = 1u << 0;
    fi.mouse_buttons_down     = 0;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1'010'101);
}

TEST(ScriptCursor, ClickOutsideIsIgnored) {
    owe::SceneNode node;
    node.SetTranslate({ 500.0f, 500.0f, 0.0f });
    node.SetSize({ 200.0f, 200.0f });

    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let click = 0;
            export function cursorClick() { click++; }
            export function update() { return click; }
        )JS",
        "test/cursor_outside_click",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    auto fi                  = MakeFi();
    fi.cursor_in_window      = true;
    fi.cursor_x              = 100.0f / 1920.0f; // outside the AABB
    fi.cursor_y              = 100.0f / 1080.0f;
    fi.mouse_buttons_pressed = 1u << 0;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 0.0);
}

TEST(ScriptCursor, CursorOutOfWindowSuppressesEvents) {
    owe::SceneNode node;
    node.SetTranslate({ 500.0f, 500.0f, 0.0f });
    node.SetSize({ 200.0f, 200.0f });

    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let n = 0;
            export function cursorEnter() { n++; }
            export function cursorMove()  { n++; }
            export function update() { return n; }
        )JS",
        "test/cursor_out_of_window",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    auto fi             = MakeFi();
    fi.cursor_in_window = false; // outside window: events suppressed
    fi.cursor_x         = 500.0f / 1920.0f;
    fi.cursor_y         = 500.0f / 1080.0f;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 0.0);
}

TEST(ScriptCursor, GlobalInputRefreshesFrameFields) {
    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                return input.cursorScreenPosition.x +
                       input.cursorScreenPosition.y * 1000 +
                       (input.cursorLeftDown ? 1000000 : 0) +
                       input.mouseButtonsDown * 10000000;
            }
        )JS",
        "test/global_input_refresh",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0));
    ASSERT_NE(fs, nullptr);

    auto fi               = MakeFi();
    fi.screen_w           = 800.0f;
    fi.screen_h           = 600.0f;
    fi.cursor_x           = 0.25f;
    fi.cursor_y           = 0.5f;
    fi.mouse_buttons_down = 1u << 0;
    fi.cursor_in_window   = true;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 11'300'200.0);

    fi.cursor_x           = 0.5f;
    fi.mouse_buttons_down = 0;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 300'400.0);
}

TEST(ScriptCursor, WorldPositionFlipsTopDownInputY) {
    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                return new Vec3(
                    input.cursorWorldPosition.x,
                    input.cursorWorldPosition.y,
                    input.cursorScreenPosition.y);
            }
        )JS",
        "test/global_input_world_y",
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0.0 0.0 0.0"));
    ASSERT_NE(fs, nullptr);

    auto fi     = MakeFi();
    fi.screen_h = 600.0f;
    fi.cursor_x = 0.25f;
    fi.cursor_y = 0.25f;
    rt.SetFrameInputs(fi);
    rt.TickAll();

    ASSERT_TRUE(std::holds_alternative<Vec3Value>(fs->last_value()));
    const auto& v = std::get<Vec3Value>(fs->last_value());
    EXPECT_NEAR(v.x, 480.0, 0.001);
    EXPECT_NEAR(v.y, 810.0, 0.001);
    EXPECT_NEAR(v.z, 150.0, 0.001);
}

// ---------------------------------------------------------------------------
// Texture animation override

TEST(ScriptTexAnim, SetFramePinsAndStopsPlayback) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let anim = thisLayer.getTextureAnimation();
            anim.setFrame(2);
            export function update() {
                return anim.getFrame() * 10 + (anim.isPlaying() ? 1 : 0);
            }
        )JS",
        "test/texanim_setframe",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(node.TexAnim().current_frame, 2);
    EXPECT_FALSE(node.TexAnim().playing);
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 20.0);
}

TEST(ScriptTexAnim, PlayResumesAutoAdvance) {
    owe::SceneNode node;
    node.TexAnim().current_frame = 5;
    node.TexAnim().playing       = false;

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.getTextureAnimation().play();
            export function update() {}
        )JS",
        "test/texanim_play",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(node.TexAnim().current_frame, -1);
    EXPECT_TRUE(node.TexAnim().playing);
}

TEST(ScriptTexAnim, PauseFreezesAtCurrent) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.getTextureAnimation().pause();
            export function update() {}
        )JS",
        "test/texanim_pause",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_FALSE(node.TexAnim().playing);
    EXPECT_EQ(node.TexAnim().current_frame, -1); // pause keeps auto cursor
}

TEST(ScriptTexAnim, UnboundLayerFallsBackToJsStub) {
    // No node bound — getTextureAnimation() returns the JS-side stub from
    // the bootstrap, which silently accepts setFrame / play / etc.
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let a = thisLayer.getTextureAnimation();
            a.setFrame(7);
            export function update() { return a.getFrame(); }
        )JS",
        "test/texanim_unbound",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    // JS stub records the frame in a closure local; getFrame returns it.
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 7.0);
}

// ---------------------------------------------------------------------------
// localStorage

namespace
{
std::string MakeTmpLsPath(const char* tag) {
    auto p = std::filesystem::temp_directory_path() / (std::string("owe_ls_") + tag + ".json");
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p.native();
}
} // namespace

TEST(ScriptLocalStorage, InMemoryWithoutPersistencePath) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            localStorage.set('k', 42);
            localStorage.set('o', { a: 1, b: 'two' });
            export function update() {
                let v = localStorage.get('k');
                let o = localStorage.get('o');
                return v + (o ? o.a + (o.b === 'two' ? 100 : 0) : 0);
            }
        )JS",
        "test/ls_inmemory",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 42 + 1 + 100);
}

TEST(ScriptLocalStorage, RemoveDeletesKey) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            localStorage.set('k', 9);
            localStorage.remove('k');
            export function update() {
                let v = localStorage.get('k');
                return v === undefined ? -1 : v;
            }
        )JS",
        "test/ls_remove",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, -1.0);
}

TEST(ScriptLocalStorage, PersistsAcrossRuntimes) {
    const std::string path = MakeTmpLsPath("persist");

    {
        JsRuntime rt;
        rt.SetPersistence(path);
        FrameInputs fi {};
        rt.SetFrameInputs(fi);
        auto* fs = rt.MakeFieldScript(
            R"JS(
                localStorage.set('count', 7);
                localStorage.set('label', 'hello');
                export function update() {}
            )JS",
            "test/ls_writer",
            FieldKind::Scalar,
            owe::MakeObject(),
            owe::IntoJson(0),
            nullptr);
        ASSERT_NE(fs, nullptr);
    }

    // Fresh runtime reading the same file should see the prior writes.
    {
        JsRuntime rt;
        rt.SetPersistence(path);
        FrameInputs fi {};
        rt.SetFrameInputs(fi);
        auto* fs = rt.MakeFieldScript(
            R"JS(
                export function update() {
                    let c = localStorage.get('count');
                    let l = localStorage.get('label');
                    return (c ?? -1) + (l === 'hello' ? 1000 : 0);
                }
            )JS",
            "test/ls_reader",
            FieldKind::Scalar,
            owe::MakeObject(),
            owe::IntoJson(0),
            nullptr);
        ASSERT_NE(fs, nullptr);
        rt.TickAll();
        EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 7 + 1000);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ScriptLocalStorage, ObjectRoundTrip) {
    const std::string path = MakeTmpLsPath("obj");

    {
        JsRuntime rt;
        rt.SetPersistence(path);
        FrameInputs fi {};
        rt.SetFrameInputs(fi);
        rt.MakeFieldScript(
            R"JS(
                localStorage.set('pos', { x: 10, y: 20 });
                export function update() {}
            )JS",
            "test/ls_obj_write",
            FieldKind::Scalar,
            owe::MakeObject(),
            owe::IntoJson(0),
            nullptr);
    }
    {
        JsRuntime rt;
        rt.SetPersistence(path);
        FrameInputs fi {};
        rt.SetFrameInputs(fi);
        auto* fs = rt.MakeFieldScript(
            R"JS(
                export function update() {
                    let p = localStorage.get('pos');
                    return (p && p.x === 10 && p.y === 20) ? 1 : 0;
                }
            )JS",
            "test/ls_obj_read",
            FieldKind::Scalar,
            owe::MakeObject(),
            owe::IntoJson(0),
            nullptr);
        rt.TickAll();
        EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ScriptNodeChildren, WalksSceneNodeChildren) {
    auto parent = rstd::sync::Arc<owe::SceneNode>::make();
    auto a      = rstd::sync::Arc<owe::SceneNode>::make();
    auto b      = rstd::sync::Arc<owe::SceneNode>::make();
    parent->AppendChild(a.clone());
    parent->AppendChild(b.clone());
    a->SetTranslate({ 10.0f, 0.0f, 0.0f });
    b->SetTranslate({ 20.0f, 0.0f, 0.0f });

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                const cs = thisLayer.getChildren();
                return cs.length * 1000 + (cs[0] ? cs[0].origin.x : 0)
                                       + (cs[1] ? cs[1].origin.x : 0);
            }
        )JS",
        "test/getChildren_walk",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        parent.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 2000 + 10 + 20);
}

TEST(ScriptLayerLookup, MissingLayerHandleResolvesLater) {
    auto root = rstd::sync::Arc<owe::SceneNode>::make();

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    rt.SetSceneRoot(root.as_ptr());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let late;
            export function init() {
                late = thisScene.getLayer("late-sound");
                late.stop();
            }
            export function applyUserProperties(changed) {
                if (changed.go) late.play();
            }
            export function update() { return late.isPlaying() ? 1 : 0; }
        )JS",
        "test/lazy_layer_lookup",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root.as_ptr());
    ASSERT_NE(fs, nullptr);

    auto late = rstd::sync::Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "late-sound");
    root->AppendChild(late.clone());
    rt.SetUserProperty("go", rstd::json::from_str(R"({"type":"bool","value":true})"_str).unwrap());
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);
}

TEST(ScriptLayerLookup, GetEffectVisibleWritesSceneDirty) {
    owe::Scene scene;
    auto       root  = Box<owe::SceneNode>::make();
    auto       layer = rstd::sync::Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "audio-layer");
    root->AppendChild(layer.clone());
    auto root_pointer = root.get();
    scene.SetRoot(rstd::move(root));

    layer->SetCamera("audio-effect-camera");
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(256, 256, -1.0, 1.0));
    auto effect_layer = std::make_shared<owe::SceneImageEffectLayer>(
        layer.as_ptr(), 256.0f, 256.0f, "_rt_effect_pingpong_a_test", "_rt_effect_pingpong_b_test");
    auto effect             = std::make_shared<owe::SceneImageEffect>();
    effect->name            = "audio-color";
    effect->runtime_visible = true;
    effect_layer->AddEffect(effect);
    camera->AttatchImgEffect(effect_layer);
    scene.RegisterCamera(String::make("audio-effect-camera"_str), rstd::move(camera));

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    rt.SetScene(&scene);
    rt.SetSceneRoot(root_pointer);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                const effect = thisScene.getLayer("audio-layer").getEffect("audio-color");
                effect.visible = false;
                return effect.visible ? 1 : 0;
            }
        )JS",
        "test/layer_get_effect_visible",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root_pointer);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 0.0);
    EXPECT_FALSE(effect->runtime_visible);
    EXPECT_TRUE(scene.ConsumeRenderGraphDirty());
    EXPECT_FALSE(scene.ConsumeRenderGraphDirty());
}

TEST(ScriptLayerLookup, EffectIndexAndMaterialWritesUseSceneMaterialOwner) {
    owe::Scene scene;
    auto       root  = Box<owe::SceneNode>::make();
    auto       layer = Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "color-layer");
    root->AppendChild(layer.clone());
    auto* root_pointer = root.get();
    scene.SetRoot(rstd::move(root));

    layer->SetCamera("color-effect-camera");
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(256, 256, -1.0, 1.0));
    auto effect_layer = std::make_shared<owe::SceneImageEffectLayer>(layer.as_ptr(),
                                                                     256.0f,
                                                                     256.0f,
                                                                     "_rt_effect_pingpong_a_color",
                                                                     "_rt_effect_pingpong_b_color");
    auto effect       = std::make_shared<owe::SceneImageEffect>();
    effect->name      = "color";
    auto                        effect_node = Arc<owe::SceneNode>::make();
    auto                        mesh        = std::make_shared<owe::SceneMesh>();
    owe::SceneMaterial          material;
    owe::SceneShaderVariantDesc variant;
    variant.uniform_aliases["color"]       = "g_TintColor";
    variant.uniform_aliases["channelMask"] = "g_ChannelMask";
    material.customShader.variant          = std::move(variant);
    material.customShader.constValues["g_TintColor"] =
        owe::ShaderValue(rstd::array<float, 3> { 1.0f, 0.0f, 0.0f });
    material.customShader.constValues["g_ChannelMask"] =
        owe::ShaderValue(rstd::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f });
    mesh->AddMaterial(std::move(material));
    auto* effect_material = mesh->Material();
    effect_node->AddMesh(std::move(mesh));
    effect->nodes.push_back(owe::SceneImageEffectNode {
        .output    = "_rt_effect_pingpong_b_color",
        .sceneNode = effect_node.clone(),
    });
    effect_layer->AddEffect(effect);
    camera->AttatchImgEffect(effect_layer);
    scene.RegisterCamera(String::make("color-effect-camera"_str), rstd::move(camera));

    JsRuntime rt;
    rt.SetScene(&scene);
    rt.SetSceneRoot(root_pointer);
    auto  properties = rstd::json::from_str(R"({"color":"0.2 0.4 0.6"})"_str).unwrap();
    auto* fs         = rt.MakeFieldScript(
        R"JS(
            export var scriptProperties = createScriptProperties()
                .addColor({ name: 'color', value: new Vec3(1, 0, 0) })
                .finish();
            export function update() {
                const effect = thisLayer.getEffect(0);
                effect.getMaterial(0).color = scriptProperties.color;
                effect.getMaterial(0).channelMask = new Vec4(0, 0.25, 0.5, 0.75);
                return thisLayer.getEffectCount() + (effect.name === "color" ? 1 : 0);
            }
        )JS",
        "test/layer_effect_material",
        FieldKind::Scalar,
        properties,
        owe::IntoJson(0),
        layer.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 2.0);
    ASSERT_NE(effect_material, nullptr);
    auto color = effect_material->customShader.constValues.find("g_TintColor");
    ASSERT_NE(color, effect_material->customShader.constValues.end());
    ASSERT_EQ(color->second.size(), usize(3));
    EXPECT_FLOAT_EQ(color->second[usize()], 0.2f);
    EXPECT_FLOAT_EQ(color->second[usize(1)], 0.4f);
    EXPECT_FLOAT_EQ(color->second[usize(2)], 0.6f);
    auto channel_mask = effect_material->customShader.constValues.find("g_ChannelMask");
    ASSERT_NE(channel_mask, effect_material->customShader.constValues.end());
    ASSERT_EQ(channel_mask->second.size(), usize(4));
    EXPECT_FLOAT_EQ(channel_mask->second[usize()], 0.0f);
    EXPECT_FLOAT_EQ(channel_mask->second[usize(1)], 0.25f);
    EXPECT_FLOAT_EQ(channel_mask->second[usize(2)], 0.5f);
    EXPECT_FLOAT_EQ(channel_mask->second[usize(3)], 0.75f);
}

TEST(ScriptLayerLookup, MissingLayerKeepsDefaultTransformShape) {
    auto root = rstd::sync::Arc<owe::SceneNode>::make();

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    rt.SetSceneRoot(root.as_ptr());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let resolved = -1;
            export function init() {
                const late = thisScene.getLayer("late-sound");
                resolved = late.scale.x + late.origin.x + late.angles.x;
            }
            export function update() { return resolved; }
        )JS",
        "test/lazy_layer_default_transform",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);
}

TEST(ScriptWEMath, SmoothStepCamelCaseAndAliases) {
    // ~165 corpus callsites use camelCase smoothStep; lowercase exists too.
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            import * as M from 'WEMath';
            export function update() {
                // smoothStep(0,1,0.5) → 0.5
                let a = M.smoothStep(0, 1, 0.5);
                let b = M.smoothstep(0, 1, 0.5);
                let c = M.deg2rad(180);   // ≈ π
                let d = M.rad2deg(Math.PI);  // 180
                let e = 180 * M.deg2rad;
                let f = Math.PI * M.rad2deg;
                return Math.round(a * 100) + Math.round(b * 100) * 100
                       + Math.round(c * 1000) * 10000   // π*1000 ≈ 3142
                       + Math.round(d) * 1000000000
                       + Math.round((e - c) * 1000000)
                       + Math.round((f - d) * 1000000);
            }
        )JS",
        "test/wemath_smoothstep",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    // expected: 50 + 50*100 + 3142*10000 + 180*1e9
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v,
              50.0 + 50.0 * 100 + 3142.0 * 10000 + 180.0 * 1e9);
}

TEST(ScriptVector, InstanceMixInterpolatesVectors) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update(value) {
                let a = new Vec3(1, 2, 3).mix(new Vec3(5, 6, 7), 0.25);
                let b = new Vec2(2, 6).mix(new Vec2(10, 14), 0.5);
                let c = new Vec3(2).mix(6, 0.25);
                return new Vec3(a.x + b.x, a.y + b.y, a.z + c.z);
            }
        )JS",
        "test/vector_mix",
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0.0 0.0 0.0"),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    ASSERT_TRUE(std::holds_alternative<Vec3Value>(fs->last_value()));
    const auto& v = std::get<Vec3Value>(fs->last_value());
    EXPECT_NEAR(v.x, 8.0, 0.001);
    EXPECT_NEAR(v.y, 13.0, 0.001);
    EXPECT_NEAR(v.z, 7.0, 0.001);
}

TEST(ScriptVector, Vec2ConstructorCopiesVectorComponents) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update(value) {
                let fromVec3 = new Vec2(new Vec3(100, 200, 300));
                let fromObject = new Vec2({ x: 3, y: 4, z: 5 });
                return new Vec3(fromVec3.x, fromVec3.y, fromObject.length());
            }
        )JS",
        "test/vector_vec2_copy_ctor",
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0.0 0.0 0.0"),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    ASSERT_TRUE(std::holds_alternative<Vec3Value>(fs->last_value()));
    const auto& v = std::get<Vec3Value>(fs->last_value());
    EXPECT_NEAR(v.x, 100.0, 0.001);
    EXPECT_NEAR(v.y, 200.0, 0.001);
    EXPECT_NEAR(v.z, 5.0, 0.001);
}

TEST(ScriptVector, LengthSqrMatchesWallpaperEngineVectors) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update(value) {
                return new Vec3(2, 3, 6).lengthSqr() + new Vec2(5, 12).lengthSqr();
            }
        )JS",
        "test/vector_length_sqr",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_NEAR(std::get<ScalarValue>(fs->last_value()).v, 218.0, 0.001);
}

TEST(ScriptVector, NormalizeReturnsUnitVectors) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update(value) {
                let a = new Vec3(3, 4, 0).normalize();
                let b = new Vec2(0, 5).normalize();
                let z = new Vec3(0, 0, 0).normalize();
                return new Vec3(a.x, a.y + b.y * 10, z.length());
            }
        )JS",
        "test/vector_normalize",
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0.0 0.0 0.0"),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    ASSERT_TRUE(std::holds_alternative<Vec3Value>(fs->last_value()));
    const auto& v = std::get<Vec3Value>(fs->last_value());
    EXPECT_NEAR(v.x, 0.6, 0.001);
    EXPECT_NEAR(v.y, 10.8, 0.001);
    EXPECT_NEAR(v.z, 0.0, 0.001);
}

TEST(ScriptVector, EngineCanvasSizeSupportsVectorMethods) {
    JsRuntime   rt;
    FrameInputs fi {};
    fi.canvas_w = 3840.0f;
    fi.canvas_h = 2160.0f;
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                const v = engine.canvasSize.divide(2);
                return v.x + v.y * 10000;
            }
        )JS",
        "test/canvas_size_vec2_methods",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1920.0 + 1080.0 * 10000);
}

TEST(ScriptScene, InitialLayerConfigPreservesAuthoredEffects) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    rt.RegisterInitialLayerConfig(
        &node,
        rstd::json::from_str(
            R"({"name":"Brush","effects":[{"name":"Square"},{"name":"Glider"}]})"_str)
            .unwrap());
    rt.SetSceneRoot(&node);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let seen = -1;
            export function init() {
                const cfg = thisScene.getInitialLayerConfig(thisLayer);
                seen = cfg.name === 'Brush' &&
                       cfg.effects[0].name === 'Square' &&
                       cfg.effects[1].name === 'Glider'
                    ? cfg.effects.length
                    : -1;
            }
            export function update() { return seen; }
        )JS",
        "test/initial_layer_config",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 2.0);
}

TEST(ScriptScene, DestroyLayerHidesSceneNode) {
    auto root  = rstd::sync::Arc<owe::SceneNode>::make();
    auto child = rstd::sync::Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "coin");
    root->AppendChild(child.clone());

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    rt.SetSceneRoot(root.as_ptr());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let hidden = 0;
            export function init() {
                const coin = thisScene.getLayer("coin");
                thisScene.destroyLayer(coin);
                hidden = coin.visible ? 0 : 1;
            }
            export function update() { return hidden; }
        )JS",
        "test/destroy_layer_hides_node",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_FALSE(child->Visible());
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);
}

TEST(ScriptScene, CreateLayerUsesRegisteredAssetQueue) {
    auto root = rstd::sync::Arc<owe::SceneNode>::make();
    auto coin = rstd::sync::Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "coin-clone");
    coin->SetVisible(false);

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    rt.SetSceneRoot(root.as_ptr());

    std::unordered_map<std::string, std::vector<owe::SceneNode*>> assets;
    assets["models/coin_0.json"].push_back(coin.as_ptr());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let ok = 0;
            export function init() {
                const asset = engine.registerAsset('models/coin_0.json');
                const coin = thisScene.createLayer(asset);
                coin.origin = new Vec3(42, 7, 0);
                thisScene.destroyLayer(coin);
                const again = thisScene.createLayer(asset);
                again.origin = new Vec3(8, 9, 0);
                ok = again.visible && again.origin.x === 8 && again.origin.y === 9 ? 1 : 0;
            }
            export function update() { return ok; }
        )JS",
        "test/create_layer_asset_queue",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root.as_ptr(),
        std::vector<owe::SceneNode*> {},
        std::move(assets));
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_TRUE(coin->Visible());
    EXPECT_FLOAT_EQ(coin->Translate().x(), 8.0f);
    EXPECT_FLOAT_EQ(coin->Translate().y(), 9.0f);
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);
}

TEST(ScriptScene, CreateLayerActivatesOnlyConsumedGenericClones) {
    auto root  = Arc<owe::SceneNode>::make();
    auto first = Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "first");
    auto second = Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "second");
    first->SetVisible(false);
    second->SetVisible(false);

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    rt.SetSceneRoot(root.as_ptr());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let ok = 0;
            export function init() {
                const layer = thisScene.createLayer('models/bar.json');
                ok = layer.visible ? 1 : 0;
            }
            export function update() { return ok; }
        )JS",
        "test/create_layer_activates_consumed_generic_clone",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root.as_ptr(),
        std::vector<owe::SceneNode*> { first.as_ptr(), second.as_ptr() });
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_TRUE(first->Visible());
    EXPECT_FALSE(second->Visible());
    EXPECT_DOUBLE_EQ(LastScalar(fs), 1.0);
}

// ---------------------------------------------------------------------------
// Workshop 3327063360 repro: scripted-origin layer should land at canvas
// center when scriptProperties.{x,y} fall back to their declared 0.5.

TEST(ScriptUserProperty, UserPropertyOverridesFallback) {
    // ResolveConfigValue stores the {user, value} wrapper verbatim; the
    // bootstrap getter unwraps at access time. SetUserProperty in
    // between should win.
    JsRuntime rt;
    owe::Json properties = rstd::json::from_str(R"({"x":{"user":"x1","value":0.5}})"_str).unwrap();
    rt.SetUserProperty("x1",
                       rstd::json::from_str(R"({"type":"slider","value":-0.665})"_str).unwrap());
    FrameInputs fi {};
    fi.canvas_w = 3840.0f;
    fi.canvas_h = 2160.0f;
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export var scriptProperties = createScriptProperties()
              .addSlider({ name: 'x', value: 0.5, min: 0, max: 1 })
              .finish();
            export function update() { return scriptProperties.x; }
        )JS",
        "test/user_prop_override",
        FieldKind::Scalar,
        properties,
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    // User value passes through verbatim — WE doesn't clamp, even when the
    // user's slider range (e.g. project.json [-1,1]) exceeds the script's
    // declared range. Workshop 3327063360 relies on this: x1=-0.665 fed
    // into `scriptProperties.x * canvasSize.x` produces a negative offset
    // that shifts the Clock cluster off the master-component origin.
    EXPECT_NEAR(std::get<ScalarValue>(fs->last_value()).v, -0.665, 1e-4);
}

TEST(ScriptUserProperty, FallbackWhenUserPropMissing) {
    JsRuntime rt;
    owe::Json properties =
        rstd::json::from_str(R"({"x":{"user":"missing","value":0.5}})"_str).unwrap();
    FrameInputs fi {};
    fi.canvas_w = 3840.0f;
    fi.canvas_h = 2160.0f;
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export var scriptProperties = createScriptProperties()
              .addSlider({ name: 'x', value: 0.5, min: 0, max: 1 })
              .finish();
            export function update() { return scriptProperties.x; }
        )JS",
        "test/user_prop_fallback",
        FieldKind::Scalar,
        properties,
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_NEAR(std::get<ScalarValue>(fs->last_value()).v, 0.5, 1e-4);
}

TEST(ScriptUserProperty, ApplyUserPropertiesReceivesUnwrappedValue) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/apply_user_properties_value",
                         R"JS(
        let seen = 0;
        export function applyUserProperties(changed) {
            if (changed.music === "5") seen = 1;
        }
        export function update() { return seen; }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.SetUserProperty("music",
                       rstd::json::from_str(R"({"type":"combo","value":"5"})"_str).unwrap());
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);
}

TEST(ScriptUserProperty, TextInputValueRemainsAString) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/apply_text_user_property",
                         R"JS(
        let seen = 0;
        export function applyUserProperties(changed) {
            if (changed.text === "true" && typeof changed.text === "string") seen = 1;
        }
        export function update() { return seen; }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.SetUserProperty("text",
                       rstd::json::from_str(R"({"type":"textinput","value":"true"})"_str).unwrap());
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);
}

TEST(ScriptMedia, DispatchesPropertiesPlaybackAndThumbnailEvents) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/media_events",
                         R"JS(
        let props = 0, playback = 0, thumb = 0;
        export function mediaPropertiesChanged(event) {
            if (event.title === "Song" && event.artist === "Artist" &&
                event.album === "Album" && event.albumArtist === "Album Artist") {
                props = 1;
            }
        }
        export function mediaPlaybackChanged(event) {
            if (event.state === MediaPlaybackEvent.PLAYBACK_PLAYING) playback = 1;
        }
        export function mediaThumbnailChanged(event) {
            thisObject.visible = event.hasThumbnail;
            const colorDelta = event.textColor.subtract(event.tertiaryColor)
                .add(event.highContrastColor);
            if (event.hasThumbnail && event.thumbnail === "/tmp/cover.png" &&
                event.previousThumbnail === "/tmp/previous.png" &&
                event.primaryColor.x === 1 && event.secondaryColor.x === 0 &&
                event.tertiaryColor.x === 0 && colorDelta.lengthSqr() === 0) {
                thumb = 1;
            }
        }
        export function update() { return props && playback && thumb && thisObject.visible ? 1 : 0; }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.SetMediaStatus(MediaStatus { .state            = 1,
                                    .title            = "Song",
                                    .artist           = "Artist",
                                    .album            = "Album",
                                    .album_artist     = "Album Artist",
                                    .art_url          = "/tmp/cover.png",
                                    .previous_art_url = "/tmp/previous.png" });
    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 1.0);
}

TEST(ScriptUserProperty, ScriptedOriginLandsAtCenter) {
    JsRuntime   rt;
    FrameInputs fi {};
    fi.canvas_w = 3840.0f;
    fi.canvas_h = 2160.0f;
    rt.SetFrameInputs(fi);

    owe::Json properties =
        rstd::json::from_str(R"({"x":{"user":"x7","value":0.5},"y":{"user":"y8","value":0.5}})"_str)
            .unwrap();

    auto* fs = rt.MakeFieldScript(
        R"JS(
            'use strict';
            export var scriptProperties = createScriptProperties()
              .addSlider({ name: 'x', label: 'X', value: 0.5, min: 0, max: 1 })
              .addSlider({ name: 'y', label: 'Y', value: 0.5, min: 0, max: 1 })
              .finish();
            export function update(value) {
                value.x = scriptProperties.x * engine.canvasSize.x;
                value.y = scriptProperties.y * engine.canvasSize.y;
                return value;
            }
        )JS",
        "test/workshop_3327_repro",
        FieldKind::Vec3,
        properties,
        owe::IntoJson("1315.0 1419.0 0.0"),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    ASSERT_TRUE(std::holds_alternative<Vec3Value>(fs->last_value()));
    const auto& v = std::get<Vec3Value>(fs->last_value());
    EXPECT_NEAR(v.x, 1920.0, 0.5);
    EXPECT_NEAR(v.y, 1080.0, 0.5);
}

TEST(SceneNodeTrans, SetTranslateRecomputesModelTrans) {
    owe::SceneNode parent;
    parent.SetTranslate({ 100.0f, 200.0f, 0.0f });
    auto child = rstd::sync::Arc<owe::SceneNode>::make();
    child->SetTranslate({ 10.0f, 20.0f, 0.0f });
    parent.AppendChild(child.clone());

    child->UpdateTrans();
    Eigen::Matrix4d m1 = child->ModelTrans();
    EXPECT_DOUBLE_EQ(m1(0, 3), 110.0); // world x
    EXPECT_DOUBLE_EQ(m1(1, 3), 220.0); // world y

    // Mutate the parent and re-read the child without explicit dirty.
    parent.SetTranslate({ 500.0f, 600.0f, 0.0f });
    child->UpdateTrans();
    Eigen::Matrix4d m2 = child->ModelTrans();
    EXPECT_DOUBLE_EQ(m2(0, 3), 510.0);
    EXPECT_DOUBLE_EQ(m2(1, 3), 620.0);
}

TEST(SceneNodeTrans, SetScaleAndRotationMarkDirty) {
    owe::SceneNode n;
    n.UpdateTrans();
    // After first UpdateTrans the cache is clean.
    n.SetScale({ 2.0f, 2.0f, 1.0f });
    n.UpdateTrans();
    Eigen::Matrix4d m = n.ModelTrans();
    EXPECT_DOUBLE_EQ(m(0, 0), 2.0);
    EXPECT_DOUBLE_EQ(m(1, 1), 2.0);
}

TEST(ScriptNodeSize, UnsetFallsBackTo100x100) {
    owe::SceneNode node; // m_size defaults to (0,0)
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() { return thisLayer.size.x + thisLayer.size.y * 1000; }
        )JS",
        "test/node_size_unset",
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 100.0 + 100.0 * 1000);
}

TEST(ScriptTimer, ClearIntervalStops) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/clear_interval",
                         R"JS(
        let n = 0;
        let h = setInterval(() => { n++; }, 100);
        export function update() {
            if (n >= 2) clearInterval(h);
            return n;
        }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.25); // fires at 0.1, 0.2 → n=2
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 2.0);

    Tick(rt, 1.50); // would have fired many more, but update cleared it
    EXPECT_EQ(std::get<ScalarValue>(fs->last_value()).v, 2.0);
}
