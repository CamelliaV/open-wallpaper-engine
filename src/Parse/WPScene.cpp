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

bool WPSceneGeneral::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "ambientcolor", ambientcolor);
    GET_JSON_NAME_VALUE(json, "skylightcolor", skylightcolor);
	GET_JSON_NAME_VALUE(json, "clearcolor", clearcolor);
	GET_JSON_NAME_VALUE(json, "cameraparallax", cameraparallax);
	GET_JSON_NAME_VALUE(json, "cameraparallaxamount", cameraparallaxamount);
	GET_JSON_NAME_VALUE(json, "cameraparallaxdelay", cameraparallaxdelay);
	GET_JSON_NAME_VALUE(json, "cameraparallaxmouseinfluence", cameraparallaxmouseinfluence);
	GET_JSON_NAME_VALUE_NOWARN(json, "zoom", zoom);
	GET_JSON_NAME_VALUE_NOWARN(json, "fov", fov);
	GET_JSON_NAME_VALUE_NOWARN(json, "nearz", nearz);
	GET_JSON_NAME_VALUE_NOWARN(json, "farz", farz);
    if(json.contains("orthogonalprojection")) {
        const auto& ortho = json.at("orthogonalprojection");
        if(ortho.is_null())
            isOrtho = false;
        else {
            isOrtho = true;
            orthogonalprojection.FromJson(ortho);
        }
    }
    return true;
}

bool WPScene::FromJson(const nlohmann::json& json) {
    return FromJson(json, kSceneVersionUnknown);
}

bool WPScene::FromJson(const nlohmann::json& json, SceneVersion v) {
    pkg_version        = v;
    scene_json_version = DetectSceneJsonVersion(json);
    if(json.contains("camera")) {
        camera.FromJson(json.at("camera"));
    } else {
        LOG_ERROR("scene no camera");
        return false;
    }
    if(json.contains("general")) {
        general.FromJson(json.at("general"));
    } else {
        LOG_ERROR("scene no genera data");
        return false;
    }
    return true;
}
