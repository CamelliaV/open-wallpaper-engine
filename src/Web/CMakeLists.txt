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
target_include_directories(owe-web-host PUBLIC .)
target_link_libraries(owe-web-host
    PUBLIC libcef_lib libcef_dll_wrapper nlohmann_json)
target_compile_features(owe-web-host PUBLIC cxx_std_20)
weweb_apply_cef_target_settings(owe-web-host)
set_property(TARGET owe-web-host PROPERTY POSITION_INDEPENDENT_CODE ON)
