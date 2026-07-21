module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import rstd.log;
import rstd.cppstd;
import wescene.json;
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

SceneJsonVersion DetectSceneJsonVersion(const owe::Json& root) {
    auto version = root.get("version");
    if (version.is_none()) return kSceneJsonVersionDefault;
    auto value = (*version)->as_u64();
    if (value.is_some() && value->to_primitive() <= std::numeric_limits<SceneJsonVersion>::max())
        return static_cast<SceneJsonVersion>(value->to_primitive());
    return kSceneJsonVersionDefault;
}

} // namespace owe::wpscene

bool Orthogonalprojection::FromJson(const owe::Json& json) {
    if (json.is_null()) return false;
    if (json.get("auto").is_some()) {
        owe::GetJsonValue(json, "auto", auto_);
    } else {
        owe::GetJsonValue(json, "width", width);
        owe::GetJsonValue(json, "height", height);
    }
    return true;
}

bool SceneCamera::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "center", center);
    owe::GetJsonValue(json, "eye", eye);
    owe::GetJsonValue(json, "up", up);
    if (auto raw_paths = json.get("paths"); raw_paths.is_some()) {
        auto array = (*raw_paths)->as_array();
        if (array.is_none()) return true;
        for (const auto& path : **array) {
            auto value = path.as_str();
            if (value.is_some()) paths.push_back(rstd::cppstd::to_string(*value));
        }
    }
    return true;
}

bool SceneLightConfig::FromJson(const owe::Json& json) {
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

void capture_user_bindings(SceneGeneral& g, const owe::Json& json) {
    auto object = json.as_object();
    if (object.is_none()) return;
    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto& field             = *entry_value;
        if (! field.is_object()) return;
        auto user = field.get("user");
        if (user.is_none() || ! (*user)->is_string()) return;
        g.user_bindings[rstd::cppstd::to_string(entry_key->as_str())] =
            rstd::cppstd::to_string(*(*user)->as_str());
    });
}

void parse_baseline(SceneGeneral& g, const owe::Json& json) {
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
    g.isOrtho = false;
    if (auto ortho = json.get("orthogonalprojection"); ortho.is_some()) {
        if ((*ortho)->is_null())
            g.isOrtho = false;
        else {
            g.isOrtho = true;
            g.orthogonalprojection.FromJson(**ortho);
        }
    }
}

void parse_v10_plus(SceneGeneral& g, const owe::Json& json) {
    owe::GetJsonValue(json, "hdr", g.hdr, false);
    owe::GetJsonValue(json, "norecompile", g.norecompile, false);
    owe::GetJsonValue(json, "bloomhdrfeather", g.bloomhdrfeather, false);
    owe::GetJsonValue(json, "bloomhdriterations", g.bloomhdriterations, false);
    owe::GetJsonValue(json, "bloomhdrscatter", g.bloomhdrscatter, false);
    owe::GetJsonValue(json, "bloomhdrstrength", g.bloomhdrstrength, false);
    owe::GetJsonValue(json, "bloomhdrthreshold", g.bloomhdrthreshold, false);
}

void parse_v20_plus(SceneGeneral& g, const owe::Json& json) {
    owe::GetJsonValue(json, "bloomtint", g.bloomtint, false);
}

void parse_v21_plus(SceneGeneral& g, const owe::Json& json) {
    owe::GetJsonValue(json, "perspectiveoverridefov", g.perspectiveoverridefov, false);
    owe::GetJsonValue(json, "windenabled", g.windenabled, false);
    owe::GetJsonValue(json, "winddirection", g.winddirection, false);
    owe::GetJsonValue(json, "windstrength", g.windstrength, false);
    owe::GetJsonValue(json, "gravitydirection", g.gravitydirection, false);
    owe::GetJsonValue(json, "gravitystrength", g.gravitystrength, false);
}

void parse_v22_plus(SceneGeneral& g, const owe::Json& json) {
    owe::GetJsonValue(json, "transparentsorting", g.transparentsorting, false);
    owe::GetJsonValue(json, "fogdistance", g.fogdistance, false);
    owe::GetJsonValue(json, "fogdistancestart", g.fogdistancestart, false);
    owe::GetJsonValue(json, "fogdistanceend", g.fogdistanceend, false);
    owe::GetJsonValue(json, "fogdistancecolor", g.fogdistancecolor, false);
    owe::GetJsonValue(json, "fogdistancestartdensity", g.fogdistancestartdensity, false);
    owe::GetJsonValue(json, "fogdistanceenddensity", g.fogdistanceenddensity, false);
}

void parse_v23_plus(SceneGeneral& g, const owe::Json& json) {
    owe::GetJsonValue(json, "fogheight", g.fogheight, false);
    owe::GetJsonValue(json, "fogheightstart", g.fogheightstart, false);
    owe::GetJsonValue(json, "fogheightend", g.fogheightend, false);
    owe::GetJsonValue(json, "fogheightcolor", g.fogheightcolor, false);
    owe::GetJsonValue(json, "fogheightstartdensity", g.fogheightstartdensity, false);
    owe::GetJsonValue(json, "fogheightenddensity", g.fogheightenddensity, false);
}

void parse_lightconfig(SceneGeneral& g, const owe::Json& json) {
    if (auto lightconfig = json.get("lightconfig");
        lightconfig.is_some() && (*lightconfig)->is_object()) {
        g.lightconfig.FromJson(**lightconfig);
    }
}

SceneObjectKind object_kind(const owe::Json& obj) {
    if (! obj.is_object()) return SceneObjectKind::Unknown;
    if (auto value = obj.get("image"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Image;
    if (auto value = obj.get("particle"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Particle;
    if (auto value = obj.get("sound"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Sound;
    if (auto value = obj.get("light"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Light;
    if (auto value = obj.get("text"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Text;
    if (auto value = obj.get("model"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Model;
    if (auto value = obj.get("camera"); value.is_some() && ! (*value)->is_null())
        return SceneObjectKind::Camera;
    return SceneObjectKind::Container;
}

SceneObjectMetadata parse_object_metadata(const owe::Json& obj, std::size_t raw_index) {
    SceneObjectMetadata metadata;
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

std::vector<SceneObjectMetadata> parse_objects_metadata(const owe::Json& root) {
    std::vector<SceneObjectMetadata> objects;
    auto                             raw_objects = root.get("objects");
    if (raw_objects.is_none()) return objects;

    auto array = (*raw_objects)->as_array();
    if (array.is_none()) return objects;
    const auto count = (*array)->len().to_primitive();
    objects.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        objects.push_back(parse_object_metadata((**array)[rstd::usize(i)], i));
    }
    return objects;
}

std::optional<std::array<uint32_t, 2>> image_extent(const SceneObjectMetadata& obj) {
    if (obj.kind != SceneObjectKind::Image || ! obj.size) return std::nullopt;
    return std::array<uint32_t, 2> { static_cast<uint32_t>((*obj.size)[0]),
                                     static_cast<uint32_t>((*obj.size)[1]) };
}

std::optional<std::array<uint32_t, 2>>
largest_image_extent(std::span<const SceneObjectMetadata> objects) {
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
scene_canvas_extent(const SceneMetadata&                 metadata,
                    std::span<const SceneObjectMetadata> objects_metadata) {
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

bool SceneGeneral::FromJson(const owe::Json& json) { return FromJson(json, kSceneVersionUnknown); }

bool SceneGeneral::FromJson(const owe::Json& json, SceneVersion v) {
    parse_baseline(*this, json);
    if (wants(v, 10)) parse_v10_plus(*this, json);
    if (wants(v, 20)) parse_v20_plus(*this, json);
    if (wants(v, 21)) parse_v21_plus(*this, json);
    if (wants(v, 22)) parse_v22_plus(*this, json);
    if (wants(v, 23)) parse_v23_plus(*this, json);
    if (wants(v, 21)) parse_lightconfig(*this, json);
    AbsorbAllFieldBindings(json, field_bindings);
    capture_user_bindings(*this, json);
    return true;
}

bool SceneMetadata::FromJson(const owe::Json& json) { return FromJson(json, kSceneVersionUnknown); }

bool SceneMetadata::FromJson(const owe::Json& json, SceneVersion v) {
    pkg_version        = v;
    scene_json_version = DetectSceneJsonVersion(json);
    if (auto camera_json = json.get("camera"); camera_json.is_some()) {
        // camera schema is identical across PKGV0001..PKGV0023; no version gate needed.
        camera.FromJson(**camera_json);
    } else {
        rstd_error("scene no camera");
        return false;
    }
    if (auto general_json = json.get("general"); general_json.is_some()) {
        general.FromJson(**general_json, v);
    } else {
        rstd_error("scene no genera data");
        return false;
    }
    return true;
}

namespace owe::wpscene
{

std::optional<SceneDocument> ParseSceneDocumentJson(std::string_view buf,
                                                    SceneVersion     pkg_version) {
    SceneDocument doc;
    auto          parsed = owe::ParseJson(buf);
    if (parsed.is_err()) {
        rstd_error("Can't parse scene json: {}", parsed.unwrap_err());
        return std::nullopt;
    }
    doc.root_json = parsed.unwrap();
    if (! doc.metadata.FromJson(doc.root_json, pkg_version)) return std::nullopt;
    doc.objects_metadata       = parse_objects_metadata(doc.root_json);
    doc.metadata.canvas_extent = scene_canvas_extent(doc.metadata, doc.objects_metadata);
    return doc;
}

std::optional<SceneDocument> LoadSceneDocumentFromVfs(fs::VFS& vfs, std::string_view scene_path,
                                                      SceneVersion pkg_version) {
    auto f = fs::OpenBinary(vfs, scene_path);
    if (f.is_err()) return std::nullopt;
    return ParseSceneDocumentJson(f->ReadAllStr(), pkg_version);
}

std::optional<SceneDocument> LoadSceneDocumentFromPkg(std::string_view pkg_path) {
    if (pkg_path.empty()) return std::nullopt;
    auto pkg = fs::WPPkgFs::open(fs::ToPath(pkg_path));
    if (pkg.is_err()) return std::nullopt;

    auto scene_source = pkg->open_read("/scene.json");
    if (scene_source.is_err()) return std::nullopt;
    auto scene_file = fs::BinaryReader(rstd::move(scene_source).unwrap_unchecked());

    auto       stamp       = pkg->pkg_version_stamp();
    const auto pkg_version = ParsePkgVersionStamp(
        std::string_view(reinterpret_cast<const char*>(stamp.data()), stamp.size().to_primitive()));
    return ParseSceneDocumentJson(scene_file.ReadAllStr(), pkg_version);
}

std::optional<SceneDocument> LoadSceneDocumentFromSource(std::string_view source_path) {
    if (source_path.empty()) return std::nullopt;

    std::filesystem::path path { std::string(source_path) };
    auto                  ext = path.extension().string();
    for (auto& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }

    if (ext == ".pkg") return LoadSceneDocumentFromPkg(source_path);
    if (ext != ".json") return std::nullopt;

    auto scene_file = fs::OpenPhysicalBinary(source_path);
    if (scene_file.is_err()) return std::nullopt;
    return ParseSceneDocumentJson(scene_file->ReadAllStr(), kSceneVersionUnknown);
}

} // namespace owe::wpscene
