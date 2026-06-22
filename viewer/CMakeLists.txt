# Top-level viewer dir hosts both standalone viewers:
#   - SceneViewer : wescene  scene.pkg viewer (BUILD_WESCENE + BUILD_VIEWER)
#   - WebViewer   : weweb    HTML/JS  viewer (BUILD_WEWEB    + glfw3 found)
# Modules:
#   - viewer.common — argv/option parsing + GLFW/exe-dir helpers. Always built.
#   - viewer.web    — Presenter + VulkanBlitter + EglPresenter. BUILD_WEWEB only.

# viewer.common: shared helpers + arg parsing.
add_library(viewer-common STATIC)
target_sources(viewer-common
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
        Common.cppm
        Arg.cppm)
target_sources(viewer-common PRIVATE Common.cpp)
target_include_directories(viewer-common PUBLIC .)
target_link_libraries(viewer-common PUBLIC PkgConfig::GLFW argparse::argparse rstd.cppstd)
target_compile_features(viewer-common PUBLIC cxx_std_20)
set_target_properties(viewer-common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_SCAN_FOR_MODULES ON)

# viewer.web: CEF DMA-BUF presenter implementations (Vulkan + EGL/GLES).
if(BUILD_WEWEB)
    find_package(Vulkan REQUIRED)
    pkg_check_modules(WEWEB_EGL    REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(WEWEB_GLES3  REQUIRED IMPORTED_TARGET glesv2)
    pkg_check_modules(WEWEB_X11    REQUIRED IMPORTED_TARGET x11)
    pkg_check_modules(WEWEB_WLEGL  REQUIRED IMPORTED_TARGET wayland-egl)

    add_library(viewer-web STATIC)
    target_sources(viewer-web
        PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
            Web.cppm
            Presenter.cppm
            VulkanBlitter.cppm
            EglPresenter.cppm)
    target_sources(viewer-web PRIVATE VulkanBlitter.cpp EglPresenter.cpp)
    target_include_directories(viewer-web
        PUBLIC . ${CMAKE_SOURCE_DIR}/src/Web)
    target_link_libraries(viewer-web
        PUBLIC
            viewer-common
            owe-web-api
            rstd.cppstd
            wavsen::ffi::vulkan
            PkgConfig::GLFW
            PkgConfig::WEWEB_EGL
            PkgConfig::WEWEB_GLES3
            PkgConfig::WEWEB_X11
            PkgConfig::WEWEB_WLEGL)
    target_compile_features(viewer-web PUBLIC cxx_std_20)
    set_target_properties(viewer-web PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_SCAN_FOR_MODULES ON)
endif()

# wescene scene viewer.
if(BUILD_WESCENE AND BUILD_VIEWER)
    add_executable(SceneViewer SceneViewer.cpp)
    target_link_libraries(SceneViewer
        PRIVATE
            viewer-common
            PkgConfig::GLFW
            owe-renderer
            argparse::argparse)
    set_target_properties(SceneViewer PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

# weweb web viewer.
if(BUILD_WEWEB)
    add_executable(WebViewer WebViewer.cpp)
    target_link_libraries(WebViewer PRIVATE
        viewer-common
        viewer-web
        owe-web-host
        wavsen::audio
        PkgConfig::GLFW
        Vulkan::Vulkan
        argparse::argparse
        ${CEF_STANDARD_LIBS})
    set_target_properties(WebViewer PROPERTIES
        CXX_SCAN_FOR_MODULES ON
        BUILD_RPATH   "\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON)
    weweb_stage_cef_runtime(WebViewer)
endif()
