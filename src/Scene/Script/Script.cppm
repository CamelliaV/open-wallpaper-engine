module;



export module wescene.script;
import nlohmann.json;
import wescene.core;
import rstd.cppstd;
import wescene.scene;

export namespace owe::script
{

// --- shared value variant ----------------------------------------------------

// Result of a script's update() call, after coercion. The variant is a
// snapshot of what the JS code returned this frame; the actuator (in the
// renderer) reads it and writes into the bound C++ field.
struct ScalarValue {
    double v { 0.0 };
};
struct Vec2Value {
    double x { 0.0 }, y { 0.0 };
};
struct Vec3Value {
    double x { 0.0 }, y { 0.0 }, z { 0.0 };
};
struct ColorValue {
    double r { 0.0 }, g { 0.0 }, b { 0.0 };
};
struct StringValue {
    std::string s;
};
struct BoolValue {
    bool v { false };
};

using ScriptValue = std::variant<std::monostate, ScalarValue, BoolValue,
                                  Vec2Value, Vec3Value, ColorValue, StringValue>;

// What kind of value a FieldScript is expected to produce. Set at parse
// time based on the field name's well-known type — see the per-field-kind
// table in the API doc.
enum class FieldKind {
    Unknown,
    Scalar,
    Bool,
    Vec2,
    Vec3,
    Color,
    String
};

// --- frame inputs -----------------------------------------------------------

// One snapshot of host-supplied per-frame state, fed by the renderer into
// `JsRuntime::TickFieldScripts` once per frame. Mirrors the engine.* fields
// the audio-response cluster (and the parallax cluster) actually read.
struct FrameInputs {
    float        frametime { 0.0f };  // seconds since last frame
    float        runtime { 0.0f };    // seconds since wallpaper start
    float        time_of_day { 0.0f }; // 0..1, 0=midnight, 0.5=noon
    float        canvas_w { 1920.0f };
    float        canvas_h { 1080.0f };
    float        screen_w { 1920.0f };
    float        screen_h { 1080.0f };
    // 16-bin (left+right averaged) audio buffer, populated by the audio
    // chain. Values are typically 0..1 with peaks above. Length is fixed
    // at 16 in MVP regardless of the script-requested resolution.
    std::array<float, 16> audio_average {};
};

// --- script properties (configuration) --------------------------------------

// One descriptor produced by createScriptProperties().addX() calls inside
// the JS module. Captured at module-load time and then merged with the
// per-binding `scriptproperties` config from scene.json before exposing
// the resulting `scriptProperties.<name>` accessors back to the script.
struct PropDescriptor {
    enum class Kind { Slider, Checkbox, Text, Combo, Color, Delimiter, Other };
    Kind        kind { Kind::Other };
    std::string name;
    std::string label;
    nlohmann::json default_value;  // captured verbatim
    double      min { 0.0 };
    double      max { 1.0 };
    bool        integer { false };
};

// --- runtime ----------------------------------------------------------------

class FieldScript;

// One JsRuntime per Scene. Owns one JSRuntime and one JSContext. Compiled
// modules are deduped by sha so duplicated sources across many bound fields
// only allocate once. The runtime is not thread-safe; the renderer's frame
// tick is the single owner.
class JsRuntime : NoCopy, NoMove {
public:
    JsRuntime();
    ~JsRuntime();

    // Returns nullptr on hard compile/init failure (logs once).
    FieldScript* MakeFieldScript(std::string_view source,
                                 std::string_view script_sha,
                                 FieldKind        field_kind,
                                 const nlohmann::json& properties_config,
                                 const nlohmann::json& initial_value);

    // Push one frame's worth of host state into the runtime. The next
    // FieldScript::Update call will see these values via `engine.*`.
    void SetFrameInputs(const FrameInputs& fi);

    // Drive every alive FieldScript once. Invokes their cached `update`
    // export and stores the coerced return into FieldScript::last_value().
    // Exceptions are caught and logged once per script_sha.
    void TickAll();

    // Walk every live FieldScript created by this runtime. Caller-provided
    // function gets a non-owning pointer; the renderer uses this to push
    // last_value() into per-field actuators.
    using EachFn = void(*)(FieldScript*, void*);
    void ForEachScript(EachFn fn, void* user);

    // Same exposure rule as FieldScript::Impl above: opaque outside the
    // module, but visible to peer module impl files.
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class FieldScript : NoCopy, NoMove {
public:
    FieldScript();
    ~FieldScript();

    FieldKind         field_kind() const noexcept;
    const ScriptValue& last_value() const noexcept;
    bool              alive() const noexcept;
    std::string_view  script_sha() const noexcept;

    // Impl is intentionally exposed inside the wescene.script module so
    // JsRuntime::Impl (in the same module) can mutate it directly. Treated
    // as opaque by every other consumer; see Script.cpp.
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// --- per-Scene script runtime + actuators -----------------------------------

// Where on a SceneNode the transform-style script value should be written.
// Used by MakeNodeTransformApply to manufacture the corresponding closure.
enum class NodeTransformTarget {
    Translate,  // Vec3 → SceneNode m_translate (origin field)
    Scale,      // Vec3 → SceneNode m_scale (scale field)
    Rotation,   // Vec3 → SceneNode m_rotation (angles field)
};

// One write-back binding from script.last_value() to whatever subsystem
// owns the bound field. The closure does the type coercion + write; the
// generic ScriptScene::Tick has no idea what 'apply' does.
struct Actuator {
    FieldScript*                            script { nullptr };
    std::function<void(const ScriptValue&)> apply;
};

// Build the closure that drives a SceneNode transform field. Encapsulates
// the Vec3/Vec2/Scalar/Bool coercion table so callers stay one-liners.
std::function<void(const ScriptValue&)>
MakeNodeTransformApply(owe::SceneNode* node, NodeTransformTarget target);

// Owns one JsRuntime + the actuator list for one Scene. Constructed and
// populated by the parser, attached to the Scene as an opaque pointer
// (Scene::script_scene). Ticked once per frame by `TickSceneScripts`.
class ScriptScene : NoCopy, NoMove {
public:
    ScriptScene();
    ~ScriptScene();

    JsRuntime&                  runtime() noexcept;
    void                        AddActuator(Actuator a);
    bool                        empty() const noexcept;

    // Push the host's per-frame state, drive every FieldScript, drain
    // results into actuators. Call once per frame, before the renderer
    // begins drawing.
    void Tick(const FrameInputs& fi);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Attach a ScriptScene to a Scene via the opaque-pointer slot. Takes
// ownership; replaces any previous attachment.
void InstallScriptScene(owe::Scene&             scene,
                        std::unique_ptr<ScriptScene>  ss);

// Convenience tick: looks up the ScriptScene attached to `scene` and
// drives one frame. No-op when no ScriptScene is installed (image-only
// pkgs, scenes without script bindings).
void TickSceneScripts(owe::Scene& scene, const FrameInputs& fi);

} // namespace owe::script
