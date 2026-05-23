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

// Resolve a config value. {"user":"name","value":X} stays as-is — the
// bootstrap getter resolves it lazily against engine.userProperties at
// access time, so SetUserProperty calls after parse propagate.
// Everything else passes through.
JSValue ResolveConfigValue(JSContext* ctx, const json& v) {
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

// One scheduled engine.setTimeout / setInterval entry. fn is an owned ref;
// dead entries are tombstoned during sweep and compacted afterwards so
// callbacks that schedule more callbacks don't iterate over invalid storage.
struct DeferredCb {
    uint32_t handle;
    double   fire_at;     // engine.runtime seconds when due
    double   interval_s;  // for setInterval; 0 for setTimeout
    JSValue  fn;          // owned
    bool     repeating;
    bool     dead;
};

struct EngineHostState {
    FrameInputs               inputs;
    JSValue                   audio_buffer { JS_UNDEFINED };
    bool                      audio_buffer_built { false };
    // Cached `globalThis.Vec3` ctor, populated lazily on first node access.
    // Used by the SceneNode wrapper to hand back Vec3 instances so scripts
    // can call `.add` / `.subtract` on `thisLayer.origin`.
    JSValue                   vec3_ctor { JS_UNDEFINED };
    // The original JS-side `thisLayer` / `thisScene` stubs, captured at
    // bootstrap. Per-script binding restores them when a script has no
    // backing SceneNode.
    JSValue                   default_layer { JS_UNDEFINED };
    JSValue                   default_scene { JS_UNDEFINED };
    // engine.setTimeout / setInterval queue. Swept once per frame in
    // JsRuntime::TickAll before the script update loop runs.
    std::vector<DeferredCb>   deferred;
    uint32_t                  next_handle { 1 };
    // localStorage backing. Values are JSON-serialised strings so we can
    // round-trip arbitrary script values through the persistence file.
    // Empty `ls_path` means in-memory only (the legacy bootstrap shape).
    std::unordered_map<std::string, std::string> ls_data;
    std::string                                  ls_path;
    // The script currently running. createLayer pops clones from this
    // FieldScript's clone_queue. Set around every init/update/cursor invoke.
    FieldScript* active_field_script { nullptr };
    // SceneNode -> text-content setter. Populated by text layers in the
    // parser; consulted by NodeSetText so `thisLayer.text = "..."` reaches
    // TextLayouter::SetText. Missing entry means the layer is not text-
    // capable; writes silently no-op.
    std::unordered_map<owe::SceneNode*,
                       std::function<void(std::string_view)>> text_setters;
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
    // Layer-B: the SceneNode this script's `thisLayer` resolves to. Null →
    // fall back to the generic JS stub. `wrapped_layer` caches the JSValue
    // wrapper so per-frame swap doesn't reallocate.
    owe::SceneNode*   node { nullptr };
    JSValue           wrapped_layer { JS_UNDEFINED };
    // Per-script cursor-inside-bbox state used to edge-detect
    // cursorEnter / cursorLeave between frames.
    bool              cursor_inside { false };
    // Pre-spawned SceneNode clones available to thisScene.createLayer.
    // Populated by WireFieldScripts for audio-bar style scripts; popped
    // from the front each createLayer call.
    std::vector<owe::SceneNode*> clone_queue;
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
    std::uint64_t                                       next_module_serial { 0 };
    std::vector<std::unique_ptr<FieldScript>>           scripts;
    // Set of error-logged shas to log once.
    std::unordered_set<std::string>                     errored;
    // Scene root for `thisScene`. Wrapped lazily; freed in dtor.
    owe::SceneNode*                                     scene_root { nullptr };
    JSValue                                             wrapped_scene { JS_UNDEFINED };

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

// Cancel CFunction returned by setTimeout / setInterval. data[0] holds the
// deferred handle ID; invoking it tombstones the corresponding entry. The
// corpus uses both `clearTimeout(handle)` and `handle()` self-cancel forms,
// so handle is itself a callable.
JSValue EngineCancelDeferred(JSContext* ctx, JSValueConst /*this_val*/,
                              int /*argc*/, JSValueConst* /*argv*/,
                              int /*magic*/, JSValue* data) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    int32_t h = 0;
    JS_ToInt32(ctx, &h, data[0]);
    for (auto& d : host->deferred) {
        if (d.handle == uint32_t(h)) { d.dead = true; break; }
    }
    return JS_UNDEFINED;
}

JSValue MakeCancelFn(JSContext* ctx, uint32_t handle) {
    JSValue data[1] = { JS_NewInt32(ctx, int32_t(handle)) };
    JSValue fn = JS_NewCFunctionData(ctx, EngineCancelDeferred,
                                     /*length=*/0, /*magic=*/0,
                                     /*data_len=*/1, data);
    JS_FreeValue(ctx, data[0]);
    return fn;
}

JSValue EngineSetTimerImpl(JSContext* ctx, int argc, JSValueConst* argv,
                            bool repeating) {
    if (argc < 1 || ! JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
    auto*  host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    double ms   = 0.0;
    if (argc >= 2) JS_ToFloat64(ctx, &ms, argv[1]);
    double   interval_s = ms / 1000.0;
    uint32_t h          = host->next_handle++;
    host->deferred.push_back(DeferredCb {
        .handle     = h,
        .fire_at    = host->inputs.runtime + interval_s,
        .interval_s = interval_s,
        .fn         = JS_DupValue(ctx, argv[0]),
        .repeating  = repeating,
        .dead       = false,
    });
    return MakeCancelFn(ctx, h);
}

JSValue EngineSetTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return EngineSetTimerImpl(ctx, argc, argv, false);
}
JSValue EngineSetInterval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return EngineSetTimerImpl(ctx, argc, argv, true);
}

// Accepts either the cancel function (calls it) or any other value (ignored
// — old corpus shape sometimes hardcodes -1 from the previous noop).
JSValue EngineClearDeferred(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc == 0) return JS_UNDEFINED;
    if (JS_IsFunction(ctx, argv[0])) {
        JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(ctx, r);
    }
    return JS_UNDEFINED;
}

// --- localStorage backing ---------------------------------------------------
// Three CFunctions wired onto globalThis.localStorage. Values round-trip
// through JSON.stringify/parse: scripts get JSON-restorable types only
// (primitives, plain objects, arrays). Vec3 instances become plain
// {x,y,z} objects without `.add` / `.subtract` methods — corpus scripts
// that round-trip Vec3 through localStorage must re-wrap; none observed
// in the surveyed corpus.
//
// Writes flush to disk synchronously when a persistence path is set.
// The file format is a flat JSON object: {"k1": "json string", ...}.

namespace { struct PersistedLocalStorage {}; }

void FlushLocalStorage(EngineHostState* host) {
    if (host->ls_path.empty()) return;
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [k, v] : host->ls_data) out[k] = v;
    // ofstream defaults to ios_base::out | trunc, which is what we want.
    std::ofstream f(host->ls_path);
    if (! f) {
        rstd_warn("localStorage flush: cannot open {}", host->ls_path);
        return;
    }
    f << out.dump();
}

void LoadLocalStorage(EngineHostState* host) {
    host->ls_data.clear();
    if (host->ls_path.empty()) return;
    std::ifstream f(host->ls_path);
    if (! f) return;
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        rstd_warn("localStorage load: bad JSON in {}: {}", host->ls_path, e.what());
        return;
    }
    if (! doc.is_object()) return;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (it.value().is_string()) host->ls_data[it.key()] = it.value().get<std::string>();
    }
}

JSValue LocalStorageGet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* key = JS_ToCString(ctx, argv[0]);
    if (! key) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto it    = host->ls_data.find(key);
    JS_FreeCString(ctx, key);
    if (it == host->ls_data.end()) return JS_UNDEFINED;
    const auto& s = it->second;
    return JS_ParseJSON(ctx, s.data(), s.size(), "<localStorage>");
}

JSValue LocalStorageSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    const char* key = JS_ToCString(ctx, argv[0]);
    if (! key) return JS_UNDEFINED;
    JSValue     jv  = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(jv) || JS_IsUndefined(jv)) {
        JS_FreeValue(ctx, jv);
        JS_FreeCString(ctx, key);
        return JS_UNDEFINED;
    }
    const char* s    = JS_ToCString(ctx, jv);
    auto*       host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (s) host->ls_data[key] = s;
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, jv);
    JS_FreeCString(ctx, key);
    FlushLocalStorage(host);
    return JS_UNDEFINED;
}

JSValue LocalStorageRemove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* key  = JS_ToCString(ctx, argv[0]);
    if (! key) return JS_UNDEFINED;
    auto*       host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    host->ls_data.erase(key);
    JS_FreeCString(ctx, key);
    FlushLocalStorage(host);
    return JS_UNDEFINED;
}

void InstallLocalStorage(JSContext* ctx) {
    JSValue g  = JS_GetGlobalObject(ctx);
    JSValue ls = JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ls, "get",
        JS_NewCFunction(ctx, LocalStorageGet, "get", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ls, "set",
        JS_NewCFunction(ctx, LocalStorageSet, "set", 2), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ls, "remove",
        JS_NewCFunction(ctx, LocalStorageRemove, "remove", 1), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, g, "localStorage", ls, JS_PROP_C_W_E);
    JS_FreeValue(ctx, g);
}

// Project (cursor_x, cursor_y) — normalised canvas coords with y-down —
// into the same units the scene uses for SceneNode origins / sizes.
// SceneWallpaper feeds canvas_w / canvas_h matching the active camera's
// ortho rect, so cursor_pixel_x is in scene-units along X.
struct CursorWorld {
    double  x { 0 }, y { 0 };
};

CursorWorld CursorToWorld(const FrameInputs& fi) {
    return CursorWorld {
        .x = double(fi.cursor_x) * double(fi.canvas_w),
        // The cursor's Y comes in top-down (GLFW / canvas pixels). Scene
        // graph layers are positioned with Y also top-down (image
        // `origin[1]` is verbatim from scene.json, no inversion in the
        // parser today). Match that — don't flip.
        .y = double(fi.cursor_y) * double(fi.canvas_h),
    };
}

bool HitTestNode(owe::SceneNode* n, const CursorWorld& c) {
    if (! n) return false;
    n->UpdateTrans();
    Eigen::Matrix4d m = n->ModelTrans();
    Eigen::Vector2f sz = n->Size();
    if (sz.x() == 0.0f && sz.y() == 0.0f) sz = Eigen::Vector2f { 100.0f, 100.0f };
    double hx = sz.x() * 0.5, hy = sz.y() * 0.5;
    Eigen::Vector4d corners[4] = {
        { -hx, -hy, 0, 1 }, { hx, -hy, 0, 1 },
        {  hx,  hy, 0, 1 }, { -hx, hy, 0, 1 },
    };
    double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
    for (auto& corner : corners) {
        Eigen::Vector4d w = m * corner;
        minx = std::min(minx, w.x()); maxx = std::max(maxx, w.x());
        miny = std::min(miny, w.y()); maxy = std::max(maxy, w.y());
    }
    return c.x >= minx && c.x <= maxx && c.y >= miny && c.y <= maxy;
}

// Build the event object passed to cursor callbacks. WE scripts read
// event.worldPosition (a Vec2 in scene units) and event.button (0/1/2).
JSValue MakeCursorEvent(JSContext* ctx, const CursorWorld& c, int button) {
    JSValue ev = JS_NewObject(ctx);
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JSValue wp;
    if (! JS_IsUndefined(host->vec3_ctor)) {
        JSValue args[3] {
            JS_NewFloat64(ctx, c.x), JS_NewFloat64(ctx, c.y), JS_NewFloat64(ctx, 0)
        };
        wp = JS_CallConstructor(ctx, host->vec3_ctor, 3, args);
        for (auto& a : args) JS_FreeValue(ctx, a);
    } else {
        wp = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, wp, "x", JS_NewFloat64(ctx, c.x), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, wp, "y", JS_NewFloat64(ctx, c.y), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, wp, "z", JS_NewFloat64(ctx, 0), JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(ctx, ev, "worldPosition", wp, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, ev, "button", JS_NewInt32(ctx, button), JS_PROP_C_W_E);
    return ev;
}

// Invoke `name` on the script's module namespace if exported, passing one
// event arg. `thisLayer` should already be bound to the script's node by
// the caller. Exceptions are caught and logged once per sha.
void InvokeCursorCallback(JSContext* ctx, JSValue ns, const char* name,
                          JSValue ev, JsRuntime::Impl* rt, std::string_view sha) {
    JSValue fn = JS_GetPropertyStr(ctx, ns, name);
    if (JS_IsFunction(ctx, fn)) {
        JSValue arg = JS_DupValue(ctx, ev);
        JSValue r   = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(r)) {
            rt->LogError(ctx, sha, name);
            JS_FreeValue(ctx, r);
        } else {
            JS_FreeValue(ctx, r);
        }
    }
    JS_FreeValue(ctx, fn);
}

// Fire any deferred callbacks whose fire_at has passed. Called by TickAll
// before the script update loop. Repeating callbacks reschedule against
// their previous fire_at so steady-state drift is bounded.
void SweepDeferred(JSContext* ctx, EngineHostState* host) {
    const double now = host->inputs.runtime;
    // Iterate by index; callbacks may push_back new entries.
    for (size_t i = 0; i < host->deferred.size(); ++i) {
        if (host->deferred[i].dead) continue;
        while (! host->deferred[i].dead && host->deferred[i].fire_at <= now) {
            JSValue fn  = JS_DupValue(ctx, host->deferred[i].fn);
            JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
            JS_FreeValue(ctx, fn);
            if (JS_IsException(ret)) {
                JSValue exc = JS_GetException(ctx);
                const char* msg = JS_ToCString(ctx, exc);
                rstd_error("script timer callback threw: {}",
                           msg ? msg : "<no message>");
                if (msg) JS_FreeCString(ctx, msg);
                JS_FreeValue(ctx, exc);
                host->deferred[i].dead = true;
            }
            JS_FreeValue(ctx, ret);
            if (host->deferred[i].dead) break;
            if (! host->deferred[i].repeating) {
                host->deferred[i].dead = true;
                break;
            }
            host->deferred[i].fire_at += host->deferred[i].interval_s;
            // Guard against zero-interval intervals starving the loop.
            if (host->deferred[i].interval_s <= 0.0) {
                host->deferred[i].dead = true;
                break;
            }
        }
    }
    // Compact dead entries.
    for (auto& d : host->deferred) {
        if (d.dead && ! JS_IsUndefined(d.fn)) {
            JS_FreeValue(ctx, d.fn);
            d.fn = JS_UNDEFINED;
        }
    }
    host->deferred.erase(
        std::remove_if(host->deferred.begin(), host->deferred.end(),
                       [](const DeferredCb& d) { return d.dead; }),
        host->deferred.end());
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
  //   When _hostValues[name] is a {user, value} pair, resolve at access
  //   time against engine.userProperties so SetUserProperty calls made
  //   after parse propagate.
  builder.finish = function () {
    const _hostValues = builder._hostValues || {};
    const target = {};
    const unwrapUserProp = (h) => {
      if (h === undefined || h === null) return undefined;
      if (typeof h !== 'object' || !('user' in h) || !('value' in h)) return h;
      const u = engine.userProperties[h.user];
      if (u !== undefined) {
        // project.json stores user props as { type, value, ... }; pluck
        // .value when present, else use the bare value directly.
        if (typeof u === 'object' && u !== null && 'value' in u) return u.value;
        return u;
      }
      return h.value;
    };
    // WE substitutes user-prop values verbatim, even when the user's
    // slider range is wider than the script's declared range — corpus
    // wallpapers (e.g. workshop 3327063360) wire `min:-1,max:1` sliders
    // into scripts declaring `min:0,max:1` and rely on the negative
    // values reaching the formula to shift origin off-parent.
    for (const d of _props) {
      if (d && d.name) {
        Object.defineProperty(target, d.name, {
          enumerable: true,
          configurable: true,
          get() {
            if (Object.prototype.hasOwnProperty.call(_hostValues, d.name))
              return unwrapUserProp(_hostValues[d.name]);
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
    // Single-number arg splats to both components (WE convention,
    // e.g. `new Vec2(0.5)` => Vec2(0.5, 0.5)).
    if (typeof x === 'number' && y === undefined) { this.x = x; this.y = x; return; }
    this.x = (typeof x === 'number') ? x : 0;
    this.y = (typeof y === 'number') ? y : 0;
  }
  add(o)      { return new Vec2(this.x + (o.x ?? o), this.y + (o.y ?? o)); }
  subtract(o) { return new Vec2(this.x - (o.x ?? o), this.y - (o.y ?? o)); }
  multiply(o) { return new Vec2(this.x * (o.x ?? o), this.y * (o.y ?? o)); }
  divide(o)   { return new Vec2(this.x / (o.x ?? o), this.y / (o.y ?? o)); }
  copy()      { return new Vec2(this.x, this.y); }
  clone()     { return new Vec2(this.x, this.y); }
  length()    { return Math.sqrt(this.x*this.x + this.y*this.y); }
}
class Vec3 {
  constructor(x, y, z) {
    if (typeof x === 'object' && x !== null) {
      this.x = x.x ?? 0; this.y = x.y ?? 0; this.z = x.z ?? 0;
    } else if (typeof x === 'number' && y === undefined && z === undefined) {
      // Single-number arg splats to all three components (WE convention,
      // e.g. `new Vec3(scriptProperties.barWidth)` => Vec3(5,5,5)).
      this.x = x; this.y = x; this.z = x;
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
  copy()      { return new Vec3(this.x, this.y, this.z); }
  clone()     { return new Vec3(this.x, this.y, this.z); }
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

// localStorage is installed from C++ in InstallEngineGlobal so it can
// optionally persist to a JSON file under cache_path keyed by scene_id.
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
    define_fn("setTimeout",           EngineSetTimeout,    2);
    define_fn("setInterval",          EngineSetInterval,   2);
    define_fn("clearTimeout",         EngineClearDeferred, 1);
    define_fn("clearInterval",        EngineClearDeferred, 1);
    define_fn("registerAsset",        EngineRegisterAsset, 1);

    // A handful of corpus scripts call setTimeout/clearTimeout bare (no
    // `engine.` prefix). Mirror onto globalThis so they resolve.
    auto alias_to_global = [&](const char* name) {
        JSValue f = JS_GetPropertyStr(ctx, engine, name);
        JS_DefinePropertyValueStr(ctx, global, name, f, JS_PROP_C_W_E);
    };
    alias_to_global("setTimeout");
    alias_to_global("setInterval");
    alias_to_global("clearTimeout");
    alias_to_global("clearInterval");

    JS_FreeValue(ctx, engine);
    JS_FreeValue(ctx, global);

    // C++-backed localStorage. Replaces the in-memory JS Map; values
    // persist to a JSON file when JsRuntime::SetPersistence is called.
    InstallLocalStorage(ctx);
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
function smoothstep_impl(edge0, edge1, x) {
    const t = Math.max(0, Math.min(1, (x - edge0) / (edge1 - edge0)));
    return t * t * (3 - 2 * t);
}
// Corpus uses both casings; smoothStep (camelCase) is by far the more
// common form (~165 callsites vs lowercase).
export const smoothstep = smoothstep_impl;
export const smoothStep = smoothstep_impl;
export function step(edge, x) { return x < edge ? 0 : 1; }
export function sign(x) { return Math.sign(x); }
export function fract(x) { return x - Math.floor(x); }
export function deg2rad(d) { return d * (Math.PI / 180); }
export function rad2deg(r) { return r * (180 / Math.PI); }
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

// --- Layer-B: SceneNode wrapper class ---------------------------------------
// `thisLayer` / `thisScene` resolve to instances of WWLayer. The class holds
// the SceneNode pointer in JS_GetOpaque; lifetime is owned by Scene, the
// finalizer is a no-op (we don't dereference on free, just drop the ref).

static JSClassID s_layer_class_id = 0;

void LayerFinalizer(JSRuntime*, JSValue) {
    // SceneNode is owned by Scene; nothing to free here.
}

JSClassDef s_layer_class_def {
    .class_name = "WWLayer",
    .finalizer  = LayerFinalizer,
};

inline owe::SceneNode* GetLayerNode(JSValueConst v) {
    return static_cast<owe::SceneNode*>(JS_GetOpaque(v, s_layer_class_id));
}

JSValue WrapLayerNode(JSContext* ctx, owe::SceneNode* node) {
    JSValue obj = JS_NewObjectClass(ctx, s_layer_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, node);
    return obj;
}

inline JSValue MakeVec3(JSContext* ctx, double x, double y, double z) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    if (JS_IsUndefined(host->vec3_ctor)) {
        JSValue g       = JS_GetGlobalObject(ctx);
        host->vec3_ctor = JS_GetPropertyStr(ctx, g, "Vec3");
        JS_FreeValue(ctx, g);
    }
    JSValue args[3] {
        JS_NewFloat64(ctx, x),
        JS_NewFloat64(ctx, y),
        JS_NewFloat64(ctx, z),
    };
    JSValue r = JS_CallConstructor(ctx, host->vec3_ctor, 3, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, args[2]);
    return r;
}

inline bool ReadXYZ(JSContext* ctx, JSValueConst v, double& x, double& y, double& z) {
    if (! JS_IsObject(v)) return false;
    JSValue jx = JS_GetPropertyStr(ctx, v, "x");
    JSValue jy = JS_GetPropertyStr(ctx, v, "y");
    JSValue jz = JS_GetPropertyStr(ctx, v, "z");
    bool ok = (JS_ToFloat64(ctx, &x, jx) == 0)
            && (JS_ToFloat64(ctx, &y, jy) == 0)
            && (JS_ToFloat64(ctx, &z, jz) == 0);
    JS_FreeValue(ctx, jx);
    JS_FreeValue(ctx, jy);
    JS_FreeValue(ctx, jz);
    return ok;
}

// --- property accessors -----------------------------------------------------

JSValue NodeGetOrigin(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto v = n->Translate();
    return MakeVec3(ctx, v.x(), v.y(), v.z());
}
JSValue NodeSetOrigin(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0;
    if (! ReadXYZ(ctx, val, x, y, z)) return JS_UNDEFINED;
    n->SetTranslate({ float(x), float(y), float(z) });
    return JS_UNDEFINED;
}
JSValue NodeGetScale(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto v = n->Scale();
    return MakeVec3(ctx, v.x(), v.y(), v.z());
}
JSValue NodeSetScale(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0;
    if (! ReadXYZ(ctx, val, x, y, z)) return JS_UNDEFINED;
    n->SetScale({ float(x), float(y), float(z) });
    return JS_UNDEFINED;
}
JSValue NodeGetAngles(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto v = n->Rotation();
    return MakeVec3(ctx, v.x(), v.y(), v.z());
}
JSValue NodeSetAngles(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0;
    if (! ReadXYZ(ctx, val, x, y, z)) return JS_UNDEFINED;
    n->SetRotation({ float(x), float(y), float(z) });
    return JS_UNDEFINED;
}

// Stubs — properties scripts read but writing them would force RG rebuild.
JSValue NodeGetSize(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return MakeVec3(ctx, 100, 100, 0);
    const auto& s = n->Size();
    // Zero is the parser's "unknown" sentinel (particle/light nodes never
    // set it). Fall back to the legacy 100×100 the bootstrap stub returned.
    if (s.x() == 0.0f && s.y() == 0.0f) return MakeVec3(ctx, 100, 100, 0);
    return MakeVec3(ctx, s.x(), s.y(), 0);
}
JSValue NodeGetVisible(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    return JS_NewBool(ctx, n ? n->Visible() : true);
}
JSValue NodeSetVisible(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (n) n->SetVisible(JS_ToBool(ctx, val) != 0);
    return JS_UNDEFINED;
}
JSValue NodeGetAlpha(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    return JS_NewFloat64(ctx, n ? n->UserAlpha() : 1.0);
}
JSValue NodeSetAlpha(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double a = 1.0;
    JS_ToFloat64(ctx, &a, val);
    n->SetUserAlpha(float(a));
    return JS_UNDEFINED;
}
JSValue NodeGetBrightness(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    return JS_NewFloat64(ctx, n ? n->Brightness() : 1.0);
}
JSValue NodeSetBrightness(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double b = 1.0;
    JS_ToFloat64(ctx, &b, val);
    n->SetBrightness(float(b));
    return JS_UNDEFINED;
}
JSValue NodeGetColor(JSContext* ctx, JSValueConst this_val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return MakeVec3(ctx, 1, 1, 1);
    const auto& c = n->Color();
    return MakeVec3(ctx, c.x(), c.y(), c.z());
}
JSValue NodeSetColor(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    double x = 0, y = 0, z = 0;
    if (! ReadXYZ(ctx, val, x, y, z)) return JS_UNDEFINED;
    n->SetColor({ float(x), float(y), float(z) });
    return JS_UNDEFINED;
}
JSValue NodeGetVAlign(JSContext* ctx, JSValueConst)       { return JS_NewString(ctx, "center"); }
JSValue NodeGetHAlign(JSContext* ctx, JSValueConst)       { return JS_NewString(ctx, "center"); }
JSValue NodeSetIgnore(JSContext*, JSValueConst, JSValueConst) { return JS_UNDEFINED; }

// `text` is the only string-valued property on WWLayer. Most scripts only
// write it (clock / date / locale formatters); GetText therefore returns
// an empty string rather than tracking last-applied text state.
JSValue NodeGetText(JSContext* ctx, JSValueConst) {
    return JS_NewString(ctx, "");
}
JSValue NodeSetText(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_UNDEFINED;
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto  it   = host->text_setters.find(n);
    if (it == host->text_setters.end()) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (s == nullptr) return JS_UNDEFINED;
    it->second(std::string_view(s));
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

// --- methods ----------------------------------------------------------------

// Always return SOMETHING — many scripts cache `parent = thisLayer.getParent()`
// at init time and dereference it later without a null check. When there's
// no real parent (root layer or unbound node), hand back the default JS
// stub so `parent.origin` etc. silently no-op instead of TypeError'ing.
JSValue NodeGetParent(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetLayerNode(this_val);
    if (n && n->Parent()) return WrapLayerNode(ctx, n->Parent());
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    return JS_DupValue(ctx, host->default_layer);
}

JSValue NodeGetTransformMatrix(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto*   n   = GetLayerNode(this_val);
    JSValue m   = JS_NewArray(ctx);
    JSValue obj = JS_NewObject(ctx);
    if (n) {
        n->UpdateTrans();
        const auto& mat = n->ModelTrans();  // Eigen Matrix4d column-major
        for (int i = 0; i < 16; ++i) {
            JS_DefinePropertyValueUint32(ctx, m, i,
                                         JS_NewFloat64(ctx, mat.data()[i]),
                                         JS_PROP_C_W_E);
        }
    } else {
        constexpr double id[16] { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        for (int i = 0; i < 16; ++i) {
            JS_DefinePropertyValueUint32(ctx, m, i, JS_NewFloat64(ctx, id[i]),
                                         JS_PROP_C_W_E);
        }
    }
    JS_DefinePropertyValueStr(ctx, obj, "m", m, JS_PROP_C_W_E);
    return obj;
}

JSValue NodeGetChildren(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetLayerNode(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (! n) return arr;
    uint32_t i = 0;
    for (const auto& child : n->GetChildren()) {
        if (! child) continue;
        JS_DefinePropertyValueUint32(ctx, arr, i++,
                                     WrapLayerNode(ctx, child.get()),
                                     JS_PROP_C_W_E);
    }
    return arr;
}

JSValue NodeGetName(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetLayerNode(this_val);
    if (! n) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, n->Name().data(), n->Name().size());
}

JSValue NodeGetLayer(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* n    = GetLayerNode(this_val);
    if (! n || argc < 1) return JS_DupValue(ctx, host->default_layer);
    const char* name = JS_ToCString(ctx, argv[0]);
    if (! name) return JS_DupValue(ctx, host->default_layer);
    owe::SceneNode* hit = n->FindByName(name);
    JS_FreeCString(ctx, name);
    return hit ? WrapLayerNode(ctx, hit) : JS_DupValue(ctx, host->default_layer);
}

// thisScene.createLayer(model_path) — WE-style runtime layer spawn. The
// model path is ignored: parser-side pre-spawned a queue of SceneNode clones
// (one per expected createLayer call) when the script binding showed the
// audio-bar pattern. Pop the next clone here; fall back to the default
// stub when no clones remain so the script's caller still gets an object.
JSValue NodeSceneCreateLayer(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                             JSValueConst* /*argv*/) {
    auto* host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    auto* fs   = host->active_field_script;
    if (! fs || fs->m_impl->clone_queue.empty())
        return JS_DupValue(ctx, host->default_layer);
    owe::SceneNode* node = fs->m_impl->clone_queue.front();
    fs->m_impl->clone_queue.erase(fs->m_impl->clone_queue.begin());
    return WrapLayerNode(ctx, node);
}

// thisScene.getLayerIndex(layer) / sortLayer(layer, idx). owe doesn't have
// a draw-order layer index that scripts can mutate at runtime; return 0
// and no-op so audio-bar style scripts complete init without error.
JSValue NodeSceneGetLayerIndex(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt32(ctx, 0);
}
JSValue NodeSceneSortLayer(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

// --- WWTextureAnimation -----------------------------------------------------
// Wraps a SceneNode*'s TextureAnimatorState. Slot 0 only — every workshop
// script that touches `getTextureAnimation()` in the corpus uses the primary
// (diffuse) texture animation; multi-slot would need a different API shape.

static JSClassID s_texanim_class_id = 0;

JSClassDef s_texanim_class_def {
    .class_name = "WWTextureAnimation",
    .finalizer  = nullptr,  // SceneNode owns the state
};

inline owe::SceneNode* GetTexAnimNode(JSValueConst v) {
    return static_cast<owe::SceneNode*>(JS_GetOpaque(v, s_texanim_class_id));
}

JSValue TexAnimPlay(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* n = GetTexAnimNode(this_val)) {
        auto& a = n->TexAnim();
        a.current_frame = -1;
        a.playing       = true;
    }
    return JS_UNDEFINED;
}
JSValue TexAnimStop(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* n = GetTexAnimNode(this_val)) n->TexAnim().playing = false;
    return JS_UNDEFINED;
}
JSValue TexAnimPause(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    if (auto* n = GetTexAnimNode(this_val)) n->TexAnim().playing = false;
    return JS_UNDEFINED;
}
JSValue TexAnimSetFrame(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    auto* n = GetTexAnimNode(this_val);
    if (! n) return JS_UNDEFINED;
    int32_t f = 0;
    JS_ToInt32(ctx, &f, argv[0]);
    if (f < 0) f = 0;
    n->TexAnim().current_frame = f;
    n->TexAnim().playing       = false;
    return JS_UNDEFINED;
}
JSValue TexAnimGetFrame(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetTexAnimNode(this_val);
    if (! n) return JS_NewInt32(ctx, 0);
    const int f = n->TexAnim().current_frame;
    return JS_NewInt32(ctx, f < 0 ? 0 : f);
}
JSValue TexAnimIsPlaying(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetTexAnimNode(this_val);
    return JS_NewBool(ctx, n ? n->TexAnim().playing : false);
}

const JSCFunctionListEntry s_texanim_proto_funcs[] = {
    JS_CFUNC_DEF("play",      0, TexAnimPlay),
    JS_CFUNC_DEF("stop",      0, TexAnimStop),
    JS_CFUNC_DEF("pause",     0, TexAnimPause),
    JS_CFUNC_DEF("setFrame",  1, TexAnimSetFrame),
    JS_CFUNC_DEF("getFrame",  0, TexAnimGetFrame),
    JS_CFUNC_DEF("isPlaying", 0, TexAnimIsPlaying),
};

void InitTexAnimClass(JSContext* ctx, JSRuntime* rt) {
    if (s_texanim_class_id == 0) JS_NewClassID(rt, &s_texanim_class_id);
    JS_NewClass(rt, s_texanim_class_id, &s_texanim_class_def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, s_texanim_proto_funcs,
                                sizeof(s_texanim_proto_funcs) /
                                    sizeof(s_texanim_proto_funcs[0]));
    JS_SetClassProto(ctx, s_texanim_class_id, proto);
}

JSValue NodeGetTextureAnimation(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* n = GetLayerNode(this_val);
    if (! n) {
        // Unbound (default) layer — fall back to the JS-side stub so reads
        // like .getFrame() don't TypeError.
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue f = JS_GetPropertyStr(ctx, g, "__wwCreateTexAnimStub");
        JSValue r = JS_Call(ctx, f, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(ctx, f);
        JS_FreeValue(ctx, g);
        return r;
    }
    JSValue obj = JS_NewObjectClass(ctx, s_texanim_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, n);
    return obj;
}

const JSCFunctionListEntry s_layer_proto_funcs[] = {
    JS_CGETSET_DEF("origin",          NodeGetOrigin,  NodeSetOrigin),
    JS_CGETSET_DEF("scale",           NodeGetScale,   NodeSetScale),
    JS_CGETSET_DEF("angles",          NodeGetAngles,  NodeSetAngles),
    JS_CGETSET_DEF("size",            NodeGetSize,    NodeSetIgnore),
    JS_CGETSET_DEF("visible",         NodeGetVisible,    NodeSetVisible),
    JS_CGETSET_DEF("alpha",           NodeGetAlpha,      NodeSetAlpha),
    JS_CGETSET_DEF("brightness",      NodeGetBrightness, NodeSetBrightness),
    JS_CGETSET_DEF("color",           NodeGetColor,      NodeSetColor),
    JS_CGETSET_DEF("text",            NodeGetText,    NodeSetText),
    JS_CGETSET_DEF("verticalalign",   NodeGetVAlign,  NodeSetIgnore),
    JS_CGETSET_DEF("horizontalalign", NodeGetHAlign,  NodeSetIgnore),
    JS_CFUNC_DEF("getParent",           0, NodeGetParent),
    JS_CFUNC_DEF("getTransformMatrix",  0, NodeGetTransformMatrix),
    JS_CFUNC_DEF("getChildren",         0, NodeGetChildren),
    JS_CFUNC_DEF("getName",             0, NodeGetName),
    JS_CFUNC_DEF("getLayer",            1, NodeGetLayer),
    JS_CFUNC_DEF("getTextureAnimation", 0, NodeGetTextureAnimation),
    JS_CFUNC_DEF("createLayer",         1, NodeSceneCreateLayer),
    JS_CFUNC_DEF("getLayerIndex",       1, NodeSceneGetLayerIndex),
    JS_CFUNC_DEF("sortLayer",           2, NodeSceneSortLayer),
};

void InitLayerClass(JSContext* ctx, JSRuntime* rt) {
    if (s_layer_class_id == 0) JS_NewClassID(rt, &s_layer_class_id);
    JS_NewClass(rt, s_layer_class_id, &s_layer_class_def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, s_layer_proto_funcs,
                                sizeof(s_layer_proto_funcs) /
                                    sizeof(s_layer_proto_funcs[0]));
    JS_SetClassProto(ctx, s_layer_class_id, proto);
}

// Stash the bootstrap's `thisLayer` / `thisScene` stubs for restore.
void CaptureDefaultBindings(JSContext* ctx) {
    auto*   host = static_cast<EngineHostState*>(JS_GetContextOpaque(ctx));
    JSValue g    = JS_GetGlobalObject(ctx);
    host->default_layer = JS_GetPropertyStr(ctx, g, "thisLayer");
    host->default_scene = JS_GetPropertyStr(ctx, g, "thisScene");
    JS_FreeValue(ctx, g);
}

// Write `globalThis.thisLayer = val`. `val` is duplicated; ownership of
// the original ref stays with the caller.
void BindThisLayer(JSContext* ctx, JSValueConst val) {
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "thisLayer", JS_DupValue(ctx, val));
    JS_FreeValue(ctx, g);
}
void BindThisScene(JSContext* ctx, JSValueConst val) {
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "thisScene", JS_DupValue(ctx, val));
    JS_FreeValue(ctx, g);
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
    InitLayerClass(m_impl->ctx, m_impl->rt);
    InitTexAnimClass(m_impl->ctx, m_impl->rt);
    InstallEngineGlobal(m_impl->ctx);
    // Bootstrap created stub `thisLayer` / `thisScene` on globalThis.
    // Capture them now so per-script binding can fall back to the stub
    // when no SceneNode is provided.
    CaptureDefaultBindings(m_impl->ctx);
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
            if (! JS_IsUndefined(fs->m_impl->wrapped_layer))
                JS_FreeValue(m_impl->ctx, fs->m_impl->wrapped_layer);
        }
    }
    m_impl->scripts.clear();
    for (auto& [_sha, ns] : m_impl->ns_by_sha) JS_FreeValue(m_impl->ctx, ns);
    m_impl->ns_by_sha.clear();
    if (! JS_IsUndefined(m_impl->wrapped_scene))
        JS_FreeValue(m_impl->ctx, m_impl->wrapped_scene);
    if (! JS_IsUndefined(m_impl->host.vec3_ctor))
        JS_FreeValue(m_impl->ctx, m_impl->host.vec3_ctor);
    if (! JS_IsUndefined(m_impl->host.default_layer))
        JS_FreeValue(m_impl->ctx, m_impl->host.default_layer);
    if (! JS_IsUndefined(m_impl->host.default_scene))
        JS_FreeValue(m_impl->ctx, m_impl->host.default_scene);
    if (m_impl->host.audio_buffer_built) {
        JS_FreeValue(m_impl->ctx, m_impl->host.audio_buffer);
        m_impl->host.audio_buffer_built = false;
    }
    for (auto& d : m_impl->host.deferred) {
        if (! JS_IsUndefined(d.fn)) JS_FreeValue(m_impl->ctx, d.fn);
    }
    m_impl->host.deferred.clear();
    if (m_impl->ctx) JS_FreeContext(m_impl->ctx);
    if (m_impl->rt) JS_FreeRuntime(m_impl->rt);
}

void JsRuntime::SetFrameInputs(const FrameInputs& fi) {
    m_impl->host.inputs = fi;
    if (m_impl->host.audio_buffer_built) RefreshAudioBuffer(m_impl->ctx);
}

void JsRuntime::SetUserProperty(std::string_view key, const json& property) {
    if (! m_impl || ! m_impl->ctx) return;
    std::string key_str { key };
    JSContext*  ctx = m_impl->ctx;
    JSValue     global = JS_GetGlobalObject(ctx);
    JSValue     engine = JS_GetPropertyStr(ctx, global, "engine");
    JSValue     props  = JS_GetPropertyStr(ctx, engine, "userProperties");
    if (! JS_IsObject(props)) {
        JS_FreeValue(ctx, props);
        props = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, engine, "userProperties",
                                  JS_DupValue(ctx, props), JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(ctx, props, key_str.c_str(),
                              JsonToJs(ctx, property), JS_PROP_C_W_E);
    JS_FreeValue(ctx, props);
    JS_FreeValue(ctx, engine);
    JS_FreeValue(ctx, global);
}

void JsRuntime::SetPersistence(std::string path) {
    m_impl->host.ls_path = std::move(path);
    LoadLocalStorage(&m_impl->host);
}

void JsRuntime::SetSceneRoot(owe::SceneNode* root) {
    if (! m_impl || ! m_impl->ctx) return;
    if (! JS_IsUndefined(m_impl->wrapped_scene))
        JS_FreeValue(m_impl->ctx, m_impl->wrapped_scene);
    m_impl->scene_root    = root;
    m_impl->wrapped_scene = root ? WrapLayerNode(m_impl->ctx, root) : JS_UNDEFINED;
    if (! JS_IsUndefined(m_impl->wrapped_scene))
        BindThisScene(m_impl->ctx, m_impl->wrapped_scene);
}

void JsRuntime::TickAll() {
    JSContext* ctx = m_impl->ctx;
    SweepDeferred(ctx, &m_impl->host);

    // Cursor event dispatch. For every script bound to a SceneNode, hit-
    // test the cursor against the node's world AABB and fire any of
    // cursorEnter/Leave/Move/Down/Up/Click that the script's module
    // exports. Runs before update() so update can react to state writes
    // the callbacks made this frame.
    const CursorWorld cursor      = CursorToWorld(m_impl->host.inputs);
    const bool        in_window   = m_impl->host.inputs.cursor_in_window;
    const uint32_t    btn_pressed = m_impl->host.inputs.mouse_buttons_pressed;
    const uint32_t    btn_release = m_impl->host.inputs.mouse_buttons_released;
    JSValue           ev_shared   = JS_UNDEFINED;
    auto              ensure_ev   = [&](int button) -> JSValue {
        if (! JS_IsUndefined(ev_shared)) JS_FreeValue(ctx, ev_shared);
        ev_shared = MakeCursorEvent(ctx, cursor, button);
        return ev_shared;
    };
    for (auto& fs : m_impl->scripts) {
        auto* I = fs->m_impl.get();
        if (! I->alive || ! I->node) continue;
        const bool now_inside = in_window && HitTestNode(I->node, cursor);
        BindThisLayer(ctx, JS_IsUndefined(I->wrapped_layer) ? m_impl->host.default_layer
                                                            : I->wrapped_layer);
        m_impl->host.active_field_script = fs.get();
        if (now_inside != I->cursor_inside) {
            InvokeCursorCallback(ctx, I->module_ns,
                                 now_inside ? "cursorEnter" : "cursorLeave",
                                 ensure_ev(-1), m_impl.get(), I->sha);
            I->cursor_inside = now_inside;
        }
        if (now_inside) {
            InvokeCursorCallback(ctx, I->module_ns, "cursorMove",
                                 ensure_ev(-1), m_impl.get(), I->sha);
        }
        if (btn_pressed && now_inside) {
            for (int b = 0; b < 3; ++b) {
                if (btn_pressed & (1u << b)) {
                    InvokeCursorCallback(ctx, I->module_ns, "cursorDown",
                                         ensure_ev(b), m_impl.get(), I->sha);
                    InvokeCursorCallback(ctx, I->module_ns, "cursorClick",
                                         ensure_ev(b), m_impl.get(), I->sha);
                }
            }
        }
        if (btn_release && now_inside) {
            for (int b = 0; b < 3; ++b) {
                if (btn_release & (1u << b)) {
                    InvokeCursorCallback(ctx, I->module_ns, "cursorUp",
                                         ensure_ev(b), m_impl.get(), I->sha);
                }
            }
        }
    }
    if (! JS_IsUndefined(ev_shared)) JS_FreeValue(ctx, ev_shared);

    for (auto& fs : m_impl->scripts) {
        auto* I = fs->m_impl.get();
        if (! I->alive) continue;
        if (JS_IsUndefined(I->update_fn)) continue;
        // Swap `thisLayer` to this script's bound node before update. When
        // unbound, restore the original stub captured at bootstrap.
        BindThisLayer(ctx,
                      JS_IsUndefined(I->wrapped_layer) ? m_impl->host.default_layer
                                                       : I->wrapped_layer);
        m_impl->host.active_field_script = fs.get();
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
    m_impl->host.active_field_script = nullptr;
}

void JsRuntime::ForEachScript(EachFn fn, void* user) {
    for (auto& fs : m_impl->scripts) fn(fs.get(), user);
}

void JsRuntime::RegisterTextSetter(owe::SceneNode* node,
                                   std::function<void(std::string_view)> setter) {
    if (node == nullptr) return;
    m_impl->host.text_setters[node] = std::move(setter);
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
                                        const json&      initial_value,
                                        owe::SceneNode*  node,
                                        std::vector<owe::SceneNode*> clones) {
    JSContext* ctx = m_impl->ctx;
    if (! ctx) return nullptr;

    // Wrap `node` (if any) up front. Bind it as `thisLayer` for the
    // duration of module eval + init so module-body top-level statements
    // like `let parent = thisLayer.getParent()` see the real node.
    JSValue wrapped = node ? WrapLayerNode(ctx, node) : JS_UNDEFINED;
    BindThisLayer(ctx, JS_IsUndefined(wrapped) ? m_impl->host.default_layer : wrapped);

    // 1. Compile + evaluate the module fresh per FieldScript. Caching by
    //    source-sha would share `scriptProperties._hostValues` across all
    //    instances using the same source — workshop wallpapers commonly
    //    reuse the position-template script across many layers (each with
    //    distinct {user, value} bindings), so a shared _hostValues makes
    //    every instance read whichever binding was wired last.
    JSValue       ns;
    auto          sha_str = std::string(script_sha);
    std::uint64_t uniq    = m_impl->next_module_serial++;
    std::string   fname   = "scripts/" + sha_str + "-" +
                              std::to_string(uniq) + ".js";
    {
        JSValue compiled = JS_Eval(ctx, source.data(), source.size(),
                                   fname.c_str(),
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            m_impl->LogError(ctx, script_sha, "compile failed");
            JS_FreeValue(ctx, compiled);
            if (! JS_IsUndefined(wrapped)) JS_FreeValue(ctx, wrapped);
            return nullptr;
        }
        JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JSValue ev = JS_EvalFunction(ctx, compiled);
        if (JS_IsException(ev)) {
            m_impl->LogError(ctx, script_sha, "module eval failed");
            JS_FreeValue(ctx, ev);
            if (! JS_IsUndefined(wrapped)) JS_FreeValue(ctx, wrapped);
            return nullptr;
        }
        JS_FreeValue(ctx, ev);
        ns = JS_GetModuleNamespace(ctx, m);
    }

    // 2. Build the FieldScript handle.
    auto fs           = std::make_unique<FieldScript>();
    auto* I           = fs->m_impl.get();
    I->rt             = m_impl.get();
    I->ctx            = ctx;
    I->sha            = sha_str;
    I->kind           = (field_kind_in == FieldKind::Unknown) ? FieldKind::Scalar : field_kind_in;
    I->module_ns      = ns;  // owns one ref now
    I->node           = node;
    I->wrapped_layer  = wrapped;  // takes ownership; freed in JsRuntime dtor
    I->clone_queue    = std::move(clones);

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
    //    coerced to match the bound field's expected JS shape. Mark this
    //    fs as the active script so init-time createLayer calls pop from
    //    its clone_queue.
    JSValue init_fn = JS_GetPropertyStr(ctx, ns, "init");
    JSValue init_arg = CoerceInitialValue(ctx, initial_value, I->kind);
    if (JS_IsFunction(ctx, init_fn)) {
        m_impl->host.active_field_script = fs.get();
        JSValue r = JS_Call(ctx, init_fn, JS_UNDEFINED, 1, &init_arg);
        if (JS_IsException(r)) {
            m_impl->LogError(ctx, script_sha, "init threw");
        }
        JS_FreeValue(ctx, r);
        m_impl->host.active_field_script = nullptr;
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
// Empty = no scripts AND no actuators. Visibility-bound side-effect-only
// scripts (audio bar fanout) don't register an actuator but still need
// their TickAll to run, so emptiness must also consult the runtime.
bool       ScriptScene::empty() const noexcept {
    if (! m_impl->actuators.empty()) return false;
    bool has_script = false;
    m_impl->rt.ForEachScript(
        [](script::FieldScript*, void* u) { *static_cast<bool*>(u) = true; },
        &has_script);
    return ! has_script;
}

std::function<void(const ScriptValue&)>
MakeNodeTransformApply(std::shared_ptr<owe::SceneNode> node, NodeTransformTarget target) {
    return [node = std::move(node), target](const ScriptValue& v) {
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

void SetSceneUserProperty(owe::Scene& scene, std::string_view key,
                          const nlohmann::json& property) {
    if (auto* ss = static_cast<ScriptScene*>(scene.script_scene.get()); ss != nullptr) {
        ss->runtime().SetUserProperty(key, property);
    }
    // WE field-binding fanout: lights whose `visible` field is tied to
    // `engine.userProperties[key]` flip runtime visibility here. Scene-only
    // (no JS) wallpapers also need this, so it runs unconditionally.
    bool have_bool = false;
    bool v = false;
    if (property.is_object() && property.contains("value") &&
        property.at("value").is_boolean()) {
        v = property.at("value").get<bool>();
        have_bool = true;
    } else if (property.is_boolean()) {
        v = property.get<bool>();
        have_bool = true;
    }
    if (have_bool) {
        const std::string key_s { key };
        for (auto& lp : scene.lights) {
            if (lp && lp->visibleUserKey() == key_s) lp->setRuntimeVisible(v);
        }
    }
}

void SetScenePersistence(owe::Scene& scene, std::string path) {
    auto* ss = static_cast<ScriptScene*>(scene.script_scene.get());
    if (! ss) return;
    ss->runtime().SetPersistence(std::move(path));
}

}  // namespace owe::script
