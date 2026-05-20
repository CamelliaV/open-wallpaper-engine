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

set(warn_opts
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wno-unused-variable
    CACHE INTERNAL "")

# DXC's dxcapi.h sits under <dxc_SOURCE_DIR>/include. The dxcompiler target
# does not always export this include path correctly when consumed via
# add_subdirectory; force-republish via FetchContent_GetProperties.
include(FetchContent)
FetchContent_GetProperties(dxc SOURCE_DIR _dxc_source_dir)

add_subdirectory(Core)
add_subdirectory(Vulkan)
add_subdirectory(Scene)

# ---------------------------------------------------------------------------
# owe-renderer (INTERFACE umbrella for back-compat).
# ---------------------------------------------------------------------------
add_library(owe-renderer INTERFACE)
target_link_libraries(owe-renderer
  INTERFACE owe-render owe-parse owe-script
            owe-vulkan owe-shader-compile owe-base)

endif()  # BUILD_WESCENE

# ============================================================================
# owe-web-host (CEF browser embedding for HTML/JS wallpapers)
# ============================================================================
if(BUILD_WEWEB)
  add_subdirectory(Web)
endif()
