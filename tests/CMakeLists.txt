# Shared test harness: the doctest entry point plus fixtures used by every suite.
add_library(cordite_test_support STATIC
    support/TestMain.cpp
    support/TestSupport.cpp
    support/TestSupport.h
    support/MathTestSupport.h)
add_library(cordite::test_support ALIAS cordite_test_support)

target_include_directories(cordite_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cordite_test_support PUBLIC doctest::doctest cordite::core)
target_compile_definitions(cordite_test_support PRIVATE
    CORDITE_TEST_TEMP_DIR="${PROJECT_BINARY_DIR}/test-tmp"
    CORDITE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(cordite_test_support PROPERTIES
    FOLDER "Cordite/Tests"
    COMPILE_WARNING_AS_ERROR ${CORDITE_WARNINGS_AS_ERRORS})

cordite_add_test_suite(core
    SOURCES
        unit/core/AssertTests.cpp
        unit/core/BinaryStreamTests.cpp
        unit/core/ConfigTests.cpp
        unit/core/FileSystemTests.cpp
        unit/core/HashTests.cpp
        unit/core/JobSystemTests.cpp
        unit/core/JsonTests.cpp
        unit/core/LogTests.cpp
        unit/core/MemoryTests.cpp
        unit/core/PlatformTests.cpp
        unit/core/ResultTests.cpp
        unit/core/StringUtilsTests.cpp
        unit/core/TimeTests.cpp
        unit/core/UuidTests.cpp
    LIBRARIES
        cordite::core)

cordite_add_test_suite(math
    SOURCES
        unit/math/ColorTests.cpp
        unit/math/CurveTests.cpp
        unit/math/EncodingTests.cpp
        unit/math/FrustumTests.cpp
        unit/math/GeometryTests.cpp
        unit/math/MathJsonTests.cpp
        unit/math/ProjectionTests.cpp
        unit/math/RandomTests.cpp
        unit/math/TransformTests.cpp
    LIBRARIES
        cordite::math)

cordite_add_test_suite(platform
    SOURCES
        unit/platform/InputTests.cpp
        unit/platform/KeyCodesTests.cpp
        unit/platform/WindowTests.cpp
    LIBRARIES
        cordite::platform)

cordite_add_test_suite(assets
    SOURCES
        unit/assets/ImageCompareTests.cpp
        unit/assets/ImageTests.cpp
    LIBRARIES
        cordite::assets)

cordite_add_test_suite(shaders
    SOURCES
        unit/shaders/ShaderCacheTests.cpp
        unit/shaders/ShaderLibraryTests.cpp
        unit/shaders/ShaderTestSupport.h
        unit/shaders/ShaderTypesTests.cpp
    LIBRARIES
        cordite::shaders)

# Compiles real shaders with Slang; needs no GPU.
cordite_add_test_suite(shader_compiler
    SOURCES
        unit/shader_compiler/EngineShadersTests.cpp
        unit/shader_compiler/SlangShaderCompilerTests.cpp
    LIBRARIES
        cordite::shader_compiler)

# Everything below renders on a real Vulkan device.
cordite_add_test_suite(render
    SOURCES
        render/EngineTests.cpp
        render/GraphicsDeviceTests.cpp
        render/RenderPassTests.cpp
        render/RenderTestSupport.cpp
        render/RenderTestSupport.h
        render/SwapchainTests.cpp
    LIBRARIES
        cordite::engine
    LABELS
        gpu)

# Runs the real editor executable in batch mode. It exits with a non-zero status when
# anything fails or when the GPU validation layers report a problem.
add_test(NAME editor_smoke
    COMMAND cordite-editor
        --headless --validation --frames 5 --fixed-time-step 0.1
        --width 320 --height 180
        --screenshot "${PROJECT_BINARY_DIR}/test-tmp/editor-smoke.png"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(editor_smoke PROPERTIES LABELS "gpu" TIMEOUT 120)
cordite_configure_gpu_test(editor_smoke "gpu")

cordite_add_layering_test()
