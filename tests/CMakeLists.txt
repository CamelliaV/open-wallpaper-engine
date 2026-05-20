# Test suite for wescene-renderer.
#
# Two layers:
#   * gtest binaries (scene_parse_tests, scene_schema_tests, tex_schema_tests,
#     version_tests, deep_version_tests) — assert structural invariants over
#     the corpus. Indexed once via DumpWorkshop, no on-disk fixtures.
#   * wescene-test — a single CLI tool that walks pkgs and runs scene parse,
#     with optional --validate-tex / --validate-shader / --validate-mdl
#     validators and --name / --pkgv / --limit filters. Replaces the older
#     wpparse / wpshadercompile / wpdump / wpscan / wptexparse / wpscript*
#     tools (now archived under tests/old/).

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
set(BUILD_GMOCK OFF CACHE INTERNAL "")
set(INSTALL_GTEST OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(googletest)

# Shared test-side library: corpus index + dump + scene-key scanner +
# scene-parse probe + pkg-header parser. Linked by every gtest binary
# and by wescene-test.
add_library(wescene_test_lib STATIC
    corpus.cpp
    scene_keys.cpp
    scene_parse_probe.cpp
    pkg_header.cpp)
target_link_libraries(
    wescene_test_lib
    PUBLIC
        owe-parse
        owe-shader-compile
        owe-base
        nlohmann_json::nlohmann_json
)
target_include_directories(wescene_test_lib PUBLIC .)
target_compile_features(wescene_test_lib PUBLIC cxx_std_20)
set_target_properties(wescene_test_lib PROPERTIES CXX_SCAN_FOR_MODULES ON)
target_compile_definitions(
    wescene_test_lib PUBLIC
    "WAYWALLEN_WORKSHOP_DIR=\"${CMAKE_SOURCE_DIR}/workshop\""
    "WAYWALLEN_ASSETS_DIR=\"${CMAKE_SOURCE_DIR}/assets\""
    "WAYWALLEN_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

add_executable(version_tests version_tests.cpp)
target_link_libraries(version_tests PRIVATE wescene_test_lib GTest::gtest_main)

# Consolidated CLI: walks pkgs, runs scene parse; --validate-{tex,shader,mdl}
# add per-asset checks; --name / --pkgv / --limit filter the pkg set.
# `valid` subcommand regenerates fixture JSONs via DumpWorkshop.
add_executable(wescene-test wescene_test_main.cpp)
target_link_libraries(wescene-test PRIVATE wescene_test_lib)
set_target_properties(wescene-test PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Regression net for the version-aware WPScene::FromJson path.
# Stays decoupled from Corpus so unrelated .mdl/.tex parser bugs cannot
# mask scene.json parse regressions.
add_executable(scene_parse_tests scene_parse_tests.cpp)
target_link_libraries(scene_parse_tests PRIVATE wescene_test_lib GTest::gtest_main)

# Schema reverse coverage: asserts the parser doesn't read keys that
# never appear in the corpus, and prints unparsed-but-common keys per
# pkg version as a TODO list for future refactor passes.
add_executable(scene_schema_tests scene_schema_tests.cpp)
target_link_libraries(scene_schema_tests PRIVATE wescene_test_lib GTest::gtest_main)

# Same idea, applied to the binary .tex container's (texv, texi, texb,
# texs) sub-version tuples. Asserts every observed tuple parses and is
# in the supported set documented by WPTexFormatVersion.
add_executable(tex_schema_tests tex_schema_tests.cpp)
target_link_libraries(tex_schema_tests PRIVATE wescene_test_lib GTest::gtest_main)

# JS runtime tests: setTimeout/setInterval/clearTimeout/clearInterval and
# SceneNode wrapper surface. Stands separately from wescene_test_lib so
# it can pull owe-script directly.
add_executable(script_runtime_tests script_runtime_tests.cpp)
target_link_libraries(script_runtime_tests PRIVATE owe-script GTest::gtest_main)
set_target_properties(script_runtime_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Per-version deep tests live in tests/version/<category>_<version>_test.cpp.
# Each is a tiny gtest TU built into the same binary so the corpus + gtest
# fixture machinery is amortised across them.
file(GLOB version_test_srcs CONFIGURE_DEPENDS "version/*_test.cpp")
if(version_test_srcs)
    add_executable(deep_version_tests ${version_test_srcs})
    target_link_libraries(deep_version_tests PRIVATE wescene_test_lib GTest::gtest_main)
    target_include_directories(deep_version_tests PRIVATE .)
endif()

# Use gtest_discover_tests so each TEST_P instance becomes its own ctest
# entry, runnable individually with `ctest -R ScenePkgVersionTest`.
include(GoogleTest)
if(TARGET deep_version_tests)
    gtest_discover_tests(
        deep_version_tests
        PROPERTIES TIMEOUT 30
        DISCOVERY_TIMEOUT 60
    )
endif()
gtest_discover_tests(
    scene_parse_tests
    PROPERTIES TIMEOUT 60
    DISCOVERY_TIMEOUT 60
)
gtest_discover_tests(
    scene_schema_tests
    PROPERTIES TIMEOUT 60
    DISCOVERY_TIMEOUT 60
)
gtest_discover_tests(
    tex_schema_tests
    PROPERTIES TIMEOUT 60
    DISCOVERY_TIMEOUT 60
)
gtest_discover_tests(
    script_runtime_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
