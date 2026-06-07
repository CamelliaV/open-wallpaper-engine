module;

export module wescene.parse:wp_sound_object;
import rstd.cppstd;
import wavsen.audio;
import wescene.fs;

import wescene.json;
export import :wp_animation;
import :wp_scene;

export namespace owe

{

namespace wpscene
{

struct WPSoundObject {
    std::int32_t             id { 0 };
    std::string              playbackmode { "loop" };
    std::array<float, 3>     origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>     angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>     scale { 1.0f, 1.0f, 1.0f };
    float                    maxtime { 10.0f };
    float                    mintime { 0.0f };
    float                    volume { 1.0f };
    bool                     visible { true };
    std::string              name;
    std::vector<std::string> sound;

    // Common cross-kind metadata.
    bool                      locktransforms { false };
    bool                      muteineditor { false };
    bool                      nointerpolation { false };
    std::uint32_t             parent { 0 };
    std::vector<std::int32_t> dependencies;
    nlohmann::json            instance;
    WPFieldBindings           field_bindings;

    // Sound-kind specifics.
    bool        startsilent { false };    // PKGV0002+
    bool        blockalign { false };     // PKGV0018+
    bool        spatialization { false }; // PKGV0023+
    std::string queuemode;                // PKGV0020+

    // `visible:{user:"<key>",value:bool}` -> key.
    std::string visible_user_key;
    std::string volume_user_key;

    bool FromJson(const nlohmann::json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }

    bool FromJson(const nlohmann::json& json, fs::VFS&, SceneVersion /*v*/) {
        owe::GetJsonValue(json, "volume", volume);
        if (json.contains("volume") && json.at("volume").is_object()) {
            const auto& jv = json.at("volume");
            if (jv.contains("user") && jv.at("user").is_string())
                volume_user_key = jv.at("user").get<std::string>();
        }
        owe::GetJsonValue(json, "playbackmode", playbackmode);
        owe::GetJsonValue(json, "origin", origin, false);
        owe::GetJsonValue(json, "angles", angles, false);
        owe::GetJsonValue(json, "scale", scale, false);
        owe::GetJsonValue(json, "mintime", mintime, false);
        owe::GetJsonValue(json, "maxtime", maxtime, false);
        owe::GetJsonValue(json, "visible", visible, false);
        if (json.contains("visible") && json.at("visible").is_object()) {
            const auto& jv = json.at("visible");
            if (jv.contains("user") && jv.at("user").is_string())
                visible_user_key = jv.at("user").get<std::string>();
        }
        owe::GetJsonValue(json, "name", name, false);
        owe::GetJsonValue(json, "id", id, false);

        owe::GetJsonValue(json, "locktransforms", locktransforms, false);
        owe::GetJsonValue(json, "muteineditor", muteineditor, false);
        owe::GetJsonValue(json, "nointerpolation", nointerpolation, false);
        owe::GetJsonValue(json, "parent", parent, false);
        owe::GetJsonValue(json, "dependencies", dependencies, false);
        if (json.contains("instance")) instance = json.at("instance");

        owe::GetJsonValue(json, "startsilent", startsilent, false);
        owe::GetJsonValue(json, "blockalign", blockalign, false);
        owe::GetJsonValue(json, "spatialization", spatialization, false);
        owe::GetJsonValue(json, "queuemode", queuemode, false);

        if (! json.contains("sound") || ! json.at("sound").is_array()) {
            return false;
        }
        for (const auto& el : json.at("sound")) {
            std::string name;
            owe::GetJsonValue(el, name);
            if (! name.empty()) sound.push_back(name);
        }
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};
} // namespace wpscene
} // namespace owe
