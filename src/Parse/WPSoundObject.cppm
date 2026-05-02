module;

#include "WPJson.hpp"
#include <nlohmann/json.hpp>

export module wescene.parse:wp_sound_object;
import cppstd;
import wescene.audio;
import wescene.fs;

import wescene.json;
import :wp_scene;

export namespace wallpaper

{

namespace wpscene
{

struct WPSoundObject {
    std::int32_t             id { 0 };
    std::string              playbackmode { "loop" };
    float                    maxtime { 10.0f };
    float                    mintime { 0.0f };
    float                    volume { 1.0f };
    bool                     visible { true };
    std::string              name;
    std::vector<std::string> sound;

    // Common cross-kind metadata.
    bool                     locktransforms { false };
    bool                     muteineditor { false };
    bool                     nointerpolation { false };
    std::uint32_t            parent { 0 };

    // Sound-kind specifics.
    bool                     startsilent { false };       // PKGV0002+
    bool                     blockalign { false };        // PKGV0018+
    bool                     spatialization { false };    // PKGV0023+
    std::string              queuemode;                   // PKGV0020+

    bool FromJson(const nlohmann::json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }

    bool FromJson(const nlohmann::json& json, fs::VFS&, SceneVersion /*v*/) {
        GET_JSON_NAME_VALUE(json, "volume", volume);
        GET_JSON_NAME_VALUE(json, "playbackmode", playbackmode);
        GET_JSON_NAME_VALUE_NOWARN(json, "mintime", mintime);
        GET_JSON_NAME_VALUE_NOWARN(json, "maxtime", maxtime);
        GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
        GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
        GET_JSON_NAME_VALUE_NOWARN(json, "id", id);

        GET_JSON_NAME_VALUE_NOWARN(json, "locktransforms", locktransforms);
        GET_JSON_NAME_VALUE_NOWARN(json, "muteineditor", muteineditor);
        GET_JSON_NAME_VALUE_NOWARN(json, "nointerpolation", nointerpolation);
        GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);

        GET_JSON_NAME_VALUE_NOWARN(json, "startsilent", startsilent);
        GET_JSON_NAME_VALUE_NOWARN(json, "blockalign", blockalign);
        GET_JSON_NAME_VALUE_NOWARN(json, "spatialization", spatialization);
        GET_JSON_NAME_VALUE_NOWARN(json, "queuemode", queuemode);

        if (! json.contains("sound") || ! json.at("sound").is_array()) {
            return false;
        }
        for (const auto& el : json.at("sound")) {
            std::string name;
            GET_JSON_VALUE(el, name);
            if (! name.empty()) sound.push_back(name);
        }
        return true;
    }
};
} // namespace wpscene
} // namespace wallpaper
