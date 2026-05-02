cmake_minimum_required(VERSION 3.28)
project(wescene-renderer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

link_libraries(Eigen3::Eigen)
add_subdirectory(src)


# Test suite — gated by BUILD_TESTS (default ON). Include CTest so
# gtest_discover_tests / add_test inside tests/ register with ctest.
option(BUILD_TESTS "Build the wescene-renderer test suite" ON)
if(BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()

option(BUILD_WAYWALLEN "Build the waywallen-renderer executable" ON)
if(BUILD_WAYWALLEN)
    add_subdirectory(waywallen)
endif()

# Standalone GLFW viewer — auto-detected from glfw3 by default; force with
# -DBUILD_VIEWER=ON/OFF.
pkg_check_modules(GLFW IMPORTED_TARGET glfw3)
option(BUILD_VIEWER "Build the GLFW standalone viewer" ${GLFW_FOUND})
if(BUILD_VIEWER)
    if(NOT GLFW_FOUND)
        message(FATAL_ERROR
            "BUILD_VIEWER=ON but glfw3 was not found via pkg-config. "
            "Install glfw3 development headers or pass -DBUILD_VIEWER=OFF.")
    endif()
    add_subdirectory(viewer)
endif()
