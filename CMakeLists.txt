cmake_minimum_required(VERSION 3.20)

project(remap LANGUAGES CXX)

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
endforeach()
