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

#include <cstdio>

import rstd.argparse;
import wescene.pkg.parse;
import wescene.json;
import wescene.fs;
import wescene.pkg_fs;
import wescene.scene;
import wescene.spec_names;
import wescene.types;
import wavsen.audio;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.cli;
import wescene.testing.corpus;
import wescene.testing.pkg_header;
import wescene.testing.json_builder;

namespace fs   = std::filesystem;
using Json     = owe::Json;
using JsonSink = rstd::Option<rstd::mut_ref<Json>>;

namespace
{

using rstd::i32;
using rstd::u32;
using rstd::usize;
using rstd::argparse::Arg;
using rstd::argparse::ArgGroup;
using rstd::argparse::ArgKey;
using rstd::argparse::Command;
using rstd::argparse::from_str_parser;
using rstd::argparse::Matches;
using rstd::argparse::NumArgs;
using rstd::argparse::string_parser;
using rstd::string::String;

std::string ToStdString(const String& value) {
    return { value.data(), value.size().to_primitive() };
}

template<typename T>
const T& ArgValue(const Matches& matches, const ArgKey<T>& key) {
    auto value = matches.get_one(key);
    if (value.is_err() || value->is_none()) rstd::unreachable();
    return ***value;
}

bool ArgFlag(const Matches& matches, const ArgKey<bool>& key) {
    auto value = matches.get_one(key);
    if (value.is_err()) rstd::unreachable();
    return value->is_some() && ***value;
}

std::string OptionalArgString(const Matches& matches, const ArgKey<String>& key) {
    auto value = matches.get_one(key);
    if (value.is_err()) rstd::unreachable();
    return value->is_some() ? ToStdString(***value) : std::string {};
}

template<typename T>
std::vector<T> ArgValues(const Matches& matches, const ArgKey<T>& key) {
    std::vector<T> out;
    auto           values = matches.get_many(key);
    if (values.is_err()) rstd::unreachable();
    if (values->is_none()) return out;
    auto iterator = rstd::move(**values);
    while (auto value = iterator.next()) out.push_back(**value);
    return out;
}

std::vector<std::string> ArgStringValues(const Matches& matches, const ArgKey<String>& key) {
    std::vector<std::string> out;
    auto                     values = matches.get_many(key);
    if (values.is_err()) rstd::unreachable();
    if (values->is_none()) return out;
    auto iterator = rstd::move(**values);
    while (auto value = iterator.next()) out.push_back(ToStdString(**value));
    return out;
}

template<typename T>
void SetJsonField(Json& object, std::string_view key, T&& value) {
    owe::SetMember(object, key, std::forward<T>(value));
}

Json StatusJson(bool ok, std::string_view error = {}) {
    auto out = owe::MakeObject();
    SetJsonField(out, "ok", ok);
    if (! error.empty()) SetJsonField(out, "error", error);
    return out;
}

void AppendJson(JsonSink array, Json value) {
    if (array.is_some()) owe::AppendJson(**array, std::move(value));
}

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

std::string RootedPathString(owe::fs::Path path) {
    auto out        = rstd::path::PathBuf::from("/");
    auto components = path.components();
    while (true) {
        auto component = components.next();
        if (component.is_none()) break;
        if ((*component).is_root_dir() || (*component).is_cur_dir()) continue;
        out.push(owe::fs::Path((*component).as_os_str()));
    }
    return owe::fs::ToStdString(out.as_path());
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
    auto input = owe::fs::ToPath(arg);
    auto path  = input.strip_prefix(owe::fs::ToPath("/assets"));
    if (path.is_none()) path = input.strip_prefix(owe::fs::ToPath("assets"));
    return RootedPathString(path.unwrap_or(input));
}

// ---------------------------------------------------------------------------
// scan subcommand
// ---------------------------------------------------------------------------

struct ScanOptions {
    std::string              workshop_dir = kDefaultWorkshopDir;
    std::string              assets_dir   = kDefaultAssetsDir;
    bool                     p_tex { false };
    bool                     p_shader { false };
    bool                     p_mdl { false };      // header-only by default
    bool                     p_mdl_full { false }; // upgrade to full WPMdlParser::Parse
    bool                     full { false };
    std::vector<std::string> name_filters;
    std::vector<unsigned>    pkgv_filters;
    int                      limit { 0 };
    int                      offset { 0 };
    bool                     quiet { false };
    bool                     stop_on_fail { false };
    std::string              json_out; // empty = no single JSON file (--json)
    std::string              json_dir; // empty = no per-workshop dump dir (--json-dir)
};

struct ScanArgs {
    ArgKey<String> workshop_dir;
    ArgKey<String> assets;
    ArgKey<bool>   full;
    ArgKey<bool>   parse_tex;
    ArgKey<bool>   parse_shader;
    ArgKey<bool>   parse_mdl;
    ArgKey<bool>   parse_mdl_full;
    ArgKey<bool>   parse_all;
    ArgKey<String> name;
    ArgKey<u32>    pkgv;
    ArgKey<i32>    limit;
    ArgKey<i32>    offset;
    ArgKey<bool>   quiet;
    ArgKey<bool>   stop_on_fail;
    ArgKey<String> json;
    ArgKey<String> json_dir;
};

ScanArgs AddScanArgs(Command& command) {
    command.about("Walk pkgs and run scene parse, optionally with per-asset passes.");
    auto workshop_dir = command.add_arg(
        Arg<String>::value("workshop-dir", string_parser())
            .long_name("workshop-dir")
            .default_value(kDefaultWorkshopDir)
            .help("workshop root (children are <id>/scene.pkg) or a single pkg dir"));
    auto assets = command.add_arg(Arg<String>::value("assets", string_parser())
                                      .long_name("assets")
                                      .default_value(kDefaultAssetsDir)
                                      .help("shared engine assets, mounted at /assets fallback"));
    auto full   = command.add_arg(Arg<bool>::flag("full").long_name("full").help(
        "run full WPSceneParser::Parse (shader compile, bloom inject, glslang) "
        "instead of the cheap FromJson+ExpandObjects+AdjustAuto base"));
    auto parse_tex =
        command.add_arg(Arg<bool>::flag("parse-tex")
                            .long_name("parse-tex")
                            .help("run WPTexImageParser on every /materials/**/*.tex"));
    auto parse_shader = command.add_arg(
        Arg<bool>::flag("parse-shader")
            .long_name("parse-shader")
            .help("run WPShaderParser::CompileMaterialShader on every /materials/**/*.json"));
    auto parse_mdl = command.add_arg(
        Arg<bool>::flag("parse-mdl")
            .long_name("parse-mdl")
            .help("run WPMdlParser::ParseHeader on every /models/**/*.mdl (header-only)"));
    auto parse_mdl_full = command.add_arg(
        Arg<bool>::flag("parse-mdl-full")
            .long_name("parse-mdl-full")
            .help("upgrade --parse-mdl to full WPMdlParser::Parse (slow; some hang/reject)"));
    auto parse_all =
        command.add_arg(Arg<bool>::flag("parse-all")
                            .long_name("parse-all")
                            .help("--parse-tex + --parse-shader + --parse-mdl (header-only)"));
    auto name = command.add_arg(
        Arg<String>::value("name", string_parser())
            .long_name("name")
            .append()
            .help("only pkgs whose dir name contains SUBSTR (ci); repeatable, OR'd"));
    auto pkgv  = command.add_arg(Arg<u32>::value("pkgv", from_str_parser<u32>())
                                     .long_name("pkgv")
                                     .append()
                                     .help("only pkgs with PKGV stamp == N; repeatable, OR'd"));
    auto limit = command.add_arg(Arg<i32>::value("limit", from_str_parser<i32>())
                                     .long_name("limit")
                                     .default_value("0")
                                     .help("stop after N matched pkgs (default 0 = all)"));
    auto offset =
        command.add_arg(Arg<i32>::value("offset", from_str_parser<i32>())
                            .long_name("offset")
                            .default_value("0")
                            .help("skip the first N matched pkgs before --limit applies"));
    auto quiet        = command.add_arg(Arg<bool>::flag("quiet").long_name("quiet").help(
        "suppress per-asset OK lines; only FAIL + summary"));
    auto stop_on_fail = command.add_arg(
        Arg<bool>::flag("stop-on-fail")
            .long_name("stop-on-fail")
            .help("exit non-zero on the first per-asset failure (resume at next --offset)"));
    auto json =
        command.add_arg(Arg<String>::value("json", string_parser())
                            .long_name("json")
                            .help("write structured validator results to FILE (pkgs[] + summary)"));
    auto json_dir =
        command.add_arg(Arg<String>::value("json-dir", string_parser())
                            .long_name("json-dir")
                            .help("write per-workshop DumpWorkshop snapshots to DIR/<id>.json"));
    command.add_group(ArgGroup::make("json-output").arg(json).arg(json_dir).multiple(false));
    return { workshop_dir,   assets,       full, parse_tex, parse_shader, parse_mdl,
             parse_mdl_full, parse_all,    name, pkgv,      limit,        offset,
             quiet,          stop_on_fail, json, json_dir };
}

ScanOptions ReadScanOptions(const Matches& matches, const ScanArgs& args) {
    ScanOptions opt;
    opt.workshop_dir = ToStdString(ArgValue(matches, args.workshop_dir));
    opt.assets_dir   = ToStdString(ArgValue(matches, args.assets));
    const bool all   = ArgFlag(matches, args.parse_all);
    opt.full         = ArgFlag(matches, args.full);
    opt.p_mdl_full   = ArgFlag(matches, args.parse_mdl_full);
    opt.p_tex        = all || ArgFlag(matches, args.parse_tex);
    opt.p_shader     = all || ArgFlag(matches, args.parse_shader);
    opt.p_mdl        = all || opt.p_mdl_full || ArgFlag(matches, args.parse_mdl);
    opt.name_filters = ArgStringValues(matches, args.name);
    for (auto value : ArgValues(matches, args.pkgv)) {
        opt.pkgv_filters.push_back(value.to_primitive());
    }
    opt.limit        = ArgValue(matches, args.limit).to_primitive();
    opt.offset       = ArgValue(matches, args.offset).to_primitive();
    opt.quiet        = ArgFlag(matches, args.quiet);
    opt.stop_on_fail = ArgFlag(matches, args.stop_on_fail);
    opt.json_out     = OptionalArgString(matches, args.json);
    opt.json_dir     = OptionalArgString(matches, args.json_dir);
    return opt;
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
    auto stream = owe::fs::OpenBinary(vfs, "/assets/scene.json");
    if (stream.is_err()) {
        err = "scene.json not in pkg";
        return false;
    }
    const std::string text   = stream->ReadAllStr();
    auto              parsed = owe::ParseJson(text);
    if (parsed.is_err()) {
        err = "scene.json parse failed";
        return false;
    }
    auto                        j = parsed.unwrap();
    owe::wpscene::SceneMetadata sc;
    if (! sc.FromJson(j, pkg_v)) {
        err = "SceneMetadata::FromJson returned false";
        return false;
    }
    auto scene_objs = owe::ExpandObjects(j, vfs, pkg_v);
    (void)owe::ResolveOrthoProjectionExtent(sc, scene_objs);
    (void)scene_objs;
    return true;
}

// Runs the full WPSceneParser::Parse pipeline: scene parse + per-image
// shader compile + bloom auto-injection + scene-graph allocation.
// SoundManager is default-constructed but never Init()'d so Sound
// objects parse without opening an audio device.
bool RunSceneParseFull(owe::fs::VFS& vfs, owe::wpscene::SceneVersion pkg_v,
                       const std::string& pkg_id, std::string& err) {
    auto stream = owe::fs::OpenBinary(vfs, "/assets/scene.json");
    if (stream.is_err()) {
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

void ValidateTextures(const std::vector<owe::testing::PkgEntry>& entries, owe::fs::VFS& vfs,
                      const std::string& pkg_id, Counters& c, bool quiet, const JsonSink& sink) {
    owe::WPTexImageParser      parser(&vfs);
    constexpr std::string_view prefix = "/materials/";
    constexpr std::string_view suffix = ".tex";
    for (const auto& e : entries) {
        if (! StartsWith(e.path, prefix) || ! EndsWith(e.path, suffix)) continue;
        if (e.path.size() < prefix.size() + suffix.size()) continue;
        // ParseHeader takes the bare name (no /materials/ prefix, no .tex
        // suffix), matching Material.textures shape.
        const std::string name =
            e.path.substr(prefix.size(), e.path.size() - prefix.size() - suffix.size());
        bool        ok    = false;
        bool        video = false;
        std::string err;
        try {
            owe::ImageHeader h = parser.ParseHeader(name);
            ok                 = (h.width > 0 && h.height > 0);
            video              = (h.type == owe::ImageType::VIDEO);
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
                    std::fprintf(stdout,
                                 "OK    %s tex %s (video container)\n",
                                 pkg_id.c_str(),
                                 e.path.c_str());
                } else {
                    std::fprintf(stdout, "OK    %s tex %s\n", pkg_id.c_str(), e.path.c_str());
                }
            }
        } else {
            ++c.tex_fail;
            std::fprintf(
                stdout, "FAIL  %s tex %s  %s\n", pkg_id.c_str(), e.path.c_str(), err.c_str());
        }
        if (sink) {
            auto entry = owe::MakeObject();
            SetJsonField(entry, "path", e.path);
            SetJsonField(entry, "ok", ok);
            if (ok) {
                if (video) SetJsonField(entry, "video", true);
            } else {
                SetJsonField(entry, "error", err);
            }
            AppendJson(sink, std::move(entry));
        }
    }
}

void ValidateShaders(const std::vector<owe::testing::PkgEntry>& entries, owe::fs::VFS& vfs,
                     const std::string& pkg_id, Counters& c, bool quiet, const JsonSink& sink) {
    for (const auto& e : entries) {
        if (! StartsWith(e.path, "/materials/") || ! EndsWith(e.path, ".json")) continue;
        const std::string vfs_path = "/assets" + e.path;
        auto              stream   = owe::fs::OpenBinary(vfs, vfs_path);
        if (stream.is_err()) {
            ++c.shader_fail;
            std::fprintf(
                stdout, "FAIL  %s shader %s  cannot open\n", pkg_id.c_str(), e.path.c_str());
            if (sink) {
                auto entry = StatusJson(false, "cannot open");
                SetJsonField(entry, "path", e.path);
                AppendJson(sink, std::move(entry));
            }
            continue;
        }
        const std::string text   = stream->ReadAllStr();
        auto              parsed = owe::ParseJson(text);
        if (parsed.is_err()) {
            ++c.shader_fail;
            std::fprintf(
                stdout, "FAIL  %s shader %s  invalid JSON\n", pkg_id.c_str(), e.path.c_str());
            if (sink) {
                auto entry = StatusJson(false, "invalid JSON");
                SetJsonField(entry, "path", e.path);
                AppendJson(sink, std::move(entry));
            }
            continue;
        }
        auto                             jmat = parsed.unwrap();
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
                std::fprintf(stdout,
                             "OK    %s shader %s [%s]\n",
                             pkg_id.c_str(),
                             e.path.c_str(),
                             r.shader_name.c_str());
        } else {
            ++c.shader_fail;
            std::fprintf(stdout,
                         "FAIL  %s shader %s [%s]  %s\n",
                         pkg_id.c_str(),
                         e.path.c_str(),
                         r.shader_name.c_str(),
                         r.error.c_str());
        }
        if (sink) {
            auto entry = StatusJson(r.ok, r.ok ? std::string_view {} : std::string_view(r.error));
            SetJsonField(entry, "path", e.path);
            SetJsonField(entry, "shader_name", r.shader_name);
            AppendJson(sink, std::move(entry));
        }
    }
}

void ValidateMdls(const std::vector<owe::testing::PkgEntry>& entries, owe::fs::VFS& vfs,
                  const std::string& pkg_id, Counters& c, bool quiet, const JsonSink& sink) {
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
            if (! quiet) std::fprintf(stdout, "OK    %s mdl %s\n", pkg_id.c_str(), e.path.c_str());
        } else {
            ++c.mdl_fail;
            std::fprintf(
                stdout, "FAIL  %s mdl %s  %s\n", pkg_id.c_str(), e.path.c_str(), err.c_str());
        }
        if (sink) {
            auto entry = StatusJson(ok, ok ? std::string_view {} : std::string_view(err));
            SetJsonField(entry, "path", e.path);
            AppendJson(sink, std::move(entry));
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

void ValidateMdlsHeader(const std::vector<owe::testing::PkgEntry>& entries, owe::fs::VFS& vfs,
                        const std::string& pkg_id, Counters& c, bool quiet, const JsonSink& sink) {
    for (const auto& e : entries) {
        if (! EndsWith(e.path, ".mdl")) continue;
        std::string      name(e.path.substr(1));
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
                std::fprintf(stdout,
                             "OK    %s mdl-header %s  mdlv=%d mesh=%u flag=%s\n",
                             pkg_id.c_str(),
                             e.path.c_str(),
                             h.mdlv,
                             h.mesh_count,
                             FormatMdlFlag(h.mdl_flag).c_str());
        } else {
            ++c.mdl_fail;
            std::fprintf(stdout,
                         "FAIL  %s mdl-header %s  %s\n",
                         pkg_id.c_str(),
                         e.path.c_str(),
                         err.c_str());
        }
        if (sink) {
            auto entry = StatusJson(ok, ok ? std::string_view {} : std::string_view(err));
            SetJsonField(entry, "path", e.path);
            if (ok) {
                SetJsonField(entry, "mdlv", h.mdlv);
                SetJsonField(entry, "mesh_count", h.mesh_count);
                auto flag_arr = owe::MakeArray();
                for (int byte_idx = 0; byte_idx < 4; ++byte_idx) {
                    uint8_t     b = static_cast<uint8_t>((h.mdl_flag >> (byte_idx * 8)) & 0xFFu);
                    std::string bits(8, '0');
                    for (int i = 0; i < 8; ++i)
                        if (b & (1u << (7 - i))) bits[i] = '1';
                    owe::AppendElement(flag_arr, std::move(bits));
                }
                owe::SetJson(entry, "flag", std::move(flag_arr));
            }
            AppendJson(sink, std::move(entry));
        }
    }
}

bool ProcessOnePkg(const fs::path& pkg_dir, const ScanOptions& opt, Counters& c,
                   const JsonSink& pkgs_arr) {
    const std::string pkg_id = pkg_dir.filename().string();

    for (const auto* sk : kSkipIds) {
        if (pkg_id == sk) {
            std::fprintf(stderr, "SKIP  %s (in kSkipIds)\n", pkg_id.c_str());
            if (pkgs_arr.is_some()) {
                auto entry = owe::MakeObject();
                SetJsonField(entry, "id", pkg_id);
                SetJsonField(entry, "skipped", true);
                AppendJson(pkgs_arr, std::move(entry));
            }
            return true;
        }
    }

    const std::string pkg_path = (pkg_dir / "scene.pkg").string();
    if (! fs::exists(pkg_path)) {
        ++c.parsed_fail;
        std::fprintf(stdout, "FAIL  %s parse  scene.pkg not found\n", pkg_id.c_str());
        if (pkgs_arr.is_some()) {
            auto entry = owe::MakeObject();
            SetJsonField(entry, "id", pkg_id);
            owe::SetJson(entry, "parse", StatusJson(false, "scene.pkg not found"));
            AppendJson(pkgs_arr, std::move(entry));
        }
        return false;
    }

    std::string                         version_stamp;
    std::vector<owe::testing::PkgEntry> entries;
    if (! owe::testing::ReadPkgHeader(pkg_path, version_stamp, entries)) {
        ++c.parsed_fail;
        std::fprintf(stdout, "FAIL  %s parse  ReadPkgHeader\n", pkg_id.c_str());
        if (pkgs_arr.is_some()) {
            auto entry = owe::MakeObject();
            SetJsonField(entry, "id", pkg_id);
            owe::SetJson(entry, "parse", StatusJson(false, "ReadPkgHeader"));
            AppendJson(pkgs_arr, std::move(entry));
        }
        return false;
    }
    const auto pkg_v = owe::wpscene::ParsePkgVersionStamp(version_stamp);

    if (! MatchesPkgvFilters((unsigned)pkg_v, opt.pkgv_filters)) return true;

    owe::fs::VFS vfs;
    if (! opt.assets_dir.empty()) {
        auto pfs = owe::fs::make_physical_fs(owe::fs::ToPath(opt.assets_dir));
        if (pfs.is_ok()) {
            (void)vfs.mount("/assets", std::move(pfs).unwrap_unchecked());
        }
    }
    auto wfs = owe::fs::WPPkgFs::open(owe::fs::ToPath(pkg_path));
    if (wfs.is_err()) {
        ++c.parsed_fail;
        std::fprintf(stdout, "FAIL  %s parse  WPPkgFs::open\n", pkg_id.c_str());
        if (pkgs_arr.is_some()) {
            auto entry = owe::MakeObject();
            SetJsonField(entry, "id", pkg_id);
            SetJsonField(entry, "pkg_version", (unsigned)pkg_v);
            owe::SetJson(entry, "parse", StatusJson(false, "WPPkgFs::open"));
            AppendJson(pkgs_arr, std::move(entry));
        }
        return false;
    }
    (void)vfs.mount("/assets", wfs->mount_handle());

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
        std::fprintf(stdout,
                     "FAIL  %s parse  v=%u  %s\n",
                     pkg_id.c_str(),
                     (unsigned)pkg_v,
                     parse_err.c_str());
    }

    auto     pkg_obj     = owe::MakeObject();
    JsonSink tex_sink    = rstd::None();
    JsonSink shader_sink = rstd::None();
    JsonSink mdl_sink    = rstd::None();
    if (pkgs_arr.is_some()) {
        SetJsonField(pkg_obj, "id", pkg_id);
        SetJsonField(pkg_obj, "pkg_version", (unsigned)pkg_v);
        owe::SetJson(
            pkg_obj,
            "parse",
            StatusJson(parse_ok, parse_ok ? std::string_view {} : std::string_view(parse_err)));
        if (opt.p_tex) {
            owe::SetJson(pkg_obj, "textures", owe::MakeArray());
            tex_sink = pkg_obj.get_mut("textures");
        }
        if (opt.p_shader) {
            owe::SetJson(pkg_obj, "shaders", owe::MakeArray());
            shader_sink = pkg_obj.get_mut("shaders");
        }
        if (opt.p_mdl) {
            const char* key = opt.p_mdl_full ? "mdls" : "mdls_header";
            owe::SetJson(pkg_obj, key, owe::MakeArray());
            mdl_sink = pkg_obj.get_mut(key);
        }
    }

    if (opt.p_tex) ValidateTextures(entries, vfs, pkg_id, c, opt.quiet, tex_sink);
    if (opt.p_shader) ValidateShaders(entries, vfs, pkg_id, c, opt.quiet, shader_sink);
    if (opt.p_mdl) {
        if (opt.p_mdl_full)
            ValidateMdls(entries, vfs, pkg_id, c, opt.quiet, mdl_sink);
        else
            ValidateMdlsHeader(entries, vfs, pkg_id, c, opt.quiet, mdl_sink);
    }

    AppendJson(pkgs_arr, std::move(pkg_obj));

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
        Json        snap = owe::testing::DumpWorkshop(pkg_dir.string(), derr, df);
        if (! derr.empty()) {
            snap = owe::MakeObject();
            SetJsonField(snap, "workshop_dir", pkg_id);
            SetJsonField(snap, "error", derr);
        }
        const auto    out_path = fs::path(opt.json_dir) / (pkg_id + ".json");
        std::ofstream ofs(out_path);
        if (ofs)
            ofs << owe::Dump(snap, 2) << "\n";
        else
            std::fprintf(stderr, "wescene-test scan: cannot write %s\n", out_path.string().c_str());
    }

    return parse_ok;
}

int CmdScan(const ScanOptions& opt) {
    if (! fs::exists(opt.workshop_dir) || ! fs::is_directory(opt.workshop_dir)) {
        std::fprintf(
            stderr, "wescene-test scan: %s is not a directory\n", opt.workshop_dir.c_str());
        return 1;
    }

    if (! opt.json_dir.empty()) {
        std::error_code ec;
        fs::create_directories(opt.json_dir, ec);
        if (ec || ! fs::is_directory(opt.json_dir)) {
            std::fprintf(stderr,
                         "wescene-test scan: cannot create --json-dir '%s': %s\n",
                         opt.json_dir.c_str(),
                         ec.message().c_str());
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
            if ((size_t)opt.offset >= dirs.size())
                dirs.clear();
            else
                dirs.erase(dirs.begin(), dirs.begin() + opt.offset);
        }
        if (opt.limit > 0 && (int)dirs.size() > opt.limit) {
            dirs.resize((size_t)opt.limit);
        }
    }

    if (dirs.empty()) {
        std::fprintf(stderr, "wescene-test scan: no matching pkgs\n");
        return 1;
    }

    auto     doc      = owe::MakeObject();
    JsonSink pkgs_arr = rstd::None();
    if (! opt.json_out.empty()) {
        owe::SetJson(doc, "pkgs", owe::MakeArray());
        pkgs_arr = doc.get_mut("pkgs");
    }

    Counters c;
    auto     t0 = std::chrono::steady_clock::now();

    size_t processed = 0;
    for (const auto& d : dirs) {
        ProcessOnePkg(d, opt, c, pkgs_arr);
        ++processed;
        if (opt.stop_on_fail && (c.parsed_fail + c.tex_fail + c.shader_fail + c.mdl_fail) > 0) {
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
                 dirs.size(),
                 c.parsed_ok,
                 c.parsed_ok + c.parsed_fail);
    if (opt.p_tex) std::fprintf(stderr, " | tex %d/%d", c.tex_ok, c.tex_ok + c.tex_fail);
    if (opt.p_shader)
        std::fprintf(stderr, " | shader %d/%d", c.shader_ok, c.shader_ok + c.shader_fail);
    if (opt.p_mdl)
        std::fprintf(stderr,
                     " | %s %d/%d",
                     opt.p_mdl_full ? "mdl" : "mdl-header",
                     c.mdl_ok,
                     c.mdl_ok + c.mdl_fail);
    std::fprintf(stderr, " | %lldms\n", (long long)ms);

    if (pkgs_arr.is_some()) {
        auto summary = owe::MakeObject();
        SetJsonField(summary, "pkgs", dirs.size());
        SetJsonField(summary, "ms", (long long)ms);
        auto parse_summary = owe::MakeObject();
        SetJsonField(parse_summary, "ok", c.parsed_ok);
        SetJsonField(parse_summary, "fail", c.parsed_fail);
        owe::SetJson(summary, "parse", std::move(parse_summary));
        auto add_summary = [&summary](std::string_view key, int ok, int fail) {
            auto value = owe::MakeObject();
            SetJsonField(value, "ok", ok);
            SetJsonField(value, "fail", fail);
            owe::SetJson(summary, key, std::move(value));
        };
        if (opt.p_tex) add_summary("tex", c.tex_ok, c.tex_fail);
        if (opt.p_shader) add_summary("shader", c.shader_ok, c.shader_fail);
        if (opt.p_mdl) {
            const char* key = opt.p_mdl_full ? "mdl" : "mdl-header";
            add_summary(key, c.mdl_ok, c.mdl_fail);
        }
        owe::SetJson(doc, "summary", std::move(summary));

        std::ofstream out(opt.json_out);
        if (! out) {
            std::fprintf(
                stderr, "wescene-test scan: cannot open --json file '%s'\n", opt.json_out.c_str());
            return 1;
        }
        out << owe::Dump(doc, 2) << "\n";
    }

    const int total_fail = c.parsed_fail + c.tex_fail + c.shader_fail + c.mdl_fail;
    return total_fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// extract subcommand
// ---------------------------------------------------------------------------

struct ExtractArgs {
    ArgKey<String> pkg;
    ArgKey<String> asset;
    ArgKey<String> output;
};

ExtractArgs AddExtractArgs(Command& command) {
    command.about("List entries of a single scene.pkg, or export one asset to stdout/-o FILE.");
    auto pkg   = command.add_arg(Arg<String>::value("pkg", string_parser())
                                     .value_name("PKG")
                                     .help("path to scene.pkg, or a directory containing one")
                                     .required());
    auto asset = command.add_arg(
        Arg<String>::value("asset", string_parser())
            .value_name("ASSET")
            .num_args(NumArgs::optional())
            .default_value("")
            .help("in-pkg path to extract (e.g. /scene.json). Omit to list all entries."));
    auto output = command.add_arg(Arg<String>::value("output", string_parser())
                                      .short_name('o')
                                      .long_name("output")
                                      .default_value("")
                                      .help("write to FILE (default stdout; '-' forces stdout)"));
    return { pkg, asset, output };
}

int CmdExtract(const Matches& matches, const ExtractArgs& args) {
    const std::string pkg_arg   = ToStdString(ArgValue(matches, args.pkg));
    const std::string asset_arg = ToStdString(ArgValue(matches, args.asset));
    const std::string out_file  = ToStdString(ArgValue(matches, args.output));

    const std::string pkg_path = ResolvePkgPath(pkg_arg);
    if (pkg_path.empty()) {
        std::fprintf(stderr,
                     "wescene-test extract: '%s' is not a scene.pkg or directory containing one\n",
                     pkg_arg.c_str());
        return 1;
    }

    std::string                         version_stamp;
    std::vector<owe::testing::PkgEntry> entries;
    if (! owe::testing::ReadPkgHeader(pkg_path, version_stamp, entries)) {
        std::fprintf(
            stderr, "wescene-test extract: ReadPkgHeader failed on %s\n", pkg_path.c_str());
        return 1;
    }

    // No asset path → list entries (sorted, one per line).
    if (asset_arg.empty()) {
        std::vector<std::string> paths;
        paths.reserve(entries.size());
        for (const auto& e : entries) paths.push_back(e.path);
        std::sort(paths.begin(), paths.end());
        std::fprintf(stderr,
                     "wescene-test extract: %s (%s, %zu entries)\n",
                     pkg_path.c_str(),
                     version_stamp.c_str(),
                     paths.size());
        for (const auto& p : paths) std::fprintf(stdout, "%s\n", p.c_str());
        return 0;
    }

    const std::string in_pkg_path = NormalisePkgAssetPath(asset_arg);
    const std::string vfs_path    = "/assets" + in_pkg_path;

    owe::fs::VFS vfs;
    auto         wfs = owe::fs::WPPkgFs::open(owe::fs::ToPath(pkg_path));
    if (wfs.is_err()) {
        std::fprintf(
            stderr, "wescene-test extract: WPPkgFs::open failed on %s\n", pkg_path.c_str());
        return 1;
    }
    (void)vfs.mount("/assets", wfs->mount_handle());

    auto stream = owe::fs::OpenBinary(vfs, vfs_path);
    if (stream.is_err()) {
        std::fprintf(stderr, "wescene-test extract: '%s' not found in pkg\n", in_pkg_path.c_str());
        return 1;
    }

    const std::string body = stream->ReadAllStr();

    FILE* out       = nullptr;
    bool  close_out = false;
    if (out_file.empty() || out_file == "-") {
        out = stdout;
    } else {
        out = std::fopen(out_file.c_str(), "wb");
        if (! out) {
            std::fprintf(
                stderr, "wescene-test extract: cannot open '%s' for writing\n", out_file.c_str());
            return 1;
        }
        close_out = true;
    }

    const size_t n = std::fwrite(body.data(), 1, body.size(), out);
    if (close_out) std::fclose(out);
    if (n != body.size()) {
        std::fprintf(
            stderr, "wescene-test extract: short write (%zu of %zu bytes)\n", n, body.size());
        return 1;
    }

    if (! out_file.empty() && out_file != "-") {
        std::fprintf(
            stderr, "wescene-test extract: wrote %zu bytes to %s\n", body.size(), out_file.c_str());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// grep subcommand
// ---------------------------------------------------------------------------

struct GrepOptions {
    enum class Mode
    {
        Context,
        Only,
        Files,
        Count
    };

    std::string              workshop_dir = kDefaultWorkshopDir;
    std::string              pattern;
    std::vector<std::string> name_filters;
    std::string              path_filter;          // substring (ci); empty = ".json" suffix
    bool                     search_all { false }; // search every entry, not just .json
    bool                     icase { false };
    int                      limit { 0 };
    int                      offset { 0 };
    Mode                     mode { Mode::Context };
    bool                     json_out { false };
    int                      snippet { 48 }; // flat-mode char window (no -A/-B/-C)
    int                      before { 0 };   // -B: lines of leading context
    int                      after { 0 };    // -A: lines of trailing context

    bool line_context() const { return before > 0 || after > 0; }
};

struct GrepArgs {
    ArgKey<String> pattern;
    ArgKey<String> workshop_dir;
    ArgKey<String> name;
    ArgKey<i32>    limit;
    ArgKey<i32>    offset;
    ArgKey<String> path_filter;
    ArgKey<bool>   all_text;
    ArgKey<bool>   ignore_case;
    ArgKey<i32>    snippet;
    ArgKey<i32>    after;
    ArgKey<i32>    before;
    ArgKey<i32>    around;
    ArgKey<bool>   json;
    ArgKey<bool>   only_matching;
    ArgKey<bool>   files_with_matches;
    ArgKey<bool>   count;
};

GrepArgs AddGrepArgs(Command& command) {
    command.about("Regex-search (ECMAScript / std::regex) the text entries inside each pkg's "
                  "VFS. By default only .json entries are searched (scene.json + every "
                  "materials/**.json + effects/**.json).");
    auto pattern      = command.add_arg(Arg<String>::value("pattern", string_parser())
                                            .value_name("PATTERN")
                                            .help("ECMAScript / std::regex pattern")
                                            .required());
    auto workshop_dir = command.add_arg(
        Arg<String>::value("workshop-dir", string_parser())
            .long_name("workshop-dir")
            .default_value(kDefaultWorkshopDir)
            .help("workshop root (children are <id>/scene.pkg) or a single pkg dir"));
    auto name = command.add_arg(
        Arg<String>::value("name", string_parser())
            .long_name("name")
            .append()
            .help("only pkgs whose dir name contains SUBSTR (ci); repeatable, OR'd"));
    auto limit = command.add_arg(Arg<i32>::value("limit", from_str_parser<i32>())
                                     .long_name("limit")
                                     .default_value("0")
                                     .help("stop after N matched pkgs (default 0 = all)"));
    auto offset =
        command.add_arg(Arg<i32>::value("offset", from_str_parser<i32>())
                            .long_name("offset")
                            .default_value("0")
                            .help("skip the first N matched pkgs before --limit applies"));
    auto path_filter = command.add_arg(
        Arg<String>::value("path-filter", string_parser())
            .long_name("path-filter")
            .default_value("")
            .help("only search in-pkg paths containing SUBSTR (ci); overrides .json gate"));
    auto all_text = command.add_arg(
        Arg<bool>::flag("all-text")
            .long_name("all-text")
            .help("search every entry, not just .json (combine with --path-filter)"));
    auto ignore_case = command.add_arg(Arg<bool>::flag("ignore-case")
                                           .short_name('i')
                                           .long_name("ignore-case")
                                           .help("case-insensitive match"));
    auto snippet =
        command.add_arg(Arg<i32>::value("snippet", from_str_parser<i32>())
                            .long_name("snippet")
                            .default_value("48")
                            .help("chars of context around a match in the flat default mode "
                                  "(ignored when -A/-B/-C is set)"));
    auto after = command.add_arg(
        Arg<i32>::value("after", from_str_parser<i32>())
            .short_name('A')
            .long_name("after")
            .default_value("0")
            .help("print N lines after each match (grep -A); switches default mode to "
                  "line context with <path>:<lineno>: prefixes"));
    auto before = command.add_arg(Arg<i32>::value("before", from_str_parser<i32>())
                                      .short_name('B')
                                      .long_name("before")
                                      .default_value("0")
                                      .help("print N lines before each match (grep -B)"));
    auto around = command.add_arg(Arg<i32>::value("around", from_str_parser<i32>())
                                      .short_name('C')
                                      .long_name("around")
                                      .default_value("0")
                                      .help("print N lines before AND after each match (grep -C)"));
    auto json   = command.add_arg(Arg<bool>::flag("json").long_name("json").help(
        "structured array of {id, path, matches[]}"));
    auto only_matching =
        command.add_arg(Arg<bool>::flag("only-matching")
                            .short_name('o')
                            .long_name("only-matching")
                            .help("print only the matched substring, one per line"));
    auto files_with_matches =
        command.add_arg(Arg<bool>::flag("files-with-matches")
                            .short_name('l')
                            .long_name("files-with-matches")
                            .help("print <id>\\t<path> once per matching entry"));
    auto count = command.add_arg(Arg<bool>::flag("count").short_name('c').long_name("count").help(
        "print <id>\\t<count> per pkg with at least one match"));
    command.add_group(ArgGroup::make("output-mode")
                          .arg(only_matching)
                          .arg(files_with_matches)
                          .arg(count)
                          .multiple(false));
    return { pattern,     workshop_dir, name,        limit,         offset,
             path_filter, all_text,     ignore_case, snippet,       after,
             before,      around,       json,        only_matching, files_with_matches,
             count };
}

GrepOptions ReadGrepOptions(const Matches& matches, const GrepArgs& args) {
    GrepOptions opt;
    opt.pattern      = ToStdString(ArgValue(matches, args.pattern));
    opt.workshop_dir = ToStdString(ArgValue(matches, args.workshop_dir));
    opt.name_filters = ArgStringValues(matches, args.name);
    opt.path_filter  = ToStdString(ArgValue(matches, args.path_filter));
    opt.search_all   = ArgFlag(matches, args.all_text);
    opt.icase        = ArgFlag(matches, args.ignore_case);
    opt.limit        = ArgValue(matches, args.limit).to_primitive();
    opt.offset       = ArgValue(matches, args.offset).to_primitive();
    opt.snippet      = ArgValue(matches, args.snippet).to_primitive();
    const int around = ArgValue(matches, args.around).to_primitive();
    opt.before       = std::max<int>(ArgValue(matches, args.before).to_primitive(), around);
    opt.after        = std::max<int>(ArgValue(matches, args.after).to_primitive(), around);
    opt.json_out     = ArgFlag(matches, args.json);
    if (ArgFlag(matches, args.only_matching))
        opt.mode = GrepOptions::Mode::Only;
    else if (ArgFlag(matches, args.files_with_matches))
        opt.mode = GrepOptions::Mode::Files;
    else if (ArgFlag(matches, args.count))
        opt.mode = GrepOptions::Mode::Count;
    return opt;
}

// Decide whether an in-pkg entry path is in scope for grep.
bool GrepWantPath(const std::string& path, const GrepOptions& opt) {
    const std::string lo = LowerCopy(path);
    if (! opt.path_filter.empty() && lo.find(LowerCopy(opt.path_filter)) == std::string::npos)
        return false;
    if (opt.search_all) return true;
    if (! opt.path_filter.empty()) return true; // explicit filter overrides .json default
    return EndsWith(lo, ".json");
}

// One match's surrounding window, flattened to a single line.
std::string GrepContext(const std::string& text, std::size_t pos, std::size_t len, int ctx) {
    const std::size_t start = pos > (std::size_t)ctx ? pos - (std::size_t)ctx : 0;
    const std::size_t end   = std::min(text.size(), pos + len + (std::size_t)ctx);
    std::string       s     = text.substr(start, end - start);
    for (auto& ch : s)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    return s;
}

// Max chars of a single source line printed in line-context mode. WE pkg JSON
// is mostly pretty-printed; the cap only bites on minified scene.json or
// inlined "script" strings, where the flat default mode is the better tool.
constexpr int kGrepLineCap = 400;

// Byte offset where each line starts; starts[i] = first byte of line i.
std::vector<std::size_t> GrepLineStarts(const std::string& text) {
    std::vector<std::size_t> starts { 0 };
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') starts.push_back(i + 1);
    return starts;
}

// 0-based index of the line containing byte position pos.
std::size_t GrepLineOf(const std::vector<std::size_t>& starts, std::size_t pos) {
    auto it = std::upper_bound(starts.begin(), starts.end(), pos);
    return (std::size_t)(it - starts.begin()) - 1;
}

// Line `idx` text, newline/CR stripped and capped to kGrepLineCap chars.
std::string GrepLineText(const std::string& text, const std::vector<std::size_t>& starts,
                         std::size_t idx) {
    const std::size_t s = starts[idx];
    std::size_t       e = (idx + 1 < starts.size()) ? starts[idx + 1] - 1 : text.size();
    if (e > s && text[e - 1] == '\r') --e;
    std::string line = text.substr(s, e - s);
    if (line.size() > (std::size_t)kGrepLineCap)
        line = line.substr(0, (std::size_t)kGrepLineCap) + " …";
    return line;
}

// grep -A/-B/-C style emission: filename + line numbers + surrounding lines.
// Match lines use a ':' separator, context lines '-'; overlapping windows are
// merged and distinct windows separated by a "--" line (as in grep).
void GrepPrintLineContext(const std::string& pkg_id, const std::string& path,
                          const std::string& text, const std::vector<std::size_t>& match_pos,
                          const GrepOptions& opt) {
    const auto    starts = GrepLineStarts(text);
    const int     nlines = (int)starts.size();
    std::set<int> match_lines;
    for (auto p : match_pos) match_lines.insert((int)GrepLineOf(starts, p));

    std::vector<std::pair<int, int>> ranges; // inclusive [lo, hi] line indices
    for (int ml : match_lines) {             // std::set iterates in order
        const int lo = std::max(0, ml - opt.before);
        const int hi = std::min(nlines - 1, ml + opt.after);
        if (! ranges.empty() && lo <= ranges.back().second + 1)
            ranges.back().second = std::max(ranges.back().second, hi);
        else
            ranges.push_back({ lo, hi });
    }
    for (std::size_t ri = 0; ri < ranges.size(); ++ri) {
        if (ri) std::fprintf(stdout, "--\n");
        for (int ln = ranges[ri].first; ln <= ranges[ri].second; ++ln) {
            const char sep = match_lines.count(ln) ? ':' : '-';
            std::fprintf(stdout,
                         "%s\t%s%c%d%c %s\n",
                         pkg_id.c_str(),
                         path.c_str(),
                         sep,
                         ln + 1,
                         sep,
                         GrepLineText(text, starts, (std::size_t)ln).c_str());
        }
    }
}

// Collect pkg dirs under workshop_dir applying name filters + offset/limit.
// Mirrors CmdScan's selection so grep and scan agree on which pkgs match.
std::vector<fs::path> CollectGrepPkgs(const GrepOptions& opt) {
    std::vector<fs::path> dirs;
    if (fs::exists(fs::path(opt.workshop_dir) / "scene.pkg")) {
        dirs.push_back(opt.workshop_dir);
        return dirs;
    }
    for (auto& e : fs::directory_iterator(opt.workshop_dir)) {
        if (! e.is_directory()) continue;
        if (! fs::exists(e.path() / "scene.pkg")) continue;
        if (! MatchesNameFilters(e.path().filename().string(), opt.name_filters)) continue;
        dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());
    if (opt.offset > 0) {
        if ((size_t)opt.offset >= dirs.size())
            dirs.clear();
        else
            dirs.erase(dirs.begin(), dirs.begin() + opt.offset);
    }
    if (opt.limit > 0 && (int)dirs.size() > opt.limit) dirs.resize((size_t)opt.limit);
    return dirs;
}

int CmdGrep(const GrepOptions& opt) {
    if (! fs::exists(opt.workshop_dir) || ! fs::is_directory(opt.workshop_dir)) {
        std::fprintf(
            stderr, "wescene-test grep: %s is not a directory\n", opt.workshop_dir.c_str());
        return 1;
    }

    std::regex re;
    try {
        auto flags = std::regex::ECMAScript;
        if (opt.icase) flags |= std::regex::icase;
        re = std::regex(opt.pattern, flags);
    } catch (const std::regex_error& ex) {
        std::fprintf(stderr, "wescene-test grep: bad pattern: %s\n", ex.what());
        return 2;
    }

    const auto dirs = CollectGrepPkgs(opt);
    if (dirs.empty()) {
        std::fprintf(stderr, "wescene-test grep: no matching pkgs\n");
        return 1;
    }

    auto doc     = owe::MakeArray();
    long n_match = 0;
    int  n_files = 0;
    int  n_pkgs  = 0;

    for (const auto& d : dirs) {
        const std::string pkg_id   = d.filename().string();
        const std::string pkg_path = (d / "scene.pkg").string();

        std::string                         version_stamp;
        std::vector<owe::testing::PkgEntry> entries;
        if (! owe::testing::ReadPkgHeader(pkg_path, version_stamp, entries)) {
            std::fprintf(stderr, "wescene-test grep: ReadPkgHeader failed on %s\n", pkg_id.c_str());
            continue;
        }

        owe::fs::VFS vfs;
        auto         wfs = owe::fs::WPPkgFs::open(owe::fs::ToPath(pkg_path));
        if (wfs.is_err()) {
            std::fprintf(stderr, "wescene-test grep: WPPkgFs::open failed on %s\n", pkg_id.c_str());
            continue;
        }
        (void)vfs.mount("/assets", wfs->mount_handle());

        std::vector<std::string> paths;
        paths.reserve(entries.size());
        for (const auto& e : entries)
            if (GrepWantPath(e.path, opt)) paths.push_back(e.path);
        std::sort(paths.begin(), paths.end());

        int pkg_count = 0;
        for (const auto& p : paths) {
            auto stream = owe::fs::OpenBinary(vfs, "/assets" + p);
            if (stream.is_err()) continue;
            const std::string text = stream->ReadAllStr();

            std::vector<std::pair<std::size_t, std::size_t>> matches; // (pos, len)
            for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
                 it != std::sregex_iterator();
                 ++it)
                matches.emplace_back((std::size_t)it->position(), (std::size_t)it->length());
            if (matches.empty()) continue;

            n_match += (long)matches.size();
            pkg_count += (int)matches.size();
            ++n_files;

            if (opt.json_out) {
                auto file_matches = owe::MakeArray();
                for (const auto& [pos, len] : matches)
                    owe::AppendElement(file_matches, GrepContext(text, pos, len, opt.snippet));
                auto entry = owe::MakeObject();
                SetJsonField(entry, "id", pkg_id);
                SetJsonField(entry, "path", p);
                owe::SetJson(entry, "matches", std::move(file_matches));
                owe::AppendJson(doc, std::move(entry));
                continue;
            }

            switch (opt.mode) {
            case GrepOptions::Mode::Context:
                if (opt.line_context()) {
                    std::vector<std::size_t> mp;
                    mp.reserve(matches.size());
                    for (const auto& [pos, len] : matches) mp.push_back(pos);
                    GrepPrintLineContext(pkg_id, p, text, mp, opt);
                } else {
                    for (const auto& [pos, len] : matches)
                        std::fprintf(stdout,
                                     "%s\t%s\t%s\n",
                                     pkg_id.c_str(),
                                     p.c_str(),
                                     GrepContext(text, pos, len, opt.snippet).c_str());
                }
                break;
            case GrepOptions::Mode::Only:
                for (const auto& [pos, len] : matches)
                    std::fprintf(stdout, "%s\n", text.substr(pos, len).c_str());
                break;
            case GrepOptions::Mode::Files:
                std::fprintf(stdout, "%s\t%s\n", pkg_id.c_str(), p.c_str());
                break;
            case GrepOptions::Mode::Count: break; // accumulated, printed per pkg below
            }
        }

        if (pkg_count > 0) {
            ++n_pkgs;
            if (! opt.json_out && opt.mode == GrepOptions::Mode::Count)
                std::fprintf(stdout, "%s\t%d\n", pkg_id.c_str(), pkg_count);
        }
    }

    if (opt.json_out) {
        const auto dump = owe::Dump(doc, 2);
        std::fprintf(stdout, "%s\n", dump.c_str());
    }
    std::fprintf(stderr,
                 "wescene-test grep: %ld match%s in %d file%s across %d/%zu pkg%s\n",
                 n_match,
                 n_match == 1 ? "" : "es",
                 n_files,
                 n_files == 1 ? "" : "s",
                 n_pkgs,
                 dirs.size(),
                 dirs.size() == 1 ? "" : "s");
    return n_match > 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// rendergraph subcommand
// ---------------------------------------------------------------------------

struct RendergraphArgs {
    ArgKey<String> pkg;
    ArgKey<String> output;
};

RendergraphArgs AddRendergraphArgs(Command& command) {
    command.about("Parse one pkg with WPSceneParser::Parse and print every pass "
                  "sceneToRenderGraph would emit, in execution order: shader, output RT, "
                  "color-write mask, blend mode, sampled textures, resolved image-effect "
                  "chains, and the post-process chain. No Vulkan; pure structural dump.");
    auto pkg    = command.add_arg(Arg<String>::value("pkg", string_parser())
                                      .value_name("PKG")
                                      .help("path to scene.pkg, or a directory containing one")
                                      .required());
    auto output = command.add_arg(Arg<String>::value("output", string_parser())
                                      .short_name('o')
                                      .long_name("output")
                                      .default_value("")
                                      .help("write to FILE (default stdout; '-' forces stdout)"));
    return { pkg, output };
}

const char* BlendModeStr(owe::BlendMode m) {
    switch (m) {
    case owe::BlendMode::Disable: return "disable";
    case owe::BlendMode::Translucent: return "translucent";
    case owe::BlendMode::Additive: return "additive";
    case owe::BlendMode::Normal: return "normal";
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
                     rt.width,
                     rt.height,
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
            std::string tag = "[node id=" + std::to_string(n->ID().to_primitive()) +
                              " depth=" + std::to_string(depth) + "]";
            std::string output { owe::SpecTex_Default };
            std::shared_ptr<owe::SceneImageEffectLayer> eff_layer;
            if (! n->Camera().empty() && scene.cameras.count(n->Camera())) {
                auto& cam = scene.cameras.at(n->Camera());
                if (cam->HasImgEffect()) {
                    eff_layer = cam->GetImgEffect();
                    if (eff_layer->EffectCount() == usize() ||
                        eff_layer->HasRuntimeVisibleEffect()) {
                        output = eff_layer->FirstTarget();
                    }
                }
            }
            if (eff_layer) {
                std::size_t ni = 0;
                for (auto& enode : eff_layer->PrefillNodes()) {
                    std::string tag2 = "[prefill node " + std::to_string(ni++) + "]";
                    DumpPass(out, "    " + tag2, *enode.sceneNode.as_ptr(), enode.output);
                }
            }
            DumpPass(out, tag, *n, output);

            if (eff_layer && eff_layer->HasRuntimeVisibleEffect()) {
                eff_layer->ResolveEffect(scene.default_effect_mesh, "effect");
                const usize effect_count = eff_layer->EffectCount();
                std::fprintf(
                    out, "    image-effect chain (%zu effects):\n", effect_count.to_primitive());
                for (usize ei {}; ei < effect_count; ++ei) {
                    auto&       eff = eff_layer->GetEffect(ei);
                    std::size_t ni  = 0;
                    for (auto cmd_it = eff->commands.begin(); cmd_it != eff->commands.end();
                         ++cmd_it) {
                        std::fprintf(out,
                                     "      [eff %zu cmd] copy %s -> %s (afterpos=%d)\n",
                                     ei.to_primitive(),
                                     cmd_it->src.c_str(),
                                     cmd_it->dst.c_str(),
                                     cmd_it->afterpos.to_primitive());
                    }
                    for (auto& enode : eff->nodes) {
                        std::string tag2 = "[eff " + std::to_string(ei.to_primitive()) + " node " +
                                           std::to_string(ni++) + "]";
                        DumpPass(out, "    " + tag2, *enode.sceneNode.as_ptr(), enode.output);
                    }
                }
            }
        }
        for (auto& child : n->GetChildren()) walk(child.as_ptr(), depth + 1);
    };
    walk(scene.sceneGraph.as_ptr(), 0);
}

void DumpPostProcesses(FILE* out, const owe::Scene& scene) {
    std::fprintf(out,
                 "\nPost-processes (scene.post_processes, %zu chain%s):\n",
                 scene.post_processes.size(),
                 scene.post_processes.size() == 1 ? "" : "s");
    if (scene.post_processes.empty()) {
        std::fprintf(out, "  (none)\n");
        return;
    }
    for (std::size_t ci = 0; ci < scene.post_processes.size(); ++ci) {
        const auto& pp = *scene.post_processes[ci];
        std::fprintf(
            out, "  chain[%zu] name=\"%s\" steps=%zu\n", ci, pp.name.c_str(), pp.steps.size());
        for (std::size_t si = 0; si < pp.steps.size(); ++si) {
            const auto& step = pp.steps[si];
            if (auto* sp = std::get_if<owe::ScenePostProcessPass>(&step)) {
                std::string tag =
                    "  [pp " + std::to_string(ci) + ":" + std::to_string(si) + " draw]";
                DumpPass(out,
                         tag,
                         *sp->node.as_ptr(),
                         sp->output.empty() ? std::string(owe::SpecTex_Default) : sp->output);
            } else if (auto* cp = std::get_if<owe::ScenePostProcessCopy>(&step)) {
                std::fprintf(out,
                             "    [pp %zu:%zu copy] %s -> %s\n",
                             ci,
                             si,
                             cp->src.c_str(),
                             cp->dst.c_str());
            }
        }
    }
}

int CmdRendergraph(const Matches& matches, const RendergraphArgs& args) {
    const std::string pkg_arg  = ToStdString(ArgValue(matches, args.pkg));
    const std::string out_file = ToStdString(ArgValue(matches, args.output));

    const std::string pkg_path = ResolvePkgPath(pkg_arg);
    if (pkg_path.empty()) {
        std::fprintf(
            stderr,
            "wescene-test rendergraph: '%s' is not a scene.pkg or directory containing one\n",
            pkg_arg.c_str());
        return 1;
    }

    // Mount pkg over a physical-fs fallback (pkg shadows engine assets;
    // matches viewer/daemon).
    owe::fs::VFS vfs;
    auto         pfs = owe::fs::make_physical_fs(owe::fs::ToPath(kDefaultAssetsDir));
    if (pfs.is_ok()) {
        (void)vfs.mount("/assets", std::move(pfs).unwrap_unchecked());
    }
    auto wfs = owe::fs::WPPkgFs::open(owe::fs::ToPath(pkg_path));
    if (wfs.is_err()) {
        std::fprintf(
            stderr, "wescene-test rendergraph: WPPkgFs::open failed on %s\n", pkg_path.c_str());
        return 1;
    }
    (void)vfs.mount("/assets", wfs->mount_handle());

    auto stream = owe::fs::OpenBinary(vfs, "/assets/scene.json");
    if (stream.is_err()) {
        std::fprintf(stderr, "wescene-test rendergraph: scene.json not in pkg\n");
        return 1;
    }
    const std::string text = stream->ReadAllStr();

    // Detect pkg version from header so the parser dispatches correctly.
    std::string                         version_stamp;
    std::vector<owe::testing::PkgEntry> entries;
    if (! owe::testing::ReadPkgHeader(pkg_path, version_stamp, entries)) {
        std::fprintf(
            stderr, "wescene-test rendergraph: ReadPkgHeader failed on %s\n", pkg_path.c_str());
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
            std::fprintf(stderr,
                         "wescene-test rendergraph: cannot open '%s' for writing\n",
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

struct ValidArgs {
    ArgKey<String> workshop_dir;
    ArgKey<String> output;
};

ValidArgs AddValidArgs(Command& command) {
    command.about("Run DumpWorkshop on one pkg directory and write a deterministic JSON "
                  "snapshot (used to regenerate the checked-in fixtures under tests/fixtures/).");
    auto workshop_dir = command.add_arg(Arg<String>::value("workshop_dir", string_parser())
                                            .value_name("WORKSHOP_DIR")
                                            .help("pkg directory to snapshot")
                                            .required());
    auto output       = command.add_arg(Arg<String>::value("output", string_parser())
                                            .short_name('o')
                                            .long_name("output")
                                            .default_value("")
                                            .help("write snapshot to FILE (default stdout)"));
    return { workshop_dir, output };
}

int CmdValid(const Matches& matches, const ValidArgs& args) {
    const std::string workshop_dir = ToStdString(ArgValue(matches, args.workshop_dir));
    const std::string out_file     = ToStdString(ArgValue(matches, args.output));

    std::string err;
    auto        snap = owe::testing::DumpWorkshop(workshop_dir, err);
    if (! err.empty()) {
        std::fprintf(stderr, "wescene-test valid: %s\n", err.c_str());
        return 1;
    }

    const std::string dump = owe::Dump(snap, 2);
    if (! out_file.empty()) {
        std::ofstream out(out_file);
        if (! out) {
            std::fprintf(
                stderr, "wescene-test valid: cannot open %s for writing\n", out_file.c_str());
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

    auto program = Command::make("wescene-test");
    program.version("1.0");
    program.about("Consolidated CLI for the wescene-renderer test surface. Host-only: no Vulkan "
                  "device, no GLFW.");
    program.require_subcommand();

    auto scan             = Command::make("scan");
    auto scan_args        = AddScanArgs(scan);
    auto extract          = Command::make("extract");
    auto extract_args     = AddExtractArgs(extract);
    auto grep             = Command::make("grep");
    auto grep_args        = AddGrepArgs(grep);
    auto rendergraph      = Command::make("rendergraph");
    auto rendergraph_args = AddRendergraphArgs(rendergraph);
    auto valid            = Command::make("valid");
    auto valid_args       = AddValidArgs(valid);

    program.add_subcommand(rstd::move(scan));
    program.add_subcommand(rstd::move(extract));
    program.add_subcommand(rstd::move(grep));
    program.add_subcommand(rstd::move(rendergraph));
    program.add_subcommand(rstd::move(valid));

    auto parsed = owe::cli::ParseArgs(rstd::move(program), argc, argv);
    if (parsed.is_err()) return parsed.unwrap_err().code;
    auto matches = rstd::move(parsed).unwrap();

    if (auto child = matches.subcommand_matches("scan"); child.is_some())
        return CmdScan(ReadScanOptions(**child, scan_args));
    if (auto child = matches.subcommand_matches("extract"); child.is_some())
        return CmdExtract(**child, extract_args);
    if (auto child = matches.subcommand_matches("grep"); child.is_some())
        return CmdGrep(ReadGrepOptions(**child, grep_args));
    if (auto child = matches.subcommand_matches("rendergraph"); child.is_some())
        return CmdRendergraph(**child, rendergraph_args);
    if (auto child = matches.subcommand_matches("valid"); child.is_some())
        return CmdValid(**child, valid_args);
    return 2;
}
