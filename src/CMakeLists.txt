include_directories(.)

if(BUILD_WESCENE)

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Freetype REQUIRED)
pkg_check_modules(LZ4 REQUIRED liblz4)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
# A .pc file may carry cflags that are not include paths, and pkg_check_modules
# puts those on INTERFACE_COMPILE_OPTIONS. They then reach the module consumers
# that link this target but not the module providers, which splits the C++20
# module usage profile and makes CMake 4.4 synthesize a BMI for every provider
# in the closure. Compile options come from the top-level profile; imported
# targets contribute include directories and libraries.
# (Seen with distro zlib-ng, whose .pc adds -DWITH_GZFILEOP via freetype2.)
set_property(TARGET PkgConfig::FONTCONFIG PROPERTY INTERFACE_COMPILE_OPTIONS "")

include(TestBigEndian)
test_big_endian(ENDIAN)
if(ENDIAN)
  add_compile_definitions(WP_BIG_ENDIAN)
endif()

add_subdirectory(Core)
add_subdirectory(Vulkan)
add_subdirectory(Scene)

# ---------------------------------------------------------------------------
# owe-renderer (INTERFACE umbrella for back-compat).
# ---------------------------------------------------------------------------
add_library(owe-renderer INTERFACE)
target_link_libraries(owe-renderer
  INTERFACE owe-scene-wallpaper owe-render owe-parse owe-script
            owe-resource owe-resource-registry owe-vulkan owe-shader-compile owe-base)

endif()  # BUILD_WESCENE

# ============================================================================
# owe-web-host (CEF browser embedding for HTML/JS wallpapers)
# ============================================================================
if(BUILD_WEWEB)
  add_subdirectory(Web)
endif()
