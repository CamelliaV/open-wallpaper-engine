include(FetchContent)

find_package(waywallen-bridge REQUIRED)

# Single installable plugin providing the scene renderer + WE source,
# plus the web renderer when BUILD_WEWEB.
if(OWE_WAYWALLEN_PLUGIN_BUNDLE_LAYOUT)
    set(_OWE_PLUGIN_DIR .)
    set(OWE_WESCENE_BIN bin/waywallen-wescene-renderer)
    set(OWE_WEWEB_BIN bin/weweb/waywallen-weweb-renderer)
    set(_OWE_WEWEB_FILES_PREFIX bin/weweb)
else()
    set(_OWE_PLUGIN_DIR share/waywallen/plugins/${OWE_WAYWALLEN_PLUGIN_ID})
    set(OWE_WESCENE_BIN ../../../../bin/waywallen-wescene-renderer)
    set(OWE_WEWEB_BIN ../../../../bin/weweb/waywallen-weweb-renderer)
    set(_OWE_WEWEB_FILES_PREFIX ../../../../bin/weweb)
endif()
set(_OWE_WESCENE_INSTALL_DIR bin)
set(_OWE_WEWEB_INSTALL_DIR bin/weweb)
set(_OWE_STRIP_FILES ${OWE_WESCENE_BIN})

add_library(waywallen-bridge-producer STATIC)
target_sources(waywallen-bridge-producer
    PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
        Bridge.cppm
        BridgeSession.cppm
        BridgeProducerCore.cppm
)
target_sources(waywallen-bridge-producer PRIVATE
    BridgeSession.cpp
    BridgeProducerCore.cpp
)
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
    scene_entry.cpp
)
target_sources(waywallen-wescene-renderer
    PRIVATE FILE_SET cxx_modules TYPE CXX_MODULES FILES
        scene_entry.cppm
        BridgeExSwapchain.cppm
)
target_sources(waywallen-wescene-renderer PRIVATE BridgeExSwapchain.cpp)

target_compile_features(waywallen-wescene-renderer PRIVATE cxx_std_20)
set_target_properties(waywallen-wescene-renderer PROPERTIES
    CXX_SCAN_FOR_MODULES ON
)

target_include_directories(waywallen-wescene-renderer
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/VulkanRender/include
)

target_link_libraries(waywallen-wescene-renderer
    PRIVATE
        owe-renderer
        waywallen::bridge
        waywallen-bridge-producer
        argparse::argparse
        rstd.cppstd
)

install(TARGETS waywallen-wescene-renderer
    RUNTIME DESTINATION ${_OWE_WESCENE_INSTALL_DIR}
)

# weweb-renderer host bin — gated on BUILD_WEWEB (which also gates the
# weweb-host static lib in src/). Lives next to the wescene host so
# both share the workspace `install/` prefix and the same Lua source
# component advertises both wp_types.
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
    )
    target_link_libraries(waywallen-web-producer-device
        PUBLIC
            owe-web-api
            wavsen::ffi::vulkan
            rstd.cppstd
    )
    target_compile_features(waywallen-web-producer-device PUBLIC cxx_std_20)
    set_target_properties(waywallen-web-producer-device PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_SCAN_FOR_MODULES ON)

    add_executable(waywallen-weweb-renderer
        web_main.cpp
        web_entry.cpp
    )
    target_sources(waywallen-weweb-renderer
        PRIVATE FILE_SET cxx_modules TYPE CXX_MODULES FILES
            web_entry.cppm
    )
    target_compile_features(waywallen-weweb-renderer PRIVATE cxx_std_20)
    set_target_properties(waywallen-weweb-renderer PROPERTIES
        CXX_SCAN_FOR_MODULES ON
    )
    target_include_directories(waywallen-weweb-renderer
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src/Web
    )
    # web_main.cpp does NOT include `include/cef_*.h` directly; CEF
    # settings stay inside owe-web-host.
    target_link_libraries(waywallen-weweb-renderer
        PRIVATE
            owe-web-host
            waywallen::bridge
            waywallen-bridge-producer
            waywallen-web-producer-device
            argparse::argparse
            wavsen::ffi::vulkan
            wavsen::audio
            rstd::rstd
            rstd.cppstd
    )
    weweb_stage_cef_runtime(waywallen-weweb-renderer)

    install(TARGETS waywallen-weweb-renderer
        RUNTIME DESTINATION ${_OWE_WEWEB_INSTALL_DIR}
    )
    list(APPEND _OWE_STRIP_FILES ${OWE_WEWEB_BIN})
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
                DESTINATION ${_OWE_WEWEB_INSTALL_DIR})
            if(_f STREQUAL "chrome-sandbox" OR _f MATCHES "\\.so(\\.[0-9]+)*$")
                list(APPEND _OWE_STRIP_FILES ${_OWE_WEWEB_FILES_PREFIX}/${_f})
            endif()
        endif()
    endforeach()
    # CEF_RESOURCE_FILES contains a `locales` directory entry mixed in
    # with .pak / .dat files — split per type.
    foreach(_f IN LISTS CEF_RESOURCE_FILES)
        if(_f STREQUAL "locales")
            install(DIRECTORY ${CEF_RESOURCE_DIR}/locales/
                DESTINATION ${_OWE_WEWEB_INSTALL_DIR}/locales
                FILES_MATCHING PATTERN "*.pak")
        else()
            install(FILES ${CEF_RESOURCE_DIR}/${_f}
                DESTINATION ${_OWE_WEWEB_INSTALL_DIR})
        endif()
    endforeach()
endif()

# --- Generate the plugin manifest -----------------------------------------
# The `[renderers.weweb-renderer]` block is spliced in only when BUILD_WEWEB;
# `files` is externalised to files.txt (assembled at configure time below).
set(_OWE_PLUGIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/plugins/org.waywallen.open-wallpaper-engine)
if(BUILD_WEWEB)
    file(READ ${_OWE_PLUGIN_SRC}/weweb-renderer.toml.in OWE_WEWEB_BLOCK)
    string(CONFIGURE "${OWE_WEWEB_BLOCK}" OWE_WEWEB_BLOCK @ONLY)
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
install(FILES ${_OWE_PLUGIN_SRC}/main.lua
    DESTINATION ${_OWE_PLUGIN_DIR}
)
install(FILES
    ${_OWE_PLUGIN_SRC}/wallpaper_engine/project.lua
    ${_OWE_PLUGIN_SRC}/wallpaper_engine/source.lua
    ${_OWE_PLUGIN_SRC}/wallpaper_engine/wallpaper.lua
    DESTINATION ${_OWE_PLUGIN_DIR}/wallpaper_engine
)

# files.txt — the required per-plugin manifest, assembled at configure
# time from the known install set (no install-time directory scan). The
# CEF locale paks are the one set we don't list by hand; glob the CEF
# source dir (a fixed vendored tree) to enumerate them.
set(_OWE_FILES
    main.lua
    plugin.toml
    wallpaper_engine/project.lua
    wallpaper_engine/source.lua
    wallpaper_engine/wallpaper.lua
    ${OWE_WESCENE_BIN}
)
if(BUILD_WEWEB)
    list(APPEND _OWE_FILES ${OWE_WEWEB_BIN})
    foreach(_f IN LISTS CEF_BINARY_FILES)
        if(NOT _f IN_LIST _CEF_SKIP)
            list(APPEND _OWE_FILES ${_OWE_WEWEB_FILES_PREFIX}/${_f})
        endif()
    endforeach()
    foreach(_f IN LISTS CEF_RESOURCE_FILES)
        if(_f STREQUAL "locales")
            file(GLOB _owe_paks RELATIVE ${CEF_RESOURCE_DIR}/locales
                ${CEF_RESOURCE_DIR}/locales/*.pak)
            list(TRANSFORM _owe_paks PREPEND "${_OWE_WEWEB_FILES_PREFIX}/locales/")
            list(APPEND _OWE_FILES ${_owe_paks})
        else()
            list(APPEND _OWE_FILES ${_OWE_WEWEB_FILES_PREFIX}/${_f})
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

if(OWE_WAYWALLEN_PLUGIN_BUNDLE_LAYOUT AND CMAKE_STRIP)
    set(_OWE_INSTALL_STRIP_FILES)
    foreach(_f IN LISTS _OWE_STRIP_FILES)
        list(APPEND _OWE_INSTALL_STRIP_FILES "${_OWE_PLUGIN_DIR}/${_f}")
    endforeach()
    install(CODE "
        set(_owe_strip_tool [==[${CMAKE_STRIP}]==])
        set(_owe_strip_files [==[${_OWE_INSTALL_STRIP_FILES}]==])
        foreach(_rel IN LISTS _owe_strip_files)
            set(_path \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/\${_rel}\")
            if(EXISTS \"\${_path}\")
                execute_process(
                    COMMAND \"\${_owe_strip_tool}\" --strip-unneeded \"\${_path}\"
                    RESULT_VARIABLE _strip_result
                )
                if(NOT _strip_result EQUAL 0)
                    message(FATAL_ERROR \"failed to strip \${_path}\")
                endif()
            endif()
        endforeach()
    ")
endif()
