// wpscriptdump [--workshop-dir DIR] [--output OUT_DIR] [--write-js] [--max N]
//
// Output (under OUT_DIR, default /tmp/wpscript):
//   scripts.jsonl   one record per binding, see schema below
//   report.json     totals, per-field distribution, (field, paramset)
//                   clusters, api_surface (engine.* / createScriptProperties
//                   builder methods / exports counted across deduped sources)
//   scripts/<sha>.js   present iff --write-js
//
// Per-record schema (one JSON object per line in scripts.jsonl):
//   { "workshop": "...", "json_pointer": "/objects/3/text",
//     "object_name": "Clock", "field": "text", "script_sha1": "deadbeef...",
//     "value_initial": <the JSON 'value' sibling>,
//     "scriptproperties": {...},
//     "user": "<binding-name>"|null }
//
// CLI:
//   --workshop-dir DIR   default WAYWALLEN_WORKSHOP_DIR (CMake-injected)
//   --output OUT_DIR     default /tmp/wpscript; created if missing
//   --write-js           emit deduped sources under <OUT_DIR>/scripts/
//   --max N              stop after the first N pkgs (debugging)
//   -h / --help          print usage

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "Utils/Sha.hpp"
#include "pkg_header.hpp"

import wescene.fs;
import wescene.pkg_fs;

namespace {

namespace fs = std::filesystem;
using json   = nlohmann::json;

constexpr const char* kDefaultWorkshopDir =
#ifdef WAYWALLEN_WORKSHOP_DIR
    WAYWALLEN_WORKSHOP_DIR
#else
    "workshop"
#endif
    ;

void Usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--workshop-dir DIR] [--output OUT_DIR] [--write-js]"
                 " [--max N]\n"
                 "  Walks every scene.pkg under DIR, finds every per-field\n"
                 "  {\"script\":..., \"scriptproperties\":...} binding inside\n"
                 "  scene.json, and writes scripts.jsonl + report.json to OUT_DIR.\n"
                 "  --write-js   also dump deduped JS sources to <OUT_DIR>/scripts/<sha>.js\n",
                 prog ? prog : "wpscriptdump");
}

// --- record + cluster types ---------------------------------------------------

struct Record {
    std::string workshop;
    std::string json_pointer;
    std::string object_name;  // empty if no enclosing named object
    std::string field;
    std::string sha;
    json        value_initial;
    json        scriptproperties;
    std::string user_binding;  // empty if none
};

// Cluster key: (bound field, sorted scriptproperties top-level keys).
// Two scripts with the same (field, paramset) almost always do the same
// thing — this is the empirical "WE built-in script kind" we expose.
struct ClusterKey {
    std::string              field;
    std::vector<std::string> param_keys;
    bool                     operator<(const ClusterKey& o) const {
        if (field != o.field) return field < o.field;
        return param_keys < o.param_keys;
    }
};

struct ClusterAcc {
    int                                 instances { 0 };
    std::unordered_map<std::string, int> shas;        // sha -> count
    std::unordered_map<std::string, int> workshops;   // ws -> count
};

// --- JSON pointer helpers -----------------------------------------------------

std::string EscapeForJsonPointer(std::string_view s) {
    // RFC 6901: ~ -> ~0, / -> ~1
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '~')      out += "~0";
        else if (c == '/') out += "~1";
        else               out += c;
    }
    return out;
}

// --- API surface extraction ---------------------------------------------------

struct ApiSurface {
    std::map<std::string, int> engine_calls;
    std::map<std::string, int> script_property_reads;
    std::map<std::string, int> builder_methods;
    std::map<std::string, int> exports;
};

void ExtractApi(const std::string& src, ApiSurface& s) {
    static const std::regex re_engine(R"(\bengine\.([A-Za-z_][A-Za-z0-9_]*))");
    static const std::regex re_sp(R"(\bscriptProperties\.([A-Za-z_][A-Za-z0-9_]*))");
    static const std::regex re_builder(R"(\.add([A-Z][A-Za-z]*)\b)");
    static const std::regex re_export(
        R"(\bexport\s+(?:function|var|const|let|default)\s+([A-Za-z_][A-Za-z0-9_]*))");
    auto count = [&](const std::regex& re, std::map<std::string, int>& dst) {
        // Use string::const_iterator-based iteration so we can advance past
        // each match rather than reset on suffix.
        std::sregex_iterator it { src.begin(), src.end(), re };
        std::sregex_iterator end;
        for (; it != end; ++it) ++dst[(*it)[1].str()];
    };
    count(re_engine, s.engine_calls);
    count(re_sp, s.script_property_reads);
    count(re_builder, s.builder_methods);
    count(re_export, s.exports);
}

// --- recursive scene.json walker ---------------------------------------------

struct WalkCtx {
    std::vector<Record>&                            records;
    std::unordered_map<std::string, std::string>&   sha_to_source;
    const std::string&                              workshop;
};

void WalkNode(const json& n, const std::string& ptr,
              const std::string& field_name, const std::string& object_name,
              WalkCtx& ctx) {
    if (n.is_object()) {
        // A "scripted field binding" node has a string `script` field.
        // Companion fields seen in the corpus: `value`, `scriptproperties`,
        // `user`, `animation`. We capture all of them.
        auto it_script = n.find("script");
        if (it_script != n.end() && it_script->is_string()) {
            Record r;
            r.workshop     = ctx.workshop;
            r.json_pointer = ptr;
            r.object_name  = object_name;
            r.field        = field_name;
            r.sha          = utils::genSha1(std::span<const char>(
                it_script->get_ref<const std::string&>()));
            if (auto v = n.find("value"); v != n.end())             r.value_initial = *v;
            if (auto v = n.find("scriptproperties"); v != n.end())  r.scriptproperties = *v;
            if (auto v = n.find("user"); v != n.end() && v->is_string())
                r.user_binding = v->get<std::string>();
            ctx.sha_to_source.try_emplace(r.sha,
                                          it_script->get_ref<const std::string&>());
            ctx.records.push_back(std::move(r));
            // Don't recurse into `script` (huge string); but the
            // `scriptproperties` block could in principle nest more
            // bindings (we haven't seen any in the corpus, but cheap to
            // descend anyway in case it does — minus the script string).
            for (auto it = n.begin(); it != n.end(); ++it) {
                if (it.key() == "script") continue;
                WalkNode(*it, ptr + "/" + EscapeForJsonPointer(it.key()),
                         it.key(), object_name, ctx);
            }
            return;
        }

        // Update the "nearest enclosing named object" if this dict has
        // a string `name`.
        std::string child_object_name = object_name;
        if (auto it_name = n.find("name");
            it_name != n.end() && it_name->is_string()) {
            child_object_name = it_name->get<std::string>();
        }
        for (auto it = n.begin(); it != n.end(); ++it) {
            WalkNode(*it, ptr + "/" + EscapeForJsonPointer(it.key()),
                     it.key(), child_object_name, ctx);
        }
    } else if (n.is_array()) {
        for (std::size_t i = 0; i < n.size(); ++i) {
            // Field name carries through array indices: scripts in the
            // corpus always sit on a named object field (text, scale, ...),
            // and the array indices are just JSON pointer noise.
            WalkNode(n[i], ptr + "/" + std::to_string(i), field_name,
                     object_name, ctx);
        }
    }
    // Strings/numbers/bools: terminal, ignore.
}

// --- one workshop ------------------------------------------------------------

bool ProcessOnePkg(const std::string& workshop_id, const std::string& pkg_path,
                   std::vector<Record>&                            out,
                   std::unordered_map<std::string, std::string>& sha_to_source) {
    std::string                                 pkg_version;
    std::vector<wallpaper::testing::PkgEntry>   entries;
    if (! wallpaper::testing::ReadPkgHeader(pkg_path, pkg_version, entries))
        return false;

    auto wfs = wallpaper::fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! wfs) return false;
    wallpaper::fs::VFS vfs;
    vfs.Mount("/assets", std::move(wfs));

    auto stream = vfs.Open("/assets/scene.json");
    if (! stream) return false;

    std::string text = stream->ReadAllStr();
    json        scene;
    try {
        scene = json::parse(text);
    } catch (const std::exception&) {
        return false;
    }

    WalkCtx ctx { out, sha_to_source, workshop_id };
    WalkNode(scene, "", "", "", ctx);

    // Bonus: any *.js entries in the pkg VFS itself. Currently the corpus
    // has zero, but the renderer will need to handle pkg-resident JS once
    // WE drops it that way, and surfacing it now is essentially free.
    for (const auto& e : entries) {
        if (e.path.size() < 3) continue;
        std::string_view sv(e.path);
        if (sv.size() < 3) continue;
        if (sv.substr(sv.size() - 3) != ".js") continue;
        auto js_stream = vfs.Open("/assets" + e.path);
        if (! js_stream) continue;
        std::string                                 src = js_stream->ReadAllStr();
        Record                                      r;
        r.workshop     = workshop_id;
        r.json_pointer = std::string("pkg:") + e.path;
        r.field        = "<pkg-file>";
        r.sha          = utils::genSha1(std::span<const char>(src));
        sha_to_source.try_emplace(r.sha, std::move(src));
        out.push_back(std::move(r));
    }
    return true;
}

// --- main --------------------------------------------------------------------

}  // namespace

int main(int argc, char** argv) {
    std::string workshop_dir = kDefaultWorkshopDir;
    std::string output_dir   = "/tmp/wpscript";
    bool        write_js     = false;
    int         max_pkgs     = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--workshop-dir") == 0 && i + 1 < argc) {
            workshop_dir = argv[++i];
        } else if (std::strcmp(a, "--output") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (std::strcmp(a, "--write-js") == 0) {
            write_js = true;
        } else if (std::strcmp(a, "--max") == 0 && i + 1 < argc) {
            max_pkgs = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            Usage(argv[0]);
            return 0;
        } else {
            Usage(argv[0]);
            return 2;
        }
    }

    std::error_code ec;
    fs::create_directories(output_dir, ec);
    if (write_js) fs::create_directories(output_dir + "/scripts", ec);

    std::ofstream jsonl(output_dir + "/scripts.jsonl");
    if (! jsonl) {
        std::fprintf(stderr, "wpscriptdump: cannot open %s/scripts.jsonl\n",
                     output_dir.c_str());
        return 1;
    }

    std::vector<Record>                          records;
    std::unordered_map<std::string, std::string> sha_to_source;
    int pkgs_seen = 0, pkgs_with_scripts = 0;

    std::vector<fs::path> workshops;
    for (const auto& ent : fs::directory_iterator(workshop_dir, ec)) {
        if (! ent.is_directory()) continue;
        if (! fs::exists(ent.path() / "scene.pkg")) continue;
        workshops.push_back(ent.path());
    }
    std::sort(workshops.begin(), workshops.end());

    for (const auto& wpath : workshops) {
        if (max_pkgs > 0 && pkgs_seen >= max_pkgs) break;
        const std::string ws_id    = wpath.filename().string();
        const std::string pkg_path = (wpath / "scene.pkg").string();
        ++pkgs_seen;
        std::size_t before = records.size();
        if (! ProcessOnePkg(ws_id, pkg_path, records, sha_to_source)) continue;
        if (records.size() > before) ++pkgs_with_scripts;
    }

    // Emit per-record JSONL. Doing it after the sweep keeps the per-record
    // (sha→source) dedup simple and lets us write the JS files only once.
    for (const auto& r : records) {
        json line = {
            { "workshop", r.workshop },
            { "json_pointer", r.json_pointer },
            { "object_name", r.object_name },
            { "field", r.field },
            { "script_sha1", r.sha },
            { "value_initial", r.value_initial },
            { "scriptproperties", r.scriptproperties },
            { "user", r.user_binding.empty() ? json(nullptr) : json(r.user_binding) },
        };
        jsonl << line.dump() << '\n';
    }
    jsonl.close();

    // Write deduped JS sources.
    if (write_js) {
        for (const auto& [sha, src] : sha_to_source) {
            std::ofstream out(output_dir + "/scripts/" + sha + ".js");
            if (out) out << src;
        }
    }

    // Aggregate clusters by (field, sorted-paramset).
    std::map<ClusterKey, ClusterAcc> clusters;
    std::map<std::string, int>       field_distribution;
    for (const auto& r : records) {
        ++field_distribution[r.field];
        ClusterKey ck;
        ck.field = r.field;
        if (r.scriptproperties.is_object()) {
            for (auto it = r.scriptproperties.begin();
                 it != r.scriptproperties.end(); ++it)
                ck.param_keys.push_back(it.key());
            std::sort(ck.param_keys.begin(), ck.param_keys.end());
        }
        auto& acc = clusters[ck];
        ++acc.instances;
        ++acc.shas[r.sha];
        ++acc.workshops[r.workshop];
    }

    // API-surface scan over deduped sources only (cheap, ~2877 strings).
    ApiSurface api;
    for (const auto& [_sha, src] : sha_to_source) ExtractApi(src, api);

    // Build the report JSON.
    json report;
    report["totals"] = {
        { "pkgs_seen", pkgs_seen },
        { "pkgs_with_scripts", pkgs_with_scripts },
        { "bindings", static_cast<int>(records.size()) },
        { "unique_sources", static_cast<int>(sha_to_source.size()) },
    };
    report["field_distribution"] = field_distribution;

    json jclusters = json::array();
    // Sort clusters by instance count, descending, for human readability.
    std::vector<const std::pair<const ClusterKey, ClusterAcc>*> sorted_clusters;
    sorted_clusters.reserve(clusters.size());
    for (const auto& kv : clusters) sorted_clusters.push_back(&kv);
    std::sort(sorted_clusters.begin(), sorted_clusters.end(),
              [](auto* a, auto* b) {
                  return a->second.instances > b->second.instances;
              });
    for (const auto* kv : sorted_clusters) {
        const auto& [ck, acc] = *kv;
        std::string top_sha;
        int         top_sha_count = 0;
        for (const auto& [s, c] : acc.shas)
            if (c > top_sha_count) { top_sha = s; top_sha_count = c; }
        std::vector<std::pair<std::string, int>> ws_sorted(acc.workshops.begin(),
                                                            acc.workshops.end());
        std::sort(ws_sorted.begin(), ws_sorted.end(),
                  [](auto& a, auto& b) { return a.second > b.second; });
        json top_workshops = json::array();
        for (std::size_t i = 0; i < ws_sorted.size() && i < 5; ++i)
            top_workshops.push_back(ws_sorted[i].first);
        jclusters.push_back({
            { "field", ck.field },
            { "param_keys", ck.param_keys },
            { "instances", acc.instances },
            { "distinct_sources", static_cast<int>(acc.shas.size()) },
            { "top_sha", top_sha },
            { "top_workshops", top_workshops },
        });
    }
    report["clusters"]    = std::move(jclusters);
    report["api_surface"] = {
        { "engine_calls", api.engine_calls },
        { "script_property_reads", api.script_property_reads },
        { "createScriptProperties_methods", api.builder_methods },
        { "exports", api.exports },
    };

    std::ofstream rpt(output_dir + "/report.json");
    if (! rpt) {
        std::fprintf(stderr, "wpscriptdump: cannot open %s/report.json\n",
                     output_dir.c_str());
        return 1;
    }
    rpt << report.dump(2) << '\n';
    rpt.close();

    std::fprintf(stderr,
                 "wpscriptdump: %d pkgs, %d with scripts, %zu bindings, %zu unique"
                 " sources, %zu clusters → %s/{scripts.jsonl,report.json%s}\n",
                 pkgs_seen, pkgs_with_scripts, records.size(),
                 sha_to_source.size(), clusters.size(), output_dir.c_str(),
                 write_js ? ",scripts/*.js" : "");
    return 0;
}
