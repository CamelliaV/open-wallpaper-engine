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
            wescene-renderer
            argparse::argparse)
    set_target_properties(SceneViewer PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

# weweb web viewer. Vulkan presenter (DMA-BUF blit) lives next to WebViewer.cpp.
if(BUILD_WEWEB)
    find_package(Vulkan REQUIRED)
    add_executable(WebViewer
        WebViewer.cpp
        VulkanBlitter.cpp)
    target_link_libraries(WebViewer PRIVATE
        viewer-common
        weweb-host
        PkgConfig::GLFW
        Vulkan::Vulkan
        argparse::argparse
        ${CEF_STANDARD_LIBS})
    set_target_properties(WebViewer PROPERTIES
        BUILD_RPATH   "\$ORIGIN"
        INSTALL_RPATH "\$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON)
    weweb_stage_cef_runtime(WebViewer)
endif()
