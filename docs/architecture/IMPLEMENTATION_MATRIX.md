# Cordite implementation matrix

Phase 0 audit, 12 November 2026, at commit `4e03feb` (`main`, one commit ahead of
`origin/main`). Part of the [Phase 0 audit](CORDITE_AUDIT.md).

Every row was checked against source and tests, not against the README. Labels:

- **Verified** - implemented, integrated, exercised by at least one test, and that test
  passed in the [baseline run](BASELINE.md).
- **Partial** - real code, but a documented or observed gap that the roadmap cares about.
- **Planned** - absent from the source (sometimes listed as "not yet" in the README).
- **Broken** - reproducibly wrong today.
- **Unknown** - cannot be settled without hardware or a person (for example, listening).

Paths are relative to the repository root. `src` is `engine/cordite/<module>/`; unit
tests are in `tests/unit/<module>/`, GPU tests in `tests/render/`.

## Summary

| Area | Verified | Partial | Planned | Broken | Unknown |
|------|---------:|--------:|--------:|-------:|--------:|
| Core and math | 6 | 1 | 0 | 0 | 0 |
| Platform and input | 3 | 4 | 1 | 0 | 0 |
| World, scenes, prefabs | 6 | 0 | 1 | 0 | 0 |
| Assets | 7 | 2 | 3 | 0 | 0 |
| Animation | 4 | 0 | 1 | 0 | 0 |
| Physics | 8 | 1 | 1 | 0 | 0 |
| Audio | 6 | 0 | 1 | 0 | 1 |
| Scripting | 8 | 1 | 0 | 0 | 0 |
| RHI and shaders | 5 | 2 | 3 | 0 | 1 |
| Renderer | 20 | 0 | 1 | 0 | 0 |
| Editor UI (ImGui) | 6 | 2 | 2 | 0 | 0 |
| Runtime (game) UI | 0 | 0 | 1 | 0 | 0 |
| Editor | 12 | 3 | 6 | 0 | 0 |
| Automation | 3 | 2 | 2 | 0 | 0 |
| Runtime, export, player | 5 | 1 | 0 | 0 | 1 |

Nothing in the audited scope is **Broken**. Both editor defects that the user reported
in October have fixes and regression tests, and those tests pass (see
[Editor](#editor)). No `TODO`, `FIXME` or `HACK` markers exist in `engine/` or `editor/`.

## Core and math

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| Result, logging, assertions, hashing, UUIDs, JSON, binary streams, file I/O, timing | Verified | `core/*` | `ResultTests`, `UuidTests`, `JsonTests`, ... (204 cases) | |
| Reflection (`TypeBuilder`, `PropertyPatch`, `describe`) | Verified | `core/Reflection.h:368-501` | `ReflectionTests` "a rejected update leaves the object exactly as it was" | Hand-written; data members only; no methods, maps, lists of structs, categories, display names or global type registry |
| Job system (enkiTS) | Verified, **unused** | `core/JobSystem.h:50-83` | `JobSystemTests` "parallelFor visits every index exactly once" | No engine code submits jobs. Jolt runs single-threaded |
| Linear and pool allocators, `Config` | Verified, unused | `core/memory/*`, `core/Config.cpp` | `MemoryTests`, `ConfigTests` | No users outside core |
| Directory watcher | Verified | `core/DirectoryWatcher.cpp:110` | `DirectoryWatcherTests` | The only engine worker thread; editor only |
| Document versioning | Partial | `core/Document.h:48` | `DocumentTests` | Newer versions are refused; there is no migration path |
| Math (Transform, curves, frustum, geometry, colour, SH, random) | Verified | `math/*` | 118 cases | |

## Platform and input

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| Window (GLFW) | Verified (headless) | `platform/Window.cpp:340-418` | `WindowTests` "a window can be resized, retitled, hidden and closed" | Tests use GLFW's null platform |
| Keyboard, mouse, text, file drop | Verified | `platform/Input.h:70-158` | `InputTests` "typed text is collected as UTF-8" | |
| Gamepads | Partial | `Window.cpp:428-448` | Injected state only (`injectGamepad`) | Real GLFW polling untested; at most 4; no rumble |
| `InputMap` / `InputActions` (named actions and axes, dead zones, thresholds) | Verified | `platform/InputMap.h:176-265` | `InputMapTests` "an axis has the value of whichever of its bindings is moved furthest" | |
| Input contexts, priorities, modifiers, triggers, consumption | **Planned** | `InputMap.h:92,145` | - | One flat map. GLFW `mods` are ignored (`Window.cpp:176`). **Nothing can stop gameplay from seeing input that a UI used** |
| Runtime remapping | Partial | `InputMap.h:222` `setMap` | "what is held when a map is taken was not pressed for it" | Whole map swap only; no per-binding rebind or user override layer |
| DPI / content scale | Partial | `Window.cpp:255-260` | `WindowTests` (checks `> 0`) | No content-scale or framebuffer-size callback; the UI scale is read once (`ui/UiSystem.cpp:68`) |
| Text input / IME, clipboard, cursor shapes | Partial | `Window.cpp:200,413` | "invalid code points are not turned into text" | Committed code points only; no composition, no clipboard API |

## World, scenes and prefabs

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| EnTT world, ids, names, ordered hierarchy, enable state | Verified | `world/World.h:59-208` | `WorldTests` "a hierarchy deeper than any call stack is handled" | No tags or layers |
| Component registry | Verified | `world/ComponentRegistry.h:69-113` | `ComponentRegistryTests` | Registered by hand |
| Scene files (strict JSON, duplicate with id remapping) | Verified | `world/SceneSerializer.cpp` | `SceneSerializerTests` "an invalid scene is rejected and leaves the world untouched" | Format version 2; older versions are migrated (`migrateDocument`) |
| Prefab placement, linked to the prefab (`PrefabLink`) | Verified | `world/Prefab.cpp` | world `PrefabTests` | Instances placed by scripts are linked but not followed |
| Overrides and revert, instances brought up to date on open and refresh | Verified | `world/Prefab.cpp` `syncPrefabInstance`, `editor/PrefabLinks.cpp`, `EditorActions` | world `PrefabTests`, editor `AssetCommandsTests` "a prefab that changes changes its instances..." | Editor only; a game plays what was saved |
| Apply to the prefab (one override, an entity, the whole instance with what was added) | Verified | `world/Prefab.cpp` `prefabEntitiesWithOverrides`, `EditorActions::applyPrefabOverrides` | world `PrefabTests`, editor `AssetCommandsTests` "prefab.apply writes..." | Refused for an instance holding another prefab; deleting an entity of the prefab is no override yet |
| Nested prefabs, variants | **Planned** (P1-12 slice 4; variants Phase 4) | `docs/architecture.md` "A prefab stays linked to what was placed from it" | "a prefab is not an instance of another..." | A nested instance is a copy inside its outer one |

## Assets

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| Asset database, `.meta` ids, sub-asset ids | Verified | `assets/AssetDatabase.h:187-269` | `AssetDatabaseTests` "an asset keeps its id when it is moved together with its .meta file" | Sub-asset ids follow their index in the file |
| Dependency graph (uses / used by) | Verified | `AssetDatabase.h:154,211,220` | "what needs an asset is listed with it, each once..." | What scripts load at runtime is not tracked |
| Asset manager (load by id) | Partial | `AssetManager.h:24` | `AssetManagerTests` | **Synchronous, on the calling thread, not thread-safe.** No handles, cancellation or priorities |
| glTF import and export (skins, animations) | Verified | `GltfImporter.cpp`, `GltfExporter.cpp` | `GltfImporterTests`, `GltfSkinningTests` | Morph targets dropped with a warning |
| FBX / OBJ import (ufbx, newest commit) | Partial | `FbxImporter.cpp:270-277,442` | `FbxImporterTests` (15) "an FBX file in centimetres comes in metres..." | Static meshes and materials only: no skins, animations, blend shapes or vertex colours. Embedded textures untested |
| Images, mipmaps, meshes, LODs, primitives, materials with parents | Verified | `Image.cpp`, `MeshProcessing.cpp`, `Material.cpp` | `MeshProcessingTests`, `AssetManagerTests` | |
| Skeletons, clips | Verified | `Skeleton.h`, `Animation.h:39-59` | `SkeletonTests` | TRS tracks only |
| Sounds (WAV, FLAC, MP3) | Verified | `Sound.cpp:118-170` | `SoundTests` "a FLAC file is read, to the last bit" | No Ogg Vorbis; decoded whole |
| Prefab, sequence and render-target assets | Verified | `Prefab.h`, `Sequence.h`, `RenderTarget.cpp` | `SequenceTests`, `RenderTargetTests` | Authored as files; no timeline editor |
| Import settings, derived-data cache | **Planned** | `.meta` holds only format, version and id (`AssetDatabase.cpp:28`) | - | Reimport is "refresh and parse again"; deterministic because ids are stable |
| Async loading, bundles | **Planned** | - | - | |
| Typed data assets (generic, reflected) | **Planned** | 12 fixed asset types (`AssetDatabase.h:55-83`) | - | `MaterialData` and `RenderTargetData` show the pattern to follow |

## Animation

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| Pose sampling, two-clip blend | Verified | `animation/Pose.h:47-97` | `PoseTests`, `AnimationSystemTests` | One `blendClip` per animator |
| `Animator` and `AnimationSystem` | Verified | `AnimationSystem.h:52-98` | `AnimationSystemTests` | |
| Camera shake | Verified | `CameraShake.cpp` | `CameraShakeTests` (13) | |
| Sequences (transform, property, camera-cut and event tracks) | Verified | `SequenceSystem.h:41-76` | `SequenceSystemTests` | |
| More than two clips, layers, blend trees, state machines, root motion, sockets, morph targets, IK, clip events | **Planned** | README "What animation does not do yet" | - | Confirmed absent in source |

## Physics (Jolt 5.6.0)

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| Static, kinematic and dynamic bodies; forces; CCD | Verified | `physics/PhysicsWorld.h:231-338` | `PhysicsWorldTests` "a body falls as fast as gravity makes it" | No mass or inertia override |
| Box, sphere, capsule, hull, triangle mesh, compound | Verified | `PhysicsWorld.h:248-264,500-513` | "a convex hull is the shape around its points..." | Mesh shapes only on static or kinematic bodies |
| Joints (fixed, ball, hinge, slider, distance) | Verified | `PhysicsWorld.h:163-228` | One case per type | No motors or breakable joints |
| Character controller | Verified | `CharacterSet.cpp` | `PhysicsCharacterTests` (25), `CharacterControllerTests` (14) | |
| Raycast, sphere cast, overlap | Verified | `PhysicsWorld.h:682-701` | "a ray hits the first thing in its way" | Closest hit only; no filter argument |
| Contact and trigger events | Verified | `PhysicsSystem.h:151-178` | "a sensor notices what reaches into it..." | No "stay" events |
| Fixed step | Verified | `PhysicsSystem.h:127-130` | "time passes in steps of one length..." | No render interpolation |
| Determinism | Partial | `CROSS_PLATFORM_DETERMINISTIC` | "a simulation is the same every time it is run" | Same process only; cross-machine claim untested |
| Editor debug drawing, `physics.*` commands | Verified | `editor/ViewportPanel.cpp:48-76,160-200` | `ColliderOutlinesTests`, `PhysicsCommandsTests` | |
| Collision layers and masks | **Planned** | three fixed internal layers (`PhysicsWorld.cpp:72-75`) | - | Needed by gameplay; touches components, scene format and script API |
| Multi-threaded simulation | Not applicable (single thread by design) | `JobSystemSingleThreaded` (`PhysicsWorld.cpp:1067`) | - | Chosen for determinism |

## Audio (miniaudio 0.11.25)

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| `AudioSource`, `AudioListener` | Verified | `audio/AudioComponents.h:92-131` | `AudioComponentsTests`, `AudioSystemTests` | One listener |
| Spatialisation, inverse and linear rolloff | Verified | `AudioSystem.cpp:343-358` | "a source in space is heard from its side" | No Doppler, cones, occlusion, HRTF |
| Five fixed buses plus master | Verified | `AudioComponents.h:49-63` | "every bus has a volume of its own..." | No effects or routing |
| Voice limit (64) | Verified | `AudioSystem.h:71-77` | "no more sounds are heard at once than there are voices" | No priority or stealing |
| Offline and silent output | Verified | `AudioSystem.cpp:221-258` | "an audio system mixes into memory unless it is asked for more" | |
| Editor preview, pause, clear | Verified | `AudioSystem.h:153-188` | three preview cases | |
| Output to a real device | **Unknown** | `player/Main.cpp:116`, `editor/Main.cpp:172` | Name check only (`AudioSystemTests.cpp:190`) | Never opened by a test or heard by a person (README) |
| Streaming | **Planned** | `Sound.h:19-22` | - | About 23 MB per minute of music |

## Scripting (Lua 5.4.8, compiled as C++)

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| Instruction, memory and nesting limits | Verified | `ScriptRuntime.h:117-121`, `ScriptRuntime.cpp:67-76` | "a script that does not end is stopped..." | Memory limit shared by all scripts |
| Standard library whitelist, per-chunk environments, strict globals | Verified | `ScriptRuntime.cpp:419-456` | "what reaches the machine is not there" | |
| `Script` component, `ScriptSystem` lifecycle and callbacks | Verified | `ScriptSystem.cpp:60-90` | `ScriptSystemTests` (22) | No fixed or late update |
| Hot reload | Verified (editor only) | `ScriptSystem.cpp:499-515` | "when the assets are refreshed, scripts that changed are loaded again..." | |
| Component access through reflection | Verified | `ScriptComponentApi.cpp` | `ScriptComponentApiTests` | Values go through JSON |
| Script API (about 98 functions: world, math, entity, physics, components, audio, input, log) | Verified | `Script*Api.cpp` | One file per area, plus the showcase `selftest.lua` checked by `ShowcaseSceneTests` | No joint, character, UI or animation functions |
| Error isolation | Verified | `docs/architecture.md` "A script cannot be more than an error" | "an error stops the instance, and is said once" | |
| `script.api` command | Verified | `editor/ScriptCommands.h` | `ScriptCommandsTests` | |
| Language-neutral binding layer | Partial | Catalogue metadata is neutral; every entry is `int(*)(lua_State*)` (`ScriptRuntime.h:89`) | - | `ScriptSystem.h:271-313` exposes `lua_State`. **Lua cannot be compiled out**: `engine` depends on `scripting`, and `Engine.h:53` includes `ScriptSystem.h` |

## RHI and shaders

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| Vulkan 1.3 device and validation | Verified | `rhi/vulkan/VulkanDevice.cpp:82-116` | `GraphicsDeviceTests` (18); validation is checked after every GPU test | |
| Vulkan swapchain (present modes, resize, out-of-date) | Verified | `rhi/Swapchain.cpp:300-400` | `SwapchainTests` | |
| Direct3D 12 device, offscreen, headless engine | Partial | `rhi/d3d12/D3D12Device.cpp:229-299` | `GraphicsDeviceTests` "a device of Direct3D 12 draws offscreen, on the GPU and on WARP"; `EngineTests.cpp:157` (Vulkan vs D3D12) | **No swapchain** (`canPresent=false`, `D3D12Device.cpp:294`). See [Baseline](BASELINE.md) for the full suite on D3D12 |
| Backend fallback when Vulkan is missing | **Planned** | `Auto` means Vulkan (`GraphicsDevice.cpp:28-41`) | - | |
| OpenGL | **Planned** (no code) | only `GLFW_NO_API` (`platform/Window.cpp:375`) | - | Decided 12 November: the Linux fallback when there is no Vulkan (GL track) |
| Metal / MoltenVK | **Unknown** | `VulkanDevice.cpp:168-175`, portability subset | macOS CI runs `-LE gpu` only | Not run on a Mac |
| Slang to SPIR-V, and DXIL on Windows | Verified | `shader_compiler/SlangShaderCompiler.cpp:174-189` | `tests/unit/shader_compiler` | |
| Permutation keys and disk cache | Verified | `shaders/ShaderTypes.h`, `ShaderCache.h` | `ShaderCacheTests`, `ShaderLibraryTests` | |
| Precompiled engine shader cache for exported games | Verified | `shader_compiler/EngineShaderCache.cpp` | `EngineShadersTests`, `export_smoke` | New UI shaders must be listed in `EngineShaders.cpp` |
| Device loss / removal recovery | **Planned** | none (grep) | - | A lost device ends the frame loop with an error |
| Backend-neutral public API | Partial | NVRHI is a public dependency; 31 of 55 renderer headers include `<nvrhi/nvrhi.h>`; `Engine.h` exposes `nvrhi::ICommandList*` | - | NVRHI is the engine's RHI in practice |

## Renderer

All of the following are **Verified**: source plus GPU tests in `tests/render/` and CPU
tests in `tests/unit/renderer/`, all passing in the baseline on Vulkan.

| Feature | Source | GPU tests |
|---------|--------|-----------|
| Deferred G-buffer, depth pre-pass, debug views | `MeshPass.h`, `DeferredLightingPass.h`, `RenderDebugView.h` | `SceneRendererTests` |
| Forward translucency | `MeshPass`, `shaders/passes/MeshForward.slang` | `SceneRendererTests` |
| Clustered lights, lighting channels | `LightClusters.h`, `SceneLights` | `SceneLightingTests` |
| Shadow atlas, sun cascades, filtering | `ShadowAtlas.h`, `ShadowPlan.h` | `ShadowAtlasTests`, `SceneShadowTests` |
| Image-based lighting | `EnvironmentMaps.h` | `EnvironmentTests` (20) |
| Environment probes | `EnvironmentProbes.h` | `EnvironmentProbeTests` (10) |
| Ambient occlusion | `AmbientOcclusionPass.h` | `AmbientOcclusionTests` (13) |
| Screen-space reflections | `ScreenReflectionPass.h` | `ScreenReflectionTests` (12) |
| Height fog | `Fog.h` | `FogTests` (9) |
| Volumetric (lit) fog | `VolumetricFog.h`, `VolumetricFogPass.h` | `FogLightTests` (12) |
| Decals | `Decals.h`, `DecalPass.h` | `DecalTests` (15) |
| Levels of detail | `LevelOfDetail.h` | `LevelOfDetailTests` (5) |
| Scene capture | `SceneRenderer.h` | `SceneCaptureTests` (7) |
| Motion vectors, motion blur | `MotionBlurPass.h` | `MotionVectorTests`, `MotionBlurTests` |
| Depth of field | `DepthOfFieldPass.h` | `DepthOfFieldTests` (10) |
| Light shafts | `LightShaftPass.h` | `LightShaftTests` (8) |
| Rect lights (LTC) | `RectLights.h` | `SceneLightingTests`, `SceneShadowTests` |
| Light cookies | `LightCookies` | `LightCookieTests` (7) |
| Exposure, bloom, tone mapping, grading, FXAA | `PostProcessPass.h` | `PostProcessTests` (25), `AntiAliasingTests` (9) |
| GPU skinning | `SkinPalettes` | `SkinningTests` (16) |

**Planned** (README): temporal anti-aliasing, lens flares, baked lighting, texture
streaming, orthographic editor views. The renderer has **no 2D, sprite or text path**;
only the ImGui renderer draws in 2D.

## Editor UI (Dear ImGui)

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| One ImGui context per process | Verified | `ui/UiSystem.cpp:32-93` | `UiTests` "only one interface system can exist at a time" | Global singleton |
| NVRHI renderer | Verified | `ui/UiRenderer.cpp:51-354` | `UiTests` reference images `ui-widgets-*` | 16-bit indices; whole-atlas re-upload; straight alpha in sRGB |
| Embedded fonts (Inter, JetBrains Mono) and Lucide icons | Verified | `ui/UiFonts.cpp:49-105` | `UiIconsTests` | stb_truetype through ImGui; no shaping |
| DPI-scaled themes (dark, light) | Verified | `ui/UiTheme.h`, `UiSystem.cpp:20-30` | `UiThemeTests` | Scale fixed at creation |
| Headless pointer input | Verified | `UiSystem.cpp:166-180` | `UiTests` "the pointer operates widgets without a window" | |
| Engine texture inside the interface | Verified | `UiRenderer.cpp:131-167` | `UiTests` | |
| Headless keyboard and gamepad input to ImGui | **Planned** | `UiSystem.cpp:166-180` | - | Injected keys reach `Input`, not ImGui |
| Gamepad navigation | **Planned** | only `NavEnableKeyboard` (`UiSystem.cpp:52`) | - | |
| Input capture routing (`wantsPointer`, `wantsKeyboard`) | Partial | `UiSystem.cpp:185-192` | - | Nothing outside `UiSystem` calls them |
| ImGui in shipped games | Partial | `Engine.cpp:198-199` always creates it; `Engine.cpp:324` always renders it | - | Nothing draws into it in a game; cost and cursor side effects remain |

## Runtime (game) UI

| Feature | Status | Notes |
|---------|--------|-------|
| Retained widget tree, layout, styles, focus, navigation, layers, text, designer | **Planned** | Nothing game-facing exists: no HUD, menus or UI script API; the showcase has no UI |

## Editor

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| File > Open project freeze (reported 19 Oct) | **Verified fixed** | `editor/FileDialogs.cpp:232-275` `runForSystemDialog`; menu path `EditorApp.cpp:1787` to `:2168` | `FileDialogsTests` "a dialog of the system is shown from a thread that can, and its window keeps answering it" | Passed in the baseline. No automated test shows a real dialog over a real window |
| Drag from Asset Browser to viewport / hierarchy (reported 19 Oct) | **Verified fixed** | `editor/AssetDrop.cpp`, `ViewportPanel.cpp:640-665`, `HierarchyPanel.cpp:35-42` | `EditorViewportTests` "what can be placed lands where it is dropped onto a viewport..."; `EditorAppTests` "what can be placed is dragged from the Assets panel onto a row of the hierarchy..."; `AssetDropTests` | Pointer drags are tested with the built-in cube and material only. Textures, sounds, scripts, scenes and sequences are refused on purpose |
| Viewports (up to four, editor or game camera, debug views) | Verified | `EditorApp.cpp:751-1180` | `EditorViewportTests` | |
| Hierarchy (tree, reparent, Ctrl multi-select, context menu) | Verified | `HierarchyPanel.cpp` | `EditorAppTests` | No Shift range select or search |
| Inspector (reflection-driven, one undo step per edit) | Verified | `InspectorPanel.cpp` | `PropertyEditorTests` | |
| Inspector search, multi-edit, reset to default | **Planned** | `InspectorPanel.cpp:63-68` | - | Multi-select shows a count only |
| Asset browser (tree, grid/list, filters, previews, sound preview, uses / used by) | Verified | `AssetBrowserPanel.*` | `EditorAppTests` | |
| Asset import, rename, move | Verified | `AssetImport.cpp`, `AssetMove.cpp` | `AssetCommandsTests` | Not undoable; no progress |
| Asset delete, new folder, create material | **Planned** | - | - | |
| Undo / redo | Verified | `UndoStack.h` | `EditorCommandsTests` | 256 entries, linear |
| Gizmo (move, rotate, scale, world or local) | Verified | `Gizmo.h` | `EditorAppTests`, `GizmoTests` | |
| Gizmo snapping | **Planned** | - | - | |
| Play in editor (start, pause, step, stop, exact restore) | Verified | `PlayMode.h`, `EditorContext.h:150-162` | `EditorAppTests` | |
| New / open project | Partial | `editor/Main.cpp:228-283` | `new_project_smoke`, `EditorAppTests` | Blocking, recreates the app; the menu offers Open project with unsaved changes and the command then refuses it with a console message only |
| Recent projects | **Planned** | - | - | |
| Project templates | Partial | `NewProject.h:33` | `NewProjectTests` | One starter scene |
| Long operations (import, refresh, export, move directory) | Partial | inline between frames | - | No progress or cancellation; the UI stalls |
| Export game | Verified | `GameExport.cpp` | `export_smoke`, `EditorAppTests` | Synchronous |
| Statistics, console, themes, file watching, file association | Verified | `StatisticsPanel.h`, `ConsolePanel.h`, `FileAssociation.cpp` | `ConsolePanelTests`, `EditorAppTests` | |
| UI designer, UI debugger | **Planned** | - | - | Depends on the runtime UI |
| Integrated profiler beyond the statistics panel | **Planned** | - | - | |

## Automation

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| Command registry, 71 commands, parameter validation, JSON results | Verified | `editor/CommandRegistry.h:65-88`, `.cpp:58-87` | `CommandRegistryTests`; `ReferenceDocsTests` keeps `docs/reference/editor-commands.md` in step | |
| Self-describing schema (`editor.commands`) | Verified | `CommandRegistry.cpp:166` | `EditorCommandsTests` | Shallow types: no enums, ranges, nested schemas, defaults or result schema |
| Headless script runner (`--exec`) | Verified | `EditorApp.cpp:322-356`, `editor/Main.cpp:86-110` | `editor_smoke` (about 95 steps) | |
| One path for every route | Partial | Edit menu, hierarchy, inspector and gizmo call `EditorActions` directly | - | They share actions and undo, not the registry |
| Command logic without an editor app | Partial | `registerEditorCommands` needs only World, components, assets | unit `EditorFixture` | App-level commands need `EditorApp` with an engine and a GPU |
| Read-only flags, permissions, caller identity, audit log | **Planned** | - | - | Needed for MCP |
| Path confinement for every command | **Planned** | `scene.save`, `scene.open`, `editor.screenshot`, `asset.preview`, `asset.import` sources and `project.*` take any path | - | Fine for a local user; an MCP adapter would need an allow-list |

## Runtime, export and player

| Feature | Status | Source | Tests | Notes |
|---------|--------|--------|-------|-------|
| `Engine` frame loop | Verified | `engine/Engine.cpp:248-484` | `EngineTests` | |
| `Game` and `cordite-player` | Verified | `engine/Game.cpp`, `player/Main.cpp` | `GameTests`, `player_starts` | |
| Player settings | Verified | `engine/GameSettings.h` | `GameSettingsTests` (8) | |
| Export (Windows directory) | Verified | `editor/GameExport.cpp` | `export_smoke` | |
| Linux export and run | Partial | same files | Linux CI builds and runs the tests on lavapipe since 12 November (it did not build before) | No hardware run recorded |
| macOS application bundle | Unknown | `GameExport.cpp` bundle layout | Layout tested on every OS | Never run on a Mac |
| No console window on Windows | Verified | | `programs_open_no_console` | |
