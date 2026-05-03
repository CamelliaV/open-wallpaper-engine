// wpparse [--workshop-dir DIR] [--quiet] [--report FILE] [--from-json|--full|--both]
//
// Walks workshop_dir and exercises scene.json parsing on every pkg.
// Two axes:
//   --from-json: WPScene::FromJson(json, pkg_version) only — pure JSON
//                deserialisation into wpscene::WPScene (no shader compile,
//                no .tex parse, no .mdl parse, no audio).
//   --full:      WPSceneParser::Parse(...) — the full SceneParser pipeline,
//                which transitively triggers WPShaderParser::CompileToSpv
//                for every material, plus image/particle/light/sound object
//                construction. SoundManager is default-constructed but never
//                Init()'d — Sound objects parse without actually opening
//                an audio device.
//   --both:      run both axes per pkg (default).
//
// Output: per-axis OK/FAIL line per pkg, aggregate stats on stderr,
// optional TSV report. Exit 0 iff every selected axis passed on every pkg.
//
// This is the broad coverage net for parse: scene_parse_tests covers only
// FromJson on the corpus, wpdump covers FromJson + low-level asset metadata
// but not SceneParser. wpparse fills the gap by running the full
// production-path on every pkg.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

import wescene.parse;
import wescene.pkg_fs;
import wescene.fs;
import wescene.audio;
import wescene.scene;

namespace {

namespace fs = std::filesystem;

constexpr const char* kDefaultWorkshopDir =
#ifdef WAYWALLEN_WORKSHOP_DIR
    WAYWALLEN_WORKSHOP_DIR
#else
    "workshop"
#endif
    ;

void Usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--workshop-dir DIR] [--quiet] [--report FILE]\n"
                 "          [--from-json] [--full] [--both] [--jobs N] [--max N]\n"
                 "  Walks workshop, parses scene.json on every pkg.\n"
                 "  Default mode is --both (FromJson + full SceneParser).\n"
                 "  --jobs N  parallel forks for the Full axis (default 8). FromJson\n"
                 "            runs sync in the parent.\n"
                 "  --max N   stop after N pkgs (default 0 = all).\n"
                 "  --report  writes TSV: id pkg_version fromjson_ok full_ok ...err...\n"
                 "  Note: full corpus (~810 pkgs) takes ~5min at --jobs 16; for fast\n"
                 "  dev iteration use --max 100 (~30s) or --from-json (~5s, no fork).\n",
                 prog ? prog : "wpparse");
}

enum class Mode { Both, FromJsonOnly, FullOnly };

struct PkgResult {
    std::string id;
    unsigned    pkg_version { 0 };
    bool        fromjson_ok { false };
    bool        full_ok     { false };
    std::string fromjson_err;
    std::string full_err;
};

bool LoadSceneJson(const std::string& workshop_dir,
                   wallpaper::fs::VFS&  vfs_out,
                   std::string&         text_out,
                   nlohmann::json&      json_out,
                   unsigned&            pkg_version,
                   std::string&         err) {
    const std::string pkg_path = workshop_dir + "/scene.pkg";
    if (! fs::exists(pkg_path)) {
        err = "scene.pkg not found";
        return false;
    }

    auto wfs = wallpaper::fs::WPPkgFs::CreatePkgFs(pkg_path);
    if (! wfs) {
        err = "WPPkgFs::CreatePkgFs failed";
        return false;
    }
    pkg_version = wallpaper::wpscene::ParsePkgVersionStamp(wfs->pkg_version_stamp());

    if (! vfs_out.Mount("/assets", std::move(wfs))) {
        err = "vfs.Mount failed";
        return false;
    }

    auto stream = vfs_out.Open("/assets/scene.json");
    if (! stream) {
        err = "scene.json not present in pkg";
        return false;
    }
    text_out = stream->ReadAllStr();

    try {
        json_out = nlohmann::json::parse(text_out);
    } catch (const std::exception& e) {
        err = std::string("json parse: ") + e.what();
        return false;
    }
    return true;
}

// FromJson axis runs in the parent: it's fast, never crashes, and
// avoiding fork halves the per-pkg overhead.
void DoFromJson(PkgResult& r, const nlohmann::json& j) {
    try {
        wallpaper::wpscene::WPScene scene;
        const bool                  parsed = scene.FromJson(j, r.pkg_version);
        r.fromjson_ok                      = parsed;
        if (! parsed) r.fromjson_err = "FromJson returned false";
    } catch (const std::exception& e) {
        r.fromjson_err = e.what();
    } catch (...) {
        r.fromjson_err = "unknown exception";
    }
}

// Read until N bytes or EOF / error. Returns bytes read.
ssize_t ReadAll(int fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = ::read(fd, buf + got, n - got);
        if (k > 0) { got += static_cast<size_t>(k); continue; }
        if (k == 0) break;
        if (errno == EINTR) continue;
        return -1;
    }
    return static_cast<ssize_t>(got);
}

// Full axis is forked. Each child writes back exactly one byte: 'O' for
// ok, 'N' for null-return, 'E' for caught exception. A SIGABRT/SEGV
// crash leaves the pipe empty and is detected via waitpid status.
struct InflightFull {
    pid_t  pid { -1 };
    int    read_fd { -1 };
    size_t result_idx { 0 };
};

void ReapOne(InflightFull& f, std::vector<PkgResult>& results) {
    char marker = '\0';
    ssize_t n = ReadAll(f.read_fd, &marker, 1);
    ::close(f.read_fd);

    int status = 0;
    ::waitpid(f.pid, &status, 0);

    PkgResult& r = results[f.result_idx];
    if (WIFEXITED(status)) {
        if (n > 0 && marker == 'O') {
            r.full_ok = true;
        } else if (n > 0 && marker == 'N') {
            r.full_err = "SceneParser::Parse returned null";
        } else if (n > 0 && marker == 'E') {
            r.full_err = "SceneParser threw";
        } else {
            r.full_err = "child exited without writing marker";
        }
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        r.full_err = std::string("crashed: signal ") +
                     std::to_string(sig) + " (" + ::strsignal(sig) + ")";
    } else {
        r.full_err = "child exited abnormally";
    }
}

// Fork off a Full-axis worker. Parent gets back an InflightFull; reap
// it later via ReapOne. The child re-uses the parent's mounted VFS via
// COW pages, so we don't have to re-mount in the child.
bool SpawnFull(PkgResult& r, wallpaper::fs::VFS& vfs, const std::string& text,
               size_t result_idx, InflightFull& out) {
    int fds[2];
    if (::pipe(fds) < 0) {
        r.full_err = std::string("pipe failed: ") + std::strerror(errno);
        return false;
    }
    std::fflush(stdout);
    std::fflush(stderr);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        r.full_err = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        ::close(fds[0]);
        char marker = 'E';
        try {
            wallpaper::audio::SoundManager sm;
            wallpaper::WPSceneParser       parser;
            auto scene = parser.Parse(
                r.id, text, vfs, sm,
                static_cast<wallpaper::wpscene::SceneVersion>(r.pkg_version));
            marker = scene ? 'O' : 'N';
        } catch (...) {
            marker = 'E';
        }
        ::write(fds[1], &marker, 1);
        ::close(fds[1]);
        ::_exit(0);
    }
    ::close(fds[1]);
    out.pid        = pid;
    out.read_fd    = fds[0];
    out.result_idx = result_idx;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string workshop_dir = kDefaultWorkshopDir;
    std::string report_path;
    bool        quiet = false;
    Mode        mode  = Mode::Both;
    int         jobs  = 8;
    int         max_n = 0;  // 0 == no limit

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--workshop-dir") == 0 && i + 1 < argc) {
            workshop_dir = argv[++i];
        } else if (std::strcmp(a, "--report") == 0 && i + 1 < argc) {
            report_path = argv[++i];
        } else if (std::strcmp(a, "--jobs") == 0 && i + 1 < argc) {
            jobs = std::atoi(argv[++i]);
            if (jobs < 1) jobs = 1;
        } else if (std::strcmp(a, "--max") == 0 && i + 1 < argc) {
            max_n = std::atoi(argv[++i]);
            if (max_n < 0) max_n = 0;
        } else if (std::strcmp(a, "--quiet") == 0) {
            quiet = true;
        } else if (std::strcmp(a, "--from-json") == 0) {
            mode = Mode::FromJsonOnly;
        } else if (std::strcmp(a, "--full") == 0) {
            mode = Mode::FullOnly;
        } else if (std::strcmp(a, "--both") == 0) {
            mode = Mode::Both;
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            Usage(argv[0]);
            return 0;
        } else {
            Usage(argv[0]);
            return 2;
        }
    }

    if (! fs::exists(workshop_dir) || ! fs::is_directory(workshop_dir)) {
        std::fprintf(stderr, "wpparse: %s is not a directory\n", workshop_dir.c_str());
        return 1;
    }

    // 2435537849 hangs WPMdlParser::Parse infinitely (Corpus::kSkipIds).
    // The fork-per-pkg path catches asserts but doesn't catch infinite
    // loops, so we still need to skip this one explicitly.
    static constexpr std::array<const char*, 1> kSkipIds = { "2435537849" };

    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(workshop_dir)) {
        if (! e.is_directory()) continue;
        if (! fs::exists(e.path() / "scene.pkg")) continue;
        const std::string id = e.path().filename().string();
        bool skip = false;
        for (const auto* s : kSkipIds) if (id == s) { skip = true; break; }
        if (skip) continue;
        dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());

    if (max_n > 0 && static_cast<int>(dirs.size()) > max_n) {
        dirs.resize(static_cast<size_t>(max_n));
    }

    std::ofstream report;
    if (! report_path.empty()) {
        report.open(report_path);
        if (! report) {
            std::fprintf(stderr, "wpparse: cannot open %s for writing\n",
                         report_path.c_str());
            return 1;
        }
        report << "id\tpkg_version\tfromjson_ok\tfull_ok\tfromjson_err\tfull_err\n";
    }

    int total = 0, fj_ok = 0, fj_fail = 0, full_ok = 0, full_fail = 0;
    std::map<unsigned, std::pair<int, int>> per_version_full; // version -> (ok, total)

    auto t0 = std::chrono::steady_clock::now();

    // Per-pkg state: VFS + json + result. We hold the VFS alive in the
    // parent until the corresponding child exits, because the child reads
    // shader/material/.tex files via VFS COW pages. VFS is NoCopy/NoMove
    // so the slots are unique_ptr.
    struct PkgState {
        wallpaper::fs::VFS vfs;
        std::string        text;
        nlohmann::json     j;
    };
    std::vector<std::unique_ptr<PkgState>> states(dirs.size());
    std::vector<PkgResult>                 results(dirs.size());
    std::vector<InflightFull>              inflight;
    inflight.reserve(static_cast<size_t>(jobs));

    auto emit = [&](size_t idx) {
        const PkgResult& r = results[idx];
        if (mode != Mode::FullOnly) {
            if (r.fromjson_ok) {
                ++fj_ok;
                if (! quiet) {
                    std::fprintf(stdout, "OK    %s/FromJson  v=%u\n",
                                 r.id.c_str(), r.pkg_version);
                }
            } else {
                ++fj_fail;
                std::fprintf(stdout, "FAIL  %s/FromJson  v=%u  %s\n",
                             r.id.c_str(), r.pkg_version, r.fromjson_err.c_str());
            }
        }
        if (mode != Mode::FromJsonOnly) {
            ++per_version_full[r.pkg_version].second;
            if (r.full_ok) {
                ++full_ok;
                ++per_version_full[r.pkg_version].first;
                if (! quiet) {
                    std::fprintf(stdout, "OK    %s/Full      v=%u\n",
                                 r.id.c_str(), r.pkg_version);
                }
            } else {
                ++full_fail;
                std::fprintf(stdout, "FAIL  %s/Full      v=%u  %s\n",
                             r.id.c_str(), r.pkg_version, r.full_err.c_str());
            }
        }
        if (report) {
            report << r.id << '\t' << r.pkg_version << '\t'
                   << (r.fromjson_ok ? 1 : 0) << '\t' << (r.full_ok ? 1 : 0) << '\t'
                   << r.fromjson_err << '\t' << r.full_err << '\n';
        }
    };

    auto reap_first = [&]() {
        if (inflight.empty()) return;
        InflightFull f = inflight.front();
        inflight.erase(inflight.begin());
        ReapOne(f, results);
        emit(f.result_idx);
        states[f.result_idx].reset();
    };

    for (size_t i = 0; i < dirs.size(); ++i) {
        ++total;
        PkgResult& r = results[i];
        r.id = dirs[i].filename().string();

        states[i] = std::make_unique<PkgState>();
        // Load scene.json into parent so the FromJson axis runs sync and
        // the Full child inherits the already-mounted VFS.
        std::string err;
        if (! LoadSceneJson(dirs[i].string(), states[i]->vfs, states[i]->text,
                            states[i]->j, r.pkg_version, err)) {
            r.fromjson_err = err;
            r.full_err     = err;
            emit(i);
            states[i].reset();
            continue;
        }

        if (mode != Mode::FullOnly) {
            DoFromJson(r, states[i]->j);
        }

        if (mode != Mode::FromJsonOnly) {
            // Throttle: if the pool is full, reap one before forking.
            while (static_cast<int>(inflight.size()) >= jobs) reap_first();

            InflightFull f;
            if (! SpawnFull(r, states[i]->vfs, states[i]->text, i, f)) {
                emit(i);
                states[i].reset();
            } else {
                inflight.push_back(f);
            }
        } else {
            emit(i);
            states[i].reset();
        }
    }
    while (! inflight.empty()) reap_first();

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::fprintf(stderr,
                 "wpparse: %d pkgs | FromJson: %d ok %d fail | Full: %d ok %d fail | %lld ms\n",
                 total, fj_ok, fj_fail, full_ok, full_fail, static_cast<long long>(ms));
    if (mode != Mode::FromJsonOnly && ! per_version_full.empty()) {
        std::fprintf(stderr, "  per-pkg-version (Full):");
        for (const auto& [v, c] : per_version_full) {
            std::fprintf(stderr, " v%u=%d/%d", v, c.first, c.second);
        }
        std::fprintf(stderr, "\n");
    }

    int total_fail = fj_fail + full_fail;
    return total_fail == 0 ? 0 : 1;
}
