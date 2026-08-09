# Top-level viewer dir hosts both standalone viewers:
#   - SceneViewer : wescene  scene.pkg viewer (BUILD_WESCENE + BUILD_VIEWER)
#   - WebViewer   : weweb    HTML/JS  viewer (BUILD_WEWEB    + glfw3 found)
# Modules:
#   - viewer.common — argv/option parsing + GLFW/exe-dir helpers. Always built.
#   - viewer.web    — WebViewer-owned Presenter implementations. BUILD_WEWEB only.

# viewer.common: shared helpers + arg parsing.
add_library(viewer-common STATIC)
target_sources(viewer-common
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
        Common/Common.cppm
        Common/Audio.cppm
        Common/Arg.cppm)
target_sources(viewer-common PRIVATE Common/Common.cpp)
target_include_directories(viewer-common PUBLIC Common)
target_link_libraries(viewer-common
    PUBLIC PkgConfig::GLFW owe-cli owe-audio-response wavsen::audio rstd.cppstd)
target_compile_features(viewer-common PUBLIC cxx_std_20)
set_target_properties(viewer-common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_SCAN_FOR_MODULES ON)

# wescene scene viewer.
if(BUILD_WESCENE AND BUILD_VIEWER)
    add_executable(SceneViewer SceneViewer/SceneViewer.cpp)
    target_link_libraries(SceneViewer
        PRIVATE
            viewer-common
            PkgConfig::GLFW
            owe-renderer)
    set_target_properties(SceneViewer PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

# weweb web viewer.
if(BUILD_WEWEB)
    find_package(Vulkan REQUIRED)
    pkg_check_modules(WEWEB_EGL    REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(WEWEB_GLES3  REQUIRED IMPORTED_TARGET glesv2)
    pkg_check_modules(WEWEB_X11    REQUIRED IMPORTED_TARGET x11)
    pkg_check_modules(WEWEB_WLEGL  REQUIRED IMPORTED_TARGET wayland-egl)

    add_executable(WebViewer WebViewer/WebViewer.cpp)
    target_sources(WebViewer
        PRIVATE FILE_SET cxx_modules TYPE CXX_MODULES FILES
            WebViewer/Web.cppm
            WebViewer/Presenter.cppm
            WebViewer/VulkanBlitter.cppm
            WebViewer/EglPresenter.cppm)
    target_sources(WebViewer PRIVATE
        WebViewer/VulkanBlitter.cpp
        WebViewer/EglPresenter.cpp)
    target_include_directories(WebViewer PRIVATE
        WebViewer
        ${CMAKE_SOURCE_DIR}/src/Web)
    target_link_libraries(WebViewer PRIVATE
        viewer-common
        owe-web-api
        owe-web-host
        rstd.cppstd
        vvk::ffi::vulkan
        wavsen::audio
        PkgConfig::GLFW
        PkgConfig::WEWEB_EGL
        PkgConfig::WEWEB_GLES3
        PkgConfig::WEWEB_X11
        PkgConfig::WEWEB_WLEGL
        Vulkan::Vulkan)
    target_compile_features(WebViewer PRIVATE cxx_std_20)
    set_target_properties(WebViewer PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_SCAN_FOR_MODULES ON
        BUILD_RPATH   "\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON)
    weweb_stage_cef_runtime(WebViewer)
endif()
