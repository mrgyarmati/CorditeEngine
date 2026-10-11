cmake_minimum_required(VERSION 3.28)

project(Cordite
    VERSION 0.1.0
    DESCRIPTION "Cordite - a deferred-rendering 3D game engine"
    LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a preset: cmake --preset <name>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(CORDITE_BUILD_TESTS "Build the unit and integration tests" ON)
option(CORDITE_WARNINGS_AS_ERRORS "Treat warnings in Cordite targets as errors" OFF)
option(CORDITE_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The engine does not use C++20 modules; scanning for them only slows the build down.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Link the C runtime statically so exported games run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(CorditeCompilerOptions)
include(CorditeDependencies)
include(CorditeModule)

# Must run before any add_subdirectory() so that tests registered below are generated.
if(CORDITE_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(engine)

if(CORDITE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
