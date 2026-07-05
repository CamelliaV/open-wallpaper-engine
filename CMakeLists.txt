cmake_minimum_required(VERSION 3.28)

if(CMAKE_VERSION VERSION_LESS_EQUAL "4.3.0")
    message(FATAL_ERROR
        "CMake ${CMAKE_VERSION} is not supported. "
        "CMake 4.3.0 and older can generate broken C++20 module BMI rules with Clang 22. "
        "Use CMake 4.3.1 or newer.")
endif()

set(OWE_PLUGIN_VERSION 0.1.10)
set(OWE_WAYWALLEN_PLUGIN_ID org.waywallen.open-wallpaper-engine)
set(OWE_WAYWALLEN_PLUGIN_UPDATE_URL
    "https://github.com/waywallen/open-wallpaper-engine/raw/refs/heads/main/update.json"
    CACHE STRING "URL of the waywallen plugin update manifest")

project(owe VERSION ${OWE_PLUGIN_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_INSTALL_RPATH "$ORIGIN")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
set(CMAKE_INSTALL_REMOVE_ENVIRONMENT_RPATH ON)

# Restrict to Clang.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT WAYWALLEN_ALLOW_NONCLANG)
    message(FATAL_ERROR
        "wescene-renderer requires Clang (got ${CMAKE_CXX_COMPILER_ID}). "
        "Re-run cmake with `-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang`, "
        "or pass -DWAYWALLEN_ALLOW_NONCLANG=ON to bypass this check.")
endif()


find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

if(CMAKE_USE_PTHREADS_INIT)
    # Clang records pthread support in C++ module BMIs; keep all module targets compatible.
    add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-pthread>")
    add_link_options("$<$<LINK_LANG_AND_ID:CXX,Clang,GNU>:-pthread>")
endif()

include_directories(SYSTEM third_party)
add_subdirectory(third_party)

option(BUILD_WESCENE "Build the wescene (Wallpaper Engine *scene*) renderer" ON)
option(BUILD_WEWEB   "Build the weweb (Wallpaper Engine *web*) renderer"     ON)
option(OWE_WAYWALLEN_PLUGIN_BUNDLE_LAYOUT
    "Install waywallen plugin files as a plugin-root bundle for CPack"
    OFF)

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

if(OWE_WAYWALLEN_PLUGIN_BUNDLE_LAYOUT)
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _OWE_CPACK_SYSTEM)
    set(CPACK_GENERATOR ZIP_ZSTD)
    set(CPACK_ARCHIVE_COMPRESSION_LEVEL 5)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_PACKAGE_NAME "${OWE_WAYWALLEN_PLUGIN_ID}")
    set(CPACK_PACKAGE_VENDOR "waywallen")
    set(CPACK_PACKAGE_VERSION "${OWE_PLUGIN_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME
        "${OWE_WAYWALLEN_PLUGIN_ID}-${OWE_PLUGIN_VERSION}-${_OWE_CPACK_SYSTEM}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/")
    set(CPACK_STRIP_FILES TRUE)
    include(CPack)
endif()
