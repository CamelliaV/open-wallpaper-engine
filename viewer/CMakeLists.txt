# Top-level viewer dir hosts both standalone viewers:
#   - SceneViewer : wescene  scene.pkg viewer (BUILD_WESCENE + BUILD_VIEWER)
#   - WebViewer   : weweb    HTML/JS  viewer (BUILD_WEWEB    + glfw3 found)
# Sharing is intentionally minimal — only Common.{hpp,cpp} below.

add_library(viewer-common STATIC Common.cpp)
target_include_directories(viewer-common PUBLIC .)
target_link_libraries(viewer-common PUBLIC PkgConfig::GLFW)
target_compile_features(viewer-common PUBLIC cxx_std_20)

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

# weweb web viewer. Vulkan / EGL presenters (DMA-BUF blit) live next to WebViewer.cpp.
if(BUILD_WEWEB)
    find_package(Vulkan REQUIRED)
    pkg_check_modules(WEWEB_EGL    REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(WEWEB_GLES3  REQUIRED IMPORTED_TARGET glesv2)
    pkg_check_modules(WEWEB_X11    REQUIRED IMPORTED_TARGET x11)
    pkg_check_modules(WEWEB_WLEGL  REQUIRED IMPORTED_TARGET wayland-egl)
    add_executable(WebViewer
        WebViewer.cpp
        VulkanBlitter.cpp
        EglPresenter.cpp)
    target_link_libraries(WebViewer PRIVATE
        viewer-common
        owe-web-host
        PkgConfig::GLFW
        Vulkan::Vulkan
        PkgConfig::WEWEB_EGL
        PkgConfig::WEWEB_GLES3
        PkgConfig::WEWEB_X11
        PkgConfig::WEWEB_WLEGL
        argparse::argparse
        ${CEF_STANDARD_LIBS})
    set_target_properties(WebViewer PROPERTIES
        BUILD_RPATH   "\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON)
    weweb_stage_cef_runtime(WebViewer)
endif()
