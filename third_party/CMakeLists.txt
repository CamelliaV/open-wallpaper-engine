include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# ------------------------------------------------------------------
# Locally vendored deps still living under third_party/
# (glad, effolkronium, vog, stb_image.h, vk_mem_alloc.h)
# argparse moved to FetchContent via deps.json.
# ------------------------------------------------------------------
add_subdirectory(glad)

# ------------------------------------------------------------------
# JSON-driven FetchContent for eigen, nlohmann_json, spirv_reflect,
# glslang, cubeb. To enable the flatpak-aware dependency provider,
# pass:
#   -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/FetchDepsProvider.cmake
# ------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/FetchDeps.cmake)

# Cache vars consumed by nested add_subdirectory calls inside fetchdeps().
set(JSON_BuildTests         OFF CACHE INTERNAL "")
set(JSON_Install            OFF CACHE INTERNAL "")
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

# DirectXShaderCompiler — fork of LLVM, brings hundreds of CMake options
# along. The set below approximates DXC's own cmake/caches/PredefinedParams
# cache file: trim everything except the HLSL→SPIR-V frontend.
set(LLVM_TARGETS_TO_BUILD       "None" CACHE STRING   "")
set(LLVM_INCLUDE_TESTS          OFF    CACHE INTERNAL "")
set(LLVM_INCLUDE_DOCS           OFF    CACHE INTERNAL "")
set(LLVM_INCLUDE_EXAMPLES       OFF    CACHE INTERNAL "")
set(LLVM_INCLUDE_GO_TESTS       OFF    CACHE INTERNAL "")
set(LLVM_BUILD_TESTS            OFF    CACHE INTERNAL "")
set(LLVM_BUILD_TOOLS            OFF    CACHE INTERNAL "")
set(LLVM_BUILD_EXAMPLES         OFF    CACHE INTERNAL "")
set(LLVM_BUILD_DOCS             OFF    CACHE INTERNAL "")
set(LLVM_BUILD_RUNTIME          OFF    CACHE INTERNAL "")
# LLVM_OPTIMIZED_TABLEGEN triggers a NATIVE sub-build that re-invokes cmake
# on CMAKE_SOURCE_DIR — under FetchContent that's our project root, not
# DXC's. Forcing OFF avoids the cross-build bootstrap entirely.
set(LLVM_OPTIMIZED_TABLEGEN     OFF    CACHE INTERNAL "")
set(LLVM_USE_HOST_TOOLS         OFF    CACHE INTERNAL "")
set(LLVM_REQUIRES_RTTI          ON     CACHE INTERNAL "")
set(LLVM_REQUIRES_EH            ON     CACHE INTERNAL "")
set(LLVM_ENABLE_RTTI            ON     CACHE INTERNAL "")
set(LLVM_ENABLE_EH              ON     CACHE INTERNAL "")
set(LLVM_APPEND_VC_REV          OFF    CACHE INTERNAL "")
set(LLVM_TOOL_OPENMP_BUILD      OFF    CACHE INTERNAL "")
set(LLVM_TOOL_LIBCXX_BUILD      OFF    CACHE INTERNAL "")
set(LLVM_TOOL_LIBCXXABI_BUILD   OFF    CACHE INTERNAL "")
set(LLVM_TOOL_COMPILER_RT_BUILD OFF    CACHE INTERNAL "")
set(CLANG_BUILD_EXAMPLES        OFF    CACHE INTERNAL "")
set(CLANG_INCLUDE_TESTS         OFF    CACHE INTERNAL "")
set(CLANG_ENABLE_ARCMT          OFF    CACHE INTERNAL "")
set(CLANG_ENABLE_STATIC_ANALYZER OFF   CACHE INTERNAL "")
set(HLSL_INCLUDE_TESTS          OFF    CACHE INTERNAL "")
set(HLSL_BUILD_DXILCONV         OFF    CACHE INTERNAL "")
set(HLSL_OFFICIAL_BUILD         OFF    CACHE INTERNAL "")
set(SPIRV_BUILD_TESTS           OFF    CACHE INTERNAL "")
set(SPIRV_HEADERS_SKIP_EXAMPLES ON     CACHE INTERNAL "")
set(SPIRV_HEADERS_SKIP_INSTALL  ON     CACHE INTERNAL "")
set(ENABLE_SPIRV_CODEGEN        ON     CACHE INTERNAL "")
# DXC's HCT step regenerates HlslIntrinsicOp.h etc. and verifies clang-format
# output matches the in-tree copy. We don't need the regeneration at all —
# the in-tree headers ship with DXC and are correct as-is. Disabling the
# whole HCT pipeline avoids stamp files that ninja treats as always-dirty
# and cascade-rebuilds the entire LLVM tree on every incremental build.
set(HLSL_DISABLE_SOURCE_GENERATION ON  CACHE INTERNAL "")

fetchdeps(${CMAKE_SOURCE_DIR}/deps.json)

# Post-fetch setup for eigen (header-only, source_subdir=cmake-noop).
add_library(eigen INTERFACE)
add_library(Eigen3::Eigen ALIAS eigen)
target_include_directories(eigen SYSTEM INTERFACE ${eigen_SOURCE_DIR})

# Audio output is now handled by wavsen::audio (cubeb-backed). cubeb
# itself is fetched via deps.json above and consumed transitively through
# wavsen — no post-fetch shim needed here.

# Promote CEF source dir to global scope so weweb/cmake/cef.cmake can
# reach it via add_subdirectory from the project root. fetchdeps()
# populates `cef_SOURCE_DIR` here in third_party/, but only this scope
# sees it without an explicit cache.
if(DEFINED cef_SOURCE_DIR)
    set(cef_SOURCE_DIR "${cef_SOURCE_DIR}" CACHE INTERNAL "" FORCE)
endif()
