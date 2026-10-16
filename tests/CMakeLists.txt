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
        unit/core/DirectoryWatcherTests.cpp
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
        unit/math/EquirectangularTests.cpp
        unit/math/FrustumTests.cpp
        unit/math/GeometryTests.cpp
        unit/math/MathJsonTests.cpp
        unit/math/MathReflectionTests.cpp
        unit/math/ProjectionTests.cpp
        unit/math/RandomTests.cpp
        unit/math/SphericalHarmonicsTests.cpp
        unit/math/TransformTests.cpp
    LIBRARIES
        cordite::math)

cordite_add_test_suite(world
    SOURCES
        unit/world/ComponentRegistryTests.cpp
        unit/world/PrefabTests.cpp
        unit/world/SceneSerializerTests.cpp
        unit/world/WorldTests.cpp
    LIBRARIES
        cordite::world)

cordite_add_test_suite(animation
    SOURCES
        support/SkinnedModelSupport.h
        unit/animation/AnimationSystemTests.cpp
        unit/animation/PoseTests.cpp
    LIBRARIES
        cordite::animation)

cordite_add_test_suite(physics
    SOURCES
        unit/physics/CharacterControllerTests.cpp
        unit/physics/PhysicsCharacterTests.cpp
        unit/physics/PhysicsSystemTests.cpp
        unit/physics/PhysicsWorldTests.cpp
    LIBRARIES
        cordite::physics)

cordite_add_test_suite(audio
    SOURCES
        unit/audio/AudioComponentsTests.cpp
        unit/audio/AudioSystemTests.cpp
    LIBRARIES
        cordite::audio)

# Lua's own API is part of what these tests use: the functions they give to scripts are
# written against it.
cordite_add_test_suite(scripting
    SOURCES
        unit/scripting/ScriptAudioApiTests.cpp
        unit/scripting/ScriptComponentApiTests.cpp
        unit/scripting/ScriptContactTests.cpp
        unit/scripting/ScriptEntityApiTests.cpp
        unit/scripting/ScriptInputApiTests.cpp
        unit/scripting/ScriptMathApiTests.cpp
        unit/scripting/ScriptPhysicsApiTests.cpp
        unit/scripting/ScriptRuntimeTests.cpp
        unit/scripting/ScriptSystemTests.cpp
        unit/scripting/ScriptTimerTests.cpp
        unit/scripting/ScriptTestSupport.h
        unit/scripting/ScriptValueTests.cpp
        unit/scripting/ScriptWorldApiTests.cpp
    LIBRARIES
        cordite::scripting
        cordite::lua)

cordite_add_test_suite(platform
    SOURCES
        unit/platform/InputMapTests.cpp
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
        unit/assets/GltfSkinningTests.cpp
        unit/assets/GltfTestSupport.h
        unit/assets/ImageCompareTests.cpp
        unit/assets/ImageTests.cpp
        unit/assets/MaterialTests.cpp
        unit/assets/MeshProcessingTests.cpp
        unit/assets/MeshTests.cpp
        unit/assets/MipmapsTests.cpp
        unit/assets/PrefabTests.cpp
        unit/assets/PrimitivesTests.cpp
        unit/assets/SkeletonTests.cpp
        unit/assets/SkinnedMeshTests.cpp
        unit/assets/SoundTests.cpp
        support/SkinnedModelSupport.h
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

# The parts of the renderer that need no GPU: view matrices, debug geometry, the
# components, and the lights of a world and the clusters they are sorted into.
cordite_add_test_suite(renderer
    SOURCES
        unit/renderer/AmbientOcclusionTests.cpp
        unit/renderer/CameraViewTests.cpp
        unit/renderer/DebugDrawTests.cpp
        unit/renderer/EnvironmentLightTests.cpp
        unit/renderer/FogTests.cpp
        unit/renderer/LightClustersTests.cpp
        unit/renderer/PostProcessSettingsTests.cpp
        unit/renderer/RenderComponentsTests.cpp
        unit/renderer/RenderDebugViewTests.cpp
        unit/renderer/RenderQualityTests.cpp
        unit/renderer/SceneLightsTests.cpp
        unit/renderer/ScenePostProcessTests.cpp
        unit/renderer/ShadowPlanTests.cpp
    LIBRARIES
        cordite::renderer)

# The parts of the interface that need no GPU: its themes, its typefaces and its icons.
cordite_add_test_suite(ui
    SOURCES
        unit/ui/UiIconsTests.cpp
        unit/ui/UiThemeTests.cpp
        unit/ui/UiWidgetsTests.cpp
    LIBRARIES
        cordite::ui)

# The editor without its user interface: commands, undo, the document and the camera.
cordite_add_test_suite(editor
    SOURCES
        unit/editor/AssetBrowserTests.cpp
        unit/editor/AssetCommandsTests.cpp
        unit/editor/AssetImportTests.cpp
        unit/editor/AssetMoveTests.cpp
        unit/editor/ColliderOutlinesTests.cpp
        unit/editor/CommandRegistryTests.cpp
        unit/editor/ConsolePanelTests.cpp
        unit/editor/EditorActionsTests.cpp
        unit/editor/EditorCameraTests.cpp
        unit/editor/EditorCommandsTests.cpp
        unit/editor/EditorContextTests.cpp
        unit/editor/EditorIconsTests.cpp
        unit/editor/EditorPreferencesTests.cpp
        unit/editor/EditorToolbarTests.cpp
        unit/editor/FileAssociationTests.cpp
        unit/editor/GameExportTests.cpp
        unit/editor/GizmoTests.cpp
        unit/editor/InputCommandsTests.cpp
        unit/editor/ScriptCommandsTests.cpp
        unit/editor/PhysicsCommandsTests.cpp
        unit/editor/PosedMeshTests.cpp
        unit/editor/EditorTestSupport.h
        unit/editor/UndoStackTests.cpp
    LIBRARIES
        cordite::editor)

# What the engine module adds on top of the others, as far as it needs no GPU: projects,
# placing models, and checking the assets a scene uses.
cordite_add_test_suite(engine
    SOURCES
        support/SkinnedModelSupport.h
        unit/engine/EngineShaderDirectoryTests.cpp
        unit/engine/ModelInstanceTests.cpp
        unit/engine/ProjectTests.cpp
        unit/engine/SceneAssetsTests.cpp
        unit/engine/SceneChangeTests.cpp
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
        render/AmbientOcclusionTests.cpp
        render/AntiAliasingTests.cpp
        render/DebugLineTests.cpp
        render/EditorAppTests.cpp
        render/EngineTests.cpp
        render/EnvironmentTests.cpp
        render/FogTests.cpp
        render/FramePacerTests.cpp
        render/GameTests.cpp
        render/GpuTimerTests.cpp
        render/GraphicsDeviceTests.cpp
        render/PostProcessTests.cpp
        render/PropertyEditorTests.cpp
        render/RenderPassTests.cpp
        render/RenderTargetTests.cpp
        render/RenderTestSupport.cpp
        render/RenderTestSupport.h
        render/SceneLightingTests.cpp
        render/SceneRendererTests.cpp
        render/SceneShadowTests.cpp
        render/SceneTestSupport.h
        render/ShadowAtlasTests.cpp
        render/SkinningTests.cpp
        render/SwapchainTests.cpp
        render/UiTests.cpp
        render/ViewBindingsTests.cpp
    LIBRARIES
        cordite::editor
    LABELS
        gpu
    # Every rendering feature adds to this suite, and each of its tests starts a device
    # and a renderer of its own: in a Debug build it runs for more than five minutes.
    TIMEOUT 900)

# Measures what the scenarios cost - the showcase, a scene full of lights - writes it to
# tests/perf/output/ and checks it against tests/perf/budgets.json. See the comment at
# the top of perf/PerformanceTestSupport.h.
cordite_add_test_suite(performance
    SOURCES
        perf/LightingPerformanceTests.cpp
        perf/PerformanceTestSupport.h
        perf/ShowcasePerformanceTests.cpp
        render/RenderTestSupport.cpp
        render/RenderTestSupport.h
    LIBRARIES
        cordite::engine
        cordite::shader_compiler
    LABELS
        gpu
        perf)

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

# The program of a game starts, and says what it is: it links the engine and nothing of
# the editor. That it plays a game is tested where a game is made of a project: export.
add_test(NAME player_starts COMMAND cordite-player --version)
set_tests_properties(player_starts PROPERTIES PASS_REGULAR_EXPRESSION "[0-9]+\\.[0-9]+")

# A game made of the showcase by the editor, as a person makes one, played from where it
# was put: what it needs and was not given is found here.
add_test(NAME export_smoke
    COMMAND ${CMAKE_COMMAND}
        "-DEDITOR=$<TARGET_FILE:cordite-editor>"
        "-DPROJECT=${PROJECT_SOURCE_DIR}/samples/showcase"
        "-DDESTINATION=${PROJECT_BINARY_DIR}/test-tmp/export-smoke"
        "-DPROGRAM=Showcase$<TARGET_FILE_SUFFIX:cordite-player>"
        "-DSCREENSHOT=${PROJECT_BINARY_DIR}/test-tmp/export-smoke.png"
        -P "${PROJECT_SOURCE_DIR}/cmake/RunExportedGame.cmake")
set_tests_properties(export_smoke PROPERTIES LABELS "gpu" TIMEOUT 300)
cordite_configure_gpu_test(export_smoke "gpu")

if(WIN32)
    # The editor and a game have windows of their own: started with a double click, no
    # console window opens beside them (WINDOWED in cordite_add_executable).
    add_test(NAME programs_open_no_console
        COMMAND ${CMAKE_COMMAND}
            "-DPROGRAMS=$<TARGET_FILE:cordite-editor>$<SEMICOLON>$<TARGET_FILE:cordite-player>"
            -P "${PROJECT_SOURCE_DIR}/cmake/CheckWindowsSubsystem.cmake")
endif()

cordite_add_layering_test()

# Rejects tests that are written in a way that crashes as soon as doctest is asked to
# list the assertions that pass; see the script.
add_test(NAME test_sources
    COMMAND ${CMAKE_COMMAND}
        "-DREPOSITORY_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/CheckTestSources.cmake")
set_tests_properties(test_sources PROPERTIES LABELS "lint")

# doctest builds the message of an assertion when the assertion passes, too, if it is
# asked to list those (--success). Tests written so that this crashes pass for everyone
# until someone wants to look closer: one suite is run that way, to keep it possible -
# the one that found both ways there were of getting it wrong (CheckTestSources.cmake,
# and CHECK_NEAR in support/MathTestSupport.h). What it lists goes to a file.
add_test(NAME assets_listed
    COMMAND cordite_assets_tests --success "--out=${PROJECT_BINARY_DIR}/assets-listed.txt"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(assets_listed PROPERTIES LABELS "lint" TIMEOUT 300)
