module;

#include <nlohmann/json.hpp>

export module wescene.scene:visibility;
import rstd.cppstd;

export namespace owe
{

struct SceneUserVisibilityBinding {
    std::string    key;
    nlohmann::json condition;
    bool           has_condition { false };

    bool empty() const { return key.empty(); }
};

inline const nlohmann::json& SceneUserPropertyPayload(const nlohmann::json& property) {
    if (property.is_object()) {
        auto it = property.find("value");
        if (it != property.end()) return *it;
    }
    return property;
}

inline std::optional<std::string> SceneJsonScalarString(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    if (value.is_number_unsigned()) return std::to_string(value.get<std::uint64_t>());
    if (value.is_number_float()) {
        std::ostringstream os;
        os << value.get<double>();
        return os.str();
    }
    return std::nullopt;
}

inline bool SceneJsonScalarEquals(const nlohmann::json& a, const nlohmann::json& b) {
    if (a == b) return true;
    auto as = SceneJsonScalarString(a);
    auto bs = SceneJsonScalarString(b);
    if (! as || ! bs) return false;
    if (*as == *bs) return true;
    if (a.is_boolean() && b.is_string()) {
        const auto s = b.get<std::string>();
        return (a.get<bool>() && s == "1") || (! a.get<bool>() && s == "0");
    }
    if (a.is_string() && b.is_boolean()) {
        const auto s = a.get<std::string>();
        return (b.get<bool>() && s == "1") || (! b.get<bool>() && s == "0");
    }
    return false;
}

inline std::optional<bool>
ResolveSceneUserVisibilityBinding(const SceneUserVisibilityBinding& binding,
                                  const nlohmann::json&             property) {
    if (binding.empty()) return std::nullopt;
    const auto& value = SceneUserPropertyPayload(property);
    if (binding.has_condition) return SceneJsonScalarEquals(value, binding.condition);
    if (value.is_boolean()) return value.get<bool>();
    return std::nullopt;
}

inline std::optional<bool>
ResolveSceneUserVisibilityBinding(const SceneUserVisibilityBinding& binding, std::string_view key,
                                  const nlohmann::json& property) {
    if (binding.key != key) return std::nullopt;
    return ResolveSceneUserVisibilityBinding(binding, property);
}

} // namespace owe
