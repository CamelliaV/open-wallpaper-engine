add_executable(webviewer webviewer.cpp)
target_link_libraries(webviewer PRIVATE
    weweb-host
    PkgConfig::GLFW
    argparse::argparse)

target_link_libraries(webviewer PRIVATE ${CEF_STANDARD_LIBS})
weweb_stage_cef_runtime(webviewer)
