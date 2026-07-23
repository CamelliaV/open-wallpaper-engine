module;

module wescene.pkg.scene_obj;
import rstd.cppstd;

using namespace rstd::literals;

namespace owe::wpscene
{

bool ParseAnimKeyframeTangent(const owe::Json& json, AnimKeyframeTangent& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "enabled", out.enabled, false);
    owe::GetJsonValue(json, "x", out.x, false);
    owe::GetJsonValue(json, "y", out.y, false);
    owe::GetJsonValue(json, "magic", out.magic, false);
    return true;
}

bool ParseAnimKeyframe(const owe::Json& json, AnimKeyframe& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "frame", out.frame, false);
    owe::GetJsonValue(json, "value", out.value, false);
    owe::GetJsonValue(json, "lockangle", out.lockangle, false);
    owe::GetJsonValue(json, "locklength", out.locklength, false);
    if (auto front = json.get("front"_str); front.is_some())
        ParseAnimKeyframeTangent(**front, out.front);
    if (auto back = json.get("back"_str); back.is_some())
        ParseAnimKeyframeTangent(**back, out.back);
    return true;
}

bool ParseAnimAxis(const owe::Json& json, std::vector<AnimKeyframe>& out) {
    if (! json.is_array()) return false;
    auto array = json.as_array();
    out.reserve((*array)->len().to_primitive());
    for (const auto& jK : **array) {
        AnimKeyframe k;
        if (ParseAnimKeyframe(jK, k)) out.push_back(std::move(k));
    }
    return true;
}

bool ParseAnimOptions(const owe::Json& json, AnimOptions& out) {
    if (! json.is_object()) return false;
    owe::GetJsonValue(json, "fps", out.fps, false);
    owe::GetJsonValue(json, "length", out.length, false);
    owe::GetJsonValue(json, "mode", out.mode, false);
    owe::GetJsonValue(json, "name", out.name, false);
    owe::GetJsonValue(json, "startpaused", out.startpaused, false);
    owe::GetJsonValue(json, "wraploop", out.wraploop, false);
    if (auto value = json.get("smoothing"_str); value.is_some()) out.smoothing = (*value)->clone();
    if (auto value = json.get("children"_str); value.is_some()) out.children = (*value)->clone();
    if (auto value = json.get("events"_str); value.is_some()) out.events = (*value)->clone();
    if (auto value = json.get("parent"_str); value.is_some()) out.parent = (*value)->clone();
    return true;
}

bool ParseAnimCurve(const owe::Json& json, AnimCurve& out) {
    if (! json.is_object()) return false;
    if (auto value = json.get("c0"_str); value.is_some()) ParseAnimAxis(**value, out.c0);
    if (auto value = json.get("c1"_str); value.is_some()) ParseAnimAxis(**value, out.c1);
    if (auto value = json.get("c2"_str); value.is_some()) ParseAnimAxis(**value, out.c2);
    if (auto value = json.get("options"_str); value.is_some())
        ParseAnimOptions(**value, out.options);
    owe::GetJsonValue(json, "relative", out.relative, false);
    return true;
}

auto AnimCurve::clone() const -> AnimCurve {
    AnimCurve result;
    result.c0                  = c0;
    result.c1                  = c1;
    result.c2                  = c2;
    result.options.fps         = options.fps;
    result.options.length      = options.length;
    result.options.mode        = options.mode;
    result.options.name        = options.name;
    result.options.startpaused = options.startpaused;
    result.options.wraploop    = options.wraploop;
    result.options.smoothing   = options.smoothing.clone();
    result.options.children    = options.children.clone();
    result.options.events      = options.events.clone();
    result.options.parent      = options.parent.clone();
    result.relative            = relative;
    return result;
}

std::size_t AbsorbAllFieldBindings(const owe::Json& obj_json, FieldBindings& out) {
    if (! obj_json.is_object()) return 0;
    std::size_t n      = 0;
    auto        object = obj_json.as_object();
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  field             = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& field_value       = *entry_value;
        if (! field_value.is_object()) return;
        if (auto animation = field_value.get("animation"_str); animation.is_some()) {
            AnimCurve curve;
            if (ParseAnimCurve(**animation, curve)) {
                out.animations[std::string(field)] = std::move(curve);
                ++n;
            }
        }
        if (auto properties = field_value.get("scriptproperties"_str); properties.is_some()) {
            out.scriptproperties.insert(
                ::alloc::string::String::make(rstd::cppstd::as_str(field).unwrap()),
                (*properties)->clone());
            ++n;
        }
        auto script = field_value.get("script"_str);
        if (script.is_some() && (*script)->is_string()) {
            ScriptBinding sb;
            sb.source = rstd::cppstd::to_string(*(*script)->as_str());
            if (auto properties = field_value.get("scriptproperties"_str); properties.is_some())
                sb.properties = (*properties)->clone();
            if (auto value = field_value.get("value"_str); value.is_some())
                sb.initial_value = (*value)->clone();
            if (auto user = field_value.get("user"_str); user.is_some()) {
                auto string = (*user)->as_str();
                if (string.is_some()) sb.user = rstd::cppstd::to_string(*string);
            }
            out.scripts[std::string(field)] = std::move(sb);
            ++n;
        }
    });
    return n;
}

} // namespace owe::wpscene
