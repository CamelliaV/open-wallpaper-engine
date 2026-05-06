module;

#include "WPJson.hpp"

#include <nlohmann/json.hpp>
module wescene.parse;

using namespace owe::wpscene;

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
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
    GET_JSON_NAME_VALUE_NOWARN(json, "shape", shape);

    GET_JSON_NAME_VALUE_NOWARN(json, "locktransforms", locktransforms);
    GET_JSON_NAME_VALUE_NOWARN(json, "muteineditor", muteineditor);
    GET_JSON_NAME_VALUE_NOWARN(json, "nointerpolation", nointerpolation);
    GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);

    GET_JSON_NAME_VALUE_NOWARN(json, "ledsource", ledsource);
    GET_JSON_NAME_VALUE_NOWARN(json, "castshadow", castshadow);
    GET_JSON_NAME_VALUE_NOWARN(json, "castvolumetrics", castvolumetrics);
    GET_JSON_NAME_VALUE_NOWARN(json, "outercone", outercone);
    GET_JSON_NAME_VALUE_NOWARN(json, "innercone", innercone);
    GET_JSON_NAME_VALUE_NOWARN(json, "attenuation", attenuation);
    GET_JSON_NAME_VALUE_NOWARN(json, "exponent", exponent);
    GET_JSON_NAME_VALUE_NOWARN(json, "density", density);
    GET_JSON_NAME_VALUE_NOWARN(json, "volumetricsexponent", volumetricsexponent);
    GET_JSON_NAME_VALUE_NOWARN(json, "lightsourcesize", lightsourcesize);
    GET_JSON_NAME_VALUE_NOWARN(json, "mindistance", mindistance);
    GET_JSON_NAME_VALUE_NOWARN(json, "cascadedistance0", cascadedistance0);
    GET_JSON_NAME_VALUE_NOWARN(json, "cascadedistance1", cascadedistance1);
    GET_JSON_NAME_VALUE_NOWARN(json, "cascadedistance2", cascadedistance2);
    GET_JSON_NAME_VALUE_NOWARN(json, "dependencies", dependencies);
    if (json.contains("instance")) instance = json.at("instance");
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
