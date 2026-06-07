// scene.json parse regression net.
//
// One TEST_P per observed PKGV version. For each, every workshop with
// that version is re-opened, scene.json read via VFS, and parsed via the
// canonical version-bearing WPScene::FromJson(json, pkg_version) path.
// Establishes the baseline for the upcoming WPScene refactor that splits
// FromJson by version: a regression here means the refactor changed
// what was previously parseable.
//
// Deliberately does not depend on Corpus / DumpWorkshop — those exercise
// WPMdlParser / WPTexImageParser too, which can hit unrelated assertions
// on rare .mdl inputs and would mask scene.json regressions.

#include <gtest/gtest.h>

import rstd.cppstd;
import wescene.testing.scene_parse_probe;

namespace
{

const std::vector<owe::testing::WorkshopProbe>& AllWorkshopProbes() {
    static const auto v = owe::testing::EnumerateWorkshopProbes(WAYWALLEN_WORKSHOP_DIR);
    return v;
}

const std::vector<std::string>& ObservedPkgVersionStamps() {
    static const auto v = [] {
        std::set<std::string> uniq;
        for (const auto& p : AllWorkshopProbes())
            if (! p.pkg_stamp.empty()) uniq.insert(p.pkg_stamp);
        return std::vector<std::string>(uniq.begin(), uniq.end());
    }();
    return v;
}

} // namespace

class ScenePkgVersionParseTest : public ::testing::TestWithParam<std::string> {};

TEST_P(ScenePkgVersionParseTest, AllWorkshopsParseAtExplicitVersion) {
    const auto& stamp = GetParam();
    std::size_t hits  = 0;
    for (const auto& p : AllWorkshopProbes()) {
        if (p.pkg_stamp != stamp) continue;
        ++hits;
        SCOPED_TRACE("workshop " + p.id + " " + p.pkg_stamp);

        auto r = owe::testing::ProbeSceneParse(p.dir);
        EXPECT_TRUE(r.ok) << "ProbeSceneParse failed: " << r.error;
        EXPECT_EQ(r.pkg_version, p.pkg_version) << "pkg_version mismatch for " << p.id;
    }
    EXPECT_GT(hits, 0u) << "no workshops for " << stamp;
}

INSTANTIATE_TEST_SUITE_P(All, ScenePkgVersionParseTest,
                         ::testing::ValuesIn(ObservedPkgVersionStamps()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             return info.param;
                         });

TEST(SceneParseSmoke, EnumeratesNonEmpty) {
    EXPECT_FALSE(AllWorkshopProbes().empty());
    EXPECT_FALSE(ObservedPkgVersionStamps().empty());
}
