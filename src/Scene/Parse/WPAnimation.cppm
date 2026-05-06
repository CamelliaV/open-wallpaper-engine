module;

#include "WPJson.hpp"
#include <nlohmann/json.hpp>

export module wescene.parse:wp_animation;
import cppstd;
import wescene.json;

// Property-binding side channel.
//
// In Wallpaper Engine any scalar field on a scene object can take three
// shapes:
//
//    1. Plain literal       → `42`, `"1 2 3"`, `true`
//    2. Property-bound       → `{"value": X, "user": "<binding-name>"}`
//                             (auto-unwrapped by WPJson::_GetJsonValue)
//    3. Animated / scripted  → `{"value": X, "animation": {...}}` or
//                             `{"value": X, "scriptproperties": {...}}`
//
// The renderer currently consumes only (1)/(2). The animation curve and
// scriptproperties subtrees are absorbed verbatim so the parsed data
// stays schema-complete; SceneSchema tests assert every observed leaf
// path under `*.animation.*` is captured.
//
// One curve covers a vec3 field (c0/c1/c2 axes) or a scalar (c0 only).

export namespace wallpaper::wpscene
{

struct WPAnimKeyframeTangent {
    bool  enabled { false };
    float x { 0.0f };
    float y { 0.0f };
    // `magic` is a sometimes-present opaque editor value (unsigned int);
    // captured to keep the schema check honest.
    std::int32_t magic { 0 };
};

struct WPAnimKeyframe {
    std::int32_t          frame { 0 };
    float                 value { 0.0f };
    bool                  lockangle { false };
    bool                  locklength { false };
    WPAnimKeyframeTangent front;
    WPAnimKeyframeTangent back;
};

struct WPAnimOptions {
    float                  fps { 30.0f };
    std::int32_t           length { 0 };
    std::string            mode;
    std::string            name;
    bool                   startpaused { false };
    bool                   wraploop { false };
    // `smoothing` may be null/int/float in the corpus; kept as raw json
    // until a renderer consumer needs it.
    nlohmann::json         smoothing;
    nlohmann::json         children;   // array of nested anim refs
    nlohmann::json         events;     // array of marker objects
    nlohmann::json         parent;     // object describing parent anim
};

struct WPAnimCurve {
    std::vector<WPAnimKeyframe> c0;
    std::vector<WPAnimKeyframe> c1;   // empty for scalar fields
    std::vector<WPAnimKeyframe> c2;
    WPAnimOptions               options;
    bool                        relative { false };  // only on `origin`
};

// FromJson helpers (defined in WPAnimation.cpp).
bool ParseAnimKeyframeTangent(const nlohmann::json&, WPAnimKeyframeTangent&);
bool ParseAnimKeyframe(const nlohmann::json&, WPAnimKeyframe&);
bool ParseAnimAxis(const nlohmann::json&, std::vector<WPAnimKeyframe>&);
bool ParseAnimOptions(const nlohmann::json&, WPAnimOptions&);
bool ParseAnimCurve(const nlohmann::json&, WPAnimCurve&);

// One captured `{value, script, scriptproperties, user}` per-field
// binding. `source` is the inline JS module text observed in scene.json's
// `"script"` key (5286 bindings, 2877 unique sources in the workshop
// corpus — see `tests/wpscriptdump`). `properties` mirrors the per-binding
// `scriptproperties` config block; `initial_value` is the binding's
// `value` field, fed to `init(value)` by the runtime. `user` carries the
// optional user-property name from `{user, value}` companion bindings.
struct WPScriptBinding {
    std::string    source;
    nlohmann::json properties;
    nlohmann::json initial_value;
    std::string    user;
};

// Side-channel container attached to every parseable object kind. Only
// fields that actually carry a binding contribute entries — empty maps
// for the common case where every field is a plain literal.
struct WPFieldBindings {
    std::unordered_map<std::string, WPAnimCurve>     animations;
    std::unordered_map<std::string, nlohmann::json>  scriptproperties;
    std::unordered_map<std::string, WPScriptBinding> scripts;
};

// Walks every direct child of `obj_json` and, when the child is an
// object containing `animation` and/or `scriptproperties`, captures into
// `out`. Idempotent: re-running on the same json overwrites prior
// entries. Returns the count of bindings absorbed.
std::size_t AbsorbAllFieldBindings(const nlohmann::json& obj_json, WPFieldBindings& out);

} // namespace wallpaper::wpscene
