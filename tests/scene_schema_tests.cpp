// scene.json schema reverse-coverage report.
//
// Two-direction check against the live corpus, sourced from ScanSceneKeys:
//
//  1. ASSERT — every `general.<X>` key the parser declares it reads must
//     appear in at least one observed scene. Catches typos and dead
//     declarations (e.g. someone removed a field but the read remained).
//
//  2. REPORT (stderr only, no assertion) — for each PKGV version, the
//     top general.<X> keys that *do* appear in scenes but are not in
//     kParsedGeneralKeys. Drives the priority list for future work
//     (lightconfig, fog*, hdr* etc).
//
// kParsedGeneralKeys() must be kept in sync with the parse_*/capture_*
// helpers in src/Parse/WPScene.cpp. When you add a new GET_JSON_NAME_VALUE
// for a general.* field, list it here too.

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "scene_keys.hpp"

namespace {

constexpr std::string_view kGeneralPrefix = "general.";

// Mirrors src/Parse/WPScene.cpp. Group keyed by the min PKGV version where
// the parser starts attempting the read. Updates here are docs; the
// assertion below treats the union as the parsed set.
const auto& kParsedGeneralKeys() {
    using set = std::set<std::string>;
    static const std::map<unsigned, set> m = {
        { 1u, set { "ambientcolor", "skylightcolor", "clearcolor",
                    "cameraparallax", "cameraparallaxamount",
                    "cameraparallaxdelay", "cameraparallaxmouseinfluence",
                    "zoom", "fov", "nearz", "farz",
                    "bloom", "bloomstrength", "bloomthreshold",
                    "camerashake", "camerashakeamplitude",
                    "camerashakespeed", "camerashakeroughness",
                    "orthogonalprojection" } },
        { 10u, set { "hdr",
                     "bloomhdrfeather", "bloomhdriterations",
                     "bloomhdrscatter", "bloomhdrstrength",
                     "bloomhdrthreshold", "bloomtint" } },
        { 21u, set { "perspectiveoverridefov", "lightconfig",
                     "windenabled", "winddirection", "windstrength",
                     "gravitydirection", "gravitystrength" } },
        { 22u, set { "transparentsorting",
                     "fogdistance", "fogdistancestart", "fogdistanceend",
                     "fogdistancecolor", "fogdistancestartdensity",
                     "fogdistanceenddensity" } },
        { 23u, set { "fogheight", "fogheightstart", "fogheightend",
                     "fogheightcolor", "fogheightstartdensity",
                     "fogheightenddensity" } },
    };
    return m;
}

std::set<std::string> AllParsed() {
    std::set<std::string> out;
    for (const auto& [_, ks] : kParsedGeneralKeys())
        out.insert(ks.begin(), ks.end());
    return out;
}

unsigned PkgIntFromStamp(const std::string& s) {
    if (s.size() < 5) return 0;
    return static_cast<unsigned>(std::stoi(s.substr(4)));
}

// Accepts only paths of shape "general.<X>" with no further '.' or '['
// — i.e. a direct child of `general`, not a nested sub-field.
bool IsTopLevelGeneralKey(std::string_view path) {
    if (! path.starts_with(kGeneralPrefix)) return false;
    return path.find_first_of(".[", kGeneralPrefix.size()) == std::string_view::npos;
}

const nlohmann::json& Report() {
    static const nlohmann::json r = wallpaper::testing::ScanSceneKeys(WAYWALLEN_WORKSHOP_DIR);
    return r;
}

} // namespace

TEST(SceneSchema, EveryParsedGeneralKeyIsObservedSomewhere) {
    std::set<std::string> observed;
    for (const auto& [_, ver_data] : Report().items()) {
        if (! ver_data.contains("keys")) continue;
        for (const auto& [path, __] : ver_data["keys"].items()) {
            if (! IsTopLevelGeneralKey(path)) continue;
            observed.insert(path.substr(kGeneralPrefix.size()));
        }
    }

    for (const auto& k : AllParsed()) {
        EXPECT_TRUE(observed.contains(k))
            << "general." << k
            << " is read by the parser but never appears in any scene "
               "across the corpus — typo or dead declaration?";
    }
}

TEST(SceneSchema, ParsedKeyDeclarationLowerBoundIsRespected) {
    // For each declared (min_v, key), assert the key actually appears in
    // some scene whose pkg version >= min_v. Catches off-by-one in the
    // version gating (e.g. listing a v21 field as v22).
    std::map<std::string, unsigned /*earliest_observed_pkg*/> earliest;
    for (const auto& [stamp, ver_data] : Report().items()) {
        const unsigned v = PkgIntFromStamp(stamp);
        if (! ver_data.contains("keys")) continue;
        for (const auto& [path, _] : ver_data["keys"].items()) {
            if (! IsTopLevelGeneralKey(path)) continue;
            const std::string k { path.substr(kGeneralPrefix.size()) };
            auto it = earliest.find(k);
            if (it == earliest.end() || v < it->second) earliest[k] = v;
        }
    }
    for (const auto& [min_v, keys] : kParsedGeneralKeys()) {
        for (const auto& k : keys) {
            auto it = earliest.find(k);
            if (it == earliest.end()) continue;  // covered by previous test
            // Allow the declared min_v to be <= the earliest observed (i.e.
            // we may attempt to read earlier than necessary; that's fine).
            // But warn if we declare LATER than the field actually exists,
            // because then v < min_v scenes would silently miss it.
            EXPECT_LE(min_v, it->second)
                << "general." << k << " is declared as min_v=" << min_v
                << " but observed in PKGV" << it->second << " — gate is too late";
        }
    }
}

TEST(SceneSchema, ReportTopUnparsedGeneralKeysPerVersion) {
    const auto parsed = AllParsed();

    std::cerr << "\n=== unparsed top-level general.<X> keys per pkg version "
                 "(top 15 by present_in count) ===\n";

    // Iterate in pkg-version numeric order for readability.
    std::vector<std::pair<unsigned, std::string>> stamps;
    for (const auto& [stamp, _] : Report().items())
        stamps.emplace_back(PkgIntFromStamp(stamp), stamp);
    std::sort(stamps.begin(), stamps.end());

    for (const auto& [v, stamp] : stamps) {
        const auto& ver_data = Report()[stamp];
        if (! ver_data.contains("keys")) continue;

        struct Entry {
            std::string   key;
            std::uint64_t present_in;
        };
        std::vector<Entry> miss;
        for (const auto& [path, info] : ver_data["keys"].items()) {
            if (! IsTopLevelGeneralKey(path)) continue;
            const std::string k { path.substr(kGeneralPrefix.size()) };
            if (parsed.contains(k)) continue;
            miss.push_back({ k, info.value("present_in", std::uint64_t { 0 }) });
        }
        std::sort(miss.begin(), miss.end(),
                  [](auto& a, auto& b) { return a.present_in > b.present_in; });

        std::cerr << "  " << stamp << ": ";
        if (miss.empty()) {
            std::cerr << "(all top-level general.* keys are parsed)";
        } else {
            const std::size_t n = std::min(miss.size(), std::size_t { 15 });
            for (std::size_t i = 0; i < n; ++i) {
                if (i) std::cerr << ", ";
                std::cerr << miss[i].key << "(" << miss[i].present_in << ")";
            }
            if (miss.size() > n) std::cerr << ", … +" << (miss.size() - n) << " more";
        }
        std::cerr << "\n";
    }
    SUCCEED();
}
