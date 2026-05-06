# Test suite for wescene-renderer.
#
# Strategy: a single gtest binary indexes workshop/* once via DumpWorkshop
# (no on-disk fixtures) and uses TEST_P fixtures to register one test
# instance per (category, version) pair observed in the corpus. Each
# instance asserts category-specific structural invariants on the slice
# of assets matching its parameter.
#
# Adding a new workshop with a previously-unseen pkg/tex/mdl version
# automatically grows the test matrix on the next run — no allow-list
# bumps required, but the new instance must still satisfy the invariants
# in version_tests.cpp or it fails immediately.

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

# Shared library for the dumper + corpus index. Used by the gtest binary
# and any future ad-hoc tools.
add_library(wpdump_lib STATIC
    dump.cpp
    corpus.cpp
    scene_keys.cpp
    scene_parse_probe.cpp
    pkg_header.cpp)
# Test infrastructure only needs scene-parse + shader-compile + base — no
# Vulkan runtime, no render graph, no SceneWallpaper. Drops ~25MB off
# every test binary and removes the libvulkan runtime dependency.
target_link_libraries(
    wpdump_lib
    PUBLIC
        owe-parse
        owe-shader-compile
        owe-base
        nlohmann_json::nlohmann_json
)
target_include_directories(wpdump_lib PUBLIC .)
target_compile_features(wpdump_lib PUBLIC cxx_std_20)
set_target_properties(wpdump_lib PROPERTIES CXX_SCAN_FOR_MODULES ON)
target_compile_definitions(
    wpdump_lib PUBLIC
    "WAYWALLEN_WORKSHOP_DIR=\"${CMAKE_SOURCE_DIR}/workshop\""
    "WAYWALLEN_ASSETS_DIR=\"${CMAKE_SOURCE_DIR}/assets\""
    "WAYWALLEN_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

add_executable(version_tests version_tests.cpp)
target_link_libraries(version_tests PRIVATE wpdump_lib GTest::gtest_main)

# CLI for fixture authoring: dumps a single workshop to a JSON file.
add_executable(wpdump wpdump_main.cpp)
target_link_libraries(wpdump PRIVATE wpdump_lib)

# Aggregator: walks all workshops, scans every scene.json, and writes a
# per-pkg-version key-path frequency report. Used to discover unknown /
# rare scene.json fields and schema drift across WE versions.
add_executable(wpscan wpscan_main.cpp)
target_link_libraries(wpscan PRIVATE wpdump_lib)

# Userscript dumper / aggregator. Walks every scene.pkg, finds every
# {"script":..., "scriptproperties":...} per-field binding inside scene.json,
# and writes scripts.jsonl + report.json to OUT_DIR. The deduped sources are
# the empirical WE built-in script catalogue and drive the QuickJS surface.
add_executable(wpscriptdump wpscriptdump_main.cpp)
target_link_libraries(wpscriptdump PRIVATE wpdump_lib)
set_target_properties(wpscriptdump PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Verifier: replays scripts.jsonl through a fresh JsRuntime per binding and
# asserts the runtime can compile + tick every observed userscript without
# tripping a JS exception. Cluster pass-rates also reported per field name.
add_executable(wpscriptrun wpscriptrun_main.cpp)
target_link_libraries(wpscriptrun PRIVATE wpdump_lib owe-script)
set_target_properties(wpscriptrun PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Host-only shader replay: reads a JSONL manifest produced by viewer
# under WP_SHADER_RECORD and re-runs WPShaderParser::CompileToSpv on
# each record. No Vulkan device, no GLFW, no rendering. Used as the
# primary regression net for the shader-compile pipeline.
add_executable(wpshadercompile wpshadercompile_main.cpp)
target_link_libraries(wpshadercompile PRIVATE wpdump_lib)
set_target_properties(wpshadercompile PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Host-only .tex full-decode iteration: walks every workshop, runs
# WPTexImageParser on every /materials/*.tex (header by default, full
# pixel decode with --full). The broad coverage net for the .tex format;
# tex_schema_tests asserts schema invariants on a sampled subset.
add_executable(wptexparse wptexparse_main.cpp)
target_link_libraries(wptexparse PRIVATE wpdump_lib)
set_target_properties(wptexparse PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Host-only scene.json parse iteration: walks every workshop and runs
# both axes — WPScene::FromJson (pure JSON) and WPSceneParser::Parse
# (full pipeline incl. shader compile, mdl/tex parse, material setup) —
# on every pkg. Broader than scene_parse_tests (which only does FromJson)
# and broader than wpdump (which doesn't run SceneParser at all).
add_executable(wpparse wpparse_main.cpp)
target_link_libraries(wpparse PRIVATE wpdump_lib)
set_target_properties(wpparse PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Regression net for the version-aware WPScene::FromJson path.
# Stays decoupled from Corpus so unrelated .mdl/.tex parser bugs cannot
# mask scene.json parse regressions.
add_executable(scene_parse_tests scene_parse_tests.cpp)
target_link_libraries(scene_parse_tests PRIVATE wpdump_lib GTest::gtest_main)

# Schema reverse coverage: asserts the parser doesn't read keys that
# never appear in the corpus, and prints unparsed-but-common keys per
# pkg version as a TODO list for future refactor passes.
add_executable(scene_schema_tests scene_schema_tests.cpp)
target_link_libraries(scene_schema_tests PRIVATE wpdump_lib GTest::gtest_main)

# Same idea, applied to the binary .tex container's (texv, texi, texb,
# texs) sub-version tuples. Asserts every observed tuple parses and is
# in the supported set documented by WPTexFormatVersion.
add_executable(tex_schema_tests tex_schema_tests.cpp)
target_link_libraries(tex_schema_tests PRIVATE wpdump_lib GTest::gtest_main)

# Per-version deep tests live in tests/version/<category>_<version>_test.cpp.
# Each is a tiny gtest TU built into the same binary so the corpus + gtest
# fixture machinery is amortised across them.
file(GLOB version_test_srcs CONFIGURE_DEPENDS "version/*_test.cpp")
if(version_test_srcs)
    add_executable(deep_version_tests ${version_test_srcs})
    target_link_libraries(deep_version_tests PRIVATE wpdump_lib GTest::gtest_main)
    target_include_directories(deep_version_tests PRIVATE .)
endif()

# Use gtest_discover_tests so each TEST_P instance becomes its own ctest
# entry, runnable individually with `ctest -R ScenePkgVersionTest`.
include(GoogleTest)
#gtest_discover_tests(
#    version_tests
#    PROPERTIES TIMEOUT 30
#    DISCOVERY_TIMEOUT 60
#)
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
