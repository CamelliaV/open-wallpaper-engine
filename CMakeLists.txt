cmake_minimum_required(VERSION 3.28)
project(wescene-renderer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)

# Restrict to Clang. The renderer + tests rely on Clang-specific behaviour
# (clang-format style, sanitizer flags wired in src/CMakeLists.txt, and
# observed glslang/FetchContent quirks) and we have no CI matrix for GCC.
# Override with -DWAYWALLEN_ALLOW_NONCLANG=ON if you know what you're doing.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT WAYWALLEN_ALLOW_NONCLANG)
    message(FATAL_ERROR
        "wescene-renderer requires Clang (got ${CMAKE_CXX_COMPILER_ID}). "
        "Re-run cmake with `-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`, "
        "or pass -DWAYWALLEN_ALLOW_NONCLANG=ON to bypass this check.")
endif()


find_package(PkgConfig REQUIRED)


include_directories(SYSTEM third_party)
add_subdirectory(third_party)

# open-wallpaper-engine hosts two Wallpaper Engine renderer subprojects:
#   * wescene — the WE *scene* (`scene.pkg`) renderer (Vulkan).
#   * weweb   — the WE *web*   (HTML/JS) renderer    (CEF, GLFW v1).
# Each can be built independently; their libraries deliberately don't share
# a transitive dep edge, so `-DBUILD_WESCENE=OFF` won't drag scene-only
# baggage into a weweb-only build, and vice versa.
option(BUILD_WESCENE "Build the wescene (Wallpaper Engine *scene*) renderer" ON)
option(BUILD_WEWEB   "Build the weweb (Wallpaper Engine *web*) renderer"     OFF)

# CEF integration must be set up at top-level scope so libcef_lib /
# libcef_dll_wrapper / weweb_stage_cef_runtime are visible to consumers in
# both src/ (weweb-host) and viewer/ (webviewer).
if(BUILD_WEWEB)
    include(${CMAKE_SOURCE_DIR}/cmake/CEF.cmake)
endif()

# src/ now hosts both wescene libraries (gated by BUILD_WESCENE inside) and
# weweb-host under Web/ (gated by BUILD_WEWEB inside). Always recurse — the
# CMakeLists.txt there guards each block independently.
add_subdirectory(src)


# Test suite — gated by BUILD_TESTS (default OFF; pulls in googletest via
# FetchContent and walks the workshop corpus, both costly). Enable with
# -DBUILD_TESTS=ON when iterating on parsers / fixtures. Include CTest so
# gtest_discover_tests / add_test inside tests/ register with ctest.
option(BUILD_TESTS "Build the wescene-renderer test suite" OFF)
if(BUILD_WESCENE AND BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()

option(BUILD_WAYWALLEN "Build the waywallen-renderer executable" ON)
if(BUILD_WESCENE AND BUILD_WAYWALLEN)
    add_subdirectory(waywallen)
endif()

# Standalone GLFW viewers — auto-detected from glfw3 by default; force with
# -DBUILD_VIEWER=ON/OFF. Both SceneViewer (wescene) and WebViewer (weweb)
# live in the top-level viewer/ directory.
pkg_check_modules(GLFW IMPORTED_TARGET glfw3)
option(BUILD_VIEWER "Build the GLFW standalone viewer" ${GLFW_FOUND})
if((BUILD_WESCENE AND BUILD_VIEWER) OR BUILD_WEWEB)
    if(NOT GLFW_FOUND)
        message(FATAL_ERROR
            "Need glfw3 for any standalone viewer. Install glfw3 "
            "development headers or set BUILD_VIEWER=OFF and BUILD_WEWEB=OFF.")
    endif()
    add_subdirectory(viewer)
endif()
