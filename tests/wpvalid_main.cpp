// Thin CLI around DumpWorkshop: regenerate fixture snapshots.
//
//   wpvalid <workshop_dir> [out.json]
//
// If out.json is omitted the snapshot is written to stdout. Exit code is
// non-zero when DumpWorkshop reports an error.

#include <cstdio>
#include <fstream>
#include <string>

#include "dump.hpp"

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <workshop_dir> [out.json]\n", argv[0]);
        return 2;
    }

    std::string err;
    auto        snap = owe::testing::DumpWorkshop(argv[1], err);
    if (! err.empty()) {
        std::fprintf(stderr, "wpvalid: %s\n", err.c_str());
        return 1;
    }

    const std::string dump = snap.dump(2);
    if (argc == 3) {
        std::ofstream out(argv[2]);
        if (! out) {
            std::fprintf(stderr, "wpvalid: cannot open %s for writing\n", argv[2]);
            return 1;
        }
        out << dump << "\n";
    } else {
        std::fwrite(dump.data(), 1, dump.size(), stdout);
        std::fputc('\n', stdout);
    }
    return 0;
}
