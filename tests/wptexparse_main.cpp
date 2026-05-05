// wptexparse [--workshop-dir DIR] [--quiet] [--report FILE] [--full]

#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "pkg_header.hpp"

import wescene.parse;
import wescene.pkg_fs;
import wescene.fs;
import wescene.types;

namespace {

namespace fs = std::filesystem;

using wallpaper::testing::PkgEntry;
using wallpaper::testing::ReadPkgHeader;

constexpr const char* kDefaultWorkshopDir =
#ifdef WAYWALLEN_WORKSHOP_DIR
    WAYWALLEN_WORKSHOP_DIR
#else
    "workshop"
#endif
    ;

void Usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--workshop-dir DIR] [--quiet] [--report FILE] [--full]\n"
                 "  Walks every scene.pkg under DIR, runs WPTexImageParser on each\n"
                 "  /materials/*.tex entry. Default mode: ParseHeader (header only).\n"
                 "  --full     run Parse() (full decode incl. LZ4 + image-container body).\n"
                 "  --quiet    suppress per-tex OK lines.\n"
                 "  --report   write TSV report (id, tex, ok, texv, texi, texb, texs, format, w, h, slots).\n",
                 prog ? prog : "wptexparse");
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct TexResult {
    std::string  workshop;
    std::string  tex_path;
    bool         ok { false };
    std::string  err;
    int          texv { 0 };
    int          texi { 0 };
    int          texb { 0 };
    int          texs { 0 };
    int          format { 0 };
    int          width { 0 };
    int          height { 0 };
    int          slots { 0 };
    int          mip_total { 0 };
    bool         is_sprite { false };
    long long    body_bytes { 0 };
};

// Collapse the production parser API:
// - ParseHeader fills ImageHeader (incl. extraHeader["texv"|...]).
// - Parse fills the full Image with decoded pixel data per slot/mip.
// Both throw on malformed input.
TexResult RunOne(wallpaper::fs::VFS& vfs, const std::string& workshop_id,
                 const std::string& tex_path, bool full) {
    TexResult r;
    r.workshop = workshop_id;
    r.tex_path = tex_path;

    constexpr std::string_view prefix = "/materials/";
    constexpr std::string_view suffix = ".tex";
    if (tex_path.compare(0, prefix.size(), prefix) != 0) {
        r.err = "path not under /materials/";
        return r;
    }
    if (tex_path.size() < prefix.size() + suffix.size()) {
        r.err = "path too short";
        return r;
    }
    if (! ends_with(tex_path, suffix)) {
        r.err = "missing .tex suffix";
        return r;
    }
    std::string name = tex_path.substr(
        prefix.size(), tex_path.size() - prefix.size() - suffix.size());

    wallpaper::WPTexImageParser parser(&vfs);

    auto extra = [](const wallpaper::ImageHeader& h, const std::string& k) {
        auto it = h.extraHeader.find(k);
        return it == h.extraHeader.end() ? 0 : it->second.val;
    };

    if (! full) {
        try {
            wallpaper::ImageHeader h = parser.ParseHeader(name);
            r.texv      = extra(h, "texv");
            r.texi      = extra(h, "texi");
            r.texb      = extra(h, "texb");
            r.texs      = extra(h, "texs");
            r.format    = static_cast<int>(h.format);
            r.width     = h.width;
            r.height    = h.height;
            r.slots     = h.count;
            r.is_sprite = h.isSprite;
            r.ok        = (r.texv > 0 && r.width > 0 && r.height > 0);
            if (! r.ok) r.err = "header looks invalid (zero dim or texv)";
        } catch (const std::exception& e) {
            r.err = e.what();
        } catch (...) {
            r.err = "unknown exception";
        }
        return r;
    }

    try {
        auto img = parser.Parse(name);
        if (! img) {
            r.err = "Parse returned null";
            return r;
        }
        const auto& h = img->header;
        r.texv      = extra(h, "texv");
        r.texi      = extra(h, "texi");
        r.texb      = extra(h, "texb");
        r.texs      = extra(h, "texs");
        r.format    = static_cast<int>(h.format);
        r.width     = h.width;
        r.height    = h.height;
        r.slots     = static_cast<int>(img->slots.size());
        r.is_sprite = h.isSprite;
        for (const auto& slot : img->slots) {
            r.mip_total += static_cast<int>(slot.mipmaps.size());
            for (const auto& mip : slot.mipmaps) {
                r.body_bytes += static_cast<long long>(mip.size);
            }
        }
        r.ok = (r.slots > 0 && r.mip_total > 0);
        if (! r.ok) r.err = "decoded image has zero slots/mipmaps";
    } catch (const std::exception& e) {
        r.err = e.what();
    } catch (...) {
        r.err = "unknown exception";
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    std::string workshop_dir = kDefaultWorkshopDir;
    std::string report_path;
    bool        quiet = false;
    bool        full  = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--workshop-dir") == 0 && i + 1 < argc) {
            workshop_dir = argv[++i];
        } else if (std::strcmp(a, "--report") == 0 && i + 1 < argc) {
            report_path = argv[++i];
        } else if (std::strcmp(a, "--quiet") == 0) {
            quiet = true;
        } else if (std::strcmp(a, "--full") == 0) {
            full = true;
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            Usage(argv[0]);
            return 0;
        } else {
            Usage(argv[0]);
            return 2;
        }
    }

    if (! fs::exists(workshop_dir) || ! fs::is_directory(workshop_dir)) {
        std::fprintf(stderr, "wptexparse: %s is not a directory\n", workshop_dir.c_str());
        return 1;
    }

    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(workshop_dir)) {
        if (! e.is_directory()) continue;
        if (! fs::exists(e.path() / "scene.pkg")) continue;
        dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());

    std::ofstream report;
    if (! report_path.empty()) {
        report.open(report_path);
        if (! report) {
            std::fprintf(stderr, "wptexparse: cannot open %s for writing\n",
                         report_path.c_str());
            return 1;
        }
        report << "id\ttex\tok\ttexv\ttexi\ttexb\ttexs\tformat\tw\th\tslots\tmip_total\tbody_bytes\terr\n";
    }

    int total_pkg = 0, total_tex = 0, ok_tex = 0, fail_tex = 0;
    std::map<std::string, std::pair<int, int>> per_format; // format -> (ok, total)
    std::map<int, std::pair<int, int>>         per_texb;   // texb   -> (ok, total)

    auto t0 = std::chrono::steady_clock::now();

    for (const auto& d : dirs) {
        ++total_pkg;
        const std::string id       = d.filename().string();
        const std::string pkg_path = (d / "scene.pkg").string();

        std::string                  pkg_version;
        std::vector<PkgEntry>        entries;
        if (! ReadPkgHeader(pkg_path, pkg_version, entries)) {
            std::fprintf(stdout, "FAIL  %s/?              pkg header read failed\n",
                         id.c_str());
            ++fail_tex;
            continue;
        }

        wallpaper::fs::VFS vfs;
        auto wfs = wallpaper::fs::WPPkgFs::CreatePkgFs(pkg_path);
        if (! wfs) {
            std::fprintf(stdout, "FAIL  %s/?              CreatePkgFs failed\n",
                         id.c_str());
            ++fail_tex;
            continue;
        }
        vfs.Mount("/assets", std::move(wfs));

        for (const auto& e : entries) {
            if (! ends_with(e.path, ".tex")) continue;
            if (e.path.rfind("/materials/", 0) != 0) continue;
            ++total_tex;

            TexResult r = RunOne(vfs, id, e.path, full);

            const std::string format_key = std::to_string(r.format);
            ++per_format[format_key].second;
            ++per_texb[r.texb].second;
            if (r.ok) {
                ++ok_tex;
                ++per_format[format_key].first;
                ++per_texb[r.texb].first;
                if (! quiet) {
                    std::fprintf(
                        stdout,
                        "OK    %s/%-40s  fmt=%d %dx%d slots=%d mips=%d texb=%d%s\n",
                        id.c_str(), e.path.c_str(), r.format, r.width, r.height,
                        r.slots, r.mip_total, r.texb,
                        r.is_sprite ? " sprite" : "");
                }
            } else {
                ++fail_tex;
                std::fprintf(stdout, "FAIL  %s/%s  %s\n", id.c_str(), e.path.c_str(),
                             r.err.c_str());
            }

            if (report) {
                report << id << '\t' << e.path << '\t' << (r.ok ? 1 : 0) << '\t'
                       << r.texv << '\t' << r.texi << '\t' << r.texb << '\t'
                       << r.texs << '\t' << r.format << '\t' << r.width << '\t'
                       << r.height << '\t' << r.slots << '\t' << r.mip_total << '\t'
                       << r.body_bytes << '\t' << r.err << '\n';
            }
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::fprintf(stderr,
                 "wptexparse: %d pkgs | %d textures | %d ok | %d fail | %s | %lld ms\n",
                 total_pkg, total_tex, ok_tex, fail_tex,
                 full ? "FULL decode" : "header only", static_cast<long long>(ms));
    if (! per_texb.empty()) {
        std::fprintf(stderr, "  per-texb:");
        for (const auto& [b, c] : per_texb) {
            std::fprintf(stderr, " texb%d=%d/%d", b, c.first, c.second);
        }
        std::fprintf(stderr, "\n");
    }
    if (! per_format.empty()) {
        std::fprintf(stderr, "  per-format:");
        for (const auto& [f, c] : per_format) {
            std::fprintf(stderr, " fmt%s=%d/%d", f.c_str(), c.first, c.second);
        }
        std::fprintf(stderr, "\n");
    }
    return fail_tex == 0 ? 0 : 1;
}
