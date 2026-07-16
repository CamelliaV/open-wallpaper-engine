include_directories(.)

if(BUILD_WESCENE)

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Freetype REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

include(TestBigEndian)
test_big_endian(ENDIAN)
if(ENDIAN)
  add_compile_definitions(WP_BIG_ENDIAN)
endif()

add_subdirectory(IO)
add_subdirectory(Fs)
add_subdirectory(Core)
add_subdirectory(Vulkan)
add_subdirectory(Scene)

# ---------------------------------------------------------------------------
# owe-renderer (INTERFACE umbrella for back-compat).
# ---------------------------------------------------------------------------
add_library(owe-renderer INTERFACE)
target_link_libraries(owe-renderer
  INTERFACE owe-scene-wallpaper owe-render owe-parse owe-script
            owe-resource owe-vulkan owe-shader-compile owe-base)

endif()  # BUILD_WESCENE

# ============================================================================
# owe-web-host (CEF browser embedding for HTML/JS wallpapers)
# ============================================================================
if(BUILD_WEWEB)
  add_subdirectory(Web)
endif()
