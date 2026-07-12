// Per-version gtest fixtures.
//
// One TEST_P per category (pkg / texv / texi / texb / texture format /
// mdlv / mdls / mdla). The parameter list comes from the corpus
// singleton which walks workshop/* and harvests every version stamp
// observed in the wild. Each test slices the corpus to entries
// matching its parameter and asserts:
//   * the slice is non-empty (otherwise the parameter wouldn't have
//     been generated)
//   * every matching asset satisfies category-specific structural
//     invariants (sane dimensions, plausible bone counts, etc.)
//
// Adding a new workshop with a previously-unseen version automatically
// generates a new test instance the next time the binary runs — no
// allow-list bumps required, but the new instance must still satisfy
// the invariants below or it fails immediately.

#include <gtest/gtest.h>

import rstd.cppstd;
import wescene.json;
import wescene.testing.corpus;

using owe::testing::Corpus;

namespace
{

// ----- parameter generators (see header note about static-local refs) ------

const std::vector<std::string>& AllPkgVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().pkg_versions();
        return std::vector<std::string>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllTexvVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().texv_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllTexiVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().texi_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllTexbVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().texb_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllTexFormats() {
    static const auto v = [] {
        const auto& s = Corpus::instance().tex_formats();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllMdlvVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().mdlv_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllMdlsVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().mdls_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}
const std::vector<int>& AllMdlaVersions() {
    static const auto v = [] {
        const auto& s = Corpus::instance().mdla_versions();
        return std::vector<int>(s.begin(), s.end());
    }();
    return v;
}

std::string IntName(int v) { return "V" + std::to_string(v); }

} // namespace

// ============================================================================
// scene.pkg version
// ============================================================================

class ScenePkgVersionTest : public ::testing::TestWithParam<std::string> {};

TEST_P(ScenePkgVersionTest, AllWorkshopsParseAndExposeSaneScene) {
    const auto& version = GetParam();
    auto        slice   = Corpus::instance().workshops_with_pkg(version);
    ASSERT_FALSE(slice.empty()) << "no workshops for " << version;

    for (const auto& ref : slice) {
        const auto& w = *ref.workshop;
        SCOPED_TRACE("workshop " + w.id);

        auto pkg = w.snapshot.get("pkg");
        ASSERT_TRUE(pkg.is_some());
        auto pkg_version = (*pkg)->get("version");
        ASSERT_TRUE(pkg_version.is_some());
        EXPECT_EQ(rstd::cppstd::to_string(*(*pkg_version)->as_str()), version);
        auto file_count = (*pkg)->get("file_count");
        ASSERT_TRUE(file_count.is_some());
        EXPECT_GT((*file_count)->as_i64().unwrap_or(0), 0);
        auto has_scene_json = (*pkg)->get("has_scene_json");
        ASSERT_TRUE(has_scene_json.is_some());
        EXPECT_TRUE((*has_scene_json)->as_bool().unwrap_or(false));

        auto scene = w.snapshot.get("scene");
        ASSERT_TRUE(scene.is_some());
        auto parsed = (*scene)->get("parsed");
        ASSERT_TRUE(parsed.is_some());
        auto error = (*scene)->get("error");
        EXPECT_TRUE((*parsed)->as_bool().unwrap_or(false))
            << "scene.json failed: "
            << (error.is_some() && (*error)->as_str().is_some()
                    ? rstd::cppstd::to_string(*(*error)->as_str())
                    : "");
        auto is_ortho = (*scene)->get("is_ortho");
        if (is_ortho.is_some() && (*is_ortho)->as_bool().unwrap_or(false)) {
            auto ortho = (*scene)->get("ortho");
            ASSERT_TRUE(ortho.is_some());
            auto width  = (*ortho)->get("width");
            auto height = (*ortho)->get("height");
            ASSERT_TRUE(width.is_some());
            ASSERT_TRUE(height.is_some());
            EXPECT_GT((*width)->as_i64().unwrap_or(0), 0);
            EXPECT_GT((*height)->as_i64().unwrap_or(0), 0);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(All, ScenePkgVersionTest, ::testing::ValuesIn(AllPkgVersions()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             return info.param;
                         });

// ============================================================================
// .tex header version stamps
// ============================================================================

class TextureTexvTest : public ::testing::TestWithParam<int> {};
class TextureTexiTest : public ::testing::TestWithParam<int> {};
class TextureTexbTest : public ::testing::TestWithParam<int> {};
class TextureFormatTest : public ::testing::TestWithParam<int> {};

static void CheckTexInvariants(const Corpus::TexRef& ref) {
    const auto& w    = *ref.workshop;
    const auto& t    = *ref.tex;
    auto        path = t.get("path");
    SCOPED_TRACE("workshop " + w.id + " tex " +
                 (path.is_some() && (*path)->as_str().is_some()
                      ? rstd::cppstd::to_string(*(*path)->as_str())
                      : ""));
    auto ok         = t.get("ok");
    auto width      = t.get("width");
    auto height     = t.get("height");
    auto map_width  = t.get("map_width");
    auto map_height = t.get("map_height");
    auto count      = t.get("count");
    ASSERT_TRUE(ok.is_some() && width.is_some() && height.is_some() && map_width.is_some() &&
                map_height.is_some() && count.is_some());
    EXPECT_TRUE((*ok)->as_bool().unwrap_or(false));
    EXPECT_GT((*width)->as_i64().unwrap_or(0), 0);
    EXPECT_GT((*height)->as_i64().unwrap_or(0), 0);
    EXPECT_GT((*map_width)->as_i64().unwrap_or(0), 0);
    EXPECT_GT((*map_height)->as_i64().unwrap_or(0), 0);
    EXPECT_GT((*count)->as_i64().unwrap_or(0), 0);
}

TEST_P(TextureTexvTest, AllInstancesParse) {
    auto slice = Corpus::instance().textures_with_texv(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        auto value = r.tex->get("texv");
        ASSERT_TRUE(value.is_some());
        EXPECT_EQ((*value)->as_i64().unwrap_or(-1), GetParam());
        CheckTexInvariants(r);
    }
}
TEST_P(TextureTexiTest, AllInstancesParse) {
    auto slice = Corpus::instance().textures_with_texi(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        auto value = r.tex->get("texi");
        ASSERT_TRUE(value.is_some());
        EXPECT_EQ((*value)->as_i64().unwrap_or(-1), GetParam());
        CheckTexInvariants(r);
    }
}
TEST_P(TextureTexbTest, AllInstancesParse) {
    auto slice = Corpus::instance().textures_with_texb(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        auto value = r.tex->get("texb");
        ASSERT_TRUE(value.is_some());
        EXPECT_EQ((*value)->as_i64().unwrap_or(-1), GetParam());
        CheckTexInvariants(r);
    }
}
TEST_P(TextureFormatTest, AllInstancesParse) {
    auto slice = Corpus::instance().textures_with_format(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        auto value = r.tex->get("format");
        ASSERT_TRUE(value.is_some());
        EXPECT_EQ((*value)->as_i64().unwrap_or(-1), GetParam());
        CheckTexInvariants(r);
    }
}

INSTANTIATE_TEST_SUITE_P(All, TextureTexvTest, ::testing::ValuesIn(AllTexvVersions()),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return IntName(i.param);
                         });
INSTANTIATE_TEST_SUITE_P(All, TextureTexiTest, ::testing::ValuesIn(AllTexiVersions()),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return IntName(i.param);
                         });
INSTANTIATE_TEST_SUITE_P(All, TextureTexbTest, ::testing::ValuesIn(AllTexbVersions()),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return IntName(i.param);
                         });
INSTANTIATE_TEST_SUITE_P(All, TextureFormatTest, ::testing::ValuesIn(AllTexFormats()),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return "F" + std::to_string(i.param);
                         });

// ============================================================================
// .mdl header version stamps
// ============================================================================

class MdlMdlvTest : public ::testing::TestWithParam<int> {};
class MdlMdlsTest : public ::testing::TestWithParam<int> {};
class MdlMdlaTest : public ::testing::TestWithParam<int> {};

static void CheckMdlInvariants(const Corpus::MdlRef& ref) {
    const auto& w    = *ref.workshop;
    const auto& m    = *ref.mdl;
    auto        path = m.get("path");
    SCOPED_TRACE("workshop " + w.id + " mdl " +
                 (path.is_some() && (*path)->as_str().is_some()
                      ? rstd::cppstd::to_string(*(*path)->as_str())
                      : ""));
    // Failed parses are tolerated (some .mdl files are non-puppet 3D
    // models that WPMdlParser intentionally rejects), but the version
    // stamps must still be readable.
    auto mdlv = m.get("mdlv");
    auto mdls = m.get("mdls");
    auto mdla = m.get("mdla");
    auto ok   = m.get("ok");
    ASSERT_TRUE(mdlv.is_some() && mdls.is_some() && mdla.is_some() && ok.is_some());
    EXPECT_GE((*mdlv)->as_i64().unwrap_or(-1), 0);
    EXPECT_GE((*mdls)->as_i64().unwrap_or(-1), 0);
    EXPECT_GE((*mdla)->as_i64().unwrap_or(-1), 0);
    if ((*ok)->as_bool().unwrap_or(false)) {
        auto bones = m.get("bones");
        ASSERT_TRUE(bones.is_some());
        EXPECT_GT((*bones)->as_i64().unwrap_or(0), 0);
    }
}

TEST_P(MdlMdlvTest, AllInstancesExposeStamps) {
    auto slice = Corpus::instance().mdls_with_mdlv(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        auto value = r.mdl->get("mdlv");
        ASSERT_TRUE(value.is_some());
        EXPECT_EQ((*value)->as_i64().unwrap_or(-1), GetParam());
        CheckMdlInvariants(r);
    }
}
TEST_P(MdlMdlsTest, AllInstancesExposeStamps) {
    auto slice = Corpus::instance().mdls_with_mdls(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        auto value = r.mdl->get("mdls");
        ASSERT_TRUE(value.is_some());
        EXPECT_EQ((*value)->as_i64().unwrap_or(-1), GetParam());
        CheckMdlInvariants(r);
    }
}
TEST_P(MdlMdlaTest, AllInstancesExposeStamps) {
    auto slice = Corpus::instance().mdls_with_mdla(GetParam());
    ASSERT_FALSE(slice.empty());
    for (const auto& r : slice) {
        auto value = r.mdl->get("mdla");
        ASSERT_TRUE(value.is_some());
        EXPECT_EQ((*value)->as_i64().unwrap_or(-1), GetParam());
        CheckMdlInvariants(r);
    }
}

INSTANTIATE_TEST_SUITE_P(All, MdlMdlvTest, ::testing::ValuesIn(AllMdlvVersions()),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return "Mdlv" + std::to_string(i.param);
                         });
INSTANTIATE_TEST_SUITE_P(All, MdlMdlsTest, ::testing::ValuesIn(AllMdlsVersions()),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return "Mdls" + std::to_string(i.param);
                         });
INSTANTIATE_TEST_SUITE_P(All, MdlMdlaTest, ::testing::ValuesIn(AllMdlaVersions()),
                         [](const ::testing::TestParamInfo<int>& i) {
                             return "Mdla" + std::to_string(i.param);
                         });

// ============================================================================
// Smoke: corpus must contain at least one workshop with at least one of
// each major asset class. Catches an empty workshop dir / pathing bug.
// ============================================================================

TEST(CorpusSmoke, NonEmpty) {
    const auto& c = Corpus::instance();
    EXPECT_FALSE(c.entries().empty());
    EXPECT_FALSE(c.pkg_versions().empty());
    EXPECT_FALSE(c.texv_versions().empty());
    EXPECT_FALSE(c.mdlv_versions().empty());
}
