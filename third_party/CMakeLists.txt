include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# ------------------------------------------------------------------
# Locally vendored deps still living under third_party/
# (glad, effolkronium, vog, stb_image.h, vk_mem_alloc.h)
# argparse moved to FetchContent via deps.json.
# ------------------------------------------------------------------
add_subdirectory(glad)

# ------------------------------------------------------------------
# JSON-driven FetchContent for eigen, spirv_reflect,
# glslang. To enable the flatpak-aware dependency provider, pass:
#   -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/FetchDepsProvider.cmake
# ------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/FetchDeps.cmake)

# Cache vars consumed by nested add_subdirectory calls inside fetchdeps().
set(SPIRV_REFLECT_STATIC_LIB  ON  CACHE INTERNAL "")
set(SPIRV_REFLECT_EXECUTABLE  OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_BUILD_TESTS OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_EXAMPLES    OFF CACHE INTERNAL "")
set(SPIRV_REFLECT_INSTALL    OFF CACHE INTERNAL "")
set(ARGPARSE_INSTALL        OFF CACHE INTERNAL "")
set(ARGPARSE_BUILD_TESTS    OFF CACHE INTERNAL "")
set(ARGPARSE_BUILD_SAMPLES  OFF CACHE INTERNAL "")

# QuickJS-NG: only the static library is needed; suppress the CLI / examples
# / install rules so they don't clutter our tree.
set(QJS_BUILD_EXAMPLES      OFF CACHE INTERNAL "")
set(QJS_BUILD_CLI_STATIC    OFF CACHE INTERNAL "")
set(QJS_BUILD_CLI_WITH_MIMALLOC OFF CACHE INTERNAL "")
set(QJS_BUILD_CLI_WITH_STATIC_MIMALLOC OFF CACHE INTERNAL "")
set(QJS_ENABLE_INSTALL      OFF CACHE INTERNAL "")

# glslang — trim to library targets only; we don't ship its tools or tests.
# ENABLE_OPT=OFF skips the spirv-opt dependency on SPIRV-Tools (glslang's
# External/ submodules aren't fetched in our setup).
set(ENABLE_GLSLANG_BINARIES OFF CACHE INTERNAL "")
set(ENABLE_HLSL             ON  CACHE INTERNAL "")
set(ENABLE_OPT              OFF CACHE INTERNAL "")
set(ENABLE_CTEST            OFF CACHE INTERNAL "")
set(BUILD_EXTERNAL          OFF CACHE INTERNAL "")
set(GLSLANG_TESTS           OFF CACHE INTERNAL "")
set(BUILD_TESTING           OFF CACHE INTERNAL "")

fetchdeps(${CMAKE_SOURCE_DIR}/deps.json)

# Post-fetch setup for eigen (header-only, source_subdir=cmake-noop).
add_library(eigen INTERFACE)
add_library(Eigen3::Eigen ALIAS eigen)
target_include_directories(eigen SYSTEM INTERFACE ${eigen_SOURCE_DIR})

# Audio output is handled by wavsen::audio (pulse/pipewire-backed,
# selectable via WAVSEN_AUDIO_BACKEND in the wavsen subtree).

# Promote CEF source dir to global scope so weweb/cmake/cef.cmake can
# reach it via add_subdirectory from the project root. fetchdeps()
# populates `cef_SOURCE_DIR` here in third_party/, but only this scope
# sees it without an explicit cache.
if(DEFINED cef_SOURCE_DIR)
    set(cef_SOURCE_DIR "${cef_SOURCE_DIR}" CACHE INTERNAL "" FORCE)
endif()
