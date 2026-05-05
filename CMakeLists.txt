cmake_minimum_required(VERSION 3.28)
project(owe)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)

# Restrict to Clang.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT WAYWALLEN_ALLOW_NONCLANG)
    message(FATAL_ERROR
        "wescene-renderer requires Clang (got ${CMAKE_CXX_COMPILER_ID}). "
        "Re-run cmake with `-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`, "
        "or pass -DWAYWALLEN_ALLOW_NONCLANG=ON to bypass this check.")
endif()


find_package(PkgConfig REQUIRED)


include_directories(SYSTEM third_party)
add_subdirectory(third_party)

option(BUILD_WESCENE "Build the wescene (Wallpaper Engine *scene*) renderer" ON)
option(BUILD_WEWEB   "Build the weweb (Wallpaper Engine *web*) renderer"     ON)

if(BUILD_WEWEB)
    include(${CMAKE_SOURCE_DIR}/cmake/CEF.cmake)
endif()

add_subdirectory(src)


option(BUILD_TESTS "Build the wescene-renderer test suite" OFF)
if(BUILD_WESCENE AND BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()

option(BUILD_WAYWALLEN "Build the waywallen-renderer executable" ON)
if(BUILD_WAYWALLEN)
    add_subdirectory(waywallen)
endif()

pkg_check_modules(GLFW IMPORTED_TARGET glfw3)
option(BUILD_VIEWER "Build the GLFW standalone viewer" ${GLFW_FOUND})
if((BUILD_WESCENE AND BUILD_VIEWER) OR (BUILD_WEWEB AND BUILD_VIEWER))
    if(NOT GLFW_FOUND)
        message(FATAL_ERROR
            "Need glfw3 for any standalone viewer. Install glfw3 "
            "development headers or set BUILD_VIEWER=OFF and BUILD_WEWEB=OFF.")
    endif()
    add_subdirectory(viewer)
endif()
