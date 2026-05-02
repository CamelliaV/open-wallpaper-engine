module;

#include "WPJson.hpp"
#include <nlohmann/json.hpp>

export module wescene.parse:wp_misc_object;
import cppstd;
import wescene.fs;
import wescene.json;
export import :wp_animation;
import :wp_scene;

// Object kinds beyond image/light/particle/sound: text overlays, .mdl
// model attachments, and editor camera markers. These exist only at the
// scene.json schema level; the renderer does not yet consume them, but the
// parser absorbs every observed top-level field so the data model stays
// schema-complete (drives SceneSchema.EveryParsedObjectKeyIsObserved).

export namespace wallpaper::wpscene
{

// Text-overlay object (PKGV0005+). Discriminator: top-level `text` is
// non-null. The `text` and `font` fields appear in two shapes — plain
// string, or an object (e.g. `{"script": "..."}` for property-bound
// text). Both are captured verbatim as nlohmann::json so future consumers
// can decode either path without re-parsing.
struct WPTextObject {
    // Common positional/metadata (mirrors WPImageObject prefix).
    std::int32_t         id { 0 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    bool                 visible { true };

    bool                 locktransforms { false };
    bool                 muteineditor { false };
    bool                 nointerpolation { false };
    std::uint32_t        parent { 0 };
    std::vector<std::int32_t> dependencies;
    nlohmann::json       instance;
    WPFieldBindings      field_bindings;

    // Text-specific.
    nlohmann::json       text;        // string | {script: ...} | {value: ...}
    nlohmann::json       font;        // string | {value: ...}
    float                pointsize { 12.0f };
    std::uint32_t        padding { 0 };
    std::string          horizontalalign;
    std::string          verticalalign;
    std::string          anchor;
    std::string          alignment { "center" };

    // Text-flow controls (PKGV0018+).
    std::uint32_t        maxrows { 0 };
    float                maxwidth { 0.0f };
    bool                 limitrows { false };
    bool                 limitwidth { false };
    bool                 limituseellipsis { false };

    // Visual/material overlap with image kind.
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    float                alpha { 1.0f };
    float                brightness { 1.0f };
    int32_t              colorBlendMode { 0 };
    std::array<float, 2> size { 0.0f, 0.0f };
    bool                 perspective { false };
    bool                 copybackground { false };
    bool                 solid { false };
    bool                 opaquebackground { false };
    bool                 ledsource { false };
    std::array<float, 3> backgroundcolor { 0.0f, 0.0f, 0.0f };
    float                backgroundbrightness { 1.0f };

    bool FromJson(const nlohmann::json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }
    bool FromJson(const nlohmann::json& json, fs::VFS&, SceneVersion /*v*/) {
        GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
        GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
        GET_JSON_NAME_VALUE_NOWARN(json, "origin", origin);
        GET_JSON_NAME_VALUE_NOWARN(json, "scale", scale);
        GET_JSON_NAME_VALUE_NOWARN(json, "angles", angles);
        GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
        GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
        GET_JSON_NAME_VALUE_NOWARN(json, "locktransforms", locktransforms);
        GET_JSON_NAME_VALUE_NOWARN(json, "muteineditor", muteineditor);
        GET_JSON_NAME_VALUE_NOWARN(json, "nointerpolation", nointerpolation);
        GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);
        GET_JSON_NAME_VALUE_NOWARN(json, "dependencies", dependencies);
        if (json.contains("instance")) instance = json.at("instance");

        if (json.contains("text")) text = json.at("text");
        if (json.contains("font")) font = json.at("font");

        GET_JSON_NAME_VALUE_NOWARN(json, "pointsize", pointsize);
        GET_JSON_NAME_VALUE_NOWARN(json, "padding", padding);
        GET_JSON_NAME_VALUE_NOWARN(json, "horizontalalign", horizontalalign);
        GET_JSON_NAME_VALUE_NOWARN(json, "verticalalign", verticalalign);
        GET_JSON_NAME_VALUE_NOWARN(json, "anchor", anchor);
        GET_JSON_NAME_VALUE_NOWARN(json, "alignment", alignment);

        GET_JSON_NAME_VALUE_NOWARN(json, "maxrows", maxrows);
        GET_JSON_NAME_VALUE_NOWARN(json, "maxwidth", maxwidth);
        GET_JSON_NAME_VALUE_NOWARN(json, "limitrows", limitrows);
        GET_JSON_NAME_VALUE_NOWARN(json, "limitwidth", limitwidth);
        GET_JSON_NAME_VALUE_NOWARN(json, "limituseellipsis", limituseellipsis);

        GET_JSON_NAME_VALUE_NOWARN(json, "color", color);
        GET_JSON_NAME_VALUE_NOWARN(json, "alpha", alpha);
        GET_JSON_NAME_VALUE_NOWARN(json, "brightness", brightness);
        GET_JSON_NAME_VALUE_NOWARN(json, "colorBlendMode", colorBlendMode);
        GET_JSON_NAME_VALUE_NOWARN(json, "size", size);
        GET_JSON_NAME_VALUE_NOWARN(json, "perspective", perspective);
        GET_JSON_NAME_VALUE_NOWARN(json, "copybackground", copybackground);
        GET_JSON_NAME_VALUE_NOWARN(json, "solid", solid);
        GET_JSON_NAME_VALUE_NOWARN(json, "opaquebackground", opaquebackground);
        GET_JSON_NAME_VALUE_NOWARN(json, "ledsource", ledsource);
        GET_JSON_NAME_VALUE_NOWARN(json, "backgroundcolor", backgroundcolor);
        GET_JSON_NAME_VALUE_NOWARN(json, "backgroundbrightness", backgroundbrightness);
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};

// 3D model attachment (PKGV0001+). Discriminator: top-level `model` is a
// non-null string. WE links to a `.mdl` file under /assets and optionally
// names a sub-attachment to overlay.
struct WPModelObject {
    std::int32_t         id { 0 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    bool                 visible { true };

    bool                 locktransforms { false };
    bool                 muteineditor { false };
    bool                 nointerpolation { false };
    std::uint32_t        parent { 0 };
    std::vector<std::int32_t> dependencies;
    nlohmann::json       instance;
    WPFieldBindings      field_bindings;

    std::string          model;
    std::string          attachment;
    bool                 perspective { false };

    bool FromJson(const nlohmann::json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }
    bool FromJson(const nlohmann::json& json, fs::VFS&, SceneVersion /*v*/) {
        GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
        GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
        GET_JSON_NAME_VALUE_NOWARN(json, "origin", origin);
        GET_JSON_NAME_VALUE_NOWARN(json, "scale", scale);
        GET_JSON_NAME_VALUE_NOWARN(json, "angles", angles);
        GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
        GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
        GET_JSON_NAME_VALUE_NOWARN(json, "locktransforms", locktransforms);
        GET_JSON_NAME_VALUE_NOWARN(json, "muteineditor", muteineditor);
        GET_JSON_NAME_VALUE_NOWARN(json, "nointerpolation", nointerpolation);
        GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);
        GET_JSON_NAME_VALUE_NOWARN(json, "dependencies", dependencies);
        if (json.contains("instance")) instance = json.at("instance");

        GET_JSON_NAME_VALUE_NOWARN(json, "model", model);
        GET_JSON_NAME_VALUE_NOWARN(json, "attachment", attachment);
        GET_JSON_NAME_VALUE_NOWARN(json, "perspective", perspective);
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};

// Editor camera marker (PKGV0020+). Discriminator: top-level `camera` is
// a non-null string. Carries camera animation paths and per-camera
// projection overrides.
struct WPCameraObject {
    std::int32_t         id { 0 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    bool                 visible { true };

    bool                 locktransforms { false };
    bool                 muteineditor { false };
    bool                 nointerpolation { false };
    std::uint32_t        parent { 0 };
    std::vector<std::int32_t> dependencies;
    nlohmann::json       instance;
    WPFieldBindings      field_bindings;

    std::string          camera;        // camera name reference
    std::string          path;          // animation path .json
    std::string          queuemode;
    float                fov { 50.0f };
    float                zoom { 1.0f };
    bool                 solid { false };
    bool                 disablepropagation { false };

    bool FromJson(const nlohmann::json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }
    bool FromJson(const nlohmann::json& json, fs::VFS&, SceneVersion /*v*/) {
        GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
        GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
        GET_JSON_NAME_VALUE_NOWARN(json, "origin", origin);
        GET_JSON_NAME_VALUE_NOWARN(json, "scale", scale);
        GET_JSON_NAME_VALUE_NOWARN(json, "angles", angles);
        GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
        GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
        GET_JSON_NAME_VALUE_NOWARN(json, "locktransforms", locktransforms);
        GET_JSON_NAME_VALUE_NOWARN(json, "muteineditor", muteineditor);
        GET_JSON_NAME_VALUE_NOWARN(json, "nointerpolation", nointerpolation);
        GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent);
        GET_JSON_NAME_VALUE_NOWARN(json, "dependencies", dependencies);
        if (json.contains("instance")) instance = json.at("instance");

        GET_JSON_NAME_VALUE_NOWARN(json, "camera", camera);
        GET_JSON_NAME_VALUE_NOWARN(json, "path", path);
        GET_JSON_NAME_VALUE_NOWARN(json, "queuemode", queuemode);
        GET_JSON_NAME_VALUE_NOWARN(json, "fov", fov);
        GET_JSON_NAME_VALUE_NOWARN(json, "zoom", zoom);
        GET_JSON_NAME_VALUE_NOWARN(json, "solid", solid);
        GET_JSON_NAME_VALUE_NOWARN(json, "disablepropagation", disablepropagation);
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};

} // namespace wallpaper::wpscene
