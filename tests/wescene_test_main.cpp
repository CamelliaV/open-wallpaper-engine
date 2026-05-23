// wescene-test: consolidated CLI for the wescene-renderer test surface.
//
//   wescene-test scan        [...]  walk pkgs and parse + (optionally) validate
//   wescene-test extract     [...]  list or export a single asset from one pkg
//   wescene-test rendergraph [pkg]  parse one pkg and dump the scene's predicted
//                                   render-graph structure (RTs, scene-graph
//                                   passes, image-effect chains, post-processes)
//
// Replaces wpparse / wpshadercompile / wpdump / wpscan / wptexparse /
// wpscript* (archived in tests/old/). Host-only: no Vulkan device, no
// GLFW. Exit 0 iff every selected pkg parsed AND every requested
// validator succeeded.

#include <nlohmann/json.hpp>

import wescene.parse;
import wescene.fs;
import wescene.pkg_fs;
import wescene.scene;
import wescene.spec_texs;
import wescene.types;
import wavsen.audio;
import rstd.log;
import rstd.cppstd;
import wescene.testing.corpus;
import wescene.testing.pkg_header;

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

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

// Workshops that hang or hard-crash deep parsers. Keep in sync with
// corpus.cpp's kSkipIds (same intent, different container).
constexpr std::array<const char*, 2> kSkipIds = { "2435537849", "3346715292" };

void TopUsage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s <subcommand> [options]\n"
                 "\n"
                 "Subcommands:\n"
                 "  scan         Walk pkgs and run scene parse, optionally with per-asset\n"
                 "               passes (--parse-tex / --parse-shader / --parse-mdl).\n"
                 "               Filterable by --name and --pkgv.\n"
                 "  extract      List entries of a single scene.pkg, or export one asset\n"
                 "               file (text or binary) to stdout or -o FILE.\n"
                 "  rendergraph  Parse one pkg fully and print the predicted render-graph:\n"
                 "               render targets, scene-graph passes, image-effect chains,\n"
                 "               and post-process steps. No Vulkan; pure structural dump.\n"
                 "  valid        Run DumpWorkshop on one pkg and emit the JSON fixture\n"
                 "               snapshot (to stdout or a file). Used to regenerate the\n"
                 "               checked-in fixtures under tests/fixtures/.\n"
                 "\n"
                 "Run `%s <subcommand> --help` for subcommand-specific options.\n",
                 prog ? prog : "wescene-test", prog ? prog : "wescene-test");
}

// ---------------------------------------------------------------------------
// shared helpers
// ---------------------------------------------------------------------------

std::string LowerCopy(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Resolve a user-provided pkg argument. Accepts either a direct path to
// scene.pkg or a directory that contains one. Returns empty on failure.
std::string ResolvePkgPath(std::string_view arg) {
    fs::path p { arg };
    if (fs::is_directory(p)) p /= "scene.pkg";
    if (! fs::exists(p) || ! fs::is_regular_file(p)) return {};
    return p.string();
}

// Normalise an asset path to the in-pkg shape ("/scene.json"). Accepts
// "/scene.json", "scene.json", "/assets/scene.json", "assets/scene.json".
std::string NormalisePkgAssetPath(std::string_view arg) {
    std::string p { arg };
    if (StartsWith(p, "/assets/")) p.erase(0, 7);  // → "/scene.json"
    else if (StartsWith(p, "assets/")) p.erase(0, 6); // → "/scene.json" — wait, 6 chars then ensure leading '/'
    if (! p.empty() && p.front() != '/') p.insert(p.begin(), '/');
    return p;
}

// ---------------------------------------------------------------------------
// scan subcommand
// ---------------------------------------------------------------------------

struct ScanOptions {
    std::string              workshop_dir = kDefaultWorkshopDir;
    std::string              assets_dir   = kDefaultAssetsDir;
    bool                     p_tex { false };
    bool                     p_shader { false };
    bool                     p_mdl { false };       // header-only by default
    bool                     p_mdl_full { false };  // upgrade to full WPMdlParser::Parse
    bool                     full { false };
    std::vector<std::string> name_filters;
    std::vector<unsigned>    pkgv_filters;
    int                      limit { 0 };
    int                      offset { 0 };
    bool                     quiet { false };
    bool                     stop_on_fail { false };
    std::string              json_out;              // empty = no single JSON file (--json)
    std::string              json_dir;              // empty = no per-workshop dump dir (--json-dir)
};

void ScanUsage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s scan [options]\n"
                 "  --workshop-dir DIR   workshop root (children are <id>/scene.pkg) or a\n"
                 "                       single pkg dir (DIR itself contains scene.pkg).\n"
                 "                       (default: %s)\n"
                 "  --assets DIR         shared engine assets, mounted at /assets fallback\n"
                 "                       (default: %s)\n"
                 "Parse depth:\n"
                 "  --full               run full WPSceneParser::Parse instead of the cheap\n"
                 "                       FromJson+ExpandObjects+AdjustAuto base. Triggers\n"
                 "                       per-image shader compile, bloom auto-injection,\n"
                 "                       glslang init/finalize, etc. Slower but exercises\n"
                 "                       every stage.\n"
                 "Parse passes (any combination; also drives --json-dir's per-workshop\n"
                 "section emission):\n"
                 "  --parse-tex          run WPTexImageParser on every /materials/**/*.tex\n"
                 "  --parse-shader       run WPShaderParser::CompileMaterialShader on every\n"
                 "                       /materials/**/*.json\n"
                 "  --parse-mdl          run WPMdlParser::ParseHeader on every /models/**/*.mdl;\n"
                 "                       fast, doesn't touch vertex/index/bone data.\n"
                 "  --parse-mdl-full     upgrade --parse-mdl to full WPMdlParser::Parse;\n"
                 "                       slow, some workshops are known to hang/reject.\n"
                 "  --parse-all          --parse-tex + --parse-shader + --parse-mdl\n"
                 "                       (header-only; pair with --parse-mdl-full for body)\n"
                 "Filters (repeatable; multiple --name/--pkgv values OR together):\n"
                 "  --name SUBSTR        only pkgs whose dir name contains SUBSTR (ci)\n"
                 "  --pkgv N             only pkgs with PKGV stamp == N\n"
                 "  --limit N            stop after N matched pkgs (default 0 = all)\n"
                 "  --offset N           skip the first N matched pkgs before --limit applies\n"
                 "                       (default 0). Use with --limit to chunk corpus runs.\n"
                 "Misc:\n"
                 "  --quiet              suppress per-asset OK lines; only FAIL + summary\n"
                 "  --stop-on-fail       exit non-zero on the first per-asset failure so a\n"
                 "                       wrapper loop can resume at the next --offset.\n"
                 "  --json FILE          additionally write structured validator results to\n"
                 "                       FILE (single JSON document with pkgs[] + summary).\n"
                 "  --json-dir DIR       additionally write per-workshop DumpWorkshop snapshots\n"
                 "                       to DIR/<id>.json (same format as `wescene-test valid`).\n"
                 "                       Mutually exclusive with --json.\n"
                 "  -h, --help           this message\n",
                 prog ? prog : "wescene-test",
                 kDefaultWorkshopDir,
                 *kDefaultAssetsDir ? kDefaultAssetsDir : "<unset>");
}

bool ParseScanArgs(int argc, char** argv, ScanOptions& opt, const char* prog) {
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        auto need_val = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "wescene-test scan: %s requires a value\n",
                             std::string(name).c_str());
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--workshop-dir") {
            if (auto* v = need_val(a)) opt.workshop_dir = v;
            else return false;
        } else if (a == "--assets") {
            if (auto* v = need_val(a)) opt.assets_dir = v;
            else return false;
        } else if (a == "--parse-tex") {
            opt.p_tex = true;
        } else if (a == "--parse-shader") {
            opt.p_shader = true;
        } else if (a == "--parse-mdl") {
            opt.p_mdl = true;
        } else if (a == "--parse-mdl-full") {
            opt.p_mdl = true;
            opt.p_mdl_full = true;
        } else if (a == "--parse-all") {
            opt.p_tex = opt.p_shader = opt.p_mdl = true;
        } else if (a == "--full") {
            opt.full = true;
        } else if (a == "--name") {
            if (auto* v = need_val(a)) opt.name_filters.emplace_back(v);
            else return false;
        } else if (a == "--pkgv") {
            if (auto* v = need_val(a)) opt.pkgv_filters.push_back((unsigned)std::atoi(v));
            else return false;
        } else if (a == "--limit") {
            if (auto* v = need_val(a)) opt.limit = std::atoi(v);
            else return false;
        } else if (a == "--offset") {
            if (auto* v = need_val(a)) opt.offset = std::atoi(v);
            else return false;
        } else if (a == "--quiet") {
            opt.quiet = true;
        } else if (a == "--stop-on-fail") {
            opt.stop_on_fail = true;
        } else if (a == "--json") {
            if (auto* v = need_val(a)) opt.json_out = v;
            else return false;
        } else if (a == "--json-dir") {
            if (auto* v = need_val(a)) opt.json_dir = v;
            else return false;
        } else if (a == "-h" || a == "--help") {
            ScanUsage(prog);
            std::exit(0);
        } else {
            std::fprintf(stderr, "wescene-test scan: unknown arg '%.*s'\n",
                         (int)a.size(), a.data());
            return false;
        }
    }
    if (! opt.json_out.empty() && ! opt.json_dir.empty()) {
        std::fprintf(stderr, "wescene-test scan: --json and --json-dir are mutually exclusive\n");
        return false;
    }
    return true;
}

bool MatchesNameFilters(const std::string& dir_name, const std::vector<std::string>& filters) {
    if (filters.empty()) return true;
    const std::string lo = LowerCopy(dir_name);
    for (const auto& f : filters) {
        if (lo.find(LowerCopy(f)) != std::string::npos) return true;
    }
    return false;
}

bool MatchesPkgvFilters(unsigned v, const std::vector<unsigned>& filters) {
    if (filters.empty()) return true;
    return std::find(filters.begin(), filters.end(), v) != filters.end();
}

struct Counters {
    int parsed_ok { 0 };
    int parsed_fail { 0 };
    int tex_ok { 0 }, tex_fail { 0 };
    int shader_ok { 0 }, shader_fail { 0 };
    int mdl_ok { 0 }, mdl_fail { 0 };
};

// Runs scene parse base (FromJson + ExpandObjects + AdjustAuto). Cheap;
// never touches glslang or scene-graph allocation.
bool RunSceneParseBase(owe::fs::VFS& vfs, owe::wpscene::SceneVersion pkg_v, std::string& err) {
    auto stream = vfs.Open("/assets/scene.json");
    if (! stream) {
        err = "scene.json not in pkg";
        return false;
    }
    const std::string text = stream->ReadAllStr();
    json              j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        err = std::string("scene.json parse: ") + e.what();
        return false;
    }
    owe::wpscene::WPScene sc;
    if (! sc.FromJson(j, pkg_v)) {
        err = "WPScene::FromJson returned false";
        return false;
    }
    auto wp_objs = owe::ExpandObjects(j, vfs, pkg_v);
    owe::AdjustAutoOrthoProjection(sc, wp_objs);
    (void)wp_objs;
    return true;
}

// Runs the full WPSceneParser::Parse pipeline: scene parse + per-image
// shader compile + bloom auto-injection + scene-graph allocation.
// SoundManager is default-constructed but never Init()'d so Sound
// objects parse without opening an audio device.
bool RunSceneParseFull(owe::fs::VFS& vfs, owe::wpscene::SceneVersion pkg_v,
                       const std::string& pkg_id, std::string& err) {
    auto stream = vfs.Open("/assets/scene.json");
    if (! stream) {
        err = "scene.json not in pkg";
        return false;
    }
    const std::string           text = stream->ReadAllStr();
    wavsen::audio::SoundManager sm;
    owe::WPSceneParser          parser;
    auto                        scene = parser.Parse(pkg_id, text, vfs, sm, pkg_v);
    if (! scene) {
        err = "WPSceneParser::Parse returned null";
        return false;
    }
    return true;
}

void ValidateTextures(const std::vector<owe::testing::PkgEntry>& entries,
                      owe::fs::VFS& vfs, const std::string& pkg_id,
                      Counters& c, bool quiet, json* sink) {
    owe::WPTexImageParser parser(&vfs);
    constexpr std::string_view prefix = "/materials/";
    constexpr std::string_view suffix = ".tex";
    for (const auto& e : entries) {
        if (! StartsWith(e.path, prefix) || ! EndsWith(e.path, suffix)) continue;
        if (e.path.size() < prefix.size() + suffix.size()) continue;
        // ParseHeader takes the bare name (no /materials/ prefix, no .tex
        // suffix), matching WPMaterial.textures shape.
        const std::string name = e.path.substr(prefix.size(),
                                               e.path.size() - prefix.size() - suffix.size());
        bool              ok    = false;
        bool              video = false;
        std::string       err;
        try {
            owe::ImageHeader h = parser.ParseHeader(name);
            ok = (h.width > 0 && h.height > 0);
            video = (h.type == owe::ImageType::VIDEO);
            if (! ok) err = "header looks invalid (zero dim)";
        } catch (const std::exception& ex) {
            err = ex.what();
        } catch (...) {
            err = "unknown exception";
        }
        if (ok) {
            ++c.tex_ok;
            if (! quiet) {
                if (video) {
                    std::fprintf(stdout, "OK    %s tex %s (video container)\n",
                                 pkg_id.c_str(), e.path.c_str());
                } else {
                    std::fprintf(stdout, "OK    %s tex %s\n",
                                 pkg_id.c_str(), e.path.c_str());
                }
            }
        } else {
            ++c.tex_fail;
            std::fprintf(stdout, "FAIL  %s tex %s  %s\n", pkg_id.c_str(), e.path.c_str(),
                         err.c_str());
        }
        if (sink) {
            json entry { {"path", e.path}, {"ok", ok} };
            if (ok) {
                if (video) entry["video"] = true;
            } else {
                entry["error"] = err;
            }
            sink->push_back(std::move(entry));
        }
    }
}

void ValidateShaders(const std::vector<owe::testing::PkgEntry>& entries,
                     owe::fs::VFS& vfs, const std::string& pkg_id,
                     Counters& c, bool quiet, json* sink) {
    for (const auto& e : entries) {
        if (! StartsWith(e.path, "/materials/") || ! EndsWith(e.path, ".json")) continue;
        const std::string vfs_path = "/assets" + e.path;
        auto              stream   = vfs.Open(vfs_path);
        if (! stream) {
            ++c.shader_fail;
            std::fprintf(stdout, "FAIL  %s shader %s  cannot open\n", pkg_id.c_str(),
                         e.path.c_str());
            if (sink) sink->push_back({ {"path", e.path}, {"ok", false},
                                        {"error", "cannot open"} });
            continue;
        }
        const std::string text = stream->ReadAllStr();
        json              jmat;
        try {
            jmat = json::parse(text);
        } catch (const std::exception& ex) {
            ++c.shader_fail;
            std::fprintf(stdout, "FAIL  %s shader %s  json: %s\n", pkg_id.c_str(),
                         e.path.c_str(), ex.what());
            if (sink) sink->push_back({ {"path", e.path}, {"ok", false},
                                        {"error", std::string("json: ") + ex.what()} });
            continue;
        }
        owe::CompileMaterialShaderResult r;
        try {
            r = owe::WPShaderParser::CompileMaterialShader(jmat, vfs, pkg_id);
        } catch (const std::exception& ex) {
            r.ok    = false;
            r.error = ex.what();
        } catch (...) {
            r.ok    = false;
            r.error = "unknown exception";
        }
        if (r.ok) {
            ++c.shader_ok;
            if (! quiet)
                std::fprintf(stdout, "OK    %s shader %s [%s]\n", pkg_id.c_str(),
                             e.path.c_str(), r.shader_name.c_str());
        } else {
            ++c.shader_fail;
            std::fprintf(stdout, "FAIL  %s shader %s [%s]  %s\n", pkg_id.c_str(),
                         e.path.c_str(), r.shader_name.c_str(), r.error.c_str());
        }
        if (sink) {
            json entry { {"path", e.path}, {"ok", r.ok},
                         {"shader_name", r.shader_name} };
            if (! r.ok) entry["error"] = r.error;
            sink->push_back(std::move(entry));
        }
    }
}

void ValidateMdls(const std::vector<owe::testing::PkgEntry>& entries,
                  owe::fs::VFS& vfs, const std::string& pkg_id,
                  Counters& c, bool quiet, json* sink) {
    for (const auto& e : entries) {
        if (! EndsWith(e.path, ".mdl")) continue;
        // WPMdlParser::Parse takes a path without /assets prefix.
        std::string name(e.path.substr(1));
        bool        ok = false;
        std::string err;
        try {
            owe::WPMdl mdl;
            ok = owe::WPMdlParser::Parse(name, vfs, mdl);
            if (! ok) err = "WPMdlParser::Parse returned false";
        } catch (const std::exception& ex) {
            err = ex.what();
        } catch (...) {
            err = "unknown exception";
        }
        if (ok) {
            ++c.mdl_ok;
            if (! quiet)
                std::fprintf(stdout, "OK    %s mdl %s\n", pkg_id.c_str(), e.path.c_str());
        } else {
            ++c.mdl_fail;
            std::fprintf(stdout, "FAIL  %s mdl %s  %s\n", pkg_id.c_str(), e.path.c_str(),
                         err.c_str());
        }
        if (sink) {
            json entry { {"path", e.path}, {"ok", ok} };
            if (! ok) entry["error"] = err;
            sink->push_back(std::move(entry));
        }
    }
}

std::string FormatMdlFlag(uint32_t flag) {
    std::string out;
    out.reserve(8 * 4 + 3);
    for (int i = 0; i < 4; ++i) {
        if (i) out.push_back('|');
        const uint8_t b = static_cast<uint8_t>((flag >> (i * 8)) & 0xFFu);
        for (int bit = 7; bit >= 0; --bit) out.push_back((b & (1u << bit)) ? '1' : '0');
    }
    return out;
}

void ValidateMdlsHeader(const std::vector<owe::testing::PkgEntry>& entries,
                        owe::fs::VFS& vfs, const std::string& pkg_id,
                        Counters& c, bool quiet, json* sink) {
    for (const auto& e : entries) {
        if (! EndsWith(e.path, ".mdl")) continue;
        std::string name(e.path.substr(1));
        owe::WPMdlHeader h;
        bool             ok = false;
        std::string      err;
        try {
            ok = owe::WPMdlParser::ParseHeader(name, vfs, h);
            if (! ok) err = "ParseHeader returned false";
        } catch (const std::exception& ex) {
            err = ex.what();
        } catch (...) {
            err = "unknown exception";
        }
        if (ok) {
            ++c.mdl_ok;
            if (! quiet)
                std::fprintf(stdout, "OK    %s mdl-header %s  mdlv=%d mesh=%u flag=%s\n",
                             pkg_id.c_str(), e.path.c_str(),
                             h.mdlv, h.mesh_count, FormatMdlFlag(h.mdl_flag).c_str());
        } else {
            ++c.mdl_fail;
            std::fprintf(stdout, "FAIL  %s mdl-header %s  %s\n", pkg_id.c_str(), e.path.c_str(),
                         err.c_str());
        }
        if (sink) {
            json entry { {"path", e.path}, {"ok", ok} };
            if (ok) {
                entry["mdlv"]       = h.mdlv;
                entry["mesh_count"] = h.mesh_count;
                json flag_arr = json::array();
                for (int byte_idx = 0; byte_idx < 4; ++byte_idx) {
                    uint8_t b = static_cast<uint8_t>((h.mdl_flag >> (byte_idx * 8)) & 0xFFu);
                    std::string bits(8, '0');
                    for (int i = 0; i < 8; ++i)
                        if (b & (1u << (7 - i))) bits[i] = '1';
                    flag_arr.push_back(std::move(bits));
                }
                entry["flag"] = std::move(flag_arr);
            } else {
                entry["error"] = err;
            }
            sink->push_back(std::move(entry));
        }
    }
}

bool ProcessOnePkg(const fs::path& pkg_dir, const ScanOptions& opt, Counters& c,
                   json* pkgs_arr) {
    const std::string pkg_id = pkg_dir.filename().string();

    for (const auto* sk : kSkipIds) {
        if (pkg_id == sk) {
            std::fprintf(stderr, "SKIP  %s (in kSkipIds)\n", pkg_id.c_str());
            if (pkgs_arr) pkgs_arr->push_back({ {"id", pkg_id}, {"skipped", true} });
            return true;
        }
    }

    const std::string pkg_path = (pkg_dir / "scene.pkg").string();
    if (! fs::exists(pkg_path)) {
        ++c.parsed_fail;
        std::fprintf(stdout, "FAIL  %s parse  scene.pkg not found\n", pkg_id.c_str());
        if (pkgs_arr) pkgs_arr->push_back({
            {"id", pkg_id},
            {"parse", { {"ok", false}, {"error", "scene.pkg not found"} }},
        });
        return false;
    }

    std::string                          version_stamp;
    std::vector<owe::testing::PkgEntry>  entries;
    if (! owe::testing::ReadPkgHeader(pkg_path, version_stamp, entries)) {
        ++c.parsed_fail;
        std::fprintf(stdout, "FAIL  %s parse  ReadPkgHeader\n", pkg_id.c_str());
        if (pkgs_arr) pkgs_arr->push_back({
            {"id", pkg_id},
            {"parse", { {"ok", false}, {"error", "ReadPkgHeader"} }},
        });
        return false;
    }
    const auto pkg_v = owe::wpscene::ParsePkgVersionStamp(version_stamp);

    if (! MatchesPkgvFilters((unsigned)pkg_v, opt.pkgv_filters)) return true;

    owe::fs::VFS vfs;
    if (! opt.assets_dir.empty()) {
        if (auto pfs = owe::fs::CreatePhysicalFs(opt.assets_dir)) {
            vfs.Mount("/assets", std::move(pfs));
        }
    }
    auto wfs = owe::fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! wfs) {
        ++c.parsed_fail;
        std::fprintf(stdout, "FAIL  %s parse  WPPkgFs::CreatePkgFs\n", pkg_id.c_str());
        if (pkgs_arr) pkgs_arr->push_back({
            {"id", pkg_id}, {"pkg_version", (unsigned)pkg_v},
            {"parse", { {"ok", false}, {"error", "WPPkgFs::CreatePkgFs"} }},
        });
        return false;
    }
    vfs.Mount("/assets", std::move(wfs));

    std::string parse_err;
    bool        parse_ok = false;
    try {
        parse_ok = opt.full ? RunSceneParseFull(vfs, pkg_v, pkg_id, parse_err)
                            : RunSceneParseBase(vfs, pkg_v, parse_err);
    } catch (const std::exception& ex) {
        parse_err = ex.what();
    } catch (...) {
        parse_err = "unknown exception";
    }
    if (parse_ok) {
        ++c.parsed_ok;
        if (! opt.quiet)
            std::fprintf(stdout, "OK    %s parse  v=%u\n", pkg_id.c_str(), (unsigned)pkg_v);
    } else {
        ++c.parsed_fail;
        std::fprintf(stdout, "FAIL  %s parse  v=%u  %s\n", pkg_id.c_str(), (unsigned)pkg_v,
                     parse_err.c_str());
    }

    json  pkg_obj;
    json* tex_sink    = nullptr;
    json* shader_sink = nullptr;
    json* mdl_sink    = nullptr;
    if (pkgs_arr) {
        pkg_obj["id"]          = pkg_id;
        pkg_obj["pkg_version"] = (unsigned)pkg_v;
        pkg_obj["parse"]       = parse_ok ? json { {"ok", true} }
                                          : json { {"ok", false}, {"error", parse_err} };
        if (opt.p_tex)    { pkg_obj["textures"] = json::array(); tex_sink    = &pkg_obj["textures"]; }
        if (opt.p_shader) { pkg_obj["shaders"]  = json::array(); shader_sink = &pkg_obj["shaders"]; }
        if (opt.p_mdl) {
            const char* key = opt.p_mdl_full ? "mdls" : "mdls_header";
            pkg_obj[key] = json::array();
            mdl_sink     = &pkg_obj[key];
        }
    }

    if (opt.p_tex)    ValidateTextures(entries, vfs, pkg_id, c, opt.quiet, tex_sink);
    if (opt.p_shader) ValidateShaders(entries, vfs, pkg_id, c, opt.quiet, shader_sink);
    if (opt.p_mdl) {
        if (opt.p_mdl_full) ValidateMdls(entries, vfs, pkg_id, c, opt.quiet, mdl_sink);
        else                ValidateMdlsHeader(entries, vfs, pkg_id, c, opt.quiet, mdl_sink);
    }

    if (pkgs_arr) pkgs_arr->push_back(std::move(pkg_obj));

    // --json-dir: write a per-workshop snapshot. Sections are gated by the
    // same --parse-* flags as scan's text/sink output.
    if (! opt.json_dir.empty()) {
        owe::testing::DumpFlags df {
            .tex      = opt.p_tex,
            .shader   = opt.p_shader,
            .mdl      = opt.p_mdl,
            .mdl_full = opt.p_mdl_full,
        };
        std::string derr;
        json snap = owe::testing::DumpWorkshop(pkg_dir.string(), derr, df);
        if (! derr.empty()) {
            snap = json { {"workshop_dir", pkg_id}, {"error", derr} };
        }
        const auto out_path = fs::path(opt.json_dir) / (pkg_id + ".json");
        std::ofstream ofs(out_path);
        if (ofs) ofs << snap.dump(2) << "\n";
        else std::fprintf(stderr, "wescene-test scan: cannot write %s\n",
                          out_path.string().c_str());
    }

    return parse_ok;
}

int CmdScan(int argc, char** argv, const char* prog) {
    ScanOptions opt;
    if (! ParseScanArgs(argc, argv, opt, prog)) {
        ScanUsage(prog);
        return 2;
    }

    if (! fs::exists(opt.workshop_dir) || ! fs::is_directory(opt.workshop_dir)) {
        std::fprintf(stderr, "wescene-test scan: %s is not a directory\n",
                     opt.workshop_dir.c_str());
        return 1;
    }

    if (! opt.json_dir.empty()) {
        std::error_code ec;
        fs::create_directories(opt.json_dir, ec);
        if (ec || ! fs::is_directory(opt.json_dir)) {
            std::fprintf(stderr, "wescene-test scan: cannot create --json-dir '%s': %s\n",
                         opt.json_dir.c_str(), ec.message().c_str());
            return 1;
        }
    }

    std::vector<fs::path> dirs;
    if (fs::exists(fs::path(opt.workshop_dir) / "scene.pkg")) {
        dirs.push_back(opt.workshop_dir);
    } else {
        for (auto& e : fs::directory_iterator(opt.workshop_dir)) {
            if (! e.is_directory()) continue;
            if (! fs::exists(e.path() / "scene.pkg")) continue;
            const std::string id = e.path().filename().string();
            if (! MatchesNameFilters(id, opt.name_filters)) continue;
            dirs.push_back(e.path());
        }
        std::sort(dirs.begin(), dirs.end());
        if (opt.offset > 0) {
            if ((size_t)opt.offset >= dirs.size()) dirs.clear();
            else dirs.erase(dirs.begin(), dirs.begin() + opt.offset);
        }
        if (opt.limit > 0 && (int)dirs.size() > opt.limit) {
            dirs.resize((size_t)opt.limit);
        }
    }

    if (dirs.empty()) {
        std::fprintf(stderr, "wescene-test scan: no matching pkgs\n");
        return 1;
    }

    json  doc;
    json* pkgs_arr = nullptr;
    if (! opt.json_out.empty()) {
        doc["pkgs"] = json::array();
        pkgs_arr    = &doc["pkgs"];
    }

    Counters c;
    auto     t0 = std::chrono::steady_clock::now();

    size_t processed = 0;
    for (const auto& d : dirs) {
        ProcessOnePkg(d, opt, c, pkgs_arr);
        ++processed;
        if (opt.stop_on_fail &&
            (c.parsed_fail + c.tex_fail + c.shader_fail + c.mdl_fail) > 0) {
            std::fprintf(stderr,
                         "wescene-test scan: --stop-on-fail after pkg '%s'\n",
                         d.filename().string().c_str());
            break;
        }
    }
    if (processed < dirs.size()) dirs.resize(processed);

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::fprintf(stderr,
                 "wescene-test scan: %zu pkgs | parse %d/%d",
                 dirs.size(), c.parsed_ok, c.parsed_ok + c.parsed_fail);
    if (opt.p_tex)    std::fprintf(stderr, " | tex %d/%d",    c.tex_ok,    c.tex_ok + c.tex_fail);
    if (opt.p_shader) std::fprintf(stderr, " | shader %d/%d", c.shader_ok, c.shader_ok + c.shader_fail);
    if (opt.p_mdl)    std::fprintf(stderr, " | %s %d/%d",
                                   opt.p_mdl_full ? "mdl" : "mdl-header",
                                   c.mdl_ok, c.mdl_ok + c.mdl_fail);
    std::fprintf(stderr, " | %lldms\n", (long long)ms);

    if (pkgs_arr) {
        json summary;
        summary["pkgs"]  = dirs.size();
        summary["ms"]    = (long long)ms;
        summary["parse"] = { {"ok", c.parsed_ok}, {"fail", c.parsed_fail} };
        if (opt.p_tex)    summary["tex"]    = { {"ok", c.tex_ok},    {"fail", c.tex_fail} };
        if (opt.p_shader) summary["shader"] = { {"ok", c.shader_ok}, {"fail", c.shader_fail} };
        if (opt.p_mdl) {
            const char* key = opt.p_mdl_full ? "mdl" : "mdl-header";
            summary[key] = { {"ok", c.mdl_ok}, {"fail", c.mdl_fail} };
        }
        doc["summary"] = std::move(summary);

        std::ofstream out(opt.json_out);
        if (! out) {
            std::fprintf(stderr, "wescene-test scan: cannot open --json file '%s'\n",
                         opt.json_out.c_str());
            return 1;
        }
        out << doc.dump(2) << "\n";
    }

    const int total_fail =
        c.parsed_fail + c.tex_fail + c.shader_fail + c.mdl_fail;
    return total_fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// extract subcommand
// ---------------------------------------------------------------------------

void ExtractUsage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s extract <pkg> [<asset-path>] [-o FILE]\n"
                 "\n"
                 "  <pkg>         path to scene.pkg, or a directory containing one.\n"
                 "  <asset-path>  in-pkg path to extract (e.g. '/scene.json',\n"
                 "                'materials/foo.json', 'models/bar.mdl'). Optional;\n"
                 "                if omitted, lists every entry one per line and exits.\n"
                 "  -o FILE       write to FILE. Default: write to stdout.\n"
                 "                Use '-' to force stdout.\n"
                 "\n"
                 "Examples:\n"
                 "  %s extract workshop/123                      # list entries\n"
                 "  %s extract workshop/123 /scene.json          # dump scene.json to stdout\n"
                 "  %s extract workshop/123 materials/foo.tex -o foo.tex\n",
                 prog ? prog : "wescene-test", prog ? prog : "wescene-test",
                 prog ? prog : "wescene-test", prog ? prog : "wescene-test");
}

int CmdExtract(int argc, char** argv, const char* prog) {
    std::string pkg_arg;
    std::string asset_arg;
    std::string out_file;
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "-h" || a == "--help") {
            ExtractUsage(prog);
            return 0;
        } else if (a == "-o") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "wescene-test extract: -o requires a value\n");
                return 2;
            }
            out_file = argv[++i];
        } else if (StartsWith(a, "-")) {
            std::fprintf(stderr, "wescene-test extract: unknown flag '%.*s'\n",
                         (int)a.size(), a.data());
            return 2;
        } else if (pkg_arg.empty()) {
            pkg_arg = a;
        } else if (asset_arg.empty()) {
            asset_arg = a;
        } else {
            std::fprintf(stderr, "wescene-test extract: unexpected positional '%.*s'\n",
                         (int)a.size(), a.data());
            return 2;
        }
    }

    if (pkg_arg.empty()) {
        ExtractUsage(prog);
        return 2;
    }

    const std::string pkg_path = ResolvePkgPath(pkg_arg);
    if (pkg_path.empty()) {
        std::fprintf(stderr, "wescene-test extract: '%s' is not a scene.pkg or directory containing one\n",
                     pkg_arg.c_str());
        return 1;
    }

    std::string                          version_stamp;
    std::vector<owe::testing::PkgEntry>  entries;
    if (! owe::testing::ReadPkgHeader(pkg_path, version_stamp, entries)) {
        std::fprintf(stderr, "wescene-test extract: ReadPkgHeader failed on %s\n",
                     pkg_path.c_str());
        return 1;
    }

    // No asset path → list entries (sorted, one per line).
    if (asset_arg.empty()) {
        std::vector<std::string> paths;
        paths.reserve(entries.size());
        for (const auto& e : entries) paths.push_back(e.path);
        std::sort(paths.begin(), paths.end());
        std::fprintf(stderr, "wescene-test extract: %s (%s, %zu entries)\n",
                     pkg_path.c_str(), version_stamp.c_str(), paths.size());
        for (const auto& p : paths) std::fprintf(stdout, "%s\n", p.c_str());
        return 0;
    }

    const std::string in_pkg_path = NormalisePkgAssetPath(asset_arg);
    const std::string vfs_path    = "/assets" + in_pkg_path;

    owe::fs::VFS vfs;
    auto         wfs = owe::fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! wfs) {
        std::fprintf(stderr, "wescene-test extract: WPPkgFs::CreatePkgFs failed on %s\n",
                     pkg_path.c_str());
        return 1;
    }
    vfs.Mount("/assets", std::move(wfs));

    auto stream = vfs.Open(vfs_path);
    if (! stream) {
        std::fprintf(stderr, "wescene-test extract: '%s' not found in pkg\n",
                     in_pkg_path.c_str());
        return 1;
    }

    const std::string body = stream->ReadAllStr();

    FILE* out = nullptr;
    bool  close_out = false;
    if (out_file.empty() || out_file == "-") {
        out = stdout;
    } else {
        out = std::fopen(out_file.c_str(), "wb");
        if (! out) {
            std::fprintf(stderr, "wescene-test extract: cannot open '%s' for writing\n",
                         out_file.c_str());
            return 1;
        }
        close_out = true;
    }

    const size_t n = std::fwrite(body.data(), 1, body.size(), out);
    if (close_out) std::fclose(out);
    if (n != body.size()) {
        std::fprintf(stderr, "wescene-test extract: short write (%zu of %zu bytes)\n",
                     n, body.size());
        return 1;
    }

    if (! out_file.empty() && out_file != "-") {
        std::fprintf(stderr, "wescene-test extract: wrote %zu bytes to %s\n",
                     body.size(), out_file.c_str());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// rendergraph subcommand
// ---------------------------------------------------------------------------

void RendergraphUsage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s rendergraph <pkg> [-o FILE]\n"
                 "\n"
                 "  <pkg>     path to scene.pkg, or a directory containing one.\n"
                 "  -o FILE   write to FILE. Default: stdout. Use '-' to force stdout.\n"
                 "\n"
                 "Parses the pkg with WPSceneParser::Parse, then walks the resulting\n"
                 "Scene state to print every pass that sceneToRenderGraph would emit,\n"
                 "in execution order. Reports per pass: shader, output RT, color-write\n"
                 "mask, blend mode, sampled textures. Reports image-effect chains in\n"
                 "their resolved form (after SceneImageEffectLayer::ResolveEffect).\n"
                 "Reports post-process chain (scene.post_processes) verbatim.\n"
                 "\n"
                 "Use this to spot pipeline-shape regressions without running the\n"
                 "renderer end-to-end. Catches things like missing color-write A bit\n"
                 "or post-processes failing to allocate when general.bloom=true.\n",
                 prog ? prog : "wescene-test");
}

const char* BlendModeStr(owe::BlendMode m) {
    switch (m) {
        case owe::BlendMode::Disable:     return "disable";
        case owe::BlendMode::Translucent: return "translucent";
        case owe::BlendMode::Additive:    return "additive";
        case owe::BlendMode::Normal:      return "normal";
    }
    return "?";
}

// Mirrors CustomShaderPass.cpp:294-297. Empty or "global*" cameras strip
// the A_BIT - keep this in sync.
const char* PredictColorMask(std::string_view camera) {
    bool alpha = ! (camera.empty() || StartsWith(camera, "global"));
    return alpha ? "RGBA" : "RGB";
}

void DumpPass(FILE* out, const std::string& tag, const owe::SceneNode& node,
              std::string_view output_rt) {
    auto* mesh = const_cast<owe::SceneNode&>(node).Mesh();
    if (! mesh || ! mesh->Material()) {
        std::fprintf(out, "  %s: <no mesh/material>\n", tag.c_str());
        return;
    }
    auto* material = mesh->Material();
    std::fprintf(out,
                 "  %s shader=%-32s out=%-40s cam=%-12s mask=%s blend=%s\n",
                 tag.c_str(),
                 material->name.empty() ? "?" : material->name.c_str(),
                 std::string(output_rt).c_str(),
                 node.Camera().empty() ? "(empty)" : node.Camera().c_str(),
                 PredictColorMask(node.Camera()),
                 BlendModeStr(material->blenmode));
    if (! material->textures.empty()) {
        std::fprintf(out, "      textures:");
        for (std::size_t i = 0; i < material->textures.size(); ++i) {
            const auto& t = material->textures[i];
            std::fprintf(out, " [%zu]=%s", i, t.empty() ? "(none)" : t.c_str());
        }
        std::fprintf(out, "\n");
    }
}

void DumpRenderTargets(FILE* out, const owe::Scene& scene) {
    std::fprintf(out, "Render targets (%zu):\n", scene.renderTargets.size());
    std::vector<std::string> names;
    names.reserve(scene.renderTargets.size());
    for (const auto& [k, _] : scene.renderTargets) names.push_back(k);
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        const auto& rt = scene.renderTargets.at(n);
        std::fprintf(out,
                     "  %-48s %dx%d  bind=%s%s%s scale=%.3f%s%s\n",
                     n.c_str(),
                     rt.width, rt.height,
                     rt.bind.enable ? "enable " : "",
                     rt.bind.screen ? "screen " : "",
                     rt.bind.name.empty() ? "" : ("name=" + rt.bind.name + " ").c_str(),
                     rt.bind.scale,
                     rt.has_mipmap ? " mipmap" : "",
                     rt.allowReuse ? " reuse" : "");
    }
}

void DumpSceneGraphPasses(FILE* out, owe::Scene& scene) {
    std::fprintf(out, "\nScene-graph passes (TraverseNode pre-order):\n");
    std::function<void(owe::SceneNode*, int)> walk = [&](owe::SceneNode* n, int depth) {
        if (n == nullptr) return;
        // Mirror SceneToRenderGraph::ToGraphPass: only nodes with mesh+material emit.
        auto* mesh = n->Mesh();
        if (mesh && mesh->Material()) {
            std::string tag = "[node id=" + std::to_string(n->ID()) +
                              " depth=" + std::to_string(depth) + "]";
            DumpPass(out, tag, *n, owe::SpecTex_Default);

            // If the node's camera carries an image-effect chain, ResolveEffect
            // and dump the resolved nodes. ResolveEffect mutates the layer; we
            // don't reuse the scene for rendering so this is fine.
            if (! n->Camera().empty() && scene.cameras.count(n->Camera())) {
                auto& cam = scene.cameras.at(n->Camera());
                if (cam->HasImgEffect()) {
                    auto eff_layer = cam->GetImgEffect();
                    eff_layer->ResolveEffect(scene.default_effect_mesh, "effect");
                    std::fprintf(out, "    image-effect chain (%zu effects):\n",
                                 eff_layer->EffectCount());
                    for (std::size_t ei = 0; ei < eff_layer->EffectCount(); ++ei) {
                        auto& eff = eff_layer->GetEffect(ei);
                        std::size_t ni  = 0;
                        for (auto cmd_it = eff->commands.begin(); cmd_it != eff->commands.end(); ++cmd_it) {
                            std::fprintf(out, "      [eff %zu cmd] copy %s -> %s (afterpos=%d)\n",
                                         ei, cmd_it->src.c_str(), cmd_it->dst.c_str(),
                                         cmd_it->afterpos);
                        }
                        for (auto& enode : eff->nodes) {
                            std::string tag2 = "[eff " + std::to_string(ei) +
                                               " node " + std::to_string(ni++) + "]";
                            DumpPass(out, "    " + tag2, *enode.sceneNode, enode.output);
                        }
                    }
                }
            }
        }
        for (auto& child : n->GetChildren()) walk(child.get(), depth + 1);
    };
    walk(scene.sceneGraph.get(), 0);
}

void DumpPostProcesses(FILE* out, const owe::Scene& scene) {
    std::fprintf(out, "\nPost-processes (scene.post_processes, %zu chain%s):\n",
                 scene.post_processes.size(),
                 scene.post_processes.size() == 1 ? "" : "s");
    if (scene.post_processes.empty()) {
        std::fprintf(out, "  (none)\n");
        return;
    }
    for (std::size_t ci = 0; ci < scene.post_processes.size(); ++ci) {
        const auto& pp = *scene.post_processes[ci];
        std::fprintf(out, "  chain[%zu] name=\"%s\" steps=%zu\n",
                     ci, pp.name.c_str(), pp.steps.size());
        for (std::size_t si = 0; si < pp.steps.size(); ++si) {
            const auto& step = pp.steps[si];
            if (auto* sp = std::get_if<owe::ScenePostProcessPass>(&step)) {
                std::string tag = "  [pp " + std::to_string(ci) +
                                  ":" + std::to_string(si) + " draw]";
                DumpPass(out, tag, *sp->node,
                         sp->output.empty() ? std::string(owe::SpecTex_Default) : sp->output);
            } else if (auto* cp = std::get_if<owe::ScenePostProcessCopy>(&step)) {
                std::fprintf(out,
                             "    [pp %zu:%zu copy] %s -> %s\n",
                             ci, si, cp->src.c_str(), cp->dst.c_str());
            }
        }
    }
}

int CmdRendergraph(int argc, char** argv, const char* prog) {
    std::string pkg_arg;
    std::string out_file;
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "-h" || a == "--help") {
            RendergraphUsage(prog);
            return 0;
        } else if (a == "-o") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "wescene-test rendergraph: -o requires a value\n");
                return 2;
            }
            out_file = argv[++i];
        } else if (StartsWith(a, "-")) {
            std::fprintf(stderr, "wescene-test rendergraph: unknown flag '%.*s'\n",
                         (int)a.size(), a.data());
            return 2;
        } else if (pkg_arg.empty()) {
            pkg_arg = a;
        } else {
            std::fprintf(stderr, "wescene-test rendergraph: unexpected positional '%.*s'\n",
                         (int)a.size(), a.data());
            return 2;
        }
    }

    if (pkg_arg.empty()) {
        RendergraphUsage(prog);
        return 2;
    }

    const std::string pkg_path = ResolvePkgPath(pkg_arg);
    if (pkg_path.empty()) {
        std::fprintf(stderr, "wescene-test rendergraph: '%s' is not a scene.pkg or directory containing one\n",
                     pkg_arg.c_str());
        return 1;
    }

    // Mount pkg over a physical-fs fallback (pkg shadows engine assets;
    // matches viewer/daemon).
    owe::fs::VFS vfs;
    if (auto pfs = owe::fs::CreatePhysicalFs(kDefaultAssetsDir)) {
        vfs.Mount("/assets", std::move(pfs));
    }
    auto wfs = owe::fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! wfs) {
        std::fprintf(stderr, "wescene-test rendergraph: WPPkgFs::CreatePkgFs failed on %s\n",
                     pkg_path.c_str());
        return 1;
    }
    vfs.Mount("/assets", std::move(wfs));

    auto stream = vfs.Open("/assets/scene.json");
    if (! stream) {
        std::fprintf(stderr, "wescene-test rendergraph: scene.json not in pkg\n");
        return 1;
    }
    const std::string text = stream->ReadAllStr();

    // Detect pkg version from header so the parser dispatches correctly.
    std::string                          version_stamp;
    std::vector<owe::testing::PkgEntry>  entries;
    if (! owe::testing::ReadPkgHeader(pkg_path, version_stamp, entries)) {
        std::fprintf(stderr, "wescene-test rendergraph: ReadPkgHeader failed on %s\n",
                     pkg_path.c_str());
        return 1;
    }
    auto pkg_v = owe::wpscene::ParsePkgVersionStamp(version_stamp);

    wavsen::audio::SoundManager sm;
    owe::WPSceneParser          parser;
    auto                        scene = parser.Parse(pkg_path, text, vfs, sm, pkg_v);
    if (! scene) {
        std::fprintf(stderr, "wescene-test rendergraph: WPSceneParser::Parse returned null\n");
        return 1;
    }

    FILE* out       = stdout;
    bool  close_out = false;
    if (! out_file.empty() && out_file != "-") {
        out = std::fopen(out_file.c_str(), "wb");
        if (! out) {
            std::fprintf(stderr, "wescene-test rendergraph: cannot open '%s' for writing\n",
                         out_file.c_str());
            return 1;
        }
        close_out = true;
    }

    std::fprintf(out, "wescene-test rendergraph\n");
    std::fprintf(out, "  pkg     : %s\n", pkg_path.c_str());
    std::fprintf(out, "  version : %s\n", version_stamp.c_str());
    std::fprintf(out, "  ortho   : %dx%d\n", scene->ortho[0], scene->ortho[1]);
    std::fprintf(out, "\n");

    DumpRenderTargets(out, *scene);
    DumpSceneGraphPasses(out, *scene);
    DumpPostProcesses(out, *scene);

    if (close_out) std::fclose(out);
    return 0;
}

// ---------------------------------------------------------------------------
// valid subcommand
// ---------------------------------------------------------------------------

void ValidUsage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s valid <workshop_dir> [-o OUT.json]\n"
                 "  Runs DumpWorkshop on one pkg directory and writes a deterministic\n"
                 "  JSON snapshot. With no -o the snapshot goes to stdout.\n",
                 prog ? prog : "wescene-test");
}

int CmdValid(int argc, char** argv, const char* prog) {
    std::string workshop_dir;
    std::string out_file;
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "-o") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "wescene-test valid: -o requires a value\n");
                return 2;
            }
            out_file = argv[++i];
        } else if (a == "-h" || a == "--help") {
            ValidUsage(prog);
            return 0;
        } else if (! a.empty() && a.front() == '-') {
            std::fprintf(stderr, "wescene-test valid: unknown arg '%.*s'\n",
                         (int)a.size(), a.data());
            return 2;
        } else if (workshop_dir.empty()) {
            workshop_dir = a;
        } else {
            std::fprintf(stderr, "wescene-test valid: stray positional '%.*s'\n",
                         (int)a.size(), a.data());
            return 2;
        }
    }
    if (workshop_dir.empty()) {
        ValidUsage(prog);
        return 2;
    }

    std::string err;
    auto        snap = owe::testing::DumpWorkshop(workshop_dir, err);
    if (! err.empty()) {
        std::fprintf(stderr, "wescene-test valid: %s\n", err.c_str());
        return 1;
    }

    const std::string dump = snap.dump(2);
    if (! out_file.empty()) {
        std::ofstream out(out_file);
        if (! out) {
            std::fprintf(stderr, "wescene-test valid: cannot open %s for writing\n",
                         out_file.c_str());
            return 1;
        }
        out << dump << "\n";
    } else {
        std::fwrite(dump.data(), 1, dump.size(), stdout);
        std::fputc('\n', stdout);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    static rstd::log::EnvLogger _logger;
    rstd::log::set_logger(_logger);
    rstd::log::set_max_level(_logger.filter());

    const char* prog = argc > 0 ? argv[0] : "wescene-test";
    if (argc < 2) {
        TopUsage(prog);
        return 2;
    }
    std::string_view sub = argv[1];
    if (sub == "scan")        return CmdScan(argc - 2, argv + 2, prog);
    if (sub == "extract")     return CmdExtract(argc - 2, argv + 2, prog);
    if (sub == "rendergraph") return CmdRendergraph(argc - 2, argv + 2, prog);
    if (sub == "valid")       return CmdValid(argc - 2, argv + 2, prog);
    if (sub == "-h" || sub == "--help") {
        TopUsage(prog);
        return 0;
    }
    std::fprintf(stderr, "wescene-test: unknown subcommand '%.*s'\n",
                 (int)sub.size(), sub.data());
    TopUsage(prog);
    return 2;
}
