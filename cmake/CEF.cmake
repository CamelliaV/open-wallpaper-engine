# CEF integration glue.
#
# Runs after the top-level fetchdeps() has populated cef_SOURCE_DIR with the
# extracted CEF binary distribution. Defines:
#
#   * libcef_lib         — IMPORTED target wrapping the prebuilt libcef.so.
#   * libcef_dll_wrapper — STATIC, built from CEF's libcef_dll/ sources.
#   * weweb_apply_cef_target_settings(target)
#   * weweb_apply_cef_module_target_settings(target)
#   * weweb_stage_cef_runtime(target)
#
# CEF's own cmake helpers (find_package(CEF) + cef_macros + cef_variables)
# do most of the work; we just bridge them into our project.

if(NOT DEFINED cef_SOURCE_DIR)
    message(FATAL_ERROR
        "weweb: cef_SOURCE_DIR is unset. fetchdeps() must run before "
        "add_subdirectory(weweb).")
endif()

# Tell FindCEF.cmake where to look. CACHE FORCE because find_package(CEF) is
# strict about CEF_ROOT being explicitly set.
set(CEF_ROOT "${cef_SOURCE_DIR}" CACHE PATH "CEF binary distribution root" FORCE)
list(APPEND CMAKE_MODULE_PATH "${cef_SOURCE_DIR}/cmake")

find_package(CEF REQUIRED)

# CEF's variables.cmake derives CEF_BINARY_DIR from CMAKE_BUILD_TYPE
# (`${CEF_ROOT}/${CMAKE_BUILD_TYPE}`). The `minimal` linux64 distribution
# ships only Release/, so a Debug configure would point at a non-existent
# Debug/ folder. Pin both to Release — the prebuilt libcef.so is the same
# regardless of our build type, only the wrapper compile around it differs.
set(CEF_BINARY_DIR "${CEF_BINARY_DIR_RELEASE}")
set(CEF_LIB_DEBUG  "${CEF_LIB_RELEASE}")

# Build the C++ wrapper (libcef_dll/CMakeLists.txt defines target
# libcef_dll_wrapper). Stash it under our build tree so paths stay tidy.
add_subdirectory(
    "${CEF_LIBCEF_DLL_WRAPPER_PATH}"
    "${CMAKE_CURRENT_BINARY_DIR}/libcef_dll_wrapper"
    EXCLUDE_FROM_ALL)

# IMPORTED target wrapping the prebuilt libcef.so. cef-project does this in
# its top-level CMakeLists.txt; we centralise it here.
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

# Apply CEF's required compile flags / definitions / include paths so a
# target that includes CEF headers compiles cleanly.
function(weweb_apply_cef_target_settings target)
    SET_COMMON_TARGET_PROPERTIES(${target})
endfunction()

# Apply CEF settings to a target that imports project C++20 modules. Clang's
# BMI compatibility check requires exception and RTTI flags to match between
# the module producer and importer, so keep those at the project's defaults.
function(weweb_apply_cef_module_target_settings target)
    SET_COMMON_TARGET_PROPERTIES(${target})
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fexceptions -frtti>)
endfunction()

# Stage CEF runtime files (libcef.so, libEGL.so, libGLESv2.so, libv8…,
# *.pak, icudtl.dat, locales/) next to a compiled binary so it can run
# straight out of the build tree. CefInitialize will SIGABRT if these
# aren't found in CEF_RESOURCES_DIR / CEF_BINARY_DIR.
function(weweb_stage_cef_runtime target)
    COPY_FILES(${target} "${CEF_BINARY_FILES}"   "${CEF_BINARY_DIR}"   "$<TARGET_FILE_DIR:${target}>")
    COPY_FILES(${target} "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "$<TARGET_FILE_DIR:${target}>")
endfunction()
