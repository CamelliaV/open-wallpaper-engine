module;

#include "WPJson.hpp"

#include <nlohmann/json.hpp>


#include "Utils/Logging.h"

module wescene.parse;

using namespace wallpaper::wpscene;

bool WPLightObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool WPLightObject::FromJson(const nlohmann::json& json, fs::VFS&, SceneVersion /*v*/) {
    GET_JSON_NAME_VALUE(json, "origin", origin);
    GET_JSON_NAME_VALUE(json, "angles", angles);
    GET_JSON_NAME_VALUE(json, "scale", scale);
    GET_JSON_NAME_VALUE(json, "color", color);
    GET_JSON_NAME_VALUE(json, "light", light);
    GET_JSON_NAME_VALUE(json, "radius", radius);
    GET_JSON_NAME_VALUE(json, "intensity", intensity);
    GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
    return true;
}
