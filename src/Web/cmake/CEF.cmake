# CEF target and runtime integration for its binary distribution.

if(NOT DEFINED cef_SOURCE_DIR)
    message(FATAL_ERROR
        "weweb: cef_SOURCE_DIR is unset. fetchdeps() must run before CEF setup.")
endif()

set(CEF_ROOT "${cef_SOURCE_DIR}" CACHE PATH "CEF binary distribution root" FORCE)
list(APPEND CMAKE_MODULE_PATH "${cef_SOURCE_DIR}/cmake")

if(NOT DEFINED PROJECT_ARCH)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _weweb_system_processor)
    if(_weweb_system_processor MATCHES "^(aarch64|arm64)$")
        set(PROJECT_ARCH arm64)
    elseif(_weweb_system_processor MATCHES "^(x86_64|amd64)$")
        set(PROJECT_ARCH x86_64)
    endif()
endif()

find_package(CEF REQUIRED)

# Minimal distributions only contain the Release binary directory.
set(CEF_BINARY_DIR "${CEF_BINARY_DIR_RELEASE}")
set(CEF_LIB_DEBUG  "${CEF_LIB_RELEASE}")

set_property(GLOBAL PROPERTY WEWEB_CEF_BINARY_DIR "${CEF_BINARY_DIR}")
set_property(GLOBAL PROPERTY WEWEB_CEF_BINARY_FILES "${CEF_BINARY_FILES}")
set_property(GLOBAL PROPERTY WEWEB_CEF_RESOURCE_DIR "${CEF_RESOURCE_DIR}")
set_property(GLOBAL PROPERTY WEWEB_CEF_RESOURCE_FILES "${CEF_RESOURCE_FILES}")

if(NOT TARGET libcef_dll_wrapper)
    add_subdirectory(
        "${CEF_LIBCEF_DLL_WRAPPER_PATH}"
        "${CMAKE_CURRENT_BINARY_DIR}/libcef_dll_wrapper"
        EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET libcef_lib)
    ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

target_compile_definitions(libcef_lib INTERFACE
    ${CEF_COMPILER_DEFINES}
    $<$<CONFIG:Debug>:${CEF_COMPILER_DEFINES_DEBUG}>
    $<$<CONFIG:Release>:${CEF_COMPILER_DEFINES_RELEASE}>)
target_include_directories(libcef_lib SYSTEM INTERFACE ${CEF_INCLUDE_PATH})
target_link_libraries(libcef_lib INTERFACE ${CEF_STANDARD_LIBS})

function(weweb_get_cef_runtime out_binary_dir out_binary_files out_resource_dir out_resource_files)
    get_property(_binary_dir GLOBAL PROPERTY WEWEB_CEF_BINARY_DIR)
    get_property(_binary_files GLOBAL PROPERTY WEWEB_CEF_BINARY_FILES)
    get_property(_resource_dir GLOBAL PROPERTY WEWEB_CEF_RESOURCE_DIR)
    get_property(_resource_files GLOBAL PROPERTY WEWEB_CEF_RESOURCE_FILES)
    set(${out_binary_dir} "${_binary_dir}" PARENT_SCOPE)
    set(${out_binary_files} "${_binary_files}" PARENT_SCOPE)
    set(${out_resource_dir} "${_resource_dir}" PARENT_SCOPE)
    set(${out_resource_files} "${_resource_files}" PARENT_SCOPE)
endfunction()

function(weweb_stage_cef_runtime target)
    weweb_get_cef_runtime(_binary_dir _binary_files _resource_dir _resource_files)
    COPY_FILES(${target} "${_binary_files}"   "${_binary_dir}"   "$<TARGET_FILE_DIR:${target}>")
    COPY_FILES(${target} "${_resource_files}" "${_resource_dir}" "$<TARGET_FILE_DIR:${target}>")
endfunction()
