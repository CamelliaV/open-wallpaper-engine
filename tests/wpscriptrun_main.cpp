// wpscriptrun --jsonl scripts.jsonl --js-dir scripts/ [--sample N]

#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

import wescene.script;
import cppstd;

namespace fs = std::filesystem;
using json   = nlohmann::json;
using namespace wallpaper::script;

namespace {

void Usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s --jsonl FILE --js-dir DIR [--sample N]\n"
                 "  Replays every binding from scripts.jsonl through a\n"
                 "  fresh wescene.script JsRuntime, ticking 10 frames each.\n",
                 prog ? prog : "wpscriptrun");
}

FieldKind GuessKindFromField(const std::string& f) {
    if (f == "visible")                                                return FieldKind::Bool;
    if (f == "origin" || f == "scale" || f == "angles" || f == "spriteoffset")
        return FieldKind::Vec3;
    if (f == "color" || f == "colorn") return FieldKind::Color;
    if (f == "text") return FieldKind::String;
    return FieldKind::Scalar;
}

std::string ReadFile(const fs::path& p) {
    std::ifstream f(p);
    if (! f) return {};
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    std::string jsonl_path;
    std::string js_dir;
    int         sample = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--jsonl") == 0 && i + 1 < argc)    jsonl_path = argv[++i];
        else if (std::strcmp(a, "--js-dir") == 0 && i + 1 < argc) js_dir   = argv[++i];
        else if (std::strcmp(a, "--sample") == 0 && i + 1 < argc) sample   = std::atoi(argv[++i]);
        else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            Usage(argv[0]);
            return 0;
        } else { Usage(argv[0]); return 2; }
    }
    if (jsonl_path.empty() || js_dir.empty()) { Usage(argv[0]); return 2; }

    std::ifstream jsonl(jsonl_path);
    if (! jsonl) { std::fprintf(stderr, "cannot open %s\n", jsonl_path.c_str()); return 1; }

    int total = 0, ok = 0, failed_compile = 0, failed_runtime = 0, missing_src = 0;
    std::map<std::string, std::pair<int, int>> per_field;  // field -> (ok, total)

    auto t0 = std::chrono::steady_clock::now();

    // Per-test fresh runtime keeps state isolated and surfaces leaks.
    std::string line;
    while (std::getline(jsonl, line)) {
        if (sample > 0 && total >= sample) break;
        if (line.empty()) continue;
        json rec;
        try { rec = json::parse(line); }
        catch (...) { continue; }

        ++total;
        std::string sha    = rec.value("script_sha1", "");
        std::string field  = rec.value("field", "");
        json        sp     = rec.value("scriptproperties", json::object());
        json        v0     = rec.value("value_initial", json{});
        FieldKind   kind   = GuessKindFromField(field);
        ++per_field[field].second;

        fs::path src_path = fs::path(js_dir) / (sha + ".js");
        std::string src   = ReadFile(src_path);
        if (src.empty()) { ++missing_src; continue; }

        JsRuntime rt;
        auto* script = rt.MakeFieldScript(src, sha, kind, sp, v0);
        if (! script) { ++failed_compile; continue; }

        FrameInputs fi;
        fi.frametime = 0.016f;
        for (int frame = 0; frame < 10; ++frame) {
            fi.runtime  = static_cast<float>(frame) * 0.016f;
            for (auto& bin : fi.audio_average) bin = 0.5f;
            rt.SetFrameInputs(fi);
            rt.TickAll();
        }
        if (! script->alive()) { ++failed_runtime; continue; }
        ++ok;
        ++per_field[field].first;
    }

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::fprintf(stderr,
                 "wpscriptrun: %d records, %d ok, %d compile-fail, %d runtime-fail,"
                 " %d missing-src, %lld ms\n",
                 total, ok, failed_compile, failed_runtime, missing_src,
                 static_cast<long long>(ms));
    std::fprintf(stderr, "per-field pass rate:\n");
    for (auto& [f, oktot] : per_field) {
        if (oktot.second < 5) continue;
        std::fprintf(stderr, "  %-20s %d/%d (%.0f%%)\n", f.c_str(),
                     oktot.first, oktot.second,
                     100.0 * oktot.first / oktot.second);
    }
    return ok == total - missing_src ? 0 : 1;
}
