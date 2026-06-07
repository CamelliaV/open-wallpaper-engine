module;

module wescene.parse;
import nlohmann.json;
import rstd.cppstd;

namespace owe::wpscene
{

bool ParseAnimKeyframeTangent(const nlohmann::json& json, WPAnimKeyframeTangent& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "enabled", out.enabled, false);
    owe::GetJsonValue(json, "x", out.x, false);
    owe::GetJsonValue(json, "y", out.y, false);
    owe::GetJsonValue(json, "magic", out.magic, false);
    return true;
}

bool ParseAnimKeyframe(const nlohmann::json& json, WPAnimKeyframe& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "frame", out.frame, false);
    owe::GetJsonValue(json, "value", out.value, false);
    owe::GetJsonValue(json, "lockangle", out.lockangle, false);
    owe::GetJsonValue(json, "locklength", out.locklength, false);
    if (json.contains("front")) ParseAnimKeyframeTangent(json.at("front"), out.front);
    if (json.contains("back")) ParseAnimKeyframeTangent(json.at("back"), out.back);
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
    owe::GetJsonValue(json, "fps", out.fps, false);
    owe::GetJsonValue(json, "length", out.length, false);
    owe::GetJsonValue(json, "mode", out.mode, false);
    owe::GetJsonValue(json, "name", out.name, false);
    owe::GetJsonValue(json, "startpaused", out.startpaused, false);
    owe::GetJsonValue(json, "wraploop", out.wraploop, false);
    if (json.contains("smoothing")) out.smoothing = json.at("smoothing");
    if (json.contains("children")) out.children = json.at("children");
    if (json.contains("events")) out.events = json.at("events");
    if (json.contains("parent")) out.parent = json.at("parent");
    return true;
}

bool ParseAnimCurve(const nlohmann::json& json, WPAnimCurve& out) {
    if (! json.is_object()) return false;
    if (json.contains("c0")) ParseAnimAxis(json.at("c0"), out.c0);
    if (json.contains("c1")) ParseAnimAxis(json.at("c1"), out.c1);
    if (json.contains("c2")) ParseAnimAxis(json.at("c2"), out.c2);
    if (json.contains("options")) ParseAnimOptions(json.at("options"), out.options);
    owe::GetJsonValue(json, "relative", out.relative, false);
    return true;
}

std::size_t AbsorbAllFieldBindings(const nlohmann::json& obj_json, WPFieldBindings& out) {
    if (! obj_json.is_object()) return 0;
    std::size_t n = 0;
    for (const auto& el : obj_json.items()) {
        const auto& field_value = el.value();
        if (! field_value.is_object()) continue;
        if (field_value.contains("animation")) {
            WPAnimCurve curve;
            if (ParseAnimCurve(field_value.at("animation"), curve)) {
                out.animations[el.key()] = std::move(curve);
                ++n;
            }
        }
        if (field_value.contains("scriptproperties")) {
            out.scriptproperties[el.key()] = field_value.at("scriptproperties");
            ++n;
        }
        if (field_value.contains("script") && field_value.at("script").is_string()) {
            WPScriptBinding sb;
            sb.source = field_value.at("script").get<std::string>();
            if (field_value.contains("scriptproperties"))
                sb.properties = field_value.at("scriptproperties");
            if (field_value.contains("value")) sb.initial_value = field_value.at("value");
            if (field_value.contains("user") && field_value.at("user").is_string())
                sb.user = field_value.at("user").get<std::string>();
            out.scripts[el.key()] = std::move(sb);
            ++n;
        }
    }
    return n;
}

} // namespace owe::wpscene
