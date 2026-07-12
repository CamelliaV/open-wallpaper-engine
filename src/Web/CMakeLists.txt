# ---------------------------------------------------------------------------
# owe-web-api (public C++20 modules for HTML/JS wallpapers)
# ---------------------------------------------------------------------------
add_library(owe-web-api STATIC)
target_sources(owe-web-api
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
        Web.cppm
        DmaBufFrame.cppm
        Manifest.cppm
        BrowserHost.cppm)
target_include_directories(owe-web-api PUBLIC .)
target_link_libraries(owe-web-api PUBLIC owe-core rstd.cppstd)
target_compile_features(owe-web-api PUBLIC cxx_std_20)
set_target_properties(owe-web-api PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_SCAN_FOR_MODULES ON)

# ---------------------------------------------------------------------------
# owe-web-host (CEF browser embedding for HTML/JS wallpapers)
# ---------------------------------------------------------------------------
add_library(owe-web-host STATIC
    Manifest.cpp
    UserProperties.cpp
    AppHandler.cpp
    ClientHandler.cpp
    OsrRenderHandler.cpp
    BrowserHost.cpp)
target_sources(owe-web-host
    PRIVATE FILE_SET cxx_modules TYPE CXX_MODULES FILES
        Cef.cppm
        CefInternal.cppm)
target_include_directories(owe-web-host PUBLIC .)
target_link_libraries(owe-web-host
    PUBLIC
        owe-web-api
        libcef_lib
        libcef_dll_wrapper)
target_compile_features(owe-web-host PUBLIC cxx_std_20)
weweb_apply_cef_module_target_settings(owe-web-host)
set_target_properties(owe-web-host PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_SCAN_FOR_MODULES ON)
