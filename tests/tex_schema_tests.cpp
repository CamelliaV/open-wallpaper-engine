// .tex format schema reverse-coverage report.
//
// Mirrors scene_schema_tests.cpp's pattern but for the binary `.tex`
// container produced by Wallpaper Engine. There are no string keys to
// audit; the "schema" is the (texv, texi, texb, texs) sub-version tuple,
// each component a tiny int read from a "TEXX0000" stamp embedded in the
// header / sprite payload.
//
// Source of truth: src/Parse/WPTexImageParser.cpp + the
// WPTexFormatVersion predicates in src/Parse/WPTexImageParser.cppm.
//
// Two checks driven by the live corpus:
//
//   1. ASSERT — every (texv, texi, texb, texs) sub-version observed in
//      the corpus is in the supported set documented by
//      WPTexFormatVersion. A new pkg surfacing an unknown stamp value
//      fails immediately rather than silently falling through the
//      production parser's permissive branches.
//
//   2. REPORT (stderr only) — distinct tuples and their sample counts,
//      so additions to the corpus are visible even when they fall in
//      the supported set.
//
// Reads the .tex sub-version stamps directly from the pkg blob instead of
// going through WPTexImageParser::ParseHeader. ParseHeader's sprite path
// has a long-standing assertion-failure bug on certain malformed mipmap
// counts (vector<float>::operator[] on an empty inner vector); routing
// through it would abort the test process before any tuple gets reported.
// Using a stand-alone reader keeps this test orthogonal to body-parser
// regressions and lets it scan the entire corpus unconditionally.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

constexpr const char* kWorkshopDirMacro =
#ifdef WAYWALLEN_WORKSHOP_DIR
    WAYWALLEN_WORKSHOP_DIR
#else
    "workshop"
#endif
    ;

using TexTuple = std::tuple<int, int, int, int>;  // (texv, texi, texb, texs)

struct TupleStats {
    std::size_t total { 0 };
    std::size_t header_ok { 0 };  // header read produced a sane texv/texi/texb
    std::size_t sprite { 0 };
};

// Mirrors WPTexFormatVersion::valid() and the texb/texs predicates. Every
// value in these sets must be reachable through one of the documented
// version branches.
const std::set<int>& kSupportedTexv() {
    static const std::set<int> v { 5 };
    return v;
}
const std::set<int>& kSupportedTexi() {
    static const std::set<int> v { 1 };
    return v;
}
const std::set<int>& kSupportedTexb() {
    static const std::set<int> v { 1, 2, 3, 4 };
    return v;
}
const std::set<int>& kSupportedTexs() {
    // 0 == absent (non-sprite). 2 and 3 cover all sprite samples in the
    // corpus; 1 is documented as legacy int-coords but never observed.
    static const std::set<int> v { 0, 1, 2, 3 };
    return v;
}

template <typename T>
bool read_pod(const char* buf, std::size_t len, std::size_t off, T& out) {
    if (off + sizeof(T) > len) return false;
    std::memcpy(&out, buf + off, sizeof(T));
    return true;
}

// Mirrors `ReadVersion("TEX", file)` from src/WPCommon.cppm: 9 bytes,
// expecting "TEXX" + 4-digit ASCII int. Returns 0 on prefix mismatch /
// short read.
int parse_tex_stamp(const char* buf, std::size_t len, std::size_t off) {
    if (off + 9 > len) return 0;
    const char* p = buf + off;
    if (p[0] != 'T' || p[1] != 'E' || p[2] != 'X') return 0;
    int n = 0;
    for (int i = 4; i < 8; ++i) {
        char c = p[i];
        if (c < '0' || c > '9') return 0;
        n = n * 10 + (c - '0');
    }
    return n;
}

// Read just enough of the .tex blob to recover (texv, texi, texb) plus
// the `flags` int needed to distinguish sprites from non-sprites. Walks
// past mip bodies to read the trailing texs stamp when sprite. Returns
// (-1, -1, -1, -1) on any short / malformed read.
TexTuple read_tex_versions(const std::string& blob) {
    const char*       buf = blob.data();
    const std::size_t len = blob.size();
    std::size_t       o   = 0;

    int texv = parse_tex_stamp(buf, len, o); o += 9;
    int texi = parse_tex_stamp(buf, len, o); o += 9;
    if (texv == 0 || texi == 0) return { -1, -1, -1, -1 };

    std::int32_t format = 0, flags = 0, w = 0, h = 0, mw = 0, mh = 0, _u = 0;
    if (! read_pod(buf, len, o, format)) return { texv, texi, -1, -1 }; o += 4;
    if (! read_pod(buf, len, o, flags))  return { texv, texi, -1, -1 }; o += 4;
    if (! read_pod(buf, len, o, w))      return { texv, texi, -1, -1 }; o += 4;
    if (! read_pod(buf, len, o, h))      return { texv, texi, -1, -1 }; o += 4;
    if (! read_pod(buf, len, o, mw))     return { texv, texi, -1, -1 }; o += 4;
    if (! read_pod(buf, len, o, mh))     return { texv, texi, -1, -1 }; o += 4;
    if (! read_pod(buf, len, o, _u))     return { texv, texi, -1, -1 }; o += 4;

    int texb = parse_tex_stamp(buf, len, o); o += 9;
    if (texb == 0) return { texv, texi, -1, -1 };

    std::int32_t count = 0;
    if (! read_pod(buf, len, o, count)) return { texv, texi, texb, -1 }; o += 4;

    if (texb >= 3) {
        std::int32_t image_type = 0;
        if (! read_pod(buf, len, o, image_type)) return { texv, texi, texb, -1 };
        o += 4;
    }

    const bool sprite = (flags & (1u << 2)) != 0;
    if (! sprite) return { texv, texi, texb, 0 };

    // walk past mip bodies to reach the trailing texs stamp
    for (std::int32_t i = 0; i < count; ++i) {
        std::int32_t mip_count = 0;
        if (! read_pod(buf, len, o, mip_count)) return { texv, texi, texb, -1 };
        o += 4;
        for (std::int32_t m = 0; m < mip_count; ++m) {
            std::int32_t mw0 = 0, mh0 = 0;
            if (! read_pod(buf, len, o, mw0)) return { texv, texi, texb, -1 }; o += 4;
            if (! read_pod(buf, len, o, mh0)) return { texv, texi, texb, -1 }; o += 4;
            if (texb >= 2) {
                std::int32_t lz4 = 0, dec = 0;
                if (! read_pod(buf, len, o, lz4)) return { texv, texi, texb, -1 }; o += 4;
                if (! read_pod(buf, len, o, dec)) return { texv, texi, texb, -1 }; o += 4;
            }
            std::int32_t src = 0;
            if (! read_pod(buf, len, o, src)) return { texv, texi, texb, -1 }; o += 4;
            if (src < 0 || o + static_cast<std::size_t>(src) > len)
                return { texv, texi, texb, -1 };
            o += static_cast<std::size_t>(src);
        }
    }
    int texs = parse_tex_stamp(buf, len, o);
    return { texv, texi, texb, texs };
}

// Reads scene.pkg's table of contents (mirrors dump.cpp's ReadPkgHeader).
// Returns each .tex entry as a fully-loaded blob. Empty vector on error.
struct TexEntry {
    std::string path;
    std::string blob;
};
std::vector<TexEntry> ReadTexEntries(const fs::path& pkg) {
    std::vector<TexEntry> out;
    std::ifstream         in(pkg, std::ios::binary);
    if (! in.good()) return out;

    auto read_i32 = [&](std::int32_t& v) {
        in.read(reinterpret_cast<char*>(&v), 4);
        return static_cast<bool>(in);
    };

    std::int32_t ver_len = 0;
    if (! read_i32(ver_len) || ver_len < 0) return out;
    std::string ver(static_cast<std::size_t>(ver_len), '\0');
    in.read(ver.data(), ver_len);

    std::int32_t entry_count = 0;
    if (! read_i32(entry_count) || entry_count < 0 || entry_count > 100000) return out;

    struct Entry {
        std::string  path;
        std::int32_t offset { 0 };
        std::int32_t length { 0 };
    };
    std::vector<Entry> entries;
    entries.reserve(static_cast<std::size_t>(entry_count));
    for (std::int32_t i = 0; i < entry_count; ++i) {
        std::int32_t name_len = 0;
        if (! read_i32(name_len) || name_len < 0) return out;
        Entry e;
        e.path.resize(static_cast<std::size_t>(name_len));
        in.read(e.path.data(), name_len);
        if (! read_i32(e.offset)) return out;
        if (! read_i32(e.length)) return out;
        entries.push_back(std::move(e));
    }

    const std::streampos data_start = in.tellg();
    if (data_start < 0) return out;

    for (const auto& e : entries) {
        if (e.path.size() < 4) continue;
        if (e.path.compare(e.path.size() - 4, 4, ".tex") != 0) continue;
        in.seekg(data_start + static_cast<std::streamoff>(e.offset));
        TexEntry te;
        te.path = e.path;
        te.blob.resize(static_cast<std::size_t>(std::max<std::int32_t>(e.length, 0)));
        in.read(te.blob.data(), e.length);
        out.push_back(std::move(te));
    }
    return out;
}

std::map<TexTuple, TupleStats> CollectTuples() {
    std::map<TexTuple, TupleStats> out;
    fs::path                       root { kWorkshopDirMacro };
    if (! fs::exists(root) || ! fs::is_directory(root)) {
        std::cerr << "tex_schema_tests: workshop dir " << root.string()
                  << " missing\n";
        return out;
    }

    std::vector<fs::path> pkgs;
    for (auto& e : fs::directory_iterator(root)) {
        if (! e.is_directory()) continue;
        auto pkg = e.path() / "scene.pkg";
        if (fs::exists(pkg)) pkgs.push_back(pkg);
    }
    std::sort(pkgs.begin(), pkgs.end());

    for (const auto& pkg : pkgs) {
        for (const auto& te : ReadTexEntries(pkg)) {
            auto tup = read_tex_versions(te.blob);
            auto& s  = out[tup];
            ++s.total;
            const auto [v, i, b, sp] = tup;
            if (v > 0 && i > 0 && b > 0) ++s.header_ok;
            if (sp > 0) ++s.sprite;
        }
    }
    return out;
}

const std::map<TexTuple, TupleStats>& AllTuples() {
    static const auto m = CollectTuples();
    return m;
}

} // namespace

TEST(TexSchema, EveryObservedSubVersionIsSupported) {
    const auto& stats = AllTuples();
    ASSERT_FALSE(stats.empty()) << "tex scan produced zero textures";

    for (const auto& [tup, s] : stats) {
        const auto [v, i, b, sp] = tup;
        // Skip purely malformed reads (header truncated mid-stream); those
        // surface as -1 / 0 components and are not real version drift.
        if (v <= 0 || i <= 0 || b <= 0) continue;
        EXPECT_TRUE(kSupportedTexv().contains(v))
            << "texv=" << v << " not in supported set; sample count=" << s.total;
        EXPECT_TRUE(kSupportedTexi().contains(i))
            << "texi=" << i << " not in supported set; sample count=" << s.total;
        EXPECT_TRUE(kSupportedTexb().contains(b))
            << "texb=" << b << " not in supported set; sample count=" << s.total;
        EXPECT_TRUE(kSupportedTexs().contains(sp))
            << "texs=" << sp << " not in supported set; sample count=" << s.total;
    }
}

TEST(TexSchema, NoMalformedHeadersInCorpus) {
    const auto& stats = AllTuples();
    ASSERT_FALSE(stats.empty()) << "tex scan produced zero textures";

    std::size_t malformed = 0;
    for (const auto& [tup, s] : stats) {
        const auto [v, i, b, sp] = tup;
        if (v <= 0 || i <= 0 || b <= 0 || sp < 0) malformed += s.total;
    }
    EXPECT_EQ(malformed, 0u)
        << malformed << " textures had truncated / unparseable headers";
}

TEST(TexSchema, ReportObservedTuples) {
    const auto& stats = AllTuples();
    std::cerr << "\n# observed (texv, texi, texb, texs) tuples in corpus:\n";
    for (const auto& [tup, s] : stats) {
        const auto [v, i, b, sp] = tup;
        std::cerr << "  (" << v << ", " << i << ", " << b << ", " << sp
                  << "): total=" << s.total
                  << " header_ok=" << s.header_ok
                  << " sprite=" << s.sprite << "\n";
    }
    SUCCEED();
}
