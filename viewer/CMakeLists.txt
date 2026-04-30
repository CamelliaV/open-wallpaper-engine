add_executable(viewer glfwviewer.cpp)

target_link_libraries(viewer
    PRIVATE
        PkgConfig::GLFW
        wescene-renderer
        argparse::argparse
)
