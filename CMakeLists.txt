cmake_minimum_required(VERSION 3.20)

project(remap LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

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

foreach(target remap tap-timer)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${multi_config_output_root}"
            LIBRARY_OUTPUT_DIRECTORY "${multi_config_output_root}"
            ARCHIVE_OUTPUT_DIRECTORY "${multi_config_output_root}"
        )

        foreach(config ${CMAKE_CONFIGURATION_TYPES})
            string(TOUPPER "${config}" config_upper)
            string(TOLOWER "${config}" config_lower)
            set_target_properties(${target} PROPERTIES
                "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${multi_config_output_root}/${config_lower}"
                "LIBRARY_OUTPUT_DIRECTORY_${config_upper}" "${multi_config_output_root}/${config_lower}"
                "ARCHIVE_OUTPUT_DIRECTORY_${config_upper}" "${multi_config_output_root}/${config_lower}"
            )
        endforeach()
    else()
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${single_config_output_dir}"
            LIBRARY_OUTPUT_DIRECTORY "${single_config_output_dir}"
            ARCHIVE_OUTPUT_DIRECTORY "${single_config_output_dir}"
        )
    endif()
endforeach()
