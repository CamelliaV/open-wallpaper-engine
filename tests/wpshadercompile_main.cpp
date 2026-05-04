// wpshadercompile <manifest.jsonl> [--quiet]
// wpshadercompile --workshop-dir DIR [--assets DIR] [--quiet]
//
// Two modes:
//
//   1. Manifest replay (positional arg): reads a JSONL manifest produced
//      by viewer (or any harness) under WP_SHADER_RECORD and re-runs
//      WPShaderParser::CompileToSpv on each record.
//
//   2. Workshop iteration (--workshop-dir): walks workshop, runs
//      WPSceneParser::Parse on every pkg with WP_SHADER_RECORD pointed
//      at a tmpfile (so SceneParser's transitive WPShaderParser calls
//      are captured), then replays the captured manifest. Equivalent
//      output to mode 1 but no separate viewer-record pass needed.
//
// Output: per-record one-line status, plus an aggregate summary on
// stderr. Exit code is 0 iff every record compiled without error.
//
// Both modes are host-only: no Vulkan device, no GLFW, no rendering.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "Type.hpp"

import wescene.parse;
import wescene.fs;
import wescene.scene;
import wescene.pkg_fs;
import wavsen.audio;

using json = nlohmann::json;

namespace {

namespace fs = std::filesystem;

constexpr const char* kDefaultWorkshopDir =
#ifdef WAYWALLEN_WORKSHOP_DIR
    WAYWALLEN_WORKSHOP_DIR
#else
    "workshop"
#endif
    ;

constexpr const char* kDefaultAssetsDir =
#ifdef WAYWALLEN_ASSETS_DIR
    WAYWALLEN_ASSETS_DIR
#else
    ""
#endif
    ;

void Usage(const char* prog) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s <manifest.jsonl> [--quiet]\n"
                 "  %s --workshop-dir DIR [--assets DIR] [--quiet] [--keep-manifest FILE]\n"
                 "    DIR is either a workshop root (children are <id>/scene.pkg)\n"
                 "    or a single pkg directory (DIR itself contains scene.pkg).\n",
                 prog ? prog : "wpshadercompile", prog ? prog : "wpshadercompile");
}

wallpaper::ShaderType ParseStage(const std::string& s) {
    if (s == "VERTEX")   return wallpaper::ShaderType::VERTEX;
    if (s == "FRAGMENT") return wallpaper::ShaderType::FRAGMENT;
    if (s == "GEOMETRY") return wallpaper::ShaderType::GEOMETRY;
    return wallpaper::ShaderType::VERTEX;
}

const char* StageName(wallpaper::ShaderType s) {
    switch (s) {
    case wallpaper::ShaderType::VERTEX:   return "VS";
    case wallpaper::ShaderType::FRAGMENT: return "FS";
    case wallpaper::ShaderType::GEOMETRY: return "GS";
    }
    return "??";
}

struct ReplayResult {
    bool        ok { false };
    std::string scene_id;
    std::string stages_summary;
};

ReplayResult ReplayOne(const json& rec) {
    ReplayResult r;
    r.scene_id = rec.value("scene_id", "");

    std::vector<wallpaper::WPShaderUnit> units;
    {
        std::stringstream stages_ss;
        for (const auto& js_stage : rec.at("stages")) {
            wallpaper::WPShaderUnit u;
            u.stage = ParseStage(js_stage.at("stage").get<std::string>());
            u.src   = js_stage.at("src").get<std::string>();
            if (! units.empty()) stages_ss << '+';
            stages_ss << StageName(u.stage);
            units.push_back(std::move(u));
        }
        r.stages_summary = stages_ss.str();
    }

    // Empty src means SceneParser couldn't resolve the engine shader at
    // capture time — almost always because the engine assets dir wasn't
    // mounted. Replaying would emit a synthetic "shader_main undeclared"
    // diagnostic that masks the real error class. Fail loudly instead.
    for (const auto& u : units) {
        if (u.src.empty()) {
            throw std::runtime_error("captured shader src is empty (missing --assets?)");
        }
    }

    wallpaper::WPShaderInfo shader_info;
    if (rec.contains("combos")) {
        for (auto it = rec.at("combos").begin(); it != rec.at("combos").end(); ++it) {
            shader_info.combos[it.key()] = it.value().get<std::string>();
        }
    }

    std::vector<wallpaper::WPShaderTexInfo> texs;
    if (rec.contains("tex_infos")) {
        for (const auto& jt : rec.at("tex_infos")) {
            wallpaper::WPShaderTexInfo ti;
            ti.enabled = jt.value("enabled", false);
            if (jt.contains("compos") && jt.at("compos").is_array()) {
                const auto& c = jt.at("compos");
                for (size_t i = 0; i < 3 && i < c.size(); ++i) {
                    ti.composEnabled[i] = c[i].get<bool>();
                }
            }
            texs.push_back(ti);
        }
    }

    wallpaper::fs::VFS vfs;

    std::vector<wallpaper::ShaderCode> codes;
    r.ok = wallpaper::WPShaderParser::CompileToSpv(r.scene_id, units, codes, vfs,
                                                   &shader_info, texs);
    return r;
}

// Walks workshop_dir, runs WPSceneParser::Parse on every pkg with
// WP_SHADER_RECORD set so each transitive CompileToSpv call appends a
// record to manifest_path. assets_dir, if non-empty, is mounted at
// /assets first as a physical fs so the workshop's pkg can resolve
// references to the project's bundled shaders.
//
// Two shapes are accepted: a workshop root (children are <id>/scene.pkg)
// or a single pkg directory (workshop_dir itself contains scene.pkg) for
// quick single-wallpaper iteration.
//
// Returns the number of pkgs the parser was driven through (regardless
// of whether SceneParser succeeded — the relevant signal lives in the
// manifest line count, not here).
int CapturePkgsToManifest(const std::string& workshop_dir, const std::string& assets_dir,
                          const std::string& manifest_path, bool quiet) {
    if (! fs::exists(workshop_dir) || ! fs::is_directory(workshop_dir)) {
        std::fprintf(stderr, "wpshadercompile: %s is not a directory\n", workshop_dir.c_str());
        return 0;
    }

    // Mirror Corpus::kSkipIds: WPMdlParser::Parse hangs on this pkg's
    // .mdl, so SceneParser would block forever during shader capture.
    static constexpr const char* kSkipId = "2435537849";

    std::vector<fs::path> dirs;
    if (fs::exists(fs::path(workshop_dir) / "scene.pkg")) {
        dirs.push_back(workshop_dir);
    } else {
        for (auto& e : fs::directory_iterator(workshop_dir)) {
            if (! e.is_directory()) continue;
            if (! fs::exists(e.path() / "scene.pkg")) continue;
            if (e.path().filename().string() == kSkipId) continue;
            dirs.push_back(e.path());
        }
        std::sort(dirs.begin(), dirs.end());
    }

    if (! quiet) {
        std::fprintf(stderr, "wpshadercompile: capturing %zu pkgs into %s\n",
                     dirs.size(), manifest_path.c_str());
    }

    int captured_pkgs = 0;
    for (const auto& d : dirs) {
        const std::string id = d.filename().string();
        const std::string pkg_path = (d / "scene.pkg").string();

        wallpaper::fs::VFS vfs;
        if (! assets_dir.empty()) {
            auto afs = wallpaper::fs::CreatePhysicalFs(assets_dir);
            if (afs) vfs.Mount("/assets", std::move(afs));
        }

        auto wfs = wallpaper::fs::WPPkgFs::CreatePkgFs(pkg_path);
        if (! wfs) {
            if (! quiet) std::fprintf(stderr, "skip %s: CreatePkgFs failed\n", id.c_str());
            continue;
        }
        const unsigned pkg_version =
            wallpaper::wpscene::ParsePkgVersionStamp(wfs->pkg_version_stamp());
        vfs.Mount("/assets", std::move(wfs));

        auto stream = vfs.Open("/assets/scene.json");
        if (! stream) {
            if (! quiet) std::fprintf(stderr, "skip %s: scene.json missing\n", id.c_str());
            continue;
        }
        const std::string text = stream->ReadAllStr();

        try {
            wavsen::audio::SoundManager sm;
            wallpaper::WPSceneParser       parser;
            parser.Parse(id, text, vfs, sm,
                         static_cast<wallpaper::wpscene::SceneVersion>(pkg_version));
            ++captured_pkgs;
        } catch (const std::exception& e) {
            if (! quiet) std::fprintf(stderr, "skip %s: SceneParser threw: %s\n",
                                      id.c_str(), e.what());
        } catch (...) {
            if (! quiet) std::fprintf(stderr, "skip %s: SceneParser threw\n", id.c_str());
        }
    }
    return captured_pkgs;
}

int ReplayManifest(const std::string& path, bool quiet) {
    std::ifstream in(path);
    if (! in) {
        std::fprintf(stderr, "wpshadercompile: cannot open %s\n", path.c_str());
        return 1;
    }

    int total = 0, ok = 0, fail = 0;
    int line_no = 0;

    auto t0 = std::chrono::steady_clock::now();

    std::string line;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) continue;

        json rec;
        try {
            rec = json::parse(line);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s:%d: parse error: %s\n", path.c_str(), line_no, e.what());
            ++fail;
            ++total;
            continue;
        }

        ReplayResult r;
        try {
            r = ReplayOne(rec);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s:%d: replay threw: %s\n", path.c_str(), line_no, e.what());
            ++fail;
            ++total;
            continue;
        }

        ++total;
        if (r.ok) {
            ++ok;
            if (! quiet) {
                std::fprintf(stdout, "OK    %s/%-9s  scene=%s\n",
                             path.c_str(), r.stages_summary.c_str(), r.scene_id.c_str());
            }
        } else {
            ++fail;
            std::fprintf(stdout, "FAIL  %s:%d/%-9s  scene=%s\n",
                         path.c_str(), line_no, r.stages_summary.c_str(), r.scene_id.c_str());
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::fprintf(stderr, "wpshadercompile: %d records, %d ok, %d fail | %lld ms\n",
                 total, ok, fail, static_cast<long long>(ms));
    return fail == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    bool        quiet         = false;
    std::string manifest_path;
    std::string workshop_dir;
    std::string assets_dir   = kDefaultAssetsDir;
    std::string keep_manifest;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--quiet") == 0) {
            quiet = true;
        } else if (std::strcmp(a, "--workshop-dir") == 0 && i + 1 < argc) {
            workshop_dir = argv[++i];
        } else if (std::strcmp(a, "--assets") == 0 && i + 1 < argc) {
            assets_dir = argv[++i];
        } else if (std::strcmp(a, "--keep-manifest") == 0 && i + 1 < argc) {
            keep_manifest = argv[++i];
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            Usage(argv[0]);
            return 0;
        } else if (a[0] == '-') {
            Usage(argv[0]);
            return 2;
        } else if (manifest_path.empty()) {
            manifest_path = a;
        } else {
            Usage(argv[0]);
            return 2;
        }
    }

    // Workshop iteration mode: drive SceneParser to populate a tmpfile
    // manifest, then replay it through the same code path mode 1 uses.
    if (! workshop_dir.empty()) {
        if (! manifest_path.empty()) {
            std::fprintf(stderr,
                         "wpshadercompile: --workshop-dir and positional manifest are mutually exclusive\n");
            return 2;
        }

        std::string tmp_path = keep_manifest.empty()
            ? (fs::temp_directory_path() /
               ("wpshadercompile-" + std::to_string(::getpid()) + ".jsonl")).string()
            : keep_manifest;

        // Truncate any stale tmpfile before we ask the recorder to append.
        { std::ofstream truncate(tmp_path, std::ios::trunc); }

        ::setenv("WP_SHADER_RECORD", tmp_path.c_str(), 1);
        int captured = CapturePkgsToManifest(workshop_dir, assets_dir, tmp_path, quiet);
        ::unsetenv("WP_SHADER_RECORD"); // critical: prevent replay from re-recording

        if (! quiet) {
            std::fprintf(stderr, "wpshadercompile: captured %d pkgs into %s\n",
                         captured, tmp_path.c_str());
        }

        int rc = ReplayManifest(tmp_path, quiet);

        if (keep_manifest.empty()) {
            std::error_code ec;
            fs::remove(tmp_path, ec);
        } else if (! quiet) {
            std::fprintf(stderr, "wpshadercompile: kept manifest at %s\n", tmp_path.c_str());
        }
        return rc;
    }

    // Manifest replay mode (legacy): single positional arg.
    if (manifest_path.empty()) {
        Usage(argv[0]);
        return 2;
    }
    return ReplayManifest(manifest_path, quiet);
}
