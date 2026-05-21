module;


export module wescene.parse:wp_uniform;
import rstd.cppstd;
import wescene.fs;

import wescene.json;

export namespace owe

{
namespace wpscene
{

// Texture-uniform metadata. WE's per-sampler `// {json}` annotation is parsed
// into this struct. Fields not used by the renderer (label / group / ...) are
// stored anyway so future editor / material UI consumers can read them back.
struct WPUniformTex {
    struct Component {
        std::string label;
        std::string combo;
    };
    std::string material;       // unique key for material override
    std::string label;          // editor display name
    std::string default_;       // default texture path or `_rt_*`
    std::string mode;           // opacitymask / rgbmask / flowmask
    std::string combo;          // bound? `#define <IDENT> 1` else 0
    std::array<float, 4> paintdefaultcolor { 0, 0, 0, 1.0f };
    std::vector<Component>               components;
    bool                                 requireany { false };
    std::unordered_map<std::string, int> require;

    // Corpus-observed extras (parsed, not yet consumed).
    bool        hidden { false };
    bool        nonremovable { false };
    std::string group;
    bool        linked { false };
    std::string format;          // "normalmap" etc.
    bool        formatcombo { false };
    bool        direction { false };
    std::string conversion;      // "startdelta" etc.
    int         order { 0 };

    bool FromJson(const nlohmann::json& json) {
        owe::GetJsonValue(json, "material", material, false);
        owe::GetJsonValue(json, "label", label, false);
        owe::GetJsonValue(json, "default", default_, false);

        owe::GetJsonValue(json, "mode", mode, false);
        owe::GetJsonValue(json, "combo", combo, false);
        if (json.contains("components")) {
            for (const auto& el : json.at("components")) {
                Component c;
                owe::GetJsonValue(el, "label", c.label);
                owe::GetJsonValue(el, "combo", c.combo, false);
                components.push_back(c);
            }
        }
        owe::GetJsonValue(json, "requireany", requireany, false);
        if (json.contains("require") && json.at("require").is_object()) {
            for (const auto& el : json.at("require").items()) {
                int value { false };
                owe::GetJsonValue(el.value(), value);
                require[el.key()] = value;
            }
        }

        owe::GetJsonValue(json, "hidden", hidden, false);
        owe::GetJsonValue(json, "nonremovable", nonremovable, false);
        owe::GetJsonValue(json, "group", group, false);
        owe::GetJsonValue(json, "linked", linked, false);
        owe::GetJsonValue(json, "format", format, false);
        owe::GetJsonValue(json, "formatcombo", formatcombo, false);
        owe::GetJsonValue(json, "direction", direction, false);
        owe::GetJsonValue(json, "conversion", conversion, false);
        owe::GetJsonValue(json, "order", order, false);
        return true;
    }
};

// Scalar / vec / color / UV uniform metadata. Covers `g_*` user-controlled
// scalars (e.g. `g_Brightness`) and `u_*` user-variable convention.
struct WPUniformVar {
    std::string    name;             // GLSL identifier (e.g. "g_Brightness")
    std::string    material;         // UI key for editor / project bindings
    std::string    label;
    std::string    group;
    std::string    type;             // "color" | "" (UV picker uses position:true)
    bool           position { false };
    bool           linked { false };
    bool           nobindings { false };
    bool           is_user { false };  // true iff name starts with "u_"
    std::array<float, 2> range { 0.0f, 1.0f };
    bool           has_range { false };

    // Default value as raw JSON. Host coerces to float / vec2 / vec3 / vec4
    // depending on uniform type at upload time.
    nlohmann::json default_value;

    bool FromJson(const nlohmann::json& json, std::string uniform_name) {
        name    = std::move(uniform_name);
        is_user = name.size() >= 2 && name[0] == 'u' && name[1] == '_';
        owe::GetJsonValue(json, "material", material, false);
        owe::GetJsonValue(json, "label", label, false);
        owe::GetJsonValue(json, "group", group, false);
        owe::GetJsonValue(json, "type", type, false);
        owe::GetJsonValue(json, "position", position, false);
        owe::GetJsonValue(json, "linked", linked, false);
        owe::GetJsonValue(json, "nobindings", nobindings, false);
        if (json.contains("range") && json.at("range").is_array() &&
            json.at("range").size() >= 2) {
            range[0]  = json.at("range").at(0).get<float>();
            range[1]  = json.at("range").at(1).get<float>();
            has_range = true;
        }
        if (json.contains("default")) {
            default_value = json.at("default");
        }
        return true;
    }
};

// [COMBO] preprocessor switch declaration. `combo` is the IDENT that gets
// `#define`'d in the GLSL prologue, with value `default_` (or whichever
// option the user picked in the editor).
struct WPCombo {
    std::string material;            // editor display name
    std::string combo;               // IDENT injected as #define
    std::string type;                // "options" | "" (checkbox)
    int         default_ { 0 };
    std::unordered_map<std::string, int> options;  // label → value (combo box mode)
    std::unordered_map<std::string, int> require;  // gating combos

    bool FromJson(const nlohmann::json& json) {
        owe::GetJsonValue(json, "material", material, false);
        owe::GetJsonValue(json, "combo", combo, false);
        owe::GetJsonValue(json, "type", type, false);
        owe::GetJsonValue(json, "default", default_, false);
        if (json.contains("options") && json.at("options").is_object()) {
            for (const auto& el : json.at("options").items()) {
                int value { 0 };
                owe::GetJsonValue(el.value(), value);
                options[el.key()] = value;
            }
        }
        if (json.contains("require") && json.at("require").is_object()) {
            for (const auto& el : json.at("require").items()) {
                int value { 0 };
                owe::GetJsonValue(el.value(), value);
                require[el.key()] = value;
            }
        }
        return true;
    }
};

} // namespace wpscene
} // namespace owe
