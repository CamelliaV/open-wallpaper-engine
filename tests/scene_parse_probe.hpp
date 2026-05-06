// Per-workshop probe of WPScene::FromJson with explicit pkg version.
//
// Used by the gtest version-aware regression net: for every workshop in
// the corpus, open scene.pkg, extract the pkg version stamp, read raw
// scene.json from VFS, and exercise the canonical version-bearing
// FromJson path. Returns the parse outcome plus the resolved pkg
// version so the test can assert on both.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace owe::testing {

struct SceneParseResult {
    bool        ok { false };
    std::string error;
    // pkg_version: integer parsed from "PKGV00xx"; 0 means unknown / loose dir.
    std::uint16_t pkg_version { 0 };
    // scene_json_version: scene.json's own top-level "version" field; 0 if absent.
    std::uint16_t scene_json_version { 0 };
};

// Reads workshop_dir/scene.pkg, mounts a transient VFS, parses scene.json
// via WPScene::FromJson(json, pkg_version). All failures (missing pkg,
// missing scene.json, json parse error, FromJson returning false) come
// back as ok=false with a populated `error`.
SceneParseResult ProbeSceneParse(const std::string& workshop_dir);

struct WorkshopProbe {
    std::string id;            // workshop directory name (steam id)
    std::string dir;           // absolute workshop directory
    std::string pkg_stamp;     // raw "PKGV00xx" string from pkg header
    std::uint16_t pkg_version; // ParsePkgVersionStamp(pkg_stamp); 0 on error
};

// Light-weight enumerator: walks `workshop_root` for direct subdirectories
// containing `scene.pkg`, reads only the version stamp, and returns a
// sorted list. Does NOT call DumpWorkshop / WPMdlParser / WPTexImageParser
// — safe to use even when the corpus has a workshop that crashes those.
std::vector<WorkshopProbe> EnumerateWorkshopProbes(const std::string& workshop_root);

} // namespace owe::testing
