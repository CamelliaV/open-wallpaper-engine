find_package(Vulkan REQUIRED)

add_executable(webviewer
    webviewer.cpp
    VulkanBlitter.cpp)
target_link_libraries(webviewer PRIVATE
    weweb-host
    PkgConfig::GLFW
    Vulkan::Vulkan
    argparse::argparse)

target_link_libraries(webviewer PRIVATE ${CEF_STANDARD_LIBS})

set_target_properties(webviewer PROPERTIES
    BUILD_RPATH   "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
    BUILD_WITH_INSTALL_RPATH ON)

weweb_stage_cef_runtime(webviewer)
