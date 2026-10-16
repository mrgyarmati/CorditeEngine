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

# Directory containing the Khronos validation layer manifest, used by GPU tests. Defaults
# to the copy installed by scripts/bootstrap.ps1 -WithVulkanSdk; empty relies on a
# system-wide Vulkan SDK or runs with NVRHI validation only.
set(cordite_default_layer_path "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.tools/vulkan-sdk/Bin/VkLayer_khronos_validation.json")
    set(cordite_default_layer_path "${CMAKE_CURRENT_SOURCE_DIR}/.tools/vulkan-sdk/Bin")
endif()
set(CORDITE_VULKAN_LAYER_PATH "${cordite_default_layer_path}" CACHE PATH
    "Directory with the Vulkan validation layer, added to VK_ADD_LAYER_PATH for GPU tests")

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
include(CorditeEmbed)
include(CorditeModule)

# Must run before any add_subdirectory() so that tests registered below are generated.
if(CORDITE_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(engine)
add_subdirectory(editor)
add_subdirectory(player)
add_subdirectory(tools)

# The engine's shader sources travel with the executables, so that a build tree (or an
# installed editor) is self-contained. Development builds still read the repository copy.
add_custom_target(cordite_engine_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${PROJECT_SOURCE_DIR}/shaders"
        "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/shaders"
    COMMENT "Copying engine shaders"
    VERBATIM)
set_target_properties(cordite_engine_shaders PROPERTIES FOLDER "Cordite/Build")

if(CORDITE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
