# weweb — Wallpaper Engine *web* renderer (CEF + GLFW v1).
#
# Independently buildable sibling of `wescene`. Shares no link-graph deps
# with the wescene side; wescene-base / parse / render are not pulled in.
# What we *do* share is the project-wide third_party/ FetchContent (CEF
# entry sits next to quickjs in deps.json) and the global pkg-config GLFW
# detection from the root CMakeLists.txt.

cmake_minimum_required(VERSION 3.28)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cef.cmake)

add_library(weweb-host STATIC
    src/Manifest.cpp
    src/UserProperties.cpp
    src/AppHandler.cpp
    src/ClientHandler.cpp
    src/OsrRenderHandler.cpp
    src/BrowserHost.cpp)
target_include_directories(weweb-host PUBLIC src)
target_link_libraries(weweb-host
    PUBLIC  libcef_lib libcef_dll_wrapper nlohmann_json)
target_compile_features(weweb-host PUBLIC cxx_std_20)
weweb_apply_cef_target_settings(weweb-host)
set_property(TARGET weweb-host PROPERTY POSITION_INDEPENDENT_CODE ON)

# Standalone GLFW viewer — only built when the root CMakeLists.txt found
# glfw3 via pkg-config.
if(GLFW_FOUND)
    add_subdirectory(viewer)
else()
    message(STATUS
        "weweb: glfw3 not found via pkg-config; skipping standalone viewer.")
endif()
