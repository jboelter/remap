cmake_minimum_required(VERSION 3.24)

project(remap LANGUAGES CXX RC)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(REMAP_VERSION_TAG "dev" CACHE STRING "Version tag embedded in binaries")
if(REMAP_VERSION_TAG STREQUAL "")
    set(REMAP_VERSION_TAG "dev")
endif()

set(REMAP_GIT_HASH "" CACHE STRING "Git hash embedded in binary metadata")
if(REMAP_GIT_HASH STREQUAL "")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE REMAP_GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
endif()
if(REMAP_GIT_HASH STREQUAL "")
    set(REMAP_GIT_HASH "unknown")
endif()

set(REMAP_VERSION_MAJOR 0)
set(REMAP_VERSION_MINOR 0)
set(REMAP_VERSION_PATCH 0)
set(REMAP_VERSION_TWEAK 0)

if(REMAP_VERSION_TAG MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)([^0-9].*)?$")
    set(REMAP_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(REMAP_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(REMAP_VERSION_PATCH "${CMAKE_MATCH_3}")

    set(_REMAP_version_suffix "${CMAKE_MATCH_4}")
    if(_REMAP_version_suffix MATCHES "([0-9]+)")
        set(REMAP_VERSION_TWEAK "${CMAKE_MATCH_1}")
    endif()
endif()

set(REMAP_VERSION_FILEVERSION
    "${REMAP_VERSION_MAJOR}.${REMAP_VERSION_MINOR}.${REMAP_VERSION_PATCH}.${REMAP_VERSION_TWEAK}")
string(REPLACE "." "," REMAP_VERSION_FILEVERSION_COMMAS "${REMAP_VERSION_FILEVERSION}")

set(REMAP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${REMAP_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_info.h.in"
    "${REMAP_GENERATED_DIR}/version_info.h"
    @ONLY)

if(CMAKE_CONFIGURATION_TYPES)
    set(multi_config_output_root "${CMAKE_BINARY_DIR}")
else()
    if(CMAKE_BUILD_TYPE)
        set(output_config "${CMAKE_BUILD_TYPE}")
    else()
        set(output_config "Debug")
    endif()
    string(TOLOWER "${output_config}" output_config_lower)
    set(single_config_output_dir "${CMAKE_BINARY_DIR}/${output_config_lower}")
endif()

add_executable(remap
    main.cpp
)

add_executable(tap-timer
    tap_timing.cpp
)

add_executable(request-trace
    request_trace.cpp
)

foreach(target remap tap-timer request-trace)
    if(target STREQUAL "remap")
        set(REMAP_FILE_DESCRIPTION "Windows Terminal key remapper")
    elseif(target STREQUAL "tap-timer")
        set(REMAP_FILE_DESCRIPTION "Key tap timing")
    elseif(target STREQUAL "request-trace")
        set(REMAP_FILE_DESCRIPTION "ConPTY request tracer")
    else()
        message(FATAL_ERROR "Unexpected target for version metadata: ${target}")
    endif()
    set(REMAP_INTERNAL_NAME "${target}")
    set(REMAP_ORIGINAL_FILENAME "${target}.exe")
    set(REMAP_VERSION_RESOURCE "${REMAP_GENERATED_DIR}/${target}-version.rc")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_info.rc.in"
        "${REMAP_VERSION_RESOURCE}"
        @ONLY)

    target_include_directories(${target} PRIVATE "${REMAP_GENERATED_DIR}")
    target_sources(${target} PRIVATE "${REMAP_VERSION_RESOURCE}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )

    if(MSVC)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target} PRIVATE
            /W4
            /EHsc
            "$<$<CONFIG:Release>:/Zi>"
            "$<$<CONFIG:RelWithDebInfo>:/Zi>"
            "$<$<CONFIG:MinSizeRel>:/Zi>"
        )
        target_link_options(${target} PRIVATE
            "$<$<CONFIG:Release>:/DEBUG:FULL>"
            "$<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>"
            "$<$<CONFIG:MinSizeRel>:/DEBUG:FULL>"
        )
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${multi_config_output_root}"
            LIBRARY_OUTPUT_DIRECTORY "${multi_config_output_root}"
            ARCHIVE_OUTPUT_DIRECTORY "${multi_config_output_root}"
            PDB_OUTPUT_DIRECTORY "${multi_config_output_root}"
            COMPILE_PDB_OUTPUT_DIRECTORY "${multi_config_output_root}"
        )

        foreach(config ${CMAKE_CONFIGURATION_TYPES})
            string(TOUPPER "${config}" config_upper)
            string(TOLOWER "${config}" config_lower)
            set_target_properties(${target} PROPERTIES
                "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${multi_config_output_root}/${config_lower}"
                "LIBRARY_OUTPUT_DIRECTORY_${config_upper}" "${multi_config_output_root}/${config_lower}"
                "ARCHIVE_OUTPUT_DIRECTORY_${config_upper}" "${multi_config_output_root}/${config_lower}"
                "PDB_OUTPUT_DIRECTORY_${config_upper}" "${multi_config_output_root}/${config_lower}"
                "COMPILE_PDB_OUTPUT_DIRECTORY_${config_upper}" "${multi_config_output_root}/${config_lower}"
            )
        endforeach()
    else()
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${single_config_output_dir}"
            LIBRARY_OUTPUT_DIRECTORY "${single_config_output_dir}"
            ARCHIVE_OUTPUT_DIRECTORY "${single_config_output_dir}"
            PDB_OUTPUT_DIRECTORY "${single_config_output_dir}"
            COMPILE_PDB_OUTPUT_DIRECTORY "${single_config_output_dir}"
        )
    endif()
endforeach()
