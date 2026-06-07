module;

#include <rstd/macro.hpp>

module wescene.parse;
import nlohmann.json;
import rstd.log;
import rstd.cppstd;

using namespace owe::wpscene;

namespace owe::wpscene
{

SceneVersion ParsePkgVersionStamp(std::string_view stamp) {
    constexpr std::string_view kPrefix = "PKGV";
    if (stamp.size() < kPrefix.size() + 1) return kSceneVersionUnknown;
    if (stamp.substr(0, kPrefix.size()) != kPrefix) return kSceneVersionUnknown;
    SceneVersion v       = 0;
    const char*  first   = stamp.data() + kPrefix.size();
    const char*  last    = stamp.data() + stamp.size();
    const auto [end, ec] = std::from_chars(first, last, v);
    if (ec != std::errc {} || end != last) return kSceneVersionUnknown;
    return v;
}

SceneJsonVersion DetectSceneJsonVersion(const nlohmann::json& root) {
    if (root.is_object() && root.contains("version") && root.at("version").is_number_unsigned()) {
        return root.at("version").template get<SceneJsonVersion>();
    }
    return kSceneJsonVersionDefault;
}

} // namespace owe::wpscene

bool Orthogonalprojection::FromJson(const nlohmann::json& json) {
    if (json.is_null()) return false;
    if (json.contains("auto")) {
        owe::GetJsonValue(json, "auto", auto_);
    } else {
        owe::GetJsonValue(json, "width", width);
        owe::GetJsonValue(json, "height", height);
    }
    return true;
}

bool WPSceneCamera::FromJson(const nlohmann::json& json) {
    owe::GetJsonValue(json, "center", center);
    owe::GetJsonValue(json, "eye", eye);
    owe::GetJsonValue(json, "up", up);
    return true;
}

bool WPSceneLightConfig::FromJson(const nlohmann::json& json) {
    owe::GetJsonValue(json, "directional", directional, false);
    owe::GetJsonValue(json, "directionalshadow", directionalshadow, false);
    owe::GetJsonValue(json, "point", point, false);
    owe::GetJsonValue(json, "pointshadow", pointshadow, false);
    owe::GetJsonValue(json, "spot", spot, false);
    owe::GetJsonValue(json, "spotshadow", spotshadow, false);
    return true;
}

namespace
{

// A single SceneVersion is the sole gate for "should we attempt to read
// fields introduced in PKGVxxxx". An unknown version (loose dir mount,
// dump.cpp legacy entry) falls through every gate so behaviour matches
// the pre-refactor "try everything" path.
constexpr bool wants(SceneVersion v, SceneVersion gate) {
    return v == kSceneVersionUnknown || v >= gate;
}

void parse_baseline(WPSceneGeneral& g, const nlohmann::json& json) {
    owe::GetJsonValue(json, "ambientcolor", g.ambientcolor);
    owe::GetJsonValue(json, "skylightcolor", g.skylightcolor);
    owe::GetJsonValue(json, "clearcolor", g.clearcolor);
    owe::GetJsonValue(json, "clearenabled", g.clearenabled, false);
    owe::GetJsonValue(json, "camerafade", g.camerafade, false);
    owe::GetJsonValue(json, "camerapreview", g.camerapreview, false);
    owe::GetJsonValue(json, "cameraparallax", g.cameraparallax);
    owe::GetJsonValue(json, "cameraparallaxamount", g.cameraparallaxamount);
    owe::GetJsonValue(json, "cameraparallaxdelay", g.cameraparallaxdelay);
    owe::GetJsonValue(json, "cameraparallaxmouseinfluence", g.cameraparallaxmouseinfluence);
    owe::GetJsonValue(json, "zoom", g.zoom, false);
    owe::GetJsonValue(json, "fov", g.fov, false);
    owe::GetJsonValue(json, "nearz", g.nearz, false);
    owe::GetJsonValue(json, "farz", g.farz, false);
    owe::GetJsonValue(json, "bloom", g.bloom, false);
    owe::GetJsonValue(json, "bloomstrength", g.bloomstrength, false);
    owe::GetJsonValue(json, "bloomthreshold", g.bloomthreshold, false);
    owe::GetJsonValue(json, "camerashake", g.camerashake, false);
    owe::GetJsonValue(json, "camerashakeamplitude", g.camerashakeamplitude, false);
    owe::GetJsonValue(json, "camerashakespeed", g.camerashakespeed, false);
    owe::GetJsonValue(json, "camerashakeroughness", g.camerashakeroughness, false);
    if (json.contains("orthogonalprojection")) {
        const auto& ortho = json.at("orthogonalprojection");
        if (ortho.is_null())
            g.isOrtho = false;
        else {
            g.isOrtho = true;
            g.orthogonalprojection.FromJson(ortho);
        }
    }
}

void parse_v10_plus(WPSceneGeneral& g, const nlohmann::json& json) {
    owe::GetJsonValue(json, "hdr", g.hdr, false);
    owe::GetJsonValue(json, "norecompile", g.norecompile, false);
    owe::GetJsonValue(json, "bloomhdrfeather", g.bloomhdrfeather, false);
    owe::GetJsonValue(json, "bloomhdriterations", g.bloomhdriterations, false);
    owe::GetJsonValue(json, "bloomhdrscatter", g.bloomhdrscatter, false);
    owe::GetJsonValue(json, "bloomhdrstrength", g.bloomhdrstrength, false);
    owe::GetJsonValue(json, "bloomhdrthreshold", g.bloomhdrthreshold, false);
}

void parse_v20_plus(WPSceneGeneral& g, const nlohmann::json& json) {
    owe::GetJsonValue(json, "bloomtint", g.bloomtint, false);
}

void parse_v21_plus(WPSceneGeneral& g, const nlohmann::json& json) {
    owe::GetJsonValue(json, "perspectiveoverridefov", g.perspectiveoverridefov, false);
    owe::GetJsonValue(json, "windenabled", g.windenabled, false);
    owe::GetJsonValue(json, "winddirection", g.winddirection, false);
    owe::GetJsonValue(json, "windstrength", g.windstrength, false);
    owe::GetJsonValue(json, "gravitydirection", g.gravitydirection, false);
    owe::GetJsonValue(json, "gravitystrength", g.gravitystrength, false);
}

void parse_v22_plus(WPSceneGeneral& g, const nlohmann::json& json) {
    owe::GetJsonValue(json, "transparentsorting", g.transparentsorting, false);
    owe::GetJsonValue(json, "fogdistance", g.fogdistance, false);
    owe::GetJsonValue(json, "fogdistancestart", g.fogdistancestart, false);
    owe::GetJsonValue(json, "fogdistanceend", g.fogdistanceend, false);
    owe::GetJsonValue(json, "fogdistancecolor", g.fogdistancecolor, false);
    owe::GetJsonValue(json, "fogdistancestartdensity", g.fogdistancestartdensity, false);
    owe::GetJsonValue(json, "fogdistanceenddensity", g.fogdistanceenddensity, false);
}

void parse_v23_plus(WPSceneGeneral& g, const nlohmann::json& json) {
    owe::GetJsonValue(json, "fogheight", g.fogheight, false);
    owe::GetJsonValue(json, "fogheightstart", g.fogheightstart, false);
    owe::GetJsonValue(json, "fogheightend", g.fogheightend, false);
    owe::GetJsonValue(json, "fogheightcolor", g.fogheightcolor, false);
    owe::GetJsonValue(json, "fogheightstartdensity", g.fogheightstartdensity, false);
    owe::GetJsonValue(json, "fogheightenddensity", g.fogheightenddensity, false);
}

void parse_lightconfig(WPSceneGeneral& g, const nlohmann::json& json) {
    if (json.contains("lightconfig") && json.at("lightconfig").is_object()) {
        g.lightconfig.FromJson(json.at("lightconfig"));
    }
}

} // namespace

bool WPSceneGeneral::FromJson(const nlohmann::json& json) {
    return FromJson(json, kSceneVersionUnknown);
}

bool WPSceneGeneral::FromJson(const nlohmann::json& json, SceneVersion v) {
    parse_baseline(*this, json);
    if (wants(v, 10)) parse_v10_plus(*this, json);
    if (wants(v, 20)) parse_v20_plus(*this, json);
    if (wants(v, 21)) parse_v21_plus(*this, json);
    if (wants(v, 22)) parse_v22_plus(*this, json);
    if (wants(v, 23)) parse_v23_plus(*this, json);
    if (wants(v, 21)) parse_lightconfig(*this, json);
    return true;
}

bool WPScene::FromJson(const nlohmann::json& json) { return FromJson(json, kSceneVersionUnknown); }

bool WPScene::FromJson(const nlohmann::json& json, SceneVersion v) {
    pkg_version        = v;
    scene_json_version = DetectSceneJsonVersion(json);
    if (json.contains("camera")) {
        // camera schema is identical across PKGV0001..PKGV0023; no version gate needed.
        camera.FromJson(json.at("camera"));
    } else {
        rstd_error("scene no camera");
        return false;
    }
    if (json.contains("general")) {
        general.FromJson(json.at("general"), v);
    } else {
        rstd_error("scene no genera data");
        return false;
    }
    return true;
}
