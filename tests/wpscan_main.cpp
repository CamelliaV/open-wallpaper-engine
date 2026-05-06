// wpscan [--workshop-dir DIR] [--output FILE]
//
// Walks workshop_dir, reads each scene.pkg's scene.json, and writes a
// per-pkg-version key-path aggregation. Default output is stdout, default
// workshop dir is the WAYWALLEN_WORKSHOP_DIR macro injected by CMake (the
// repo's tests/../workshop tree) or "./workshop" as a fallback.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "scene_keys.hpp"

namespace {

constexpr const char* kDefaultWorkshopDir =
#ifdef WAYWALLEN_WORKSHOP_DIR
    WAYWALLEN_WORKSHOP_DIR
#else
    "workshop"
#endif
    ;

void Usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--workshop-dir DIR] [--output FILE]\n",
                 prog ? prog : "wpscan");
}

} // namespace

int main(int argc, char** argv) {
    std::string workshop_dir = kDefaultWorkshopDir;
    std::string output_path;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--workshop-dir") == 0 && i + 1 < argc) {
            workshop_dir = argv[++i];
        } else if (std::strcmp(a, "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            Usage(argv[0]);
            return 0;
        } else {
            Usage(argv[0]);
            return 2;
        }
    }

    auto report = owe::testing::ScanSceneKeys(workshop_dir);
    std::string text = report.dump(2);
    text.push_back('\n');

    if (output_path.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
    } else {
        std::ofstream out(output_path);
        if (! out) {
            std::fprintf(stderr, "wpscan: cannot open %s for writing\n", output_path.c_str());
            return 1;
        }
        out << text;
    }
    return 0;
}
