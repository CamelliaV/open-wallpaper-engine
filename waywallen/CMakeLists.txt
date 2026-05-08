include(FetchContent)

find_package(waywallen-bridge REQUIRED)
find_package(Vulkan REQUIRED)

add_library(waywallen-bridge-producer OBJECT
    BridgeProducerCore.cpp
)
target_include_directories(waywallen-bridge-producer PUBLIC .)
target_link_libraries(waywallen-bridge-producer
    PUBLIC
        waywallen::bridge
        Vulkan::Vulkan
)
target_compile_features(waywallen-bridge-producer PUBLIC cxx_std_20)

add_executable(waywallen-wescene-renderer
    scene_main.cpp
    BridgeExSwapchain.cpp
)

target_compile_features(waywallen-wescene-renderer PRIVATE cxx_std_20)
set_target_properties(waywallen-wescene-renderer PROPERTIES
    CXX_SCAN_FOR_MODULES ON
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
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
)

target_compile_options(waywallen-wescene-renderer PRIVATE
    -Wall -Wextra -Wpedantic
)

install(TARGETS waywallen-wescene-renderer
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# weweb-renderer host bin — gated on BUILD_WEWEB (which also gates the
# weweb-host static lib in src/). Lives next to the wescene host so
# both share the workspace `install/` prefix and the same Lua source
# plugin (wallpaper_engine.lua) advertises both wp_types.
if(BUILD_WEWEB)
    # Headless Vulkan producer device (DMA-BUF import + bridge-slot blit).
    add_library(waywallen-web-producer-device STATIC
        WebProducerDevice.cpp
    )
    target_include_directories(waywallen-web-producer-device
        PUBLIC
            .
            ${CMAKE_SOURCE_DIR}/src/Web
    )
    target_link_libraries(waywallen-web-producer-device
        PUBLIC
            Vulkan::Vulkan
    )
    target_compile_features(waywallen-web-producer-device PUBLIC cxx_std_20)

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
            Vulkan::Vulkan
            rstd::rstd
            rstd.cppstd
    )
    target_compile_options(waywallen-weweb-renderer PRIVATE
        -Wall -Wextra -Wpedantic
    )
    weweb_stage_cef_runtime(waywallen-weweb-renderer)

    set(_WEWEB_INSTALL_DIR share/waywallen/weweb)
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
    set(_CEF_SKIP libvulkan.so.1 libvk_swiftshader.so vk_swiftshader_icd.json)
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
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/renderers/weweb.toml
        DESTINATION share/waywallen/renderers
    )
endif()

install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/
    DESTINATION share/waywallen/sources
    FILES_MATCHING PATTERN "*.lua"
)

# Skip weweb.toml unless BUILD_WEWEB — daemon would fail to spawn the
# bin if the manifest were installed without its target.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/renderers/wescene.toml
    DESTINATION share/waywallen/renderers
)

install(FILES $<TARGET_FILE:dxcompiler>
        DESTINATION ${CMAKE_INSTALL_LIBDIR})
