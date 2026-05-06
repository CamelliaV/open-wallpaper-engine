module;

#include "WPJson.hpp"
#include <nlohmann/json.hpp>

module wescene.parse;
import cppstd;

namespace wallpaper::wpscene {

bool ParseAnimKeyframeTangent(const nlohmann::json& json, WPAnimKeyframeTangent& out) {
    if (! json.is_object()) return false;
    GET_JSON_NAME_VALUE_NOWARN(json, "enabled", out.enabled);
    GET_JSON_NAME_VALUE_NOWARN(json, "x", out.x);
    GET_JSON_NAME_VALUE_NOWARN(json, "y", out.y);
    GET_JSON_NAME_VALUE_NOWARN(json, "magic", out.magic);
    return true;
}

bool ParseAnimKeyframe(const nlohmann::json& json, WPAnimKeyframe& out) {
    if (! json.is_object()) return false;
    GET_JSON_NAME_VALUE_NOWARN(json, "frame", out.frame);
    GET_JSON_NAME_VALUE_NOWARN(json, "value", out.value);
    GET_JSON_NAME_VALUE_NOWARN(json, "lockangle", out.lockangle);
    GET_JSON_NAME_VALUE_NOWARN(json, "locklength", out.locklength);
    if (json.contains("front")) ParseAnimKeyframeTangent(json.at("front"), out.front);
    if (json.contains("back"))  ParseAnimKeyframeTangent(json.at("back"),  out.back);
    return true;
}

bool ParseAnimAxis(const nlohmann::json& json, std::vector<WPAnimKeyframe>& out) {
    if (! json.is_array()) return false;
    out.reserve(json.size());
    for (const auto& jK : json) {
        WPAnimKeyframe k;
        if (ParseAnimKeyframe(jK, k)) out.push_back(std::move(k));
    }
    return true;
}

bool ParseAnimOptions(const nlohmann::json& json, WPAnimOptions& out) {
    if (! json.is_object()) return false;
    GET_JSON_NAME_VALUE_NOWARN(json, "fps", out.fps);
    GET_JSON_NAME_VALUE_NOWARN(json, "length", out.length);
    GET_JSON_NAME_VALUE_NOWARN(json, "mode", out.mode);
    GET_JSON_NAME_VALUE_NOWARN(json, "name", out.name);
    GET_JSON_NAME_VALUE_NOWARN(json, "startpaused", out.startpaused);
    GET_JSON_NAME_VALUE_NOWARN(json, "wraploop", out.wraploop);
    if (json.contains("smoothing")) out.smoothing = json.at("smoothing");
    if (json.contains("children"))  out.children  = json.at("children");
    if (json.contains("events"))    out.events    = json.at("events");
    if (json.contains("parent"))    out.parent    = json.at("parent");
    return true;
}

bool ParseAnimCurve(const nlohmann::json& json, WPAnimCurve& out) {
    if (! json.is_object()) return false;
    if (json.contains("c0")) ParseAnimAxis(json.at("c0"), out.c0);
    if (json.contains("c1")) ParseAnimAxis(json.at("c1"), out.c1);
    if (json.contains("c2")) ParseAnimAxis(json.at("c2"), out.c2);
    if (json.contains("options")) ParseAnimOptions(json.at("options"), out.options);
    GET_JSON_NAME_VALUE_NOWARN(json, "relative", out.relative);
    return true;
}

std::size_t AbsorbAllFieldBindings(const nlohmann::json& obj_json, WPFieldBindings& out) {
    if (! obj_json.is_object()) return 0;
    std::size_t n = 0;
    for (const auto& [field_name, field_value] : obj_json.items()) {
        if (! field_value.is_object()) continue;
        if (field_value.contains("animation")) {
            WPAnimCurve curve;
            if (ParseAnimCurve(field_value.at("animation"), curve)) {
                out.animations[field_name] = std::move(curve);
                ++n;
            }
        }
        if (field_value.contains("scriptproperties")) {
            out.scriptproperties[field_name] = field_value.at("scriptproperties");
            ++n;
        }
        if (field_value.contains("script") && field_value.at("script").is_string()) {
            WPScriptBinding sb;
            sb.source = field_value.at("script").get<std::string>();
            if (field_value.contains("scriptproperties"))
                sb.properties = field_value.at("scriptproperties");
            if (field_value.contains("value"))
                sb.initial_value = field_value.at("value");
            if (field_value.contains("user") && field_value.at("user").is_string())
                sb.user = field_value.at("user").get<std::string>();
            out.scripts[field_name] = std::move(sb);
            ++n;
        }
    }
    return n;
}

} // namespace wallpaper::wpscene
