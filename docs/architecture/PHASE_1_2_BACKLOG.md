# Phase 1 and Phase 2 backlog

Proposed from the [Phase 0 audit](CORDITE_AUDIT.md), 12 November 2026. The phases are
those of the [plan](../GAME_ENGINE_PLAN.md): Phase 1 polishes the editor and content
workflow, and Phase 2 lays the runtime UI foundation. A third track, the OpenGL backend,
runs beside both, by the owner's decision of 12 November.

Source paths are relative to the repository root, at commit `4e03feb`. All work happens
in `E:\GameEngine` ([decision D5](CORDITE_AUDIT.md#owner-decisions)).

Priorities: **P0** blocks the phase exit; **P1** belongs in the phase; **P2** can slip
to a later phase without blocking anything. Every ticket follows the repository's rules
in AGENTS.md: tests for the normal case, edges and failures; a `verify` pass; docs,
guide and reference pages updated in the same commit.

Risk ids (R1 ...) refer to [TECHNICAL_RISKS.md](TECHNICAL_RISKS.md).

## Phase 1 - Editor and content workflow

The audit found **neither October defect still present**. File > Open project no longer
deadlocks (`runForSystemDialog`). Assets are dropped onto the viewport and onto the
hierarchy. Both have regression tests that pass. Phase 1 therefore starts from the
gaps around them rather than from a fix.

Suggested order: P1-01 and P1-02 (cheap, close out the reported area), P1-11 (docs),
then P1-03 and P1-04 (the largest usability gap), then the rest.

### P1-01 Close the gaps around the two reported defects - P0

- **Modules / files:** `editor`. `tests/render/EditorAppTests.cpp`,
  `tests/render/EditorViewportTests.cpp`, `tests/unit/editor/FileDialogsTests.cpp`.
- **Depends on:** nothing.
- **Work:**
  - Extend the pointer-drag tests beyond the built-in cube and default material, to a
    model and a prefab from the showcase, onto the viewport and onto a hierarchy row.
  - Add a test in which the File > Open project dialog is answered with a path (today
    it is answered with a cancel), and the editor then opens that project.
  - Do a manual check on Windows, recorded in the PR: File > Open project in a windowed
    editor with sound enabled, then drag a model from the grid view onto the viewport.
- **Acceptance:** the new tests fail when `acceptDrop` / `acceptPlaceableDrop` are
  disabled, and when `askForPath` ignores the answer. The manual check passes.
- **Tests:** EditorAppTests and EditorViewportTests additions. Run each new GPU test 40
  times, as AGENTS.md asks for anything timing-related.

### P1-02 Unsaved changes when leaving a scene or project - P0

- **Modules / files:** `editor`. `EditorApp.cpp:1194-1202` (`refuseToLeave`) and
  `:1786-1787` (the menu items).
- **Depends on:** nothing.
- **Problem:** File > Open project and New project are offered while the scene has
  unsaved changes. The user picks a project in the dialog, and is then refused with a
  console message only.
- **Work:** ask Save / Don't save / Cancel before the path dialog when the scene has
  unsaved changes. Use a modal the editor draws, so it can be tested headless. The
  `project.open` command keeps refusing, because a script cannot answer a dialog. Add an
  explicit `discardChanges` parameter for scripts that mean it.
- **Acceptance:** no path dialog appears before the question. Save writes the scene and
  continues. Cancel leaves everything as it was. The command refuses without
  `discardChanges` and proceeds with it.
- **Tests:** EditorAppTests for each answer, through `pressShortcut` and fake dialogs.
  EditorCommandsTests for the new parameter. The reference page
  `docs/reference/editor-commands.md` regenerated.

### P1-03 Project open and new: measured, with visible progress - P1

- **Modules / files:** `editor/Main.cpp:228-283`, `EditorApp::create`,
  `engine/Engine.cpp` initialisation, `tests/perf`.
- **Depends on:** nothing.
- **Work:**
  1. Add a perf scenario "open the showcase project" that reports where the time goes:
     window, device, shader cache, asset index, scene load.
  2. Show a progress window while a project opens: a stage name and a bar, drawn by
     ImGui between the stages.
  3. When opening fails, show the reason in the next editor rather than only in the
     log.
- **Acceptance:** the scenario is in `budgets.json` with a time budget. A project that
  fails to open shows its error. The window never reports "not responding" while the
  showcase opens.
- **Measured 12 November:** the showcase took 3.5 s to open, of which 2.9 s were
  `SceneRenderer::create` compiling all 54 shaders: the editor gave the engine no cache
  directory, so the cache was in memory and nothing was kept between sessions. The editor
  now keeps it in the project's `.cordite/shaders`, and the showcase opens in 0.5 s; a test
  holds a second opening to no compilation. Items 1 and 2 matter less now: a progress
  window is for a first opening, or a project much larger than the showcase. Item 3 is
  still to do.
- **Tests:** a perf scenario; EditorAppTests for an unreadable project file and for a
  missing start scene.

### P1-04 Progress and cancellation for long editor operations - P1

- **Modules / files:** `editor` (`AssetImport.cpp`, `AssetMove.cpp`, `GameExport.cpp`,
  the `asset.refresh` handler), `core/JobSystem.h` (first real user).
- **Depends on:** P1-03, for the progress UI pattern.
- **Work:** run the CPU part of import, move directory and export as a job. Show progress
  in the status bar with a Cancel button. Apply the results to the world and the asset
  database on the main thread. Report the outcome of commands that ran in the
  background to `--exec` scripts as well. Commands stay synchronous for scripts unless
  they ask otherwise.
- **Acceptance:** importing a large glTF keeps the editor responsive. Cancel leaves no
  half-written files: export already refuses directories it did not make, and import
  must write to a temporary name and rename. Script results are unchanged.
- **Tests:** unit tests for cancellation at each stage and for the clean-up. An
  EditorAppTests case that frames keep coming while an import runs.

### P1-05 Recent projects - P1

- **Modules / files:** `editor` (the per-user settings that already keep the theme),
  File menu.
- **Depends on:** P1-02.
- **Acceptance:** File > Open recent lists the last 10 projects, newest first, without
  duplicates. A missing project is shown greyed out and can be removed. The list
  survives a restart.
- **Tests:** a unit test for the list (order, cap, duplicates, missing paths, a corrupt
  settings file). An EditorAppTests case for the menu.

### P1-06 Asset browser: delete, new folder, new material - P1

- **Modules / files:** `editor` (`AssetBrowserPanel.cpp`, `AssetCommands.cpp`),
  `assets` (`AssetDatabase`).
- **Depends on:** nothing.
- **Work:** add the commands `asset.delete` (refuses when the asset is used, unless
  forced, and lists its users), `asset.createDirectory` and `asset.createMaterial`, plus
  their context-menu entries. All paths go through `relativePathWithin`.
- **Acceptance:** a deleted asset and its `.meta` file are gone, and the `Used by` lists
  are updated. Paths outside the asset directory are refused.
- **Tests:** AssetCommandsTests for each command and each refusal. The reference page
  regenerated.

### P1-07 Report device loss and keep the user's work - P1

- **Modules / files:** `rhi/Swapchain.cpp:317-319,376-377`, `engine/Engine.cpp:352`,
  `editor/Main.cpp`.
- **Depends on:** nothing. Covers R8.
- **Acceptance:** `VK_ERROR_DEVICE_LOST` (and DXGI device removal) map to a distinct
  `ErrorCode`. The editor writes the unsaved scene to a recovery file and says where.
  The next start offers to open it.
- **Tests:** unit tests through an injected failure in the frame loop, without a real
  device loss, and for the recovery file on the next start.

### P1-08 Run the GPU suite on Direct3D 12 in CTest, with known failures listed - P1

- **Modules / files:** `tests/CMakeLists.txt`, `cmake/CorditeModule.cmake`
  (`cordite_configure_gpu_test`), `tests/render/RenderTestSupport.cpp`.
- **Depends on:** nothing. Covers R5.
- **Work:** register `render_d3d12`, which runs `cordite_render_tests` with
  `CORDITE_GRAPHICS=d3d12`, on Windows only. Exclude the known failures by name from a
  checked-in list, in place of the prose in AGENTS.md. The [baseline](BASELINE.md)
  records what this audit measured.
- **Acceptance:** the entry passes on a Windows GPU. Adding a new failure fails it.
  Fixing a listed test is noticed, because the list is also checked the other way.
  The mechanism takes a backend name, so GL-07 reuses it for OpenGL. It exists since
  P1-13 for lavapipe (`CORDITE_EXPECTED_GPU_FAILURES`, `tests/render/expected-failures/`):
  this ticket adds a list for Direct3D 12 and a CTest entry that draws with it.
- **Tests:** the entry itself.

### P1-09 Inspector: reset to default and search - P2

- **Modules / files:** `editor/InspectorPanel.cpp`, `core/Reflection.h` (defaults).
- **Depends on:** nothing.
- **Acceptance:** each property has Reset, one undo step, which sets the value a new
  component has. A search field filters properties by name across components.
- **Tests:** PropertyEditorTests and EditorAppTests.

### P1-10 Gizmo snapping - P2

- **Modules / files:** `editor/Gizmo.*`, `ViewportPanel.cpp`, the `viewport.gizmo`
  command.
- **Acceptance:** move, rotate and scale snap to configurable steps while Ctrl is held,
  or always when toggled. The settings persist.
- **Tests:** GizmoTests with exact expected values.

### P1-11 Small documentation fixes - P0, docs only

See [CORDITE_AUDIT.md, smallest doc updates](CORDITE_AUDIT.md#smallest-doc-updates).
Includes four stale header comments in the editor.

### P1-12 Prefab instances that keep their link - P0

Decided 12 November: prefabs follow the plan. Phase 1 delivers the prefab workflow of
plan section 9, item 4: nested instances, overrides, apply and revert, and showing what
is overridden. Variants come in Phase 4, where the plan lists them.

- **Modules / files:** `world/Prefab.*`, `world/Components.h` (`PrefabInstanceComponent`),
  `world/SceneSerializer.cpp`, `core/Document.h` (migrations),
  `editor/InspectorPanel.cpp`, `editor/HierarchyPanel.cpp`, `editor/EditorActions.*`,
  the `prefab.*` commands, `docs/architecture.md` ("A prefab is copied where it is
  placed").
- **Depends on:** a `Document` migration mechanism (R13), delivered in slice 1, because
  the scene format changes.
- **Work, in slices, each built and tested on its own:**
  1. **ADR and migration.** Replace "a prefab is copied where it is placed" with
     linked instances. Overrides are stored as property patches (the existing
     `PropertyPatch` fits), and added or removed children are recorded. Add
     `Document` migrations, and read version-1 scenes as unlinked copies.
  2. **Link and revert.** An instance remembers its prefab and is rebuilt from it when
     the scene loads or the prefab changes. Overrides survive. Revert, per property and
     for the whole instance, removes overrides.
  3. **Apply.** Push an instance's overrides into the prefab file, one undo step in the
     scene. The prefab file is written, as `prefab.create` already does.
  4. **Nesting.** Prefabs inside prefabs keep their own links. Overrides are addressed
     through the nesting path.
  5. **Display.** The inspector marks overridden properties and offers revert and apply
     on them. The hierarchy marks instances and their nested prefabs.
- **Acceptance:**
  - Change a prefab, and every instance in an open or reopened scene shows the change,
    except overridden properties.
  - Apply and revert are exact and undoable.
  - Version-1 scenes still open.
  - A prefab that contains itself, directly or through nesting, is refused with an
    error naming the chain.
- **Tests:**
  - world PrefabTests and SceneSerializerTests for each slice, with exact file contents
    pinned.
  - A migration test from a version-1 scene.
  - Cycle and missing-prefab failures.
  - Editor commands tests for apply and revert.
  - EditorAppTests for the inspector markings.
  - The showcase gains a nested prefab with an override, kept by `samples` tests.

This is the largest Phase 1 item.

### P1-13 The GPU test cases that fail on lavapipe - P1

Added 12 November, after Linux CI built for the first time since the repository's first
commit and reached its GPU suites.

- **Modules / files:** `tests/render/expected-failures/lavapipe.txt`, and whichever of the
  renderer, the editor's pictures or the tests each case turns out to be about.
- **Depends on:** nothing.
- **Problem:** 17 of the 479 cases of `render` fail on lavapipe (Mesa 25, LLVM 20), which
  CI draws with: eleven reference pictures (0.4 to 26 percent of their pixels, by up to
  229 of 255 - the debug lines most), two of anti-aliasing (a higher quality follows an
  edge less far; a picture turned round is not smoothed as the upright one is), three of
  decals (colors and normals off by more than 2.5 of 255) and one of motion blur (34 of
  255 from the filter's own). They are listed, so that CI is green and says what else
  breaks; each listed case runs alone and must still fail.
- **Work:** for each, find whether the engine or the driver is wrong: run it on
  lavapipe with the validation layers, compare its picture with the one the GPU draws,
  and read what differs. Fix the engine where it is wrong. Where lavapipe is, say why in
  the list, or give the test a tolerance it can justify - never a wider one to pass.
- **Acceptance:** every case left on the list has a reason beside it that names what
  lavapipe does differently.
- **Tests:** the cases themselves; `render_expected_failure_<n>` fails for a case that
  starts passing.

## Phase 2 - Runtime UI foundation

Phase 2 exits when "a C++ project can create and display runtime widgets without ImGui
as the UI runtime". The tickets below are the shortest path to that. The UMG-style
designer, layers and activatable stacks, and gamepad navigation are Phase 3. The Phase 2
model must not make them hard, so P2-00 designs for them.

Order: P2-00, then P2-01, P2-02 and P2-03 in parallel, then P2-04 to P2-10, then P2-11.

### P2-00 ADR: runtime UI architecture and module placement - P0

- **Modules / files:** `docs/architecture.md` (Design decisions), the module table in
  AGENTS.md.
- **Decide:**
  - **Module.** Proposal: a new `gui` module (named so it is not confused with the ImGui
    module `ui`), depending on core, math, platform, assets, rhi, shaders and renderer.
    `engine` depends on it. `gui` must not depend on `ui`, and `ui` must not depend on
    `gui`.
  - **Retained tree.** Ownership by `std::unique_ptr` children, explicit lifecycle, and
    dirty flags for layout and paint.
  - **Layout model.** Slots as in UMG: canvas with anchors and offsets, horizontal and
    vertical boxes, overlay, size box, padding and alignment; a DPI scale; safe zones.
  - **Input routing** together with P2-03.
  - **Rendering.** The shared batch renderer from P2-01.
  - **Serialisation** through reflection.
  - **Explicit non-goals for Phase 2:** data binding, animation, world-space widgets, the
    designer.
- **Acceptance:** the ADR is reviewed by the owner. The layering rule is added to
  AGENTS.md. No empty module is created before P2-05 has a consumer (plan section 17,
  rule 12).

### P2-01 Extract an ImGui-free 2D batch renderer - P0

- **Modules / files:** `ui/UiRenderer.cpp` (`pipelineFor`, `bindingSetFor`,
  `ensureCapacity`, scissor at `:188-262,327-343`), new `renderer/CanvasRenderer.*`
  (proposed), `shaders/ui/`, `shaders/EngineShaders.cpp:104-105`.
- **Depends on:** P2-00.
- **Work:**
  - Move the batching into a neutral renderer: its own vertex type, 32-bit indices, a
    command list of `{texture, clip rect, index range}`, and premultiplied alpha.
  - Make `UiRenderer` a thin `ImDrawData` adapter over it.
  - Add a runtime shader entry point and register every permutation for exported games.
- **Acceptance:** the reference images `ui-widgets-*` and `editor-*` are unchanged,
  which proves the extraction. The engine shader request list contains the new entries.
  `export_smoke` passes.
- **Tests:** existing UiTests. A new render test that draws rects, textured rects and
  clipped rects and checks pixels numerically. EngineShadersTests.

### P2-02 Fonts and text: bitmap atlases - P0

- **Modules / files:** `assets` (a new font asset type), `gui`, and the stb tree already
  pinned in `cmake/CorditeDependencies.cmake`. There is no new dependency.
- **Depends on:** P2-00, and P2-01 for drawing.
- **Decided 12 November: bitmap fonts for now.** Use stb_truetype atlases: Latin text,
  kerning, a few pixel sizes per font, regenerated when the DPI scale changes. Put them
  behind a `FontAtlas` interface, so shaping or MSDF can replace them later without
  changing widgets. Record this in a short ADR.
- **Acceptance:** a font asset (`.ttf` plus `.meta`) loads by id, and text lays out with
  kerning, wrapping, alignment and ellipsis. Pixel sizes follow the DPI scale. The
  embedded Inter can be used as a default.
- **Tests:** unit tests for layout (exact advances for a known font, wrapping at the
  boundaries, empty text, invalid UTF-8, missing glyphs). A render test of text.

### P2-03 Input contexts and consumption - P0

- **Modules / files:** `platform/InputMap.h:176-265`, `platform/Window.cpp:176`
  (modifiers), `engine/Engine.cpp:256-270`.
- **Depends on:** P2-00. Covers R1.
- **Work:**
  - Put a context stack with priorities in front of `InputActions`.
  - A UI layer can consume pointer, keyboard and gamepad input before actions see it.
  - Keep modifiers in bindings (Ctrl+S style chords).
  - Keep the existing input map format readable: version 1 files still load.
- **Acceptance:** while a consuming context is on top, gameplay actions bound to the
  same keys neither fire nor read as held, and they resume cleanly. A key held across
  the change must not fire a spurious "pressed".
- **Tests:** InputMapTests for priority, consumption, a key held across a push or pop,
  and modifiers. EngineTests through `input.inject`.

### P2-04 DPI and framebuffer-size events - P1

- **Modules / files:** `platform/Window.cpp` (callbacks), `platform/Window.h`,
  `ui/UiSystem.cpp:68`.
- **Depends on:** nothing. Covers R4.
- **Acceptance:** the window reports content-scale and framebuffer-size changes. The
  ImGui interface rescales when they change.
- **Tests:** WindowTests through the null platform, where GLFW allows it, or an
  injected event. A UiTests case for the rescale.

### P2-05 Widget tree, lifecycle and layout - P0

- **Modules / files:** new `gui` module (`Widget`, `Panel` types, `LayoutContext`),
  `tests/unit/gui/`.
- **Depends on:** P2-00.
- **Acceptance:**
  - Widgets construct, attach, detach and destroy with defined callbacks.
  - Layout is two-pass (desired size, then arrange). Canvas anchors, horizontal and
    vertical boxes, overlay, size box, padding and alignment give exact rectangles at
    1x and 2x scale.
  - Invalidation re-lays out only dirty subtrees.
  - No widget outlives its tree. A tree of depth 10,000 does not overflow the stack,
    as `WorldTests` require of the hierarchy.
- **Tests:** pure CPU unit tests asserting rectangles exactly, plus invalidation counts.

### P2-06 Widget assets: serialisation and instantiation - P1

- **Modules / files:** `gui`, `assets` (new asset type), `core/Reflection.h` (lists of
  structs, see R12), `core/Document.h`.
- **Depends on:** P2-05.
- **Acceptance:** a widget tree is written as a strict, versioned JSON document and read
  back identically. Invalid files are refused without partial trees. Instantiation by
  asset id works in the player as well, with no compiler.
- **Tests:** round-trip, truncated and corrupt input, unknown widget types, and exact
  file contents pinned.

### P2-07 Basic widgets - P1

- **Modules / files:** `gui`.
- **Depends on:** P2-01, P2-02, P2-05.
- **Widgets:** Image, Border (solid or nine-slice), Text, Button (normal, hovered,
  pressed, disabled, focused), ProgressBar. Sliders, checkboxes and scroll lists are
  Phase 3, in the MVP sample.
- **Acceptance:** each draws through the batch renderer with clipping. Button states
  follow pointer input.
- **Tests:** unit tests of state machines. A render test per widget with numeric pixel
  checks, plus one reference image of all widgets, reviewed by eye.

### P2-08 Hit testing, pointer input and focus - P1

- **Modules / files:** `gui`, P2-03's contexts.
- **Depends on:** P2-03, P2-05.
- **Acceptance:** the topmost hit widget receives the pointer, and clipped-out areas do
  not. There is a focus path with an explicit default focus, Tab and Shift+Tab traversal,
  and focus restoration when a focused widget is removed. Consumed input never reaches
  gameplay actions.
- **Tests:** unit tests, headless through injected input (see P2-10).

### P2-09 Engine integration; ImGui optional in games - P0

- **Modules / files:** `engine/Engine.cpp` (ownership near `:92-99,198`; update in
  `updateWorld` after scripts; draw after `drawWorld`'s blit `:413` and before ImGui
  `:324`), `engine/Engine.h`, `engine/Game.cpp`, `editor/ViewportPanel.cpp:688-691`
  (play in editor draws into its own target).
- **Depends on:** P2-01, P2-05.
- **Work:** the engine owns the runtime UI, ticks it, routes input to it and draws it
  into any target it is given. Add `EngineDesc::debugInterface`: games default to no
  ImGui context. Scene changes clear scene-owned layers.
- **Acceptance:** the player shows runtime widgets with no ImGui context alive. Play in
  editor shows them in the viewport. `export_smoke` still passes.
- **Tests:** EngineTests and GameTests: widgets drawn in a game, no ImGui context
  present, and correct order over the scene.

### P2-10 Headless UI test harness - P0

- **Modules / files:** `tests/render/` (a new support header), `editor` (`input.inject`
  already exists), `engine`.
- **Depends on:** P2-08.
- **Acceptance:** tests inject pointer, keyboard, text and gamepad input and read the
  focus path, the hovered widget and the laid-out rectangles as numbers. Any
  reference-image test also asserts numerically (R14).
- **Tests:** the harness's own tests.

### P2-11 Phase 2 exit sample - P0

- **Modules / files:** `samples/` (a minimal C++ project, or a scene in the showcase plus
  a C++ test driver), `tests/unit/samples`.
- **Depends on:** all of the above.
- **Acceptance:** a C++ program builds a small menu (title, three buttons, a progress
  bar) from a widget asset, operates it by mouse and keyboard, and blocks a gameplay
  action while it is open. Exported, it runs in `cordite-player` with no ImGui context.
- **Tests:** sample tests, `export_smoke`, and a reference image reviewed by eye.

## OpenGL track (decided 12 November)

**Goal:** on Linux without Vulkan, the editor and games run on OpenGL 4.5. The
approach is a Cordite backend that implements NVRHI's interfaces; the reasoning is in
[CORDITE_AUDIT.md](CORDITE_AUDIT.md#opengl-fallback-decided).

This track touches `rhi`, `platform`, `shaders` and `shader_compiler` only, so it can
run beside Phases 1 and 2. GL-02 goes first because it can sink the approach: if the
engine's shaders cannot be brought to GLSL with a deterministic binding map, the plan
changes before any backend code exists.

Order: GL-01 and GL-02, then GL-03, then GL-04 and GL-05 together, then GL-06, GL-07
and GL-08.

### GL-01 ADR: an OpenGL backend behind NVRHI - P0

- **Modules / files:** the design decisions in `docs/architecture.md` ("Direct3D 12
  beside Vulkan" is revised), the `rhi` row of AGENTS.md, `AIGameEngine.md` (the
  OpenGL note).
- **Decide and record:**
  - OpenGL 4.5 core as the minimum, with the required extensions listed.
  - The features the backend does not offer, each reported through
    `GraphicsDevice`'s capabilities so that nothing fakes support (plan section 7.2).
    Candidates are timeline semaphores and multiple queues.
  - The GL loader. AGENTS.md allows no Python in the build, so a generator that needs
    it at build time is out. Options: pre-generated loader sources pinned as an
    archive, or a small hand-written loader over `glfwGetProcAddress`.
  - The selection order: `Auto` tries Vulkan, then OpenGL on Linux; Windows keeps
    Vulkan then D3D12.
  - How headless tests get a GL context: a hidden GLFW window, or EGL surfaceless on
    Linux.
- **Acceptance:** reviewed by the owner. The docs that contradict it are updated in the
  same change.

### GL-02 Spike: the engine's shaders as GLSL 4.50 - P0

- **Modules / files:** `shader_compiler/SlangShaderCompiler.cpp:174-189`,
  `shaders/ShaderTypes.h` (`ShaderTarget`), `shaders/EngineShaders.cpp`
  (`engineShaderRequests()`), all of `shaders/`.
- **Depends on:** nothing.
- **Work:**
  - Compile every engine permutation with Slang's GLSL target.
  - Define how HLSL registers (b, t, u and s, with NVRHI's offsets) map to GL binding
    points for uniform buffers, textures, images and storage buffers.
  - Settle how separate textures and samplers become GL's combined samplers.
  - Compile the results with a real GL 4.5 driver (on the Windows machine), in a
    throwaway harness.
- **Acceptance:** a written report listing:
  - permutations that compile and link;
  - any that need shader changes, and what those changes are;
  - the binding map;
  - a go or no-go for option A.
- **Tests:** the report is the deliverable. Production tests come with GL-03 to GL-05.

### GL-03 A GL device, a GL window, clear and read back - P0

- **Modules / files:**
  - `rhi/GraphicsDevice.cpp:28-41` (backend choice)
  - `rhi/DeviceBackend.h`
  - a new `rhi/opengl/`
  - `platform/Window.cpp:375` (the GL context instead of `GLFW_NO_API`)
  - `rhi/CMakeLists.txt`
- **Depends on:** GL-01, GL-02.
- **Acceptance:** `GraphicsDeviceDesc::backend = OpenGL` makes a device that describes
  its adapter and GL version. It clears a texture and reads it back exactly. Errors
  from `KHR_debug` are recorded as validation issues, as Vulkan's are. A machine without
  GL 4.5 is refused with `Unsupported` and a message naming the missing version or
  extension.
- **Tests:** GraphicsDeviceTests cases mirroring the D3D12 ones, including the refusal.

### GL-04 Resources: buffers, textures, samplers, framebuffers, readback - P0

- **Modules / files:** `rhi/opengl/`.
- **Depends on:** GL-03.
- **Acceptance:** every texture kind the renderer creates works: 2D, arrays, cubes,
  cube arrays, 3D, mips, and the formats `SceneTargets` and `ShadowAtlas` use. So do
  volatile constant buffers, structured buffers, and staging and readback.
- **Tests:** GraphicsDeviceTests and readback tests run with `CORDITE_GRAPHICS=opengl`,
  including "a mip level reads back at its own size".

### GL-05 Pipelines, binding sets and command lists - P0

- **Modules / files:** `rhi/opengl/`.
- **Depends on:** GL-02, GL-04.
- **Acceptance:**
  - Graphics and compute pipelines, binding layouts and sets (following the GL-02
    map), draws, indexed and instanced draws, dispatches, copies, clears, blits.
  - Timer queries and event queries through GL sync objects, so that `GpuTimer`
    works.
  - Depth with `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`.
  - NVRHI's validation layer wraps the device without complaint.
- **Tests:** SceneRendererTests (the numeric ones) and UiTests run on GL.

### GL-06 Present and choose the backend - P0

- **Modules / files:**
  - `rhi/Swapchain.cpp:257-259` (refuses non-Vulkan today)
  - `engine/Engine.cpp`
  - `engine/Engine.h` (`EngineDesc::graphicsBackend`)
  - `player/Main.cpp`, `editor/Main.cpp` (a `--graphics` option)
- **Depends on:** GL-05.
- **Acceptance:**
  - A GL window presents with vsync on and off, and survives resizing.
  - On Linux, `Auto` falls back to GL when no Vulkan 1.3 device can be made, and
    logs why.
  - The editor and `cordite-player` run the showcase on GL.
- **Tests:** SwapchainTests and EngineTests on GL. A test of the fallback through a
  backend that is made to fail.

### GL-07 The GPU suite on OpenGL, in CTest and CI - P1

- **Modules / files:** `tests/CMakeLists.txt`, the expected-failure mechanism of P1-08,
  `.github/workflows/ci.yml` (the Linux job already runs Mesa under xvfb).
- **Depends on:** GL-06, P1-08.
- **Acceptance:**
  - `render_opengl` passes, with its list of known failures, on the Windows machine.
  - It also passes on Mesa llvmpipe in Linux CI.
  - The perf suite reports GL numbers for comparison.
- **Tests:** the entries themselves.

### GL-08 Export: games that ship GLSL - P1

- **Modules / files:** `shader_compiler/EngineShaderCache.cpp:19-27`
  (`gameShaderTargets()`), `editor/GameExport.cpp`, the user guide (`docs/guide/export.md`).
- **Depends on:** GL-06.
- **Acceptance:**
  - A game exported on Linux carries GLSL beside SPIR-V and runs on either.
  - On Windows, GLSL is shipped only if GL-01 decides Windows games may fall back to
    GL.
  - Optionally, the driver's program binaries are cached in the player's directory to
    shorten later starts.
- **Tests:** EngineShadersTests, and `export_smoke` run with the backend forced to GL.

### Deferred to Phase 3 on purpose

Gamepad navigation, layer stacks and activatable widgets, modal priority rules, action
hints, transitions and animation, data binding, styles and themes as assets, world-space
widgets, the designer and the UI debugger. Also a script API for UI, which needs a plan
decision: plan section 8.4 wants one language-neutral API (R11).
