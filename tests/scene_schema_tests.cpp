// scene.json schema reverse-coverage report.
//
// Two-direction check against the live corpus (sourced from ScanSceneKeys),
// applied independently per top-level scope:
//
//   * "general.<X>" — fields directly under the scene's `general` object.
//     Source of truth: src/Parse/WPScene.cpp parse_*/capture_* helpers.
//   * "objects[].<X>" — fields directly on each scene object (image/light/
//     particle/sound). Source of truth: the union of fields read across
//     WPImageObject / WPLightObject / WPParticleObject / WPSoundObject
//     FromJson implementations.
//
// For each scope we run:
//
//   1. ASSERT — every key the parser declares it reads must appear in at
//      least one observed scene. Catches typos and dead declarations.
//
//   2. REPORT (stderr only, no assertion) — for each PKGV version, the top
//      N keys that *do* appear in scenes but are NOT in the parsed set.
//      Drives the priority list for absorbing more keys into the structs.
//
// `general` additionally asserts (3) that each declared min_v gate is not
// later than the corpus's actual earliest observation of that key (would
// otherwise silently miss data on older scenes).
//
// kParsedGeneralKeys / kParsedObjectKeys must be kept in sync with
// src/Parse/WPScene.cpp and src/Parse/WP*Object.cpp respectively; when you
// add a new GET_JSON_NAME_VALUE for a top-level field, list it here too.

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
constexpr std::string_view kObjectsPrefix = "objects[].";

// Mirrors src/Parse/WPScene.cpp. Group keyed by the min PKGV version where
// the parser starts attempting the read. Updates here are docs; the
// assertion below treats the union as the parsed set.
const auto& kParsedGeneralKeys() {
    using set = std::set<std::string>;
    static const std::map<unsigned, set> m = {
        { 1u, set { "ambientcolor", "skylightcolor", "clearcolor", "clearenabled",
                    "camerafade", "camerapreview",
                    "cameraparallax", "cameraparallaxamount",
                    "cameraparallaxdelay", "cameraparallaxmouseinfluence",
                    "zoom", "fov", "nearz", "farz",
                    "bloom", "bloomstrength", "bloomthreshold",
                    "camerashake", "camerashakeamplitude",
                    "camerashakespeed", "camerashakeroughness",
                    "orthogonalprojection" } },
        { 10u, set { "hdr", "norecompile",
                     "bloomhdrfeather", "bloomhdriterations",
                     "bloomhdrscatter", "bloomhdrstrength",
                     "bloomhdrthreshold" } },
        { 20u, set { "bloomtint" } },
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

// Union of objects[].<X> fields read across the four object-kind parsers.
// No version gates: object schemas evolve much more slowly than general,
// and per-kind dispatch is the dominant axis. Keep this list as a flat
// set; the SceneSchema.EveryParsedObjectKeyIsObserved test verifies each
// entry actually shows up in the corpus.
const std::set<std::string>& kParsedObjectKeys() {
    static const std::set<std::string> s = {
        // shared by all kinds
        "id", "name", "visible", "origin", "angles", "scale", "parallaxDepth",
        "locktransforms", "muteineditor", "nointerpolation", "parent",
        "dependencies", "instance",
        // image-only
        "image", "alignment", "colorBlendMode", "color", "alpha", "brightness",
        "size", "effects", "animationlayers", "config",
        "perspective", "copybackground", "solid", "opaquebackground",
        "clampuvs", "castshadow", "disablepropagation", "depthtest",
        "backgroundcolor", "backgroundbrightness",
        // light-only
        "light", "radius", "intensity", "shape",
        "ledsource", "castvolumetrics", "outercone", "innercone", "attenuation",
        "exponent", "density", "volumetricsexponent", "lightsourcesize",
        "mindistance", "cascadedistance0", "cascadedistance1", "cascadedistance2",
        // particle-only
        "particle", "instanceoverride", "particlesrc", "controlpoint",
        // sound-only
        "sound", "volume", "playbackmode", "mintime", "maxtime",
        "startsilent", "blockalign", "spatialization", "queuemode",
        // text-only (WPTextObject)
        "text", "font", "pointsize", "padding", "horizontalalign",
        "verticalalign", "anchor", "maxrows", "maxwidth",
        "limitrows", "limitwidth", "limituseellipsis",
        // model-only (WPModelObject)
        "model", "attachment",
        // camera-only (WPCameraObject)
        "camera", "fov", "zoom", "path",
    };
    return s;
}

std::set<std::string> AllParsedGeneral() {
    std::set<std::string> out;
    for (const auto& [_, ks] : kParsedGeneralKeys())
        out.insert(ks.begin(), ks.end());
    return out;
}

unsigned PkgIntFromStamp(const std::string& s) {
    if (s.size() < 5) return 0;
    return static_cast<unsigned>(std::stoi(s.substr(4)));
}

// Accepts only paths of shape "<prefix><X>" with no further '.' or '['
// — i.e. a direct child of the scope, not a nested sub-field.
bool IsDirectChildOf(std::string_view prefix, std::string_view path) {
    if (! path.starts_with(prefix)) return false;
    return path.find_first_of(".[", prefix.size()) == std::string_view::npos;
}

// Parsed direct children of selected nested parents. Mirrors the parser
// code paths (see WPSceneGeneral / WPParticleObject / WPImageObject /
// WPPuppetLayer parsing). Used by ReportTopUnparsedNestedKeys.
const std::map<std::string, std::set<std::string>>& kParsedNestedKeys() {
    using set = std::set<std::string>;
    static const std::map<std::string, set> m = {
        { "general.orthogonalprojection.",
          // `auto` is read in Orthogonalprojection::FromJson when present,
          // but no scene in the live corpus exercises that branch — listing
          // it here would trip EveryParsedNestedKeyIsObservedSomewhere.
          set { "width", "height" } },
        { "general.lightconfig.",
          set { "directional", "directionalshadow",
                "point", "pointshadow",
                "spot", "spotshadow" } },
        { "objects[].config.",
          set { "passthrough" } },
        { "objects[].instance.",
          set { "id", "combos", "textures", "usertextures" } },
        { "objects[].instanceoverride.",
          set { "alpha", "size", "lifetime", "rate", "speed", "count",
                "brightness", "id", "color", "colorn",
                "controlpoint0", "controlpoint1", "controlpoint2",
                "controlpoint3", "controlpoint4", "controlpoint5",
                "controlpoint6", "controlpoint7",
                "controlpointangle0", "controlpointangle1",
                "controlpointangle2", "controlpointangle3",
                "controlpointangle4", "controlpointangle5",
                "controlpointangle6", "controlpointangle7" } },
        { "objects[].animationlayers[].",
          set { "animation", "blend", "rate", "visible",
                "id", "name",
                "additive", "blendin", "blendout", "blendtime" } },

        // Property-binding side channel — any animatable scalar field on
        // an object can carry an `.animation` curve subtree. The shape is
        // identical regardless of which field it hangs off, so a single
        // <field>.animation.* parent description applies to all fields
        // (origin, scale, alpha, color, angles, parallaxDepth, visible,
        // brightness, alignment, ...). Captured by AbsorbAllFieldBindings
        // into WPFieldBindings::animations.
        { "objects[].alpha.animation.",
          set { "c0", "options" } },
        { "objects[].alpha.animation.options.",
          set { "fps", "length", "mode", "name", "startpaused", "wraploop",
                "smoothing", "children", "events", "parent" } },
        { "objects[].alpha.animation.c0[].",
          set { "frame", "value", "lockangle", "locklength",
                "front", "back" } },
        { "objects[].alpha.animation.c0[].front.",
          set { "enabled", "x", "y", "magic" } },
        { "objects[].alpha.animation.c0[].back.",
          set { "enabled", "x", "y", "magic" } },
        { "objects[].origin.animation.",
          set { "c0", "c1", "c2", "options", "relative" } },
        { "objects[].origin.animation.options.",
          set { "fps", "length", "mode", "name", "startpaused", "wraploop",
                "smoothing", "children", "events", "parent" } },
    };
    return m;
}

const nlohmann::json& Report() {
    static const nlohmann::json r = wallpaper::testing::ScanSceneKeys(WAYWALLEN_WORKSHOP_DIR);
    return r;
}

// Print top-N unparsed direct-child keys per pkg version for the given
// scope. Pure stderr signal — no assertion.
void PrintUnparsedReport(std::string_view prefix, std::string_view scope_label,
                         const std::set<std::string>& parsed, std::size_t top_n) {
    std::cerr << "\n=== unparsed top-level " << scope_label << "<X> keys per pkg version "
                 "(top " << top_n << " by present_in count) ===\n";

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
            if (! IsDirectChildOf(prefix, path)) continue;
            const std::string k { path.substr(prefix.size()) };
            if (parsed.contains(k)) continue;
            miss.push_back({ k, info.value("present_in", std::uint64_t { 0 }) });
        }
        std::sort(miss.begin(), miss.end(),
                  [](auto& a, auto& b) { return a.present_in > b.present_in; });

        std::cerr << "  " << stamp << ": ";
        if (miss.empty()) {
            std::cerr << "(all top-level " << scope_label << "* keys are parsed)";
        } else {
            const std::size_t n = std::min(miss.size(), top_n);
            for (std::size_t i = 0; i < n; ++i) {
                if (i) std::cerr << ", ";
                std::cerr << miss[i].key << "(" << miss[i].present_in << ")";
            }
            if (miss.size() > n) std::cerr << ", … +" << (miss.size() - n) << " more";
        }
        std::cerr << "\n";
    }
}

} // namespace

TEST(SceneSchema, EveryParsedGeneralKeyIsObservedSomewhere) {
    std::set<std::string> observed;
    for (const auto& [_, ver_data] : Report().items()) {
        if (! ver_data.contains("keys")) continue;
        for (const auto& [path, __] : ver_data["keys"].items()) {
            if (! IsDirectChildOf(kGeneralPrefix, path)) continue;
            observed.insert(path.substr(kGeneralPrefix.size()));
        }
    }

    for (const auto& k : AllParsedGeneral()) {
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
            if (! IsDirectChildOf(kGeneralPrefix, path)) continue;
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

TEST(SceneSchema, EveryParsedObjectKeyIsObservedSomewhere) {
    std::set<std::string> observed;
    for (const auto& [_, ver_data] : Report().items()) {
        if (! ver_data.contains("keys")) continue;
        for (const auto& [path, __] : ver_data["keys"].items()) {
            if (! IsDirectChildOf(kObjectsPrefix, path)) continue;
            observed.insert(path.substr(kObjectsPrefix.size()));
        }
    }

    for (const auto& k : kParsedObjectKeys()) {
        EXPECT_TRUE(observed.contains(k))
            << "objects[]." << k
            << " is read by the parser but never appears in any scene "
               "across the corpus — typo or dead declaration?";
    }
}

TEST(SceneSchema, ReportTopUnparsedGeneralKeysPerVersion) {
    PrintUnparsedReport(kGeneralPrefix, "general.", AllParsedGeneral(), 15);
    SUCCEED();
}

TEST(SceneSchema, ReportTopUnparsedObjectKeysPerVersion) {
    PrintUnparsedReport(kObjectsPrefix, "objects[].", kParsedObjectKeys(), 15);
    SUCCEED();
}

TEST(SceneSchema, EveryParsedNestedKeyIsObservedSomewhere) {
    // For each nested-parent in kParsedNestedKeys, assert each declared
    // child appears under that prefix in the corpus. Catches typos in
    // sub-struct field names exactly the same way as the top-level test.
    for (const auto& [parent, parsed] : kParsedNestedKeys()) {
        std::set<std::string> observed;
        for (const auto& [_, ver_data] : Report().items()) {
            if (! ver_data.contains("keys")) continue;
            for (const auto& [path, __] : ver_data["keys"].items()) {
                if (! IsDirectChildOf(parent, path)) continue;
                observed.insert(path.substr(parent.size()));
            }
        }
        for (const auto& k : parsed) {
            EXPECT_TRUE(observed.contains(k))
                << parent << k
                << " is read by the parser but never appears in any scene "
                   "across the corpus — typo or dead declaration?";
        }
    }
}

TEST(SceneSchema, ReportTopUnparsedNestedKeys) {
    // Aggregated (cross-version) miss list per nested parent. Most of
    // these parents are sparsely populated, so per-version columns add
    // noise without insight. Print one row per parent.
    std::cerr << "\n=== unparsed direct-child keys per declared nested parent "
                 "(top 10 by aggregate present_in) ===\n";

    for (const auto& [parent, parsed] : kParsedNestedKeys()) {
        struct Entry {
            std::string   key;
            std::uint64_t present_in;
        };
        std::map<std::string, std::uint64_t> agg;
        for (const auto& [_, ver_data] : Report().items()) {
            if (! ver_data.contains("keys")) continue;
            for (const auto& [path, info] : ver_data["keys"].items()) {
                if (! IsDirectChildOf(parent, path)) continue;
                const std::string k { path.substr(parent.size()) };
                if (parsed.contains(k)) continue;
                agg[k] += info.value("present_in", std::uint64_t { 0 });
            }
        }
        std::vector<Entry> miss;
        miss.reserve(agg.size());
        for (auto& kv : agg) miss.push_back({ kv.first, kv.second });
        std::sort(miss.begin(), miss.end(),
                  [](auto& a, auto& b) { return a.present_in > b.present_in; });

        std::cerr << "  " << parent << "<X>: ";
        if (miss.empty()) {
            std::cerr << "(all observed keys parsed)";
        } else {
            const std::size_t n = std::min(miss.size(), std::size_t { 10 });
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
