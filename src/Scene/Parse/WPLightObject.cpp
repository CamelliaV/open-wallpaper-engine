module;


module wescene.parse;
import nlohmann.json;

using namespace owe::wpscene;

bool WPLightObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool WPLightObject::FromJson(const nlohmann::json& json, fs::VFS&, SceneVersion /*v*/) {
    owe::GetJsonValue(json, "origin", origin);
    owe::GetJsonValue(json, "angles", angles);
    owe::GetJsonValue(json, "scale", scale);
    owe::GetJsonValue(json, "color", color);
    owe::GetJsonValue(json, "light", light);
    owe::GetJsonValue(json, "radius", radius);
    owe::GetJsonValue(json, "intensity", intensity);
    owe::GetJsonValue(json, "visible", visible, false);
    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "id", id, false);
    owe::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);
    owe::GetJsonValue(json, "shape", shape, false);

    owe::GetJsonValue(json, "locktransforms", locktransforms, false);
    owe::GetJsonValue(json, "muteineditor", muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    owe::GetJsonValue(json, "parent", parent, false);

    owe::GetJsonValue(json, "ledsource", ledsource, false);
    owe::GetJsonValue(json, "castshadow", castshadow, false);
    owe::GetJsonValue(json, "castvolumetrics", castvolumetrics, false);
    owe::GetJsonValue(json, "outercone", outercone, false);
    owe::GetJsonValue(json, "innercone", innercone, false);
    owe::GetJsonValue(json, "attenuation", attenuation, false);
    owe::GetJsonValue(json, "exponent", exponent, false);
    owe::GetJsonValue(json, "density", density, false);
    owe::GetJsonValue(json, "volumetricsexponent", volumetricsexponent, false);
    owe::GetJsonValue(json, "lightsourcesize", lightsourcesize, false);
    owe::GetJsonValue(json, "mindistance", mindistance, false);
    owe::GetJsonValue(json, "cascadedistance0", cascadedistance0, false);
    owe::GetJsonValue(json, "cascadedistance1", cascadedistance1, false);
    owe::GetJsonValue(json, "cascadedistance2", cascadedistance2, false);
    owe::GetJsonValue(json, "dependencies", dependencies, false);
    if (json.contains("instance")) instance = json.at("instance");
    AbsorbAllFieldBindings(json, field_bindings);
    return true;
}
