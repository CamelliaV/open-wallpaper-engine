#include "scene_keys.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "pkg_header.hpp"

import wescene.pkg_fs;
import wescene.fs;

namespace wallpaper::testing {

namespace {

namespace fs = std::filesystem;
using json   = nlohmann::json;

const char* TypeName(json::value_t t) {
    using vt = json::value_t;
    switch (t) {
    case vt::null:            return "null";
    case vt::object:          return "object";
    case vt::array:           return "array";
    case vt::string:          return "string";
    case vt::boolean:         return "boolean";
    case vt::number_integer:  return "number_integer";
    case vt::number_unsigned: return "number_unsigned";
    case vt::number_float:    return "number_float";
    case vt::binary:          return "binary";
    case vt::discarded:       return "discarded";
    }
    return "unknown";
}

// Per-scene observation: count of hits per path, plus the set of value_t
// names seen at that path within this scene.
struct LocalObs {
    int                            occurrences { 0 };
    std::set<std::string>          types;
};
using LocalMap = std::unordered_map<std::string, LocalObs>;

void Walk(const json& node, std::string& path, LocalMap& out) {
    if (! path.empty()) {
        auto& obs = out[path];
        obs.occurrences += 1;
        obs.types.insert(TypeName(node.type()));
    }
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::size_t old = path.size();
            if (! path.empty()) path += '.';
            path += it.key();
            Walk(it.value(), path, out);
            path.resize(old);
        }
    } else if (node.is_array()) {
        const std::size_t old = path.size();
        path += "[]";
        for (const auto& el : node) Walk(el, path, out);
        path.resize(old);
    }
}

// Global aggregation: per pkg-version → per path → stats.
struct KeyStats {
    std::uint64_t         present_in { 0 };
    std::uint64_t         occurrences { 0 };
    std::set<std::string> value_types;
};

struct VersionAgg {
    std::uint64_t                    total_scenes { 0 };
    std::map<std::string, KeyStats>  keys;
};

void Merge(VersionAgg& agg, const LocalMap& local) {
    agg.total_scenes += 1;
    for (const auto& [path, obs] : local) {
        auto& dst = agg.keys[path];
        dst.present_in  += 1;
        dst.occurrences += static_cast<std::uint64_t>(obs.occurrences);
        for (const auto& t : obs.types) dst.value_types.insert(t);
    }
}

bool ScanOneWorkshop(const fs::path& workshop_dir,
                     std::map<std::string, VersionAgg>& by_version) {
    const std::string id       = workshop_dir.filename().string();
    const std::string pkg_path = (workshop_dir / "scene.pkg").string();

    if (! fs::exists(pkg_path)) return false;

    std::string           pkg_version;
    std::vector<PkgEntry> entries;
    if (! ReadPkgHeader(pkg_path, pkg_version, entries)) {
        std::fprintf(stderr, "wpscan: skip %s: bad pkg header\n", id.c_str());
        return false;
    }

    bool has_scene_json = false;
    for (const auto& e : entries)
        if (e.path == "/scene.json") {
            has_scene_json = true;
            break;
        }
    if (! has_scene_json) return false;

    auto wfs = wallpaper::fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! wfs) {
        std::fprintf(stderr, "wpscan: skip %s: WPPkgFs::CreatePkgFs failed\n", id.c_str());
        return false;
    }
    wallpaper::fs::VFS vfs;
    vfs.Mount("/assets", std::move(wfs));

    auto stream = vfs.Open("/assets/scene.json");
    if (! stream) {
        std::fprintf(stderr, "wpscan: skip %s: scene.json open failed\n", id.c_str());
        return false;
    }
    std::string text = stream->ReadAllStr();

    json scene;
    try {
        scene = json::parse(text);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wpscan: skip %s: %s\n", id.c_str(), e.what());
        return false;
    }

    LocalMap    local;
    std::string path;
    Walk(scene, path, local);

    Merge(by_version[pkg_version], local);
    return true;
}

json AggToJson(const std::map<std::string, VersionAgg>& by_version) {
    json out = json::object();
    for (const auto& [version, agg] : by_version) {
        json jv;
        jv["total_scenes"] = agg.total_scenes;
        json jkeys         = json::object();
        for (const auto& [path, st] : agg.keys) {
            json jk;
            jk["present_in"]  = st.present_in;
            jk["occurrences"] = st.occurrences;
            json jtypes       = json::array();
            for (const auto& t : st.value_types) jtypes.push_back(t);
            jk["value_types"] = std::move(jtypes);
            jkeys[path]       = std::move(jk);
        }
        jv["keys"]    = std::move(jkeys);
        out[version]  = std::move(jv);
    }
    return out;
}

} // namespace

json ScanSceneKeys(const std::string& workshop_root) {
    std::map<std::string, VersionAgg> by_version;

    fs::path root { workshop_root };
    if (! fs::exists(root) || ! fs::is_directory(root)) {
        std::fprintf(stderr, "wpscan: workshop dir %s missing\n", root.c_str());
        return AggToJson(by_version);
    }

    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(root)) {
        if (! e.is_directory()) continue;
        dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());

    std::uint64_t scanned = 0;
    for (const auto& d : dirs) {
        if (ScanOneWorkshop(d, by_version)) ++scanned;
    }

    std::uint64_t total_keys = 0;
    for (const auto& [_, agg] : by_version) total_keys += agg.keys.size();
    std::fprintf(stderr,
                 "wpscan: scanned %llu scenes, %zu pkg versions, %llu unique key paths\n",
                 static_cast<unsigned long long>(scanned),
                 by_version.size(),
                 static_cast<unsigned long long>(total_keys));

    return AggToJson(by_version);
}

} // namespace wallpaper::testing
