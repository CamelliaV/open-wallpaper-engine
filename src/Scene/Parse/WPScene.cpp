module;

#include <rstd/macro.hpp>

module wescene.parse;
import nlohmann.json;
import rstd.log;
import rstd.cppstd;
import wescene.pkg_fs;

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

WPSceneObjectKind object_kind(const nlohmann::json& obj) {
    if (! obj.is_object()) return WPSceneObjectKind::Unknown;
    if (obj.contains("image") && ! obj.at("image").is_null()) return WPSceneObjectKind::Image;
    if (obj.contains("particle") && ! obj.at("particle").is_null())
        return WPSceneObjectKind::Particle;
    if (obj.contains("sound") && ! obj.at("sound").is_null()) return WPSceneObjectKind::Sound;
    if (obj.contains("light") && ! obj.at("light").is_null()) return WPSceneObjectKind::Light;
    if (obj.contains("text") && ! obj.at("text").is_null()) return WPSceneObjectKind::Text;
    if (obj.contains("model") && ! obj.at("model").is_null()) return WPSceneObjectKind::Model;
    if (obj.contains("camera") && ! obj.at("camera").is_null()) return WPSceneObjectKind::Camera;
    return WPSceneObjectKind::Container;
}

WPSceneObjectMetadata parse_object_metadata(const nlohmann::json& obj, std::size_t raw_index) {
    WPSceneObjectMetadata metadata;
    metadata.raw_index = raw_index;
    metadata.kind      = object_kind(obj);
    if (! obj.is_object()) return metadata;

    owe::GetJsonValue(obj, "id", metadata.id, false);
    owe::GetJsonValue(obj, "name", metadata.name, false);
    owe::GetJsonValue(obj, "visible", metadata.visible, false);
    owe::GetJsonValue(obj, "parent", metadata.parent, false);

    std::array<float, 2> size {};
    if (owe::GetJsonValue(obj, "size", size, false) && size[0] > 0.0f && size[1] > 0.0f) {
        metadata.size = size;
    }
    return metadata;
}

std::vector<WPSceneObjectMetadata> parse_objects_metadata(const nlohmann::json& root) {
    std::vector<WPSceneObjectMetadata> objects;
    if (! root.contains("objects") || ! root.at("objects").is_array()) return objects;

    const auto& raw_objects = root.at("objects");
    objects.reserve(raw_objects.size());
    for (std::size_t i = 0; i < raw_objects.size(); ++i) {
        objects.push_back(parse_object_metadata(raw_objects.at(i), i));
    }
    return objects;
}

std::optional<std::array<uint32_t, 2>> image_extent(const WPSceneObjectMetadata& obj) {
    if (obj.kind != WPSceneObjectKind::Image || ! obj.size) return std::nullopt;
    return std::array<uint32_t, 2> { static_cast<uint32_t>((*obj.size)[0]),
                                     static_cast<uint32_t>((*obj.size)[1]) };
}

std::optional<std::array<uint32_t, 2>>
largest_image_extent(std::span<const WPSceneObjectMetadata> objects) {
    std::optional<std::array<uint32_t, 2>> best;
    uint64_t                               best_area = 0;
    for (const auto& obj : objects) {
        auto extent = image_extent(obj);
        if (! extent) continue;
        const uint64_t area =
            static_cast<uint64_t>((*extent)[0]) * static_cast<uint64_t>((*extent)[1]);
        if (area > best_area) {
            best      = *extent;
            best_area = area;
        }
    }
    return best;
}

std::optional<std::array<uint32_t, 2>>
scene_canvas_extent(const WPSceneMetadata&                 metadata,
                    std::span<const WPSceneObjectMetadata> objects_metadata) {
    const auto& general = metadata.general;
    if (! general.isOrtho) return std::nullopt;

    const auto& ortho = general.orthogonalprojection;
    if (! ortho.auto_) {
        if (ortho.width <= 0 || ortho.height <= 0) return std::nullopt;
        return std::array<uint32_t, 2> { static_cast<uint32_t>(ortho.width),
                                         static_cast<uint32_t>(ortho.height) };
    }
    return largest_image_extent(objects_metadata);
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

bool WPSceneMetadata::FromJson(const nlohmann::json& json) {
    return FromJson(json, kSceneVersionUnknown);
}

bool WPSceneMetadata::FromJson(const nlohmann::json& json, SceneVersion v) {
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

namespace owe::wpscene
{

std::optional<WPSceneDocument> ParseSceneDocumentJson(std::string_view buf,
                                                      SceneVersion     pkg_version) {
    WPSceneDocument doc;
    if (! owe::ParseJson(buf, doc.root_json)) return std::nullopt;
    if (! doc.metadata.FromJson(doc.root_json, pkg_version)) return std::nullopt;
    doc.objects_metadata       = parse_objects_metadata(doc.root_json);
    doc.metadata.canvas_extent = scene_canvas_extent(doc.metadata, doc.objects_metadata);
    return doc;
}

std::optional<WPSceneDocument> LoadSceneDocumentFromVfs(fs::VFS& vfs, std::string_view scene_path,
                                                        SceneVersion pkg_version) {
    auto f = vfs.Open(scene_path);
    if (! f) return std::nullopt;
    return ParseSceneDocumentJson(f->ReadAllStr(), pkg_version);
}

std::optional<WPSceneDocument> LoadSceneDocumentFromPkg(std::string_view pkg_path) {
    if (pkg_path.empty()) return std::nullopt;
    auto pkg = fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! pkg) return std::nullopt;

    auto scene_file = pkg->Open("/scene.json");
    if (! scene_file) return std::nullopt;

    const auto pkg_version = ParsePkgVersionStamp(pkg->pkg_version_stamp());
    return ParseSceneDocumentJson(scene_file->ReadAllStr(), pkg_version);
}

} // namespace owe::wpscene
