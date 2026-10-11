# Compiler flags shared by every Cordite target, exposed as the cordite::build_options
# interface library so third-party code is never compiled with our warning level.

add_library(cordite_build_options INTERFACE)
add_library(cordite::build_options ALIAS cordite_build_options)

target_compile_features(cordite_build_options INTERFACE cxx_std_20)

# CORDITE_ENABLE_ASSERTS stays on in RelWithDebInfo, the day-to-day development
# configuration. Release is the shipping configuration used for exported games.
target_compile_definitions(cordite_build_options INTERFACE
    $<$<CONFIG:Debug>:CORDITE_DEBUG=1>
    $<$<NOT:$<CONFIG:Release>>:CORDITE_ENABLE_ASSERTS=1>
    $<$<CONFIG:Release>:CORDITE_SHIPPING=1>)

if(WIN32)
    target_compile_definitions(cordite_build_options INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS)
endif()

set(cordite_clang_warnings
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wimplicit-fallthrough
    -Wextra-semi)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(cordite_build_options INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:preprocessor
        /Zc:__cplusplus
        /EHsc)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(cordite_build_options INTERFACE /W4 /utf-8 /EHsc ${cordite_clang_warnings})
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(cordite_build_options INTERFACE -Wall -Wextra -Wpedantic ${cordite_clang_warnings})
else()
    target_compile_options(cordite_build_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wimplicit-fallthrough)
endif()

# Clang 19+ warns when a designated initializer leaves out members that have default
# member initializers. Omitting them is the whole point of `{.key = ..., .value = ...}`.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wmissing-designated-field-initializers CORDITE_HAS_DESIGNATED_FIELD_WARNING)
    if(CORDITE_HAS_DESIGNATED_FIELD_WARNING)
        target_compile_options(cordite_build_options INTERFACE -Wno-missing-designated-field-initializers)
    endif()
endif()

if(CORDITE_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "CORDITE_ENABLE_SANITIZERS is only supported with GCC and Clang.")
    endif()
    # Sanitizer runtimes must see every translation unit, including third-party code.
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
