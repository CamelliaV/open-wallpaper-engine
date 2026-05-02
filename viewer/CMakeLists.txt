add_executable(viewer glfwviewer.cpp)

target_link_libraries(viewer
    PRIVATE
        PkgConfig::GLFW
        wescene-renderer
        argparse::argparse
)

set_target_properties(viewer PROPERTIES CXX_SCAN_FOR_MODULES ON)
