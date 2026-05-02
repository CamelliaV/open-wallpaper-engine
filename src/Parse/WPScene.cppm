module;

#include "WPJson.hpp"
#include <nlohmann/json.hpp>

export module wescene.parse:wp_scene;
import cppstd;
import wescene.fs;

export namespace wallpaper

{

namespace wpscene
{

// pkg container version (the "PKGV00xx" stamp at the head of scene.pkg).
// Spans 1..23 in the live corpus. All scene.json schema evolution is gated
// on this axis (lightconfig/fog/hdr added in v23, etc.).
using SceneVersion = std::uint16_t;

// scene.json self-reported revision (top-level "version" int). Independent
// of SceneVersion: a single PKGV0023 pkg can contain scene.json with
// version 0/1/3/4/5. Captured for diagnostics, not used for dispatch.
using SceneJsonVersion = std::uint16_t;

constexpr SceneVersion     kSceneVersionUnknown     = 0;
constexpr SceneJsonVersion kSceneJsonVersionDefault = 0;

// Parse "PKGV0023" → 23. Returns kSceneVersionUnknown on any other shape.
SceneVersion ParsePkgVersionStamp(std::string_view stamp);

// Read top-level "version" number_unsigned; returns kSceneJsonVersionDefault
// when absent or wrong type.
SceneJsonVersion DetectSceneJsonVersion(const nlohmann::json& root);

class Orthogonalprojection {
public:
    bool    FromJson(const nlohmann::json&);
    int32_t width;
    int32_t height;
    bool    auto_ { false };
};

class WPSceneCamera {
public:
    bool                 FromJson(const nlohmann::json&);
    std::array<float, 3> center { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> eye { 0.0f, 0.0f, 1.0f };
    std::array<float, 3> up { 0.0f, 1.0f, 0.0f };
};

class WPSceneGeneral {
public:
    bool                 FromJson(const nlohmann::json&);
    std::array<float, 3> clearcolor { 0.0f, 0.0f, 0.0f };
    bool                 cameraparallax { false };
    float                cameraparallaxamount;
    float                cameraparallaxdelay;
    float                cameraparallaxmouseinfluence;
    bool                 isOrtho { true };
    Orthogonalprojection orthogonalprojection { 1920, 1080 };
    float                zoom { 1.0f };
    float                fov { 50.0f };
    float                nearz { 0.01f };
    float                farz { 10000.0f };
    std::array<float, 3> ambientcolor { 0.2f, 0.2f, 0.2f };
    std::array<float, 3> skylightcolor { 0.3f, 0.3f, 0.3f };
};

class WPScene {
public:
    bool             FromJson(const nlohmann::json&);                  // legacy: defaults to unknown version
    bool             FromJson(const nlohmann::json&, SceneVersion);    // canonical entry
    SceneVersion     pkg_version { kSceneVersionUnknown };
    SceneJsonVersion scene_json_version { kSceneJsonVersionDefault };
    WPSceneCamera    camera;
    WPSceneGeneral   general;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Orthogonalprojection, width, height);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPSceneCamera, center, eye, up);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPSceneGeneral, clearcolor, orthogonalprojection, zoom);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPScene, camera, general);
} // namespace wpscene
} // namespace wallpaper
