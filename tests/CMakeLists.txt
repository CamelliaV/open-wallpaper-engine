# Test suite for wescene-renderer.
#
# Two layers:
#   * gtest binaries (scene_parse_tests, scene_schema_tests, tex_schema_tests,
#     version_tests, deep_version_tests) -- assert structural invariants over
#     the corpus. Indexed once via DumpWorkshop, no on-disk fixtures.
#   * wescene-test -- a single CLI tool that walks pkgs and runs scene parse,
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

add_library(wescene_test_json_builder STATIC)
target_sources(wescene_test_json_builder
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES JsonBuilder.cppm)
target_link_libraries(wescene_test_json_builder PUBLIC owe-core)
target_compile_features(wescene_test_json_builder PUBLIC cxx_std_20)
set_target_properties(wescene_test_json_builder PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Shared test-side library: corpus index + dump + scene-key scanner +
# scene-parse probe + pkg-header parser. Linked by every gtest binary
# and by wescene-test.
add_library(wescene_test_lib STATIC)
target_sources(wescene_test_lib
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
        PkgHeader.cppm
        SceneParseProbe.cppm
        SceneKeys.cppm
        Corpus.cppm)
target_link_libraries(
    wescene_test_lib
    PUBLIC
        owe-parse
        owe-shader-compile
        owe-base
        wescene_test_json_builder
)
target_compile_features(wescene_test_lib PUBLIC cxx_std_20)
set_target_properties(wescene_test_lib PROPERTIES CXX_SCAN_FOR_MODULES ON)
target_compile_definitions(
    wescene_test_lib PUBLIC
    "WAYWALLEN_WORKSHOP_DIR=\"${CMAKE_SOURCE_DIR}/workshop\""
    "WAYWALLEN_ASSETS_DIR=\"${CMAKE_SOURCE_DIR}/assets\""
    "WAYWALLEN_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures\"")

add_executable(version_tests version_tests.cpp)
target_link_libraries(version_tests PRIVATE wescene_test_lib GTest::gtest_main)
set_target_properties(version_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(json_adapter_tests json_adapter_tests.cpp)
target_link_libraries(json_adapter_tests PRIVATE wescene_test_json_builder GTest::gtest_main)
set_target_properties(json_adapter_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(vfs_tests vfs_tests.cpp)
target_link_libraries(vfs_tests PRIVATE owe-base GTest::gtest_main)
set_target_properties(vfs_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(image_parser_tests image_parser_tests.cpp)
target_link_libraries(image_parser_tests PRIVATE owe-parse GTest::gtest_main)
set_target_properties(image_parser_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Consolidated CLI: walks pkgs, runs scene parse; --validate-{tex,shader,mdl}
# add per-asset checks; --name / --pkgv / --limit filter the pkg set.
# `valid` subcommand regenerates fixture JSONs via DumpWorkshop.
add_executable(wescene-test wescene_test_main.cpp)
target_link_libraries(wescene-test PRIVATE wescene_test_lib owe-cli)
set_target_properties(wescene-test PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Regression net for the version-aware SceneMetadata::FromJson path.
# Stays decoupled from Corpus so unrelated .mdl/.tex parser bugs cannot
# mask scene.json parse regressions.
add_executable(scene_parse_tests scene_parse_tests.cpp)
target_link_libraries(scene_parse_tests PRIVATE wescene_test_lib GTest::gtest_main)
set_target_properties(scene_parse_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Unit coverage for the WE shader annotation parser. Verifies dead-branch
# tolerance, comment handling, and the [COMBO] / texture / scalar / u_*
# schemas.
add_executable(wpshader_parse_tests wpshader_parse_tests.cpp)
target_link_libraries(wpshader_parse_tests PRIVATE owe-parse GTest::gtest_main)
set_target_properties(wpshader_parse_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(wpmaterial_parse_tests wpmaterial_parse_tests.cpp)
target_link_libraries(wpmaterial_parse_tests PRIVATE owe-parse GTest::gtest_main)
set_target_properties(wpmaterial_parse_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(text_geometry_tests text_geometry_tests.cpp)
target_link_libraries(text_geometry_tests PRIVATE owe-parse GTest::gtest_main)
set_target_properties(text_geometry_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(wpmdl_mesh_tests wpmdl_mesh_tests.cpp)
target_link_libraries(wpmdl_mesh_tests PRIVATE wescene_test_lib GTest::gtest_main)
set_target_properties(wpmdl_mesh_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(scene_runtime_tests scene_runtime_tests.cpp)
target_link_libraries(scene_runtime_tests PRIVATE wescene_test_lib owe-script GTest::gtest_main)
set_target_properties(scene_runtime_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(scene_resource_index_tests scene_resource_index_tests.cpp)
target_link_libraries(scene_resource_index_tests PRIVATE owe-base GTest::gtest_main)
set_target_properties(scene_resource_index_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(particle_system_tests particle_system_tests.cpp)
target_link_libraries(particle_system_tests PRIVATE owe-base GTest::gtest_main)
set_target_properties(particle_system_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(buffer_resolver_tests buffer_resolver_tests.cpp)
target_link_libraries(buffer_resolver_tests PRIVATE owe-render GTest::gtest_main)
set_target_properties(buffer_resolver_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(render_resource_request_tests render_resource_request_tests.cpp)
target_link_libraries(render_resource_request_tests PRIVATE owe-render GTest::gtest_main)
set_target_properties(render_resource_request_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(resource_model_tests resource_model_tests.cpp)
target_link_libraries(resource_model_tests PRIVATE owe-resource GTest::gtest_main)
set_target_properties(resource_model_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(resource_registry_tests resource_registry_tests.cpp)
target_link_libraries(resource_registry_tests PRIVATE owe-resource-registry GTest::gtest_main)
set_target_properties(resource_registry_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

add_executable(render_graph_debug_tests render_graph_debug_tests.cpp)
target_link_libraries(render_graph_debug_tests PRIVATE owe-render GTest::gtest_main)
set_target_properties(render_graph_debug_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Unit coverage for the line-aware shader-source Cursor / LineWalker /
# preprocessor classifier in :shader_lex. Stand-alone -- only links owe-parse
# for the partition module surface.
add_executable(shader_lex_tests shader_lex_tests.cpp)
target_link_libraries(shader_lex_tests PRIVATE owe-parse GTest::gtest_main)
set_target_properties(shader_lex_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Schema reverse coverage: asserts the parser doesn't read keys that
# never appear in the corpus, and prints unparsed-but-common keys per
# pkg version as a TODO list for future refactor passes.
add_executable(scene_schema_tests scene_schema_tests.cpp)
target_link_libraries(scene_schema_tests PRIVATE wescene_test_lib GTest::gtest_main)
set_target_properties(scene_schema_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Same idea, applied to the binary .tex container's (texv, texi, texb,
# texs) sub-version tuples. Asserts every observed tuple parses and is
# in the supported set documented by WPTexFormatVersion.
add_executable(tex_schema_tests tex_schema_tests.cpp)
target_link_libraries(tex_schema_tests PRIVATE wescene_test_lib GTest::gtest_main)
set_target_properties(tex_schema_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

# JS runtime tests: setTimeout/setInterval/clearTimeout/clearInterval and
# SceneNode wrapper surface. Stands separately from wescene_test_lib so
# it can pull owe-script directly.
add_executable(script_runtime_tests script_runtime_tests.cpp)
target_link_libraries(
    script_runtime_tests PRIVATE owe-script wescene_test_json_builder GTest::gtest_main)
set_target_properties(script_runtime_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Per-version deep tests: one big TEST_P file. The fixture macro stamps
# out one TEST_P + INSTANTIATE_TEST_SUITE_P per category (pkg / texb /
# mdlv / format).
add_executable(deep_version_tests deep_version_tests.cpp)
target_link_libraries(deep_version_tests PRIVATE wescene_test_lib GTest::gtest_main)
set_target_properties(deep_version_tests PROPERTIES CXX_SCAN_FOR_MODULES ON)

# Use gtest_discover_tests so each TEST_P instance becomes its own ctest
# entry, runnable individually with `ctest -R ScenePkgVersionTest`.
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
include(GoogleTest)
gtest_discover_tests(
    deep_version_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 60
)
gtest_discover_tests(
    json_adapter_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    vfs_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    image_parser_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    scene_parse_tests
    PROPERTIES TIMEOUT 60
    DISCOVERY_TIMEOUT 60
)
gtest_discover_tests(
    wpshader_parse_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    wpmaterial_parse_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    text_geometry_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    wpmdl_mesh_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    scene_runtime_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    scene_resource_index_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    particle_system_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    buffer_resolver_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    render_resource_request_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    resource_model_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    resource_registry_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    render_graph_debug_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
)
gtest_discover_tests(
    shader_lex_tests
    PROPERTIES TIMEOUT 30
    DISCOVERY_TIMEOUT 30
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
