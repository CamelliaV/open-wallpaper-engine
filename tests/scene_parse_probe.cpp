#include "scene_parse_probe.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

import wescene.parse;
import wescene.pkg_fs;
import wescene.fs;

namespace owe::testing {

namespace {

namespace fs = std::filesystem;

// Mirrors WPPkgFs::CreatePkgFs's first read: just the length-prefixed
// version stamp. Avoids paying for the full pkg+vfs construction.
bool ReadPkgVersionStamp(const std::string& pkg_path, std::string& out) {
    auto stream = owe::fs::CreateCBinaryStream(pkg_path);
    if (! stream) return false;
    std::int32_t len = stream->ReadInt32();
    if (len < 0) return false;
    out.resize(static_cast<std::size_t>(len));
    stream->Read(out.data(), static_cast<std::size_t>(len));
    return true;
}

} // namespace

SceneParseResult ProbeSceneParse(const std::string& workshop_dir) {
    SceneParseResult out;

    const std::string pkg_path = workshop_dir + "/scene.pkg";
    if (! fs::exists(pkg_path)) {
        out.error = "scene.pkg not found";
        return out;
    }

    auto wfs = owe::fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! wfs) {
        out.error = "WPPkgFs::CreatePkgFs failed";
        return out;
    }
    out.pkg_version = wpscene::ParsePkgVersionStamp(wfs->pkg_version_stamp());

    owe::fs::VFS vfs;
    if (! vfs.Mount("/assets", std::move(wfs))) {
        out.error = "vfs.Mount failed";
        return out;
    }

    auto stream = vfs.Open("/assets/scene.json");
    if (! stream) {
        out.error = "scene.json not present in pkg";
        return out;
    }
    const std::string text = stream->ReadAllStr();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        out.error = std::string("json parse: ") + e.what();
        return out;
    }

    wpscene::WPScene scene;
    const bool       parsed = scene.FromJson(j, out.pkg_version);
    out.scene_json_version  = scene.scene_json_version;
    if (! parsed) {
        out.error = "WPScene::FromJson returned false";
        return out;
    }

    out.ok = true;
    return out;
}

std::vector<WorkshopProbe> EnumerateWorkshopProbes(const std::string& workshop_root) {
    std::vector<WorkshopProbe> out;
    fs::path                   root { workshop_root };
    if (! fs::exists(root) || ! fs::is_directory(root)) return out;

    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(root)) {
        if (! e.is_directory()) continue;
        if (! fs::exists(e.path() / "scene.pkg")) continue;
        dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());

    out.reserve(dirs.size());
    for (const auto& d : dirs) {
        WorkshopProbe p;
        p.id  = d.filename().string();
        p.dir = d.string();
        if (! ReadPkgVersionStamp((d / "scene.pkg").string(), p.pkg_stamp)) {
            continue;
        }
        p.pkg_version = wpscene::ParsePkgVersionStamp(p.pkg_stamp);
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace owe::testing
