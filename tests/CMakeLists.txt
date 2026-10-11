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
        unit/core/DocumentTests.cpp
        unit/core/FileSystemTests.cpp
        unit/core/HashTests.cpp
        unit/core/JobSystemTests.cpp
        unit/core/JsonTests.cpp
        unit/core/LogTests.cpp
        unit/core/MemoryTests.cpp
        unit/core/PlatformTests.cpp
        unit/core/ReflectionTests.cpp
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
        unit/math/MathReflectionTests.cpp
        unit/math/ProjectionTests.cpp
        unit/math/RandomTests.cpp
        unit/math/TransformTests.cpp
    LIBRARIES
        cordite::math)

cordite_add_test_suite(world
    SOURCES
        unit/world/ComponentRegistryTests.cpp
        unit/world/SceneSerializerTests.cpp
        unit/world/WorldTests.cpp
    LIBRARIES
        cordite::world)

cordite_add_test_suite(platform
    SOURCES
        unit/platform/InputTests.cpp
        unit/platform/KeyCodesTests.cpp
        unit/platform/WindowTests.cpp
    LIBRARIES
        cordite::platform)

cordite_add_test_suite(assets
    SOURCES
        unit/assets/AssetDatabaseTests.cpp
        unit/assets/AssetManagerTests.cpp
        unit/assets/AssetTestSupport.h
        unit/assets/GltfExporterTests.cpp
        unit/assets/GltfImporterTests.cpp
        unit/assets/GltfTestSupport.h
        unit/assets/ImageCompareTests.cpp
        unit/assets/ImageTests.cpp
        unit/assets/MaterialTests.cpp
        unit/assets/MeshProcessingTests.cpp
        unit/assets/MeshTests.cpp
        unit/assets/MipmapsTests.cpp
        unit/assets/PrimitivesTests.cpp
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

# The parts of the renderer that need no GPU: view matrices and debug geometry.
cordite_add_test_suite(renderer
    SOURCES
        unit/renderer/CameraViewTests.cpp
        unit/renderer/DebugDrawTests.cpp
    LIBRARIES
        cordite::renderer)

# The editor without its user interface: commands, undo, the document and the camera.
cordite_add_test_suite(editor
    SOURCES
        unit/editor/AssetCommandsTests.cpp
        unit/editor/CommandRegistryTests.cpp
        unit/editor/EditorActionsTests.cpp
        unit/editor/EditorCameraTests.cpp
        unit/editor/EditorCommandsTests.cpp
        unit/editor/EditorContextTests.cpp
        unit/editor/EditorTestSupport.h
        unit/editor/UndoStackTests.cpp
    LIBRARIES
        cordite::editor)

# What the engine module adds on top of the others, as far as it needs no GPU: projects,
# placing models, and checking the assets a scene uses.
cordite_add_test_suite(engine
    SOURCES
        unit/engine/ModelInstanceTests.cpp
        unit/engine/ProjectTests.cpp
        unit/engine/SceneAssetsTests.cpp
    LIBRARIES
        cordite::engine)

# Keeps the showcase project loading and complete. Needs no GPU.
cordite_add_test_suite(samples
    SOURCES
        unit/samples/ShowcaseSceneTests.cpp
    LIBRARIES
        cordite::engine)

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
        render/DebugLineTests.cpp
        render/EditorAppTests.cpp
        render/EngineTests.cpp
        render/GraphicsDeviceTests.cpp
        render/RenderPassTests.cpp
        render/RenderTargetTests.cpp
        render/RenderTestSupport.cpp
        render/RenderTestSupport.h
        render/SceneRendererTests.cpp
        render/SwapchainTests.cpp
        render/UiTests.cpp
    LIBRARIES
        cordite::editor
    LABELS
        gpu)

# Runs the real editor executable in batch mode: it opens the showcase project, runs a
# script of commands on it, renders and saves a picture. It exits with a non-zero status
# when anything fails, when a script step fails, or when the GPU validation layers report
# a problem.
add_test(NAME editor_smoke
    COMMAND cordite-editor
        --headless --validation --frames 5 --fixed-time-step 0.1
        --width 960 --height 540
        --project samples/showcase
        --exec tests/data/editor-smoke.json
        --exec-output "${PROJECT_BINARY_DIR}/test-tmp/editor-smoke.json"
        --screenshot "${PROJECT_BINARY_DIR}/test-tmp/editor-smoke.png"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(editor_smoke PROPERTIES LABELS "gpu" TIMEOUT 120)
cordite_configure_gpu_test(editor_smoke "gpu")

cordite_add_layering_test()
