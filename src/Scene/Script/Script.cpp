module;

#include <rstd/macro.hpp>
#include "quickjs.h"

module wescene.script;
import eigen;
import nlohmann.json;
import rstd.log;
import rstd.cppstd;
import wescene.scene;

using nlohmann::json;

namespace owe::script {

// ---------------------------------------------------------------------------
// Field-kind inference. The bound field's name is the only signal we have at
// parse time; this table mirrors the empirical distribution from the corpus
// (see docs/scripting/wallpaper_engine_api.md).
// ---------------------------------------------------------------------------

namespace {

FieldKind GuessFieldKind(std::string_view field) {
    // Visible/enabled-style fields: bool. Several scripts return numbers
    // 0/1 here too; coercion table accepts both.
    if (field == "visible") return FieldKind::Bool;
    // Vec3 (position-like) fields.
    if (field == "origin" || field == "scale" || field == "angles" ||
        field == "spriteoffset")
        return FieldKind::Vec3;
    // Color (rgb) fields.
    if (field == "color" || field == "colorn" || field == "Bg color" ||
        field == "Bar Color" || field == "Inner Color" ||
        field == "Outer Color" || field == "Color 1" || field == "Color 2" ||
        field == "Color filter")
        return FieldKind::Color;
    // Strings (text content). Recognised here so the JS can run without
    // erroring; the actuator side ignores the result for MVP scope.
    if (field == "text") return FieldKind::String;
    // Everything else is a scalar: alpha, rate, intensity, fov, volume,
    // parallaxDepth, percentage, brightness, saturation, ... .
    return FieldKind::Scalar;
}

const char* KindName(FieldKind k) {
    switch (k) {
    case FieldKind::Unknown: return "unknown";
    case FieldKind::Scalar:  return "scalar";
    case FieldKind::Bool:    return "bool";
    case FieldKind::Vec2:    return "vec2";
    case FieldKind::Vec3:    return "vec3";
    case FieldKind::Color:   return "color";
    case FieldKind::String:  return "string";
    }
    return "?";
}

// JSValue→ScriptValue coercion. Mirrors the table in the API doc; never
// throws, returns monostate for unrecognised shapes.
ScriptValue CoerceReturn(JSContext* ctx, JSValue ret, FieldKind kind) {
    if (JS_IsUndefined(ret) || JS_IsNull(ret)) return {};

    auto read_field = [&](JSValue obj, const char* name, double& out) -> bool {
        JSValue v = JS_GetPropertyStr(ctx, obj, name);
        if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return false; }
        double d = 0.0;
        int rc = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (rc < 0) return false;
        out = d;
        return true;
    };
    auto read_index = [&](JSValue arr, uint32_t i, double& out) -> bool {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return false; }
        double d = 0.0;
        int rc = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (rc < 0) return false;
        out = d;
        return true;
    };

    switch (kind) {
    case FieldKind::Bool: {
        int b = JS_ToBool(ctx, ret);
        return BoolValue { b > 0 };
    }
    case FieldKind::Scalar: {
        if (JS_IsBool(ret)) {
            int b = JS_ToBool(ctx, ret);
            return ScalarValue { b > 0 ? 1.0 : 0.0 };
        }
        double d = 0.0;
        if (JS_ToFloat64(ctx, &d, ret) >= 0) return ScalarValue { d };
        return {};
    }
    case FieldKind::Vec2: {
        Vec2Value v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.x);
            read_index(ret, 1, v.y);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "x", v.x);
            read_field(ret, "y", v.y);
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::Vec3: {
        Vec3Value v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.x);
            read_index(ret, 1, v.y);
            read_index(ret, 2, v.z);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "x", v.x);
            read_field(ret, "y", v.y);
            read_field(ret, "z", v.z);
        } else if (JS_IsNumber(ret)) {
            // Many audio-response scripts return a *scalar* even when bound
            // to scale (vec3). Splat into all three components.
            double d = 0.0;
            JS_ToFloat64(ctx, &d, ret);
            return Vec3Value { d, d, d };
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::Color: {
        ColorValue v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.r);
            read_index(ret, 1, v.g);
            read_index(ret, 2, v.b);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "r", v.r);
            read_field(ret, "g", v.g);
            read_field(ret, "b", v.b);
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::String: {
        const char* s = JS_ToCString(ctx, ret);
        if (! s) return {};
        StringValue sv { std::string(s) };
        JS_FreeCString(ctx, s);
        return sv;
    }
    case FieldKind::Unknown:
        return {};
    }
    return {};
}

// JSON → JSValue conversion for the initial-value seed. Recursive but
// scenescript values are tiny (numbers, short strings, small objects).
JSValue JsonToJs(JSContext* ctx, const json& j) {
    switch (j.type()) {
    case json::value_t::null:
        return JS_NULL;
    case json::value_t::boolean:
        return JS_NewBool(ctx, j.get<bool>());
    case json::value_t::number_integer:
    case json::value_t::number_unsigned:
        return JS_NewInt64(ctx, j.get<int64_t>());
    case json::value_t::number_float:
        return JS_NewFloat64(ctx, j.get<double>());
    case json::value_t::string: {
        const auto& s = j.get_ref<const std::string&>();
        return JS_NewStringLen(ctx, s.data(), s.size());
    }
    case json::value_t::array: {
        JSValue arr = JS_NewArray(ctx);
        uint32_t i = 0;
        for (const auto& item : j) {
            JS_DefinePropertyValueUint32(ctx, arr, i++, JsonToJs(ctx, item),
                                         JS_PROP_C_W_E);
        }
        return arr;
    }
    case json::value_t::object: {
        JSValue obj = JS_NewObject(ctx);
        for (auto it = j.begin(); it != j.end(); ++it) {
            JS_DefinePropertyValueStr(ctx, obj, it.key().c_str(),
                                       JsonToJs(ctx, it.value()),
                                       JS_PROP_C_W_E);
        }
        return obj;
    }
    case json::value_t::binary:
    case json::value_t::discarded:
    default:
        return JS_UNDEFINED;
    }
}

// Resolve a config value: strings like {"user":"name","value":X} flatten to
// the value (we don't yet plumb engine.userProperties), everything else is
// passed through. Mirrors the contract in the API doc.
JSValue ResolveConfigValue(JSContext* ctx, const json& v) {
    if (v.is_object() && v.contains("value") && v.contains("user")) {
        return JsonToJs(ctx, v.at("value"));
    }
    return JsonToJs(ctx, v);
}

// Coerce a binding's initial-value JSON into the JS shape the script's
// `init(value)` expects, given the bound field kind. Audio-response,
// parallax, and color scripts all assume `value` is already a Vec2/Vec3,
// not a raw string or array.
//   - Numbers: passthrough for scalar; for Vec3 we splat (matching WE's
//     "uniform scale" behaviour observed in the corpus).
//   - Strings: WE serialises vec values as space-separated floats —
//     "1.0 2.0 3.0" → Vec3(1,2,3). Arrays accept the same shape.
//   - Arrays / objects with x,y[,z]: construct a Vec2 / Vec3.
//   - Color: returns an array — most "color" scripts use `[r,g,b]` access.
//
// Falls back to JsonToJs for unknown shapes; better to pass garbage than
// to fail to call init().
JSValue CoerceInitialValue(JSContext* ctx, const json& v, FieldKind kind) {
    auto parse_floats = [](const std::string& s) -> std::vector<double> {
        std::vector<double> out;
        const char* p = s.c_str();
        char*       end = nullptr;
        while (*p) {
            double d = std::strtod(p, &end);
            if (end == p) break;
            out.push_back(d);
            p = end;
            while (*p == ' ' || *p == '\t') ++p;
        }
        return out;
    };
    auto build_vec = [&](double x, double y, double z, int n) -> JSValue {
        // Vec2/Vec3 are JS classes installed in the bootstrap. Construct
        // by getting the global ctor and calling new on it.
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor   = JS_GetPropertyStr(ctx, global, n == 2 ? "Vec2" : "Vec3");
        JSValue argv[3] = { JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y),
                            JS_NewFloat64(ctx, z) };
        JSValue obj    = JS_CallConstructor(ctx, ctor, n, argv);
        for (int i = 0; i < n; ++i) JS_FreeValue(ctx, argv[i]);
        if (n < 3) JS_FreeValue(ctx, argv[2]);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, global);
        return obj;
    };

    switch (kind) {
    case FieldKind::Vec2: {
        if (v.is_string()) {
            auto fs = parse_floats(v.get_ref<const std::string&>());
            return build_vec(fs.size() > 0 ? fs[0] : 0.0,
                             fs.size() > 1 ? fs[1] : 0.0, 0.0, 2);
        }
        if (v.is_array() && v.size() >= 2)
            return build_vec(v[0].get<double>(), v[1].get<double>(), 0.0, 2);
        if (v.is_number()) return build_vec(v.get<double>(), v.get<double>(), 0.0, 2);
        break;
    }
    case FieldKind::Vec3: {
        if (v.is_string()) {
            auto fs = parse_floats(v.get_ref<const std::string&>());
            double x = fs.size() > 0 ? fs[0] : 0.0;
            double y = fs.size() > 1 ? fs[1] : x;  // splat single scalar
            double z = fs.size() > 2 ? fs[2] : (fs.size() > 1 ? 0.0 : x);
            return build_vec(x, y, z, 3);
        }
        if (v.is_array() && v.size() >= 3)
            return build_vec(v[0].get<double>(), v[1].get<double>(),
                             v[2].get<double>(), 3);
        if (v.is_number()) {
            double d = v.get<double>();
            return build_vec(d, d, d, 3);
        }
        break;
    }
    case FieldKind::Color: {
        if (v.is_string()) {
            auto fs = parse_floats(v.get_ref<const std::string&>());
            JSValue arr = JS_NewArray(ctx);
            for (uint32_t i = 0; i < fs.size() && i < 3; ++i)
                JS_DefinePropertyValueUint32(ctx, arr, i,
                                             JS_NewFloat64(ctx, fs[i]),
                                             JS_PROP_C_W_E);
            return arr;
        }
        break;
    }
    case FieldKind::Scalar:
    case FieldKind::Bool:
    case FieldKind::String:
    case FieldKind::Unknown:
        break;
    }
    return JsonToJs(ctx, v);
}

}  // namespace

// ---------------------------------------------------------------------------
// FrameInputs storage. JSRuntime opaque data: a per-context FrameInputs
// snapshot the engine.* getters consult on each call.
// ---------------------------------------------------------------------------

struct EngineHostState {
    FrameInputs               inputs;
    JSValue                   audio_buffer { JS_UNDEFINED };
    bool                      audio_buffer_built { false };
};

// ---------------------------------------------------------------------------
// FieldScript impl.
// ---------------------------------------------------------------------------

struct FieldScript::Impl {
    JsRuntime::Impl*  rt { nullptr };
    JSContext*        ctx { nullptr };
    std::string       sha;
    FieldKind         kind { FieldKind::Unknown };
    JSValue           module_ns { JS_UNDEFINED };
    JSValue           update_fn { JS_UNDEFINED };
    bool              update_takes_arg { false };
    JSValue           current_value { JS_UNDEFINED };  // last `value` returned, kept as JSValue for the (value)-arg form
    ScriptValue       last_value;
    bool              alive { true };
    bool              error_logged { false };
};

FieldScript::FieldScript() : m_impl(std::make_unique<Impl>()) {}
FieldScript::~FieldScript() = default;
FieldKind FieldScript::field_kind() const noexcept { return m_impl->kind; }
const ScriptValue& FieldScript::last_value() const noexcept { return m_impl->last_value; }
bool FieldScript::alive() const noexcept { return m_impl->alive; }
std::string_view FieldScript::script_sha() const noexcept { return m_impl->sha; }

// ---------------------------------------------------------------------------
// JsRuntime impl.
// ---------------------------------------------------------------------------

struct JsRuntime::Impl {
    JSRuntime*                                          rt { nullptr };
    JSContext*                                          ctx { nullptr };
    EngineHostState                                     host;
    // Compiled-module dedup: same script source under the same sha is
    // imported once per runtime, exposing one shared namespace. A
    // FieldScript holds a JS_DupValue of the namespace.
    std::unordered_map<std::string, JSValue>            ns_by_sha;
    std::vector<std::unique_ptr<FieldScript>>           scripts;
    // Set of error-logged shas to log once.
    std::unordered_set<std::string>                     errored;

    void LogError(JSContext* c, std::string_view sha, const char* what) {
        if (errored.contains(std::string(sha))) return;
        errored.insert(std::string(sha));
        JSValue exc = JS_GetException(c);
        const char* msg = JS_ToCString(c, exc);
        rstd_error("script[{}] {}: {}",
                            sha,
                            std::string_view(what), std::string_view(msg ? msg : "<no message>"));
        if (msg) JS_FreeCString(c, msg);
        JS_FreeValue(c, exc);
    }
};

// --- engine.* getters --------------------------------------------------------

namespace {

JSValue EngineGetterFrametime(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    return JS_NewFloat64(ctx, host->inputs.frametime);
}
JSValue EngineGetterRuntime(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    return JS_NewFloat64(ctx, host->inputs.runtime);
}
JSValue EngineGetterTimeOfDay(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    return JS_NewFloat64(ctx, host->inputs.time_of_day);
}
JSValue EngineGetterCanvasSize(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JSValue v = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, v, "x", JS_NewFloat64(ctx, host->inputs.canvas_w),
                               JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, v, "y", JS_NewFloat64(ctx, host->inputs.canvas_h),
                               JS_PROP_C_W_E);
    return v;
}
JSValue EngineGetterScreenRes(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JSValue v = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, v, "x", JS_NewFloat64(ctx, host->inputs.screen_w),
                               JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, v, "y", JS_NewFloat64(ctx, host->inputs.screen_h),
                               JS_PROP_C_W_E);
    return v;
}

// engine.registerAudioBuffers(resolution) → { average: Float64Array, buffer: Float64Array }
//
// Returns a stable per-context object with two array properties whose
// underlying storage points at the FrameInputs::audio_average buffer
// rebuilt each frame. We allocate a fresh JSValue array on every call here
// (per the API contract — the script normally calls it once and caches it),
// but on subsequent calls return the same one. Higher resolutions clamp
// to 16 — the corpus has 24 mentions of AUDIO_RESOLUTION_16 vs. 3 of
// AUDIO_RESOLUTION_32, and we don't yet have a 32-bin source.
JSValue EngineRegisterAudioBuffers(JSContext* ctx, JSValueConst /*this_val*/,
                                   int /*argc*/, JSValueConst* /*argv*/) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (! host->audio_buffer_built) {
        JSValue obj = JS_NewObject(ctx);
        JSValue avg = JS_NewArray(ctx);
        JSValue buf = JS_NewArray(ctx);
        for (uint32_t i = 0; i < host->inputs.audio_average.size(); ++i) {
            JS_DefinePropertyValueUint32(ctx, avg, i,
                                         JS_NewFloat64(ctx, host->inputs.audio_average[i]),
                                         JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, buf, i,
                                         JS_NewFloat64(ctx, host->inputs.audio_average[i]),
                                         JS_PROP_C_W_E);
        }
        JS_DefinePropertyValueStr(ctx, obj, "average", avg, JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, obj, "buffer", buf, JS_PROP_C_W_E);
        host->audio_buffer       = obj;
        host->audio_buffer_built = true;
    }
    return JS_DupValue(ctx, host->audio_buffer);
}

// Refresh audio array elements from the host's current FrameInputs.
// Called by JsRuntime::SetFrameInputs every frame after host->inputs is
// updated, so the JS side sees the latest values without needing to call
// registerAudioBuffers again.
void RefreshAudioBuffer(JSContext* ctx) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (! host->audio_buffer_built) return;
    JSValue avg = JS_GetPropertyStr(ctx, host->audio_buffer, "average");
    JSValue buf = JS_GetPropertyStr(ctx, host->audio_buffer, "buffer");
    for (uint32_t i = 0; i < host->inputs.audio_average.size(); ++i) {
        JS_DefinePropertyValueUint32(ctx, avg, i,
                                     JS_NewFloat64(ctx, host->inputs.audio_average[i]),
                                     JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, buf, i,
                                     JS_NewFloat64(ctx, host->inputs.audio_average[i]),
                                     JS_PROP_C_W_E);
    }
    JS_FreeValue(ctx, avg);
    JS_FreeValue(ctx, buf);
}

// engine.setTimeout / setInterval — out of MVP scope; return -1 to signal
// "never fires", so script init paths complete and update() runs.
JSValue EngineNoop(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt32(ctx, -1);
}

// engine.registerAsset(...) — return the first arg unchanged so chained
// `.something` works without throwing (even if the result isn't useful).
JSValue EngineRegisterAsset(JSContext* ctx, JSValueConst /*this_val*/,
                            int argc, JSValueConst* argv) {
    if (argc > 0) return JS_DupValue(ctx, argv[0]);
    return JS_NewObject(ctx);
}

// createScriptProperties() — the JS-side declarative builder. We implement
// it as a thin C function that returns an object exposing addX / finish.
// addX records the descriptor on the builder object's `__props` dict; the
// host reads that dict after the module body runs to know what knobs the
// script exposes. finish() returns a Proxy whose property reads return the
// resolved current value (host fills it from scriptproperties config + the
// schema default).
//
// Implementation: we let JS itself build the builder via a small bootstrap
// snippet evaluated once into the global context. That keeps the C side
// minimal and lets the dynamic property lookup use a JS Proxy.
constexpr const char* kBootstrapJs = R"JS(
globalThis.createScriptProperties = function () {
  const _props = [];
  const _byName = new Map();
  const builder = {
    _byName,
    _props,
  };
  const adder = (kind) => (opts) => {
    const d = Object.assign({ kind }, opts);
    _props.push(d);
    if (d && d.name) _byName.set(d.name, d);
    return builder;
  };
  builder.addSlider    = adder('Slider');
  builder.addCheckbox  = adder('Checkbox');
  builder.addText      = adder('Text');
  builder.addCombo     = adder('Combo');
  builder.addColor     = adder('Color');
  builder.addDelimiter = adder('Delimiter');
  // Stubs for the long tail surfaced by wpscriptdump (Animation,
  // Interpolator, AniMapper, Task, ChangedUserProperty, Listener,
  // SpaceToTimeDelimiter, SpaceToDateDelimiter, Value): no-op, returns
  // builder so `.addX().addY().finish()` chains keep parsing.
  for (const k of ['Animation','Interpolator','AniMapper','Task',
                   'ChangedUserProperty','Listener',
                   'SpaceToTimeDelimiter','SpaceToDateDelimiter','Value']) {
    builder['add' + k] = adder(k);
  }
  // .finish() returns a Proxy. Property reads:
  //   - scriptProperties.<name> : look up in _hostValues (filled by C++),
  //                               else default value from descriptor.
  builder.finish = function () {
    const _hostValues = builder._hostValues || {};
    const target = {};
    for (const d of _props) {
      if (d && d.name) {
        Object.defineProperty(target, d.name, {
          enumerable: true,
          configurable: true,
          get() {
            if (Object.prototype.hasOwnProperty.call(_hostValues, d.name))
              return _hostValues[d.name];
            return d.value;
          },
        });
      }
    }
    target.__descriptors = _props;
    target.__hostValues  = _hostValues;
    return target;
  };
  // Host writes here before evaluating the script body (per FieldScript)
  // to override defaults.
  builder._hostValues = {};
  return builder;
};
// engine.userProperties is a plain object the host can mutate.
if (! globalThis.engine) globalThis.engine = {};
globalThis.engine.userProperties = {};
globalThis.engine.AUDIO_RESOLUTION_16 = 16;
globalThis.engine.AUDIO_RESOLUTION_32 = 32;
globalThis.engine.isRunningInEditor = false;
globalThis.engine.isScreensaver = false;

// --- Vec2 / Vec3 ---
// Pure-JS implementations of WE's vector types. The corpus relies on
// .multiply / .add / .subtract / .divide as Vec3 instance methods (used
// by every audio-response script binding scale), so a simple class with
// these methods covers the audio-responsive cluster (1023 instances).
class Vec2 {
  constructor(x, y) {
    this.x = (typeof x === 'number') ? x : 0;
    this.y = (typeof y === 'number') ? y : 0;
  }
  add(o)      { return new Vec2(this.x + (o.x ?? o), this.y + (o.y ?? o)); }
  subtract(o) { return new Vec2(this.x - (o.x ?? o), this.y - (o.y ?? o)); }
  multiply(o) { return new Vec2(this.x * (o.x ?? o), this.y * (o.y ?? o)); }
  divide(o)   { return new Vec2(this.x / (o.x ?? o), this.y / (o.y ?? o)); }
}
class Vec3 {
  constructor(x, y, z) {
    if (typeof x === 'object' && x !== null) {
      this.x = x.x ?? 0; this.y = x.y ?? 0; this.z = x.z ?? 0;
    } else {
      this.x = (typeof x === 'number') ? x : 0;
      this.y = (typeof y === 'number') ? y : 0;
      this.z = (typeof z === 'number') ? z : 0;
    }
  }
  add(o)      { return new Vec3(this.x + (o.x ?? o), this.y + (o.y ?? o), this.z + (o.z ?? o)); }
  subtract(o) { return new Vec3(this.x - (o.x ?? o), this.y - (o.y ?? o), this.z - (o.z ?? o)); }
  multiply(o) { return new Vec3(this.x * (o.x ?? o), this.y * (o.y ?? o), this.z * (o.z ?? o)); }
  divide(o)   { return new Vec3(this.x / (o.x ?? o), this.y / (o.y ?? o), this.z / (o.z ?? o)); }
  length()    { return Math.sqrt(this.x*this.x + this.y*this.y + this.z*this.z); }
}
globalThis.Vec2 = Vec2;
globalThis.Vec3 = Vec3;

// --- thisLayer / thisScene stub ---------------------------------------------
// Stand-in for the per-script SceneNode binding. Property reads return
// sensible defaults; writes are silently accepted. A few well-known
// methods (getParent / getTransformMatrix) return shaped values so
// `parent.getTransformMatrix().m[13]` style accesses don't TypeError.
function __wwCreateNodeStub() {
    const props = {
        origin:         new Vec3(0, 0, 0),
        scale:          new Vec3(1, 1, 1),
        angles:         new Vec3(0, 0, 0),
        size:           new Vec3(100, 100, 0),
        visible:        true,
        verticalalign:  'center',
        horizontalalign:'center',
        alpha:          1,
        brightness:     1,
        color:          new Vec3(1, 1, 1),
    };
    // Identity 4x4 column-major matrix. m[13] is the y-translation slot
    // some clock scripts read.
    const identity = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
    const handler = {
        get(target, key) {
            if (key === 'getParent')           return () => __wwCreateNodeStub();
            if (key === 'getTransformMatrix')  return () => ({ m: identity.slice() });
            if (key === 'getChildren')         return () => [];
            if (key === 'getName')             return () => '';
            if (key === 'getLayer')            return (_n) => __wwCreateNodeStub();
            if (key === 'getTextureAnimation') return () => __wwCreateTexAnimStub();
            if (key in target) return target[key];
            return undefined;
        },
        set(target, key, value) { target[key] = value; return true; },
        has(target, key) { return key in target; },
    };
    return new Proxy(props, handler);
}

function __wwCreateTexAnimStub() {
    let frame = 0, playing = false;
    return {
        play()     { playing = true;  },
        stop()     { playing = false; },
        pause()    { playing = false; },
        setFrame(n){ frame = n | 0;   },
        getFrame() { return frame;    },
        isPlaying(){ return playing;  },
    };
}
globalThis.thisLayer = __wwCreateNodeStub();
globalThis.thisScene = __wwCreateNodeStub();

// Hook used by the C++ side to swap the stub for a real per-script binding.
globalThis.__wwBindLayer = function(obj) { globalThis.thisLayer = obj; };
globalThis.__wwBindScene = function(obj) { globalThis.thisScene = obj; };

// --- MediaPlaybackEvent enum ------------------------------------------------
globalThis.MediaPlaybackEvent = Object.freeze({
    PLAYBACK_STOPPED: 0,
    PLAYBACK_PLAYING: 1,
    PLAYBACK_PAUSED:  2,
});

// --- shared --- cross-script object scripts mutate freely.
if (! globalThis.shared) globalThis.shared = {};

// --- localStorage --- in-memory Map; not persisted across runs.
if (! globalThis.localStorage) {
    const _ls = new Map();
    globalThis.localStorage = {
        get: (k) => _ls.has(k) ? _ls.get(k) : undefined,
        set: (k, v) => { _ls.set(k, v); },
        remove: (k) => { _ls.delete(k); },
    };
}
)JS";

void InstallEngineGlobal(JSContext* ctx) {
    // Run the bootstrap to create createScriptProperties + skeleton engine.
    JSValue r = JS_Eval(ctx, kBootstrapJs, std::strlen(kBootstrapJs),
                        "<wescene-bootstrap>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        rstd_error("script bootstrap: {}", msg ? msg : "<exc>");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, r);

    // Install the dynamic getters on engine.{frametime,runtime,timeOfDay,
    // canvasSize,screenResolution} via accessor properties so reads see
    // the latest FrameInputs.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue engine = JS_GetPropertyStr(ctx, global, "engine");

    auto define_getter = [&](const char* name, JSCFunction* f) {
        JSAtom atom  = JS_NewAtom(ctx, name);
        JSValue gfun = JS_NewCFunction(ctx, f, name, 0);
        JS_DefinePropertyGetSet(ctx, engine, atom, gfun, JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, atom);
    };
    define_getter("frametime",        EngineGetterFrametime);
    define_getter("runtime",          EngineGetterRuntime);
    define_getter("timeOfDay",        EngineGetterTimeOfDay);
    define_getter("canvasSize",       EngineGetterCanvasSize);
    define_getter("screenResolution", EngineGetterScreenRes);

    auto define_fn = [&](const char* name, JSCFunction* f, int nargs) {
        JS_DefinePropertyValueStr(ctx, engine, name,
                                   JS_NewCFunction(ctx, f, name, nargs),
                                   JS_PROP_C_W_E);
    };
    define_fn("registerAudioBuffers", EngineRegisterAudioBuffers, 1);
    define_fn("setTimeout",           EngineNoop, 2);
    define_fn("setInterval",          EngineNoop, 2);
    define_fn("registerAsset",        EngineRegisterAsset, 1);

    JS_FreeValue(ctx, engine);
    JS_FreeValue(ctx, global);
}

// --- Built-in ES modules ----------------------------------------------------
// Scripts `import * as M from 'M'`. QuickJS calls our loader with the
// bare name; we return a precompiled JSModuleDef built from the source
// below. Add an entry to extend (e.g. WEColor / WEEasing) once needed.
struct BuiltinModule {
    const char* name;
    const char* source;
};

static constexpr const char* kWEMathSrc = R"JS(
export function mix(a, b, t) { return a + (b - a) * t; }
export function lerp(a, b, t) { return a + (b - a) * t; }
export function clamp(x, lo, hi) {
    return Math.max(lo, Math.min(hi, x));
}
export function saturate(x) { return Math.max(0, Math.min(1, x)); }
export function smoothstep(edge0, edge1, x) {
    const t = Math.max(0, Math.min(1, (x - edge0) / (edge1 - edge0)));
    return t * t * (3 - 2 * t);
}
export function step(edge, x) { return x < edge ? 0 : 1; }
export function sign(x) { return Math.sign(x); }
export function fract(x) { return x - Math.floor(x); }
)JS";

static constexpr BuiltinModule kBuiltinModules[] = {
    { "WEMath", kWEMathSrc },
};

JSModuleDef* BuiltinModuleLoader(JSContext* ctx, const char* module_name, void*) {
    for (const auto& m : kBuiltinModules) {
        if (std::strcmp(m.name, module_name) != 0) continue;
        JSValue compiled = JS_Eval(ctx, m.source, std::strlen(m.source),
                                   module_name,
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            // JS_Eval already set the pending exception; QuickJS propagates.
            return nullptr;
        }
        JSModuleDef* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        // Don't free `compiled` — the pointer is the live module def.
        return def;
    }
    JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
    return nullptr;
}

}  // namespace

// --- JsRuntime methods ------------------------------------------------------

JsRuntime::JsRuntime() : m_impl(std::make_unique<Impl>()) {
    m_impl->rt  = JS_NewRuntime();
    m_impl->ctx = JS_NewContext(m_impl->rt);
    if (! m_impl->rt || ! m_impl->ctx) {
        rstd_error("script: JS_NewRuntime/JS_NewContext failed");
        return;
    }
    // QuickJS's default stack-overflow check is conservative (relative to
    // the OS thread stack at runtime init). When the wallpaper renderer
    // runs scripts from a deep call site (Vulkan render thread, post-
    // particle emission), `new Date()` and similar built-ins hit the
    // stack-frame guard and throw "Maximum call stack size exceeded".
    // Disable the soft check; the OS stack is plenty for clock/audio-
    // response style scripts in the corpus.
    JS_SetMaxStackSize(m_impl->rt, 0);
    JS_SetContextOpaque(m_impl->ctx, &m_impl->host);
    // Built-in ES modules (WEMath, …). Resolves bare `import 'WEMath'`
    // against the kBuiltinModules table; unknown names raise
    // ReferenceError via the loader.
    JS_SetModuleLoaderFunc(m_impl->rt, /*normalize=*/nullptr,
                           BuiltinModuleLoader, /*opaque=*/nullptr);
    InstallEngineGlobal(m_impl->ctx);
}

JsRuntime::~JsRuntime() {
    if (! m_impl) return;
    // Drop FieldScripts before tearing down the runtime so their JSValues
    // go through JS_FreeValue while the context is still alive.
    for (auto& fs : m_impl->scripts) {
        if (fs && fs->m_impl) {
            JS_FreeValue(m_impl->ctx, fs->m_impl->update_fn);
            JS_FreeValue(m_impl->ctx, fs->m_impl->module_ns);
            JS_FreeValue(m_impl->ctx, fs->m_impl->current_value);
        }
    }
    m_impl->scripts.clear();
    for (auto& [_sha, ns] : m_impl->ns_by_sha) JS_FreeValue(m_impl->ctx, ns);
    m_impl->ns_by_sha.clear();
    if (m_impl->host.audio_buffer_built) {
        JS_FreeValue(m_impl->ctx, m_impl->host.audio_buffer);
        m_impl->host.audio_buffer_built = false;
    }
    if (m_impl->ctx) JS_FreeContext(m_impl->ctx);
    if (m_impl->rt) JS_FreeRuntime(m_impl->rt);
}

void JsRuntime::SetFrameInputs(const FrameInputs& fi) {
    m_impl->host.inputs = fi;
    if (m_impl->host.audio_buffer_built) RefreshAudioBuffer(m_impl->ctx);
}

void JsRuntime::TickAll() {
    JSContext* ctx = m_impl->ctx;
    for (auto& fs : m_impl->scripts) {
        auto* I = fs->m_impl.get();
        if (! I->alive) continue;
        if (JS_IsUndefined(I->update_fn)) continue;
        JSValue ret;
        if (I->update_takes_arg) {
            JSValue args[1] = { JS_DupValue(ctx, I->current_value) };
            ret = JS_Call(ctx, I->update_fn, JS_UNDEFINED, 1, args);
            JS_FreeValue(ctx, args[0]);
        } else {
            ret = JS_Call(ctx, I->update_fn, JS_UNDEFINED, 0, nullptr);
        }
        if (JS_IsException(ret)) {
            m_impl->LogError(ctx, I->sha, "update threw");
            JS_FreeValue(ctx, ret);
            continue;
        }
        // For (value)-form updates, also keep the latest as the next
        // current_value so the script can mutate-and-return cumulatively.
        if (I->update_takes_arg && ! JS_IsUndefined(ret) && ! JS_IsNull(ret)) {
            JS_FreeValue(ctx, I->current_value);
            I->current_value = JS_DupValue(ctx, ret);
        }
        I->last_value = CoerceReturn(ctx, ret, I->kind);
        JS_FreeValue(ctx, ret);
    }
}

void JsRuntime::ForEachScript(EachFn fn, void* user) {
    for (auto& fs : m_impl->scripts) fn(fs.get(), user);
}

// --- Module load + FieldScript construction ---------------------------------

namespace {

// Discover whether `update` takes an argument by inspecting `length`.
// JS function objects have a `length` property = formal parameter count.
bool FunctionTakesArg(JSContext* ctx, JSValue fn) {
    JSValue len = JS_GetPropertyStr(ctx, fn, "length");
    int32_t n = 0;
    JS_ToInt32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n >= 1;
}

}  // namespace

FieldScript* JsRuntime::MakeFieldScript(std::string_view source,
                                        std::string_view script_sha,
                                        FieldKind        field_kind_in,
                                        const json&      properties_config,
                                        const json&      initial_value) {
    JSContext* ctx = m_impl->ctx;
    if (! ctx) return nullptr;

    // 1. Compile (and import) the module if not seen before. The compile
    //    step is COMPILE_ONLY so we can grab the JSModuleDef pointer and
    //    invoke EvalFunction once.
    JSValue ns;
    auto    sha_str = std::string(script_sha);
    if (auto it = m_impl->ns_by_sha.find(sha_str); it != m_impl->ns_by_sha.end()) {
        ns = JS_DupValue(ctx, it->second);
    } else {
        std::string fname = "scripts/" + sha_str + ".js";
        JSValue compiled = JS_Eval(ctx, source.data(), source.size(),
                                   fname.c_str(),
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            m_impl->LogError(ctx, script_sha, "compile failed");
            JS_FreeValue(ctx, compiled);
            return nullptr;
        }
        JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JSValue ev = JS_EvalFunction(ctx, compiled);
        if (JS_IsException(ev)) {
            m_impl->LogError(ctx, script_sha, "module eval failed");
            JS_FreeValue(ctx, ev);
            return nullptr;
        }
        JS_FreeValue(ctx, ev);
        ns = JS_GetModuleNamespace(ctx, m);
        m_impl->ns_by_sha.emplace(sha_str, JS_DupValue(ctx, ns));
    }

    // 2. Build the FieldScript handle.
    auto fs           = std::make_unique<FieldScript>();
    auto* I           = fs->m_impl.get();
    I->rt             = m_impl.get();
    I->ctx            = ctx;
    I->sha            = sha_str;
    I->kind           = (field_kind_in == FieldKind::Unknown) ? FieldKind::Scalar : field_kind_in;
    I->module_ns      = ns;  // owns one ref now

    // 3. Wire scriptProperties._hostValues from the per-binding config so
    //    `scriptProperties.foo` returns the configured value (resolving
    //    {user, value} to value) instead of the JS-default.
    JSValue sp = JS_GetPropertyStr(ctx, ns, "scriptProperties");
    if (! JS_IsUndefined(sp)) {
        JSValue hv = JS_GetPropertyStr(ctx, sp, "__hostValues");
        if (JS_IsObject(hv) && properties_config.is_object()) {
            for (auto it = properties_config.begin();
                 it != properties_config.end(); ++it) {
                JS_DefinePropertyValueStr(ctx, hv, it.key().c_str(),
                                          ResolveConfigValue(ctx, it.value()),
                                          JS_PROP_C_W_E);
            }
        }
        JS_FreeValue(ctx, hv);
    }
    JS_FreeValue(ctx, sp);

    // 4. If the module exports `init`, call it with the initial value
    //    coerced to match the bound field's expected JS shape.
    JSValue init_fn = JS_GetPropertyStr(ctx, ns, "init");
    JSValue init_arg = CoerceInitialValue(ctx, initial_value, I->kind);
    if (JS_IsFunction(ctx, init_fn)) {
        JSValue r = JS_Call(ctx, init_fn, JS_UNDEFINED, 1, &init_arg);
        if (JS_IsException(r)) {
            m_impl->LogError(ctx, script_sha, "init threw");
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, init_fn);

    // 5. Cache `update` for the per-frame tick.
    JSValue update_fn = JS_GetPropertyStr(ctx, ns, "update");
    if (JS_IsFunction(ctx, update_fn)) {
        I->update_fn        = update_fn;
        I->update_takes_arg = FunctionTakesArg(ctx, update_fn);
    } else {
        JS_FreeValue(ctx, update_fn);
        I->update_fn = JS_UNDEFINED;
    }
    // Reuse the coerced initial value as the seed for (value)-form
    // updates so the first frame's `update(value)` sees a Vec3, not a
    // raw string.
    I->current_value = init_arg;

    auto* raw = fs.get();
    m_impl->scripts.push_back(std::move(fs));
    return raw;
}

// ---------------------------------------------------------------------------
// ScriptScene — per-Scene runtime + actuator drain.
// ---------------------------------------------------------------------------

struct ScriptScene::Impl {
    JsRuntime              rt;
    std::vector<Actuator>  actuators;
};

ScriptScene::ScriptScene() : m_impl(std::make_unique<Impl>()) {}
ScriptScene::~ScriptScene() = default;

JsRuntime& ScriptScene::runtime() noexcept { return m_impl->rt; }
void       ScriptScene::AddActuator(Actuator a) { m_impl->actuators.push_back(a); }
bool       ScriptScene::empty() const noexcept { return m_impl->actuators.empty(); }

std::function<void(const ScriptValue&)>
MakeNodeTransformApply(owe::SceneNode* node, NodeTransformTarget target) {
    return [node, target](const ScriptValue& v) {
        if (! node) return;
        if (std::holds_alternative<std::monostate>(v)) return;

        Eigen::Vector3f current = [&] {
            switch (target) {
            case NodeTransformTarget::Translate: return node->Translate();
            case NodeTransformTarget::Scale:     return node->Scale();
            case NodeTransformTarget::Rotation:  return node->Rotation();
            }
            return Eigen::Vector3f { 0.0f, 0.0f, 0.0f };
        }();

        Eigen::Vector3f next = current;
        if (auto* p = std::get_if<Vec3Value>(&v)) {
            next = Eigen::Vector3f { static_cast<float>(p->x),
                                     static_cast<float>(p->y),
                                     static_cast<float>(p->z) };
        } else if (auto* p = std::get_if<Vec2Value>(&v)) {
            next = Eigen::Vector3f { static_cast<float>(p->x),
                                     static_cast<float>(p->y), current.z() };
        } else if (auto* p = std::get_if<ScalarValue>(&v)) {
            // Scalar splats across all three axes for scale; falls back to
            // current.x for translate/rotation (rare but seen in the corpus
            // when scripts mistakenly bind to the wrong field kind).
            if (target == NodeTransformTarget::Scale) {
                float s = static_cast<float>(p->v);
                next    = Eigen::Vector3f { s, s, s };
            } else {
                next.x() = static_cast<float>(p->v);
            }
        } else {
            return;
        }

        switch (target) {
        case NodeTransformTarget::Translate: node->SetTranslate(next); break;
        case NodeTransformTarget::Scale:     node->SetScale(next); break;
        case NodeTransformTarget::Rotation:  node->SetRotation(next); break;
        }
    };
}

void ScriptScene::Tick(const FrameInputs& fi) {
    m_impl->rt.SetFrameInputs(fi);
    m_impl->rt.TickAll();
    for (const auto& a : m_impl->actuators) {
        if (! a.script || ! a.apply) continue;
        a.apply(a.script->last_value());
    }
}

void InstallScriptScene(owe::Scene&             scene,
                        std::unique_ptr<ScriptScene>  ss) {
    // Move into Scene's opaque-pointer slot. The deleter knows the
    // concrete type because it's instantiated in this TU.
    void* raw = ss.release();
    scene.script_scene = decltype(scene.script_scene)(
        raw, [](void* p) noexcept { delete static_cast<ScriptScene*>(p); });
}

void TickSceneScripts(owe::Scene& scene, const FrameInputs& fi) {
    auto* ss = static_cast<ScriptScene*>(scene.script_scene.get());
    if (! ss) return;
    ss->Tick(fi);
}

}  // namespace owe::script
