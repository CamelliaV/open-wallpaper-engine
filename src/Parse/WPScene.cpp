module;

#include "WPJson.hpp"

#include <charconv>
#include <nlohmann/json.hpp>


module wescene.parse;

using namespace wallpaper::wpscene;

namespace wallpaper::wpscene {

SceneVersion ParsePkgVersionStamp(std::string_view stamp) {
    constexpr std::string_view kPrefix = "PKGV";
    if (stamp.size() < kPrefix.size() + 1) return kSceneVersionUnknown;
    if (stamp.substr(0, kPrefix.size()) != kPrefix) return kSceneVersionUnknown;
    SceneVersion     v       = 0;
    const char*      first   = stamp.data() + kPrefix.size();
    const char*      last    = stamp.data() + stamp.size();
    const auto [end, ec]     = std::from_chars(first, last, v);
    if (ec != std::errc {} || end != last) return kSceneVersionUnknown;
    return v;
}

SceneJsonVersion DetectSceneJsonVersion(const nlohmann::json& root) {
    if (root.is_object() && root.contains("version") &&
        root.at("version").is_number_unsigned()) {
        return root.at("version").template get<SceneJsonVersion>();
    }
    return kSceneJsonVersionDefault;
}

} // namespace wallpaper::wpscene

bool Orthogonalprojection::FromJson(const nlohmann::json& json) {
    if(json.is_null()) return false;
	if(json.contains("auto")) {
		GET_JSON_NAME_VALUE(json, "auto", auto_);
	}
	else {
		GET_JSON_NAME_VALUE(json, "width", width);
		GET_JSON_NAME_VALUE(json, "height", height);
	}
    return true;
}

bool WPSceneCamera::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "center", center);
    GET_JSON_NAME_VALUE(json, "eye", eye);
    GET_JSON_NAME_VALUE(json, "up", up);
    return true;
}

namespace {

// A single SceneVersion is the sole gate for "should we attempt to read
// fields introduced in PKGVxxxx". An unknown version (loose dir mount,
// dump.cpp legacy entry) falls through every gate so behaviour matches
// the pre-refactor "try everything" path.
constexpr bool wants(SceneVersion v, SceneVersion gate) {
    return v == kSceneVersionUnknown || v >= gate;
}

void parse_baseline(WPSceneGeneral& g, const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "ambientcolor", g.ambientcolor);
    GET_JSON_NAME_VALUE(json, "skylightcolor", g.skylightcolor);
    GET_JSON_NAME_VALUE(json, "clearcolor", g.clearcolor);
    GET_JSON_NAME_VALUE_NOWARN(json, "clearenabled", g.clearenabled);
    GET_JSON_NAME_VALUE_NOWARN(json, "camerafade", g.camerafade);
    GET_JSON_NAME_VALUE_NOWARN(json, "camerapreview", g.camerapreview);
    GET_JSON_NAME_VALUE(json, "cameraparallax", g.cameraparallax);
    GET_JSON_NAME_VALUE(json, "cameraparallaxamount", g.cameraparallaxamount);
    GET_JSON_NAME_VALUE(json, "cameraparallaxdelay", g.cameraparallaxdelay);
    GET_JSON_NAME_VALUE(json, "cameraparallaxmouseinfluence", g.cameraparallaxmouseinfluence);
    GET_JSON_NAME_VALUE_NOWARN(json, "zoom", g.zoom);
    GET_JSON_NAME_VALUE_NOWARN(json, "fov", g.fov);
    GET_JSON_NAME_VALUE_NOWARN(json, "nearz", g.nearz);
    GET_JSON_NAME_VALUE_NOWARN(json, "farz", g.farz);
    GET_JSON_NAME_VALUE_NOWARN(json, "bloom", g.bloom);
    GET_JSON_NAME_VALUE_NOWARN(json, "bloomstrength", g.bloomstrength);
    GET_JSON_NAME_VALUE_NOWARN(json, "bloomthreshold", g.bloomthreshold);
    GET_JSON_NAME_VALUE_NOWARN(json, "camerashake", g.camerashake);
    GET_JSON_NAME_VALUE_NOWARN(json, "camerashakeamplitude", g.camerashakeamplitude);
    GET_JSON_NAME_VALUE_NOWARN(json, "camerashakespeed", g.camerashakespeed);
    GET_JSON_NAME_VALUE_NOWARN(json, "camerashakeroughness", g.camerashakeroughness);
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
    GET_JSON_NAME_VALUE_NOWARN(json, "hdr", g.hdr);
    GET_JSON_NAME_VALUE_NOWARN(json, "norecompile", g.norecompile);
    GET_JSON_NAME_VALUE_NOWARN(json, "bloomhdrfeather", g.bloomhdrfeather);
    GET_JSON_NAME_VALUE_NOWARN(json, "bloomhdriterations", g.bloomhdriterations);
    GET_JSON_NAME_VALUE_NOWARN(json, "bloomhdrscatter", g.bloomhdrscatter);
    GET_JSON_NAME_VALUE_NOWARN(json, "bloomhdrstrength", g.bloomhdrstrength);
    GET_JSON_NAME_VALUE_NOWARN(json, "bloomhdrthreshold", g.bloomhdrthreshold);
}

void parse_v20_plus(WPSceneGeneral& g, const nlohmann::json& json) {
    GET_JSON_NAME_VALUE_NOWARN(json, "bloomtint", g.bloomtint);
}

void parse_v21_plus(WPSceneGeneral& g, const nlohmann::json& json) {
    GET_JSON_NAME_VALUE_NOWARN(json, "perspectiveoverridefov", g.perspectiveoverridefov);
    GET_JSON_NAME_VALUE_NOWARN(json, "windenabled", g.windenabled);
    GET_JSON_NAME_VALUE_NOWARN(json, "winddirection", g.winddirection);
    GET_JSON_NAME_VALUE_NOWARN(json, "windstrength", g.windstrength);
    GET_JSON_NAME_VALUE_NOWARN(json, "gravitydirection", g.gravitydirection);
    GET_JSON_NAME_VALUE_NOWARN(json, "gravitystrength", g.gravitystrength);
}

void parse_v22_plus(WPSceneGeneral& g, const nlohmann::json& json) {
    GET_JSON_NAME_VALUE_NOWARN(json, "transparentsorting", g.transparentsorting);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogdistance", g.fogdistance);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogdistancestart", g.fogdistancestart);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogdistanceend", g.fogdistanceend);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogdistancecolor", g.fogdistancecolor);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogdistancestartdensity", g.fogdistancestartdensity);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogdistanceenddensity", g.fogdistanceenddensity);
}

void parse_v23_plus(WPSceneGeneral& g, const nlohmann::json& json) {
    GET_JSON_NAME_VALUE_NOWARN(json, "fogheight", g.fogheight);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogheightstart", g.fogheightstart);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogheightend", g.fogheightend);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogheightcolor", g.fogheightcolor);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogheightstartdensity", g.fogheightstartdensity);
    GET_JSON_NAME_VALUE_NOWARN(json, "fogheightenddensity", g.fogheightenddensity);
}

void capture_raw_subtrees(WPSceneGeneral& g, const nlohmann::json& json,
                          SceneVersion v) {
    if (wants(v, 21) && json.contains("lightconfig")) {
        g.raw_lightconfig = json.at("lightconfig");
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
    capture_raw_subtrees(*this, json, v);
    return true;
}

bool WPScene::FromJson(const nlohmann::json& json) {
    return FromJson(json, kSceneVersionUnknown);
}

bool WPScene::FromJson(const nlohmann::json& json, SceneVersion v) {
    pkg_version        = v;
    scene_json_version = DetectSceneJsonVersion(json);
    if(json.contains("camera")) {
        // camera schema is identical across PKGV0001..PKGV0023; no version gate needed.
        camera.FromJson(json.at("camera"));
    } else {
        LOG_ERROR("scene no camera");
        return false;
    }
    if(json.contains("general")) {
        general.FromJson(json.at("general"), v);
    } else {
        LOG_ERROR("scene no genera data");
        return false;
    }
    return true;
}
