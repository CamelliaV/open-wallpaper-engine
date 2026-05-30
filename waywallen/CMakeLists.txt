include(FetchContent)

find_package(waywallen-bridge REQUIRED)

add_library(waywallen-bridge-producer STATIC)
target_sources(waywallen-bridge-producer
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
        BridgeProducerCore.cppm
)
target_sources(waywallen-bridge-producer PRIVATE BridgeProducerCore.cpp)
target_include_directories(waywallen-bridge-producer PUBLIC .)
target_link_libraries(waywallen-bridge-producer
    PUBLIC
        waywallen::bridge
        wavsen::ffi::vulkan
        rstd.cppstd
)
target_compile_features(waywallen-bridge-producer PUBLIC cxx_std_20)
set_target_properties(waywallen-bridge-producer PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_SCAN_FOR_MODULES ON)

add_executable(waywallen-wescene-renderer
    scene_main.cpp
)
target_sources(waywallen-wescene-renderer
    PRIVATE FILE_SET cxx_modules TYPE CXX_MODULES FILES
        BridgeExSwapchain.cppm
)
target_sources(waywallen-wescene-renderer PRIVATE BridgeExSwapchain.cpp)

target_compile_features(waywallen-wescene-renderer PRIVATE cxx_std_20)
# Bin installs under share/waywallen/plugins/<id>/bin (5 levels below
# the prefix); reach the shared libs in <prefix>/lib.
set_target_properties(waywallen-wescene-renderer PROPERTIES
    CXX_SCAN_FOR_MODULES ON
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../../../../../lib"
)

target_include_directories(waywallen-wescene-renderer
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/Vulkan/include
        ${CMAKE_SOURCE_DIR}/src/VulkanRender/include
        ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(waywallen-wescene-renderer
    PRIVATE
        owe-renderer
        waywallen::bridge
        waywallen-bridge-producer
        argparse::argparse
        rstd.cppstd
)

target_compile_options(waywallen-wescene-renderer PRIVATE
    -Wall -Wextra -Wpedantic
)

# Single installable plugin providing the scene renderer + WE source,
# plus the web renderer when BUILD_WEWEB. Folder name == plugin id.
set(_OWE_PLUGIN_DIR share/waywallen/plugins/org.waywallen.open-wallpaper-engine)
install(TARGETS waywallen-wescene-renderer
    RUNTIME DESTINATION ${_OWE_PLUGIN_DIR}/bin
)

# weweb-renderer host bin — gated on BUILD_WEWEB (which also gates the
# weweb-host static lib in src/). Lives next to the wescene host so
# both share the workspace `install/` prefix and the same Lua source
# plugin (wallpaper_engine.lua) advertises both wp_types.
if(BUILD_WEWEB)
    # Headless Vulkan producer device (DMA-BUF import + bridge-slot blit).
    add_library(waywallen-web-producer-device STATIC)
    target_sources(waywallen-web-producer-device
        PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
            WebProducerDevice.cppm
    )
    target_sources(waywallen-web-producer-device PRIVATE WebProducerDevice.cpp)
    target_include_directories(waywallen-web-producer-device
        PUBLIC
            .
            ${CMAKE_SOURCE_DIR}/src/Web
    )
    target_link_libraries(waywallen-web-producer-device
        PUBLIC
            wavsen::ffi::vulkan
            rstd.cppstd
    )
    target_compile_features(waywallen-web-producer-device PUBLIC cxx_std_20)
    set_target_properties(waywallen-web-producer-device PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_SCAN_FOR_MODULES ON)

    add_executable(waywallen-weweb-renderer
        web_main.cpp
    )
    target_compile_features(waywallen-weweb-renderer PRIVATE cxx_std_20)
    # Install layout: <prefix>/share/waywallen/weweb/{bin, libcef.so, ...}.
    set_target_properties(waywallen-weweb-renderer PROPERTIES
        CXX_SCAN_FOR_MODULES ON
        BUILD_RPATH   "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
    target_include_directories(waywallen-weweb-renderer
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src/Web
    )
    # web_main.cpp does NOT include `include/cef_*.h` directly — only
    # weweb's facade headers (BrowserHost.hpp, DmaBufFrame.hpp,
    # Manifest.hpp) — so we deliberately skip
    # `weweb_apply_cef_target_settings`, which would force
    # -fno-exceptions / -fno-rtti onto this TU and break argparse +
    # rstd module imports. weweb-host is built with the CEF flags as
    # required.
    target_link_libraries(waywallen-weweb-renderer
        PRIVATE
            owe-web-host
            waywallen::bridge
            waywallen-bridge-producer
            waywallen-web-producer-device
            argparse::argparse
            wavsen::ffi::vulkan
            rstd::rstd
            rstd.cppstd
    )
    target_compile_options(waywallen-weweb-renderer PRIVATE
        -Wall -Wextra -Wpedantic
    )
    weweb_stage_cef_runtime(waywallen-weweb-renderer)

    # weweb bin + CEF runtime sit flat in the plugin dir (beside the
    # wescene `bin/` subdir) so the weweb bin's `$ORIGIN` rpath resolves
    # libcef.so etc.
    set(_WEWEB_INSTALL_DIR ${_OWE_PLUGIN_DIR})
    install(TARGETS waywallen-weweb-renderer
        RUNTIME DESTINATION ${_WEWEB_INSTALL_DIR}
    )
    # CEF_BINARY_FILES / CEF_RESOURCE_FILES are basenames; prepend
    # CEF_BINARY_DIR / CEF_RESOURCE_DIR for absolute paths.
    #
    # TODO: make this work with our vulkan stack
    # Skipped 
    #   libvulkan.so.1         — Khronos loader CEF ships for its
    #                            SwiftShader fallback.
    #   libvk_swiftshader.so   — SwiftShader vulkan ICD; useless when
    #                            we drop the loader above.
    #   vk_swiftshader_icd.json — JSON registering the ICD;
    set(_CEF_SKIP) #libvulkan.so.1 libvk_swiftshader.so vk_swiftshader_icd.json)
    foreach(_f IN LISTS CEF_BINARY_FILES)
        if(NOT _f IN_LIST _CEF_SKIP)
            install(FILES ${CEF_BINARY_DIR}/${_f}
                DESTINATION ${_WEWEB_INSTALL_DIR})
        endif()
    endforeach()
    # CEF_RESOURCE_FILES contains a `locales` directory entry mixed in
    # with .pak / .dat files — split per type.
    foreach(_f IN LISTS CEF_RESOURCE_FILES)
        if(_f STREQUAL "locales")
            install(DIRECTORY ${CEF_RESOURCE_DIR}/locales/
                DESTINATION ${_WEWEB_INSTALL_DIR}/locales
                FILES_MATCHING PATTERN "*.pak")
        else()
            install(FILES ${CEF_RESOURCE_DIR}/${_f}
                DESTINATION ${_WEWEB_INSTALL_DIR})
        endif()
    endforeach()
endif()

# --- Generate the plugin manifest -----------------------------------------
# The `[renderers.weweb-renderer]` block is spliced in only when BUILD_WEWEB;
# `files` is externalised to files.txt (assembled at configure time below).
set(OWE_PLUGIN_VERSION 0.1.2)
set(_OWE_PLUGIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/plugins/org.waywallen.open-wallpaper-engine)
if(BUILD_WEWEB)
    file(READ ${_OWE_PLUGIN_SRC}/weweb-renderer.toml.in OWE_WEWEB_BLOCK)
else()
    set(OWE_WEWEB_BLOCK "")
endif()
configure_file(
    ${_OWE_PLUGIN_SRC}/plugin.toml.in
    ${CMAKE_CURRENT_BINARY_DIR}/owe-plugin.toml
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/owe-plugin.toml
    DESTINATION ${_OWE_PLUGIN_DIR}
    RENAME plugin.toml
)
install(FILES ${_OWE_PLUGIN_SRC}/wallpaper_engine.lua
    DESTINATION ${_OWE_PLUGIN_DIR}
)

# files.txt — the required per-plugin manifest, assembled at configure
# time from the known install set (no install-time directory scan). The
# CEF locale paks are the one set we don't list by hand; glob the CEF
# source dir (a fixed vendored tree) to enumerate them.
set(_OWE_FILES
    plugin.toml
    wallpaper_engine.lua
    bin/waywallen-wescene-renderer
)
if(BUILD_WEWEB)
    list(APPEND _OWE_FILES waywallen-weweb-renderer)
    foreach(_f IN LISTS CEF_BINARY_FILES)
        if(NOT _f IN_LIST _CEF_SKIP)
            list(APPEND _OWE_FILES ${_f})
        endif()
    endforeach()
    foreach(_f IN LISTS CEF_RESOURCE_FILES)
        if(_f STREQUAL "locales")
            file(GLOB _owe_paks RELATIVE ${CEF_RESOURCE_DIR}/locales
                ${CEF_RESOURCE_DIR}/locales/*.pak)
            list(TRANSFORM _owe_paks PREPEND "locales/")
            list(APPEND _OWE_FILES ${_owe_paks})
        else()
            list(APPEND _OWE_FILES ${_f})
        endif()
    endforeach()
endif()
list(SORT _OWE_FILES)
string(JOIN "\n" _owe_files_txt ${_OWE_FILES})
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/owe-files.txt "${_owe_files_txt}\n")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/owe-files.txt
    DESTINATION ${_OWE_PLUGIN_DIR}
    RENAME files.txt
)

