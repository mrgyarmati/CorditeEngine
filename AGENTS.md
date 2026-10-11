# Cordite - guide for agents and contributors

Cordite is a 3D game engine for Windows, macOS and Linux (Ubuntu 24.04+), written in
C++20 and rendered with Vulkan through NVRHI. Its default look is the gritty,
high-contrast, normal-mapped style of late-2000s console games. The product vision and
feature list are in [AIGameEngine.md](AIGameEngine.md); the generic coding standard is
[coding-standards.md](coding-standards.md). This file is the concrete, project-specific
version of both. When they disagree, this file wins.

Read this file before changing anything. Task-specific walkthroughs live in
[.claude/skills](.claude/skills) and are referenced from the relevant sections below.

## Hard rules

1. **Stay inside the repository.** Do not read, copy from or reference other code on
   the machine. Third-party code comes only from the pinned archives in
   `cmake/CorditeDependencies.cmake`.
2. **No hacks.** If the proper fix is larger than the task, do the proper fix or stop
   and say so. Never leave a stub that silently does nothing.
3. **Every behavior change ships with tests** that cover the normal case, the edge
   cases and the failure paths.
4. **Review before every commit** using the checklist in
   [Committing](#committing). A commit that was not built, tested, formatted and
   reviewed is not allowed.
5. **Do not reinvent the wheel.** Prefer a maintained library over new code, and
   record why when you decide otherwise (see [Design decisions](docs/architecture.md#design-decisions)).

## Commands

### Windows

```powershell
./scripts/bootstrap.ps1                  # once: CMake + Ninja into .tools/ (git-ignored)
./scripts/bootstrap.ps1 -WithVulkanSdk   # also the Vulkan SDK, for validation layers

./scripts/dev.ps1 build                  # configure if needed, build RelWithDebInfo
./scripts/dev.ps1 build -Config Debug -Target cordite_core_tests
./scripts/dev.ps1 test                   # all tests
./scripts/dev.ps1 test -Filter math      # CTest name regex
./scripts/dev.ps1 test -ExcludeLabel gpu # skip tests that need a Vulkan device
./scripts/dev.ps1 format                 # clang-format in place
./scripts/dev.ps1 format-check           # what CI runs
./scripts/dev.ps1 build -Preset windows-clang   # second compiler (clang-cl)
./scripts/dev.ps1 tidy                   # clang-tidy over the engine (needs the windows-clang preset configured)
./scripts/dev.ps1 tidy -Changed          # only sources affected by uncommitted changes
./scripts/dev.ps1 tidy -Filter math      # only files whose path matches the regex
./scripts/dev.ps1 verify                 # the whole pre-commit gate, one PASS/FAIL line per step
./scripts/dev.ps1 verify -Filter quick   # format-check plus the default build and tests
./scripts/dev.ps1 perf                   # the performance tests alone, compared with the run before

./scripts/dev.ps1 run -Target cordite-editor       # open the editor without a project
./scripts/dev.ps1 run -Target cordite-editor -- --project samples/showcase
./scripts/dev.ps1 run -Target cordite-editor -- --headless --validation --project samples/showcase --exec tests/data/editor-smoke.json --frames 3 --screenshot out.png
./scripts/dev.ps1 run -Target cordite-sample-assets -- samples/showcase/assets   # regenerate the showcase's textures and models
```

`dev.ps1` loads the MSVC environment itself, so it works from any shell. It requires
Visual Studio 2022 with the "Desktop development with C++" workload.

Run one test case directly from the suite executable:

```powershell
build/windows/bin/RelWithDebInfo/cordite_core_tests.exe -tc="*FixedTimestep*"
build/windows/bin/RelWithDebInfo/cordite_core_tests.exe -ts="Config"      # a whole suite
```

### Linux and macOS

```bash
# Ubuntu 24.04: sudo apt install build-essential cmake ninja-build xorg-dev libwayland-dev libxkbcommon-dev
#               (to run: a Vulkan 1.3 driver, e.g. mesa-vulkan-drivers; optional: vulkan-validationlayers)
# macOS:        brew install cmake ninja   (to run: the Vulkan SDK, which provides MoltenVK)
cmake --preset linux            # or: macos
cmake --build --preset linux-dev
ctest --preset linux-dev        # add -LE gpu on a machine without a Vulkan device
```

### Build configurations

| Configuration    | Use                         | Assertions | Optimized |
|------------------|-----------------------------|------------|-----------|
| `Debug`          | Debugging                   | on         | no        |
| `RelWithDebInfo` | Day-to-day development (default) | on    | yes       |
| `Release`        | Shipping / exported games   | off        | yes       |

Code must build warning-free and pass every test in all three. `Release` matters:
`CORDITE_ASSERT` compiles away there, so a bug hidden behind an assertion shows up only
in that configuration.

## Repository layout

```
cmake/                 Build helpers: dependencies, compiler options, module/test macros
engine/cordite/<mod>/  Engine modules (static libraries), one directory each
editor/                The cordite-editor executable (a thin main around the editor module)
tools/                 Development tools, such as the generator of the showcase's textures and models
shaders/               Engine shader sources (Slang): common/ is shared code, materials/ what a surface is,
                       passes/ one file per pass, ui/ the interface
samples/showcase/      The project that uses every feature of the engine; tests keep it complete
tests/support/         Test entry point and fixtures
tests/unit/<mod>/      Unit tests, mirroring engine/cordite/<mod>
tests/render/          Tests that need a GPU, and their reference images
tests/perf/            Performance tests and their budgets
tests/data/            Input files for tests, such as editor command scripts
scripts/               Developer tooling (bootstrap, dev, hash-url)
docs/                  Architecture notes and the third-party list
.claude/skills/        Step-by-step guides for recurring tasks
.tools/                Bootstrapped toolchain (git-ignored)
build/<preset>/        Build trees (git-ignored)
```

## Architecture

Full description: [docs/architecture.md](docs/architecture.md).

The engine is a set of static libraries, one per module, named `cordite::<module>`.
Each module declares the modules it may use in its `CMakeLists.txt`
(`cordite_add_module(... DEPENDS ...)`). The `module_layering` test fails if a module
includes a header from outside its transitive dependencies, so layering cannot erode
by accident.

| Module | Depends on | Contains |
|--------|-----------|----------|
| `core` | -         | Logging, assertions, `Result`, hashing, UUIDs, JSON, reflection, binary streams, file I/O, config, job system, allocators, timing |
| `math` | `core`    | glm aliases, `Transform`, bounds, rays, frustum, projections, color, curves, random, reflection of math types |
| `platform` | `core`, `math` | Windows (GLFW), keyboard / mouse / gamepad input |
| `assets` | `core`, `math` | Assets as data: images and mipmaps, meshes and what prepares them, materials, glTF import and export, the asset database (ids, `.meta` files) and the asset manager (loading by id) |
| `world` | `core`, `math` | Entities, components and their hierarchy (EnTT), the component registry, scene files |
| `rhi` | `core`, `math`, `platform` | Vulkan device, swapchain and texture readback over NVRHI |
| `shaders` | `core` | Shader requests and permutation keys, disk cache, `ShaderLibrary` |
| `shader_compiler` | `core`, `shaders` | Slang to SPIR-V compiler (not linked into exported games) |
| `renderer` | `core`, `math`, `assets`, `world`, `rhi`, `shaders` | The scene renderer and what it is made of: GPU copies of assets, the mesh passes, the G-buffer and its lighting, the lights of a world and the clusters they are sorted into, the debug views, scene targets, the `MeshRenderer`, `Light` and `AmbientLight` components; camera views, debug lines, GPU timing, GPU shader management |
| `ui` | `core`, `math`, `platform`, `rhi`, `shaders`, `renderer` | The immediate-mode interface: the Dear ImGui context and its NVRHI renderer |
| `engine` | all of the above | `Engine`: owns the subsystems, the assets, the world and the interface, and runs the frame loop; projects; placing models; checking the assets a scene uses |
| `editor` | `engine` and below | The editor: the scene document, undoable actions, scene and asset commands, panels, `EditorApp` (not linked into exported games) |

`cordite-editor` (in `editor/`) is a thin `main` around `EditorApp`. Everything else,
including what tests need to drive the editor, is in the `editor` module.

Rules that keep the architecture honest:

- A lower module never knows about a higher one. If `core` needs something from the
  renderer, invert the dependency with an interface declared in `core`.
- Third-party types stay out of public headers unless they *are* the interface (glm
  types, `Json`, `Result`). Wrap everything else behind a pimpl, as `JobSystem` does
  with enkiTS, so a dependency can be replaced without touching its users.
- Engine code never calls `exit`, never throws across a module boundary and never
  blocks the main thread on I/O it could do in a job.

## C++ standards

Language: C++20, no compiler extensions. Formatting is whatever `.clang-format`
produces (clang-format 19); do not hand-format against it. `.clang-tidy` is the linter:
it enforces the naming table below and a curated set of bug, performance and
readability checks, with every finding treated as an error. Fix findings in the code.
Suppress one only when the check is wrong for that line, with
`// NOLINT(check-name)` and a comment saying why; disable a check project-wide only in
`.clang-tidy`, next to a reason.

### Naming

| Thing | Style | Example |
|-------|-------|---------|
| Types, enums, concepts | `PascalCase` | `LinearAllocator`, `ErrorCode` |
| Functions, methods, variables, parameters | `camelCase` | `readTextFile`, `frameSeconds` |
| Private and protected data members | `m_camelCase` | `m_accumulator` |
| Public struct fields | `camelCase` | `transform.position` |
| Constants and enumerators | `kPascalCase` / `PascalCase` | `kPi`, `LogLevel::Warn` |
| Macros | `CORDITE_UPPER_SNAKE` | `CORDITE_ASSERT` |
| Namespaces | lowercase | `cordite`, `cordite::test` |
| Files | `PascalCase`, named after the main type | `FileSystem.h`, `FileSystemTests.cpp` |

Names are descriptive and unabbreviated (`index`, not `idx`; `directory`, not `dir`).
The name of the string form of an enum is `nameOf(value)`, never `toString`: doctest
finds a `toString` overload by ADL and fails to compile.

### Error handling

- **Expected failures** (I/O, malformed data, missing assets, bad user input, device
  loss) return `Result<T>` or `Status`. Build errors with `makeError(code, "format {}",
  args)`; propagate with `CORDITE_TRY` / `CORDITE_TRY_ASSIGN`. Messages say what failed
  and why, and name the file or key involved.
- **Programmer errors** (broken invariants, misuse of an API) use `CORDITE_ASSERT`.
  After the assertion, still leave the program in a safe state where that is cheap,
  because the assertion is absent in `Release`.
- **Never ignore a failure.** Either handle it, propagate it, or log it with enough
  context to act on. `(void)result;` needs a comment explaining why it is safe.
- **External input is untrusted.** Files, JSON, script arguments and automation
  requests are validated at the boundary: types, ranges, lengths, and paths (use
  `relativePathWithin` to keep paths inside a project). Readers must not crash or
  allocate unbounded memory on corrupt data; see `BinaryReader`.
- Exceptions are used only where a dependency forces them, and are caught at that
  boundary and converted to a `Result`.

### Ownership and lifetime

- Prefer values and `std::unique_ptr`. Use `std::shared_ptr` only for genuinely shared
  ownership. Raw pointers and references never own.
- Types that manage a resource delete or define all special members; use
  `CORDITE_NON_COPYABLE` / `CORDITE_NON_MOVABLE`.
- Return `[[nodiscard]]` from functions whose result must be checked or used.

### Headers and includes

- `#pragma once`. Include what you use; do not rely on transitive includes (each
  standard library ships different ones, and that is the most common way a change
  compiles on MSVC and fails on GCC or Apple Clang).
- Include order is enforced by clang-format: own header, engine headers, third-party,
  standard library.
- Keep heavy third-party headers out of public headers; forward-declare
  (`cordite/core/JsonFwd.h`) or use a pimpl.

### Documentation

Every public type and function has a `///` comment that states what it does, its
units and conventions, and how it fails. Every public header starts with a `/// @file`
block containing at least one `@code` usage example. Comments explain *why*; do not
restate what the code already says.

### Portability

Windows is the only platform that can be built on every development machine, so write
defensively for the other two:

- Paths are UTF-8 in data and `std::filesystem::path` in code. Convert with
  `pathToUtf8` / `pathFromUtf8`; never construct a path from `std::string` directly.
- No `std::from_chars` for floating point (missing in Apple's libc++), no `std::pmr`,
  no `std::format` (use fmt).
- Platform-specific code lives behind `#if CORDITE_PLATFORM_*` in a `.cpp`, never in
  a public header.
- No dependence on `long` being 32 or 64 bits, on `char` signedness, or on
  `wchar_t` size. Use `<cstdint>` types.
- Also build with `-Preset windows-clang` before committing; clang-cl catches most of
  what GCC and Apple Clang would reject.

### Math conventions

Right-handed, +Y up, metres, -Z forward. Column-major matrices, column vectors.
Radians in code, degrees in user-facing data. Direct3D-style clip space with reversed
depth for scene rendering. Colors are linear unless a name says sRGB. Quaternions are
serialized as `[x, y, z, w]`. Details are in `engine/cordite/math/Math.h`.

## Rendering and shaders

NVRHI is the rendering interface; engine code outside the `rhi` module never calls
Vulkan directly. Step-by-step guide for adding a pass:
[add-render-pass](.claude/skills/add-render-pass/SKILL.md).

- **Every GPU code path must be reachable headless.** A feature that only works with a
  window cannot be tested automatically or driven by an agent. Render to an offscreen
  target first; the swapchain only ever receives a copy.
- **Validation must stay silent.** Tests create devices with `validation = true` and end
  with `checkNoValidationIssues`. A validation message is a bug, not noise; fix the
  cause. Never filter a message out to make a test pass.
- **Shaders are Slang**, in `shaders/`. Matrices are column-major (`mul(matrix, vector)`
  matches glm's `matrix * vector`). Resources use HLSL registers with NVRHI's default
  Vulkan offsets: `tN` -> N, `sN` -> 128 + N, `bN` -> 256 + N, `uN` -> 384 + N.
- **A C++ struct that mirrors a shader constant buffer** is named after the `cbuffer`,
  has a `static_assert` on its size, and is laid out so that every packing rule agrees
  on it: fill each 16 bytes exactly with a `Vec4`, a `Vec3` followed by one float, or a
  `Vec2` and two floats, and pad with scalar floats. Never end on a `float3`; HLSL and
  std140 place it differently.
- **Debug lines and the interface are drawn in display colors** (sRGB-encoded) onto the
  finished image, after tone mapping. Everything before that is linear.
- **A texture shown in the interface** (`ImGui::Image`) must stay alive until the
  frame's interface has been rendered: ImGui only remembers a pointer. Replace such a
  texture before the widget that shows it, never after.
- **Meshes are drawn by `SceneRenderer`**, into a `SceneTargets` per view, in this
  order: depth pre-pass, base pass (fills the G-buffer), deferred lighting, forward
  pass, display encoding. Solid surfaces - opaque and masked - go through the G-buffer;
  only translucent ones are drawn forward. Front faces wind counter-clockwise; depth is
  reversed. Draws that share mesh, submesh and material are instanced, and sorted by
  the serial numbers of their GPU resources, never by address, so that two runs issue
  the same draw calls in the same order (`MeshDrawList`).
- **A pass that draws meshes is a `MeshPassDesc`**, not a new class: `MeshPass` owns
  what such passes share. All of them use the one vertex shader in
  `passes/MeshVertex.slang`. That is not a convenience: the base pass keeps only pixels
  whose depth *equals* what the depth pre-pass wrote, which holds only while both
  compute positions with the same code. Never give a mesh pass a vertex shader of its
  own.
- **Binding sets:** register space 0 is the view's (`ViewBindings`: the constants of
  `common/View.slang` at `b0` and the lights of `common/Lights.slang` at `t0` to `t2`),
  space 1 the material's in a mesh pass (`materials/Material.slang`; call
  `evaluateSurface`, never sample material textures directly) or the G-buffer's in a
  pass that reads it (`GBufferBindings`).
- **The G-buffer layout is written down twice**: `common/GBuffer.slang` and
  `GBufferTarget` in `SceneTargets.h`. Change both, and only through
  `encodeGBuffer`/`decodeGBuffer`; `SceneRendererTests` checks every channel through
  the debug views.
- **Light is computed in one place**, `shadeSurface` (`common/SceneLighting.slang`),
  which the deferred lighting and the forward pass both call. Tests render the same
  surface both ways and compare the pictures; a lighting feature that only one path
  has fails them.
- **A scene is lit by components:** `Light` (directional, point or spot) and
  `AmbientLight` (`RenderComponents.h`). A light shines from where its entity is, along
  the entity's -Z axis. An intensity of 1 lights a white, matte surface that faces the
  light to white - at any distance for a directional light, one metre away for the
  others. `SceneLights` turns that into what shaders compute with (times pi); nothing
  else converts brightness. A world without light components is black, as a game
  shows it; only an editor view asks for stand-ins (`SceneRenderSettings::defaultLighting`).
- **Point and spot lights are found through clusters** (`LightClusters.h`,
  `common/Lights.slang`): a grid of 16 x 9 x 24 boxes over what the camera sees, each
  with the list of the lights that touch it, built on the CPU for every view. The size
  of the grid, the layout of a light and the arithmetic that turns a pixel into a
  cluster are written down twice, in those two files; change both.
  `SceneLightingTests` holds a floor under hundreds of lights against the sum worked
  out on the CPU, so a light that is missing from a list fails it.
- **A light ends at its range**, and is culled by it. Whatever is added to how a point
  or spot light falls off must still be zero at the range, or the light is cut off
  along the borders of clusters.
- **Lighting channels** are eight bits on every mesh renderer and every light; a light
  shines on a surface when they share one. They reach the shaders as per-instance data
  (`MeshDrawList`) and, for the deferred path, in the alpha of G-buffer target 0.
  Ambient light does not ask for them.
- **Lights are taken in the order of their entities' ids**, and ambient lights add up:
  neither a sum nor the choice of which lights are left out when a view has too many
  (`kMaxDirectionalLights`, `kMaxLocalLights`) may depend on how components are stored.
- **A pass that samples the depth buffer cannot draw into a framebuffer that has it
  attached.** Use `SceneTargets::lightingFramebuffer()`, which has color only.
- **Every renderer feature gets a debug view** (`RenderDebugView`) when there is
  something to look at: a channel, a mask, a count. A view of the G-buffer is a `case`
  in `passes/DebugView.slang`; the editor's menu and `viewport.debugView` list them all
  from `kRenderDebugViews`.
- **Uploads go through `RenderResources`.** It records them on a command list of its
  own, created with `setEnableImmediateExecution(false)` - NVRHI allows only one
  immediate command list to be open at a time, and the frame's is. That list is dropped
  after every flush, which is what returns its staging memory.
- **Tangent space is MikkTSpace**, with glTF's convention: the bitangent is
  `cross(normal, tangent.xyz) * tangent.w`, normal maps have +Y up, texture coordinates
  start at the top-left. `SceneRendererTests` pins all of it with pixel values.
- **No pixel may become "not a number".** One such pixel spreads through every pass
  that blurs. Shaders use `normalizeOr` (`shaders/common/Math.slang`) for anything
  interpolated or computed that can have no length, and C++ makes values finite
  before they are sorted or sent to the GPU.
- **A view is perspective or orthographic** (`CameraView::orthographic`). Whatever
  needs the direction to the viewer must ask: with perspective it points at the
  camera's position, without it is the opposite of the camera's direction, everywhere.
- **At most a few frames wait for the GPU** (`FramePacer`, called by `Engine::endFrame`).
  Without a window nothing else holds a render loop back, and frames that pile up use
  up the versions of every per-view constant buffer. Code that submits frames of its
  own in a loop paces them the same way.
- **Shader permutations** are `ShaderRequest`s: path, entry point, stage and sorted
  defines. Their key is deterministic and pinned by a test; changing how it is computed
  orphans every shader cache. Every engine shader entry point is listed in
  `tests/unit/shader_compiler/EngineShadersTests.cpp`, which compiles them all without
  a GPU.
- **Swapchain back buffers** are 8-bit UNORM; the final pass writes display-encoded
  (sRGB) values itself. Do not render linear values straight to the back buffer.
- **Texture states:** create render targets with `keepInitialState = true` and a
  sensible `initialState`, and NVRHI inserts every barrier. Do not transition swapchain
  images yourself; they may only change layout while acquired.

## Scene and components

Step-by-step guide for adding a component: [add-component](.claude/skills/add-component/SKILL.md).

- **Components are plain data.** An entity is an id in a `World`; everything else about
  it is in components: structs without behavior, declared in the module that owns the
  data. Systems live in their own modules and read the components they care about.
  A component never knows which systems use it.
- **Every component is described once**, with `TypeBuilder` (`core/Reflection.h`), and
  registered by its module's `register...Components` function, which
  `registerEngineComponents` calls. That one description drives scene files, the editor
  inspector, undo and the automation API. A field that is not described cannot be saved
  or edited; describe every field, with a `description` and, for numbers, a range.
- **JSON never reaches an object unchecked.** `TypeInfo::parse` (saved data) and
  `TypeInfo::parseEdit` (a change by a person, a script or an agent) validate first and
  return a `PropertyPatch`; applying a patch cannot fail. Code that changes several
  things validates all of them before it applies any, so a failure changes nothing.
- **Property attributes mean something:** ranges are enforced on every write;
  `readOnly` properties are saved and loaded but refused as edits; `transient`
  properties are runtime state and never saved. A `semantic` says how a value is
  edited - a color with a picker, an asset reference with a list of assets, a number
  whose bits are switches (`BitMask`) with a row of check boxes - and is published in
  the automation schema.
- **Refer to entities by `Uuid`** across frames, saves and undo. `Entity` handles die
  with their entity, and references to components are invalidated when any entity is
  destroyed. Hold the `Entity`, fetch the component again.
- **Change the hierarchy only through `World`** (`createEntity`, `destroyEntity`,
  `setParent`); iterate components through `world.registry()`.
- **Include EnTT only through `cordite/world/Entt.h`**, which configures it identically
  everywhere. The `module_layering` test rejects a direct `<entt/...>` include.
- **Files do not churn.** Loading a scene and saving it again produces the same bytes.
  Floats written to a file go through `jsonFromFloat`; nothing is "improved" on load.
- **Scene loading is strict.** An unknown component, property or member is an error
  that names the entity, not something to skip.
- **The showcase uses everything.** The scene of `samples/showcase` must contain every
  component type and every named option, and use every asset and every material option
  of the project; the `samples` test fails otherwise. Adding a feature includes showing
  it there.

## Assets

Step-by-step guide: [work-with-assets](.claude/skills/work-with-assets/SKILL.md).

- **An asset is its id.** Scenes, materials and code refer to assets by `Uuid`, never
  by path. The id of a file is in the `.meta` file next to it; the id of something
  inside a file (a mesh of a glTF model) is derived from the file's id and its place
  in the file (`subAssetId`). `.meta` files are committed with their assets and moved
  with them.
- **A new file's id is derived from its path**, so a freshly generated project has the
  same ids on every machine. Only an editor writes `.meta` files
  (`AssetDatabaseDesc::writeMetaFiles`); games and tests read.
- **Indices are identity.** Entries of a `GltfDocument` are never dropped or reordered,
  because the ids of the assets in a file follow from them. What cannot be imported
  stays in place, empty, and is reported in `warnings`.
- **The slot decides the color space, not the texture.** Base color and emissive are
  sRGB, everything else is data. There is no per-texture setting to get wrong.
- **Sampling belongs to the material:** wrap modes, filter, tiling. A texture is pixels.
  All textures of a material share one layout (`uvChannel`, `uvScale`, `uvOffset`); the
  occlusion texture is the one exception, and only when `occlusionUvChannel` gives it
  the other set of coordinates, which is then used as it is.
- **Files are untrusted.** Every count and offset of a glTF file is checked before use,
  with arithmetic that cannot overflow, before cgltf's own validation runs (which
  can be made to overflow). A mesh is limited by `kMaxMeshVertices` and
  `kMaxMeshIndices`, and so is a file as a whole (`GltfImportOptions`): any number of
  meshes and images may point at the same few bytes, so every use is counted, before
  it is read. Numbers are made finite on the way in (`1e999` is valid JSON). Files a
  glTF refers to are read only from inside the asset directory.
- **A broken asset never stops a scene.** The database lists problems
  (`AssetDatabase::problems`), loading returns an error, and the renderer reports once
  and draws the rest. `scene.validate` and `findSceneProblems` find dangling references.
- **The showcase's textures and models are generated**, by `tools/sample-assets`.
  Change the generator, run it, look at the result, commit both. Its materials and its
  scene are written by hand, with comments: they are the reference for the file formats.

## Editor and automation

Step-by-step guide for scripting the editor and adding commands:
[editor-automation](.claude/skills/editor-automation/SKILL.md).

The editor is layered so that it can be driven without a person:

```
EditorContext    the document: world, selection, undo history, scene file
EditorActions    every edit, validated and undoable
EditorCommands   the same edits by name, with JSON parameters and results
panels, EditorApp    the interface, built on the three layers above
```

- **Every edit goes through `EditorActions`.** Panels and commands never change the
  world directly, so everything is undoable and nothing exists only in the interface.
- **Everything a person can do is a command.** A new editor feature is an action, a
  command and their tests first, and interface second. `editor.commands` returns the
  catalogue an agent reads, so every command and parameter needs a real description
  (a test rejects empty ones).
- **History entries hold ids and validated patches**, never `Entity` handles, component
  pointers or references into the world.
- **Commands run between frames.** A command may render frames itself (screenshots);
  lines typed in the console are queued and run before the next frame for that reason.
- **Batch mode is the test and agent entry point:** `cordite-editor --headless --project
  P --exec script.json --frames N --screenshot out.png`. Exit status: 0 success, 1
  could not run, 2 GPU validation issues, 3 a script step failed. The script's outcome
  goes to standard output as JSON, logs to standard error.
- **The interface is Dear ImGui**, reached through the `ui` module. It runs without a
  window too, with the pointer fed from `Engine::input()`, so interface code is covered
  by headless tests.

## Testing

Framework: doctest. One executable per module (`cordite_<module>_tests`), registered
with CTest under the module name. See the skills
[build-and-test](.claude/skills/build-and-test/SKILL.md) and
[render-tests](.claude/skills/render-tests/SKILL.md).

- Test files are named `<Thing>Tests.cpp` and live in `tests/unit/<module>/`.
- Three suites are not tied to one module: `samples` keeps the showcase project loading
  and complete, `editor_smoke` runs the real editor executable with a command script,
  and `module_layering` checks the include rules.
- Rendering is tested with numbers before pictures. A test that can state what a pixel
  must be does so: `SceneRendererTests` with flat emissive materials, an orthographic
  camera and no light; `SceneLightingTests` with a white, matte wall and lights whose
  brightness on it follows from the documented model. Both use the fixture in
  `tests/render/SceneTestSupport.h`. Reference images are for what only an eye can
  judge.
- glTF files for tests are built in the test (`GltfTestSupport.h`) or written by the
  exporter, never checked in as binaries.
- Test case names are sentences describing behavior: `"an exhausted arena returns
  nullptr and stays usable"`.
- Cover the normal path, the boundaries, and each documented failure. For parsers and
  readers, feed truncated and corrupt input.
- Pin exact expected values where an output is persisted or shared (hashes, UUIDs
  derived from names, file formats, random sequences). A test that compares a function
  against itself proves nothing.
- Tests are deterministic: seeded `Random`, no wall-clock dependence beyond generous
  lower bounds, no network, no files outside `TempDirectory`.
- Assertion-failure paths are tested with `CHECK_THROWS_AS(expr, AssertionFailure)`
  inside `#if defined(CORDITE_ENABLE_ASSERTS)`.
- Use `LogCapture` to assert that a warning or error was (or was not) logged.
- Suites that need a GPU carry the CTest label `gpu` and live in `tests/render/`. They
  run with the Vulkan validation layers when `scripts/bootstrap.ps1 -WithVulkanSdk` has
  been run (or a system-wide Vulkan SDK is installed).
- Rendered frames are compared with reference images in `tests/render/references/`
  within a small tolerance. A new or changed reference image is reviewed by looking at
  it before it is committed; never approve one unseen.
- **Performance is tested against budgets.** The `performance` suite (labels `gpu` and
  `perf`) runs scenarios - the showcase as a game would run it, a field under six
  hundred lights - and checks what each costs against `tests/perf/budgets.json`. Counts
  and sizes - draw calls, triangles, lights and the entries of their lists, bytes on
  the GPU - are the same everywhere and have tight budgets: a change that exceeds one
  raises it in the same commit, with a reason, or has a mistake. Times have budgets
  many times what a desktop GPU needs and are checked in optimized builds only, because
  a test must not fail for a busy machine. To see what a change did to them, run
  `./scripts/dev.ps1 perf` before and after: it runs the suite alone and prints every
  scenario's run next to the one before. The numbers come from `Engine::statistics()`,
  which the editor shows under View > Statistics and returns from `editor.statistics`.
- Windows opened by tests are hidden (`visible = false`) or use the headless window
  system, so the suite never disturbs the desktop and runs on machines without one.
- Only one `Engine`, one `WindowSystem` and one `GraphicsDevice` can exist at a time;
  tests create them one after another, never side by side.
- Get the value of a `Result` in a test with `test::unwrap(...)`, never with `*result`:
  dereferencing an error is undefined behavior and turns a clean failure into a crash.

## Dependencies

Dependencies are pinned by URL and SHA-256 in `cmake/CorditeDependencies.cmake` and
listed with their licenses in [docs/third-party.md](docs/third-party.md). The build
never tracks a branch or a mutable tag. To add or upgrade one, follow the skill
[add-dependency](.claude/skills/add-dependency/SKILL.md).

A new dependency needs a reason: what it replaces, its license, and that it builds on
all three platforms without tools the build does not already require (no Python, no
package manager).

## Committing

Follow the skill [pre-commit-review](.claude/skills/pre-commit-review/SKILL.md). In
short, before every commit:

1. `./scripts/dev.ps1 verify` prints `VERIFY PASSED`. It runs the format check, the
   MSVC build and tests in `RelWithDebInfo`, `Release` and `Debug`, the clang-cl build
   and tests, and clang-tidy on the changed files. Every step must pass with zero
   warnings.
2. You have re-read the whole diff as a reviewer: design, correctness, error paths,
   lifetime, thread safety, input validation, naming, documentation, tests.
3. Documentation that the change makes stale is updated in the same commit
   (`AGENTS.md`, `docs/`, header comments, skills).

`verify` lints only changed files. Before a commit that touches widely included
headers, also run the full `./scripts/dev.ps1 tidy`.

Commit messages: an imperative subject of at most 72 characters, a blank line, then a
body explaining what changed and why. One logical change per commit.

## Known pitfalls

- PowerShell 5.1: do not append `2>&1` to native commands; it turns stderr lines into
  errors and hides the real output.
- A test executable that cannot find its data is usually being run from the wrong
  directory; CTest runs suites from the repository root.
- The default MSVC toolset on a machine may be older than the newest one installed.
  `dev.ps1` uses whatever `vcvars64.bat` selects; do not rely on features newer than
  MSVC 19.38.
- clang-format does not touch comments (`ReflowComments` is off: reflowing joined the
  lines of `@code` examples into nonsense, three times, unnoticed). Wrap comments by
  hand at about 90 columns, and check that an example calls the functions the way they
  are declared: an example with the wrong arguments is worse than none.
- A range-for over a braced list of integer literals (`for (uint8_t v : {1, 2})`)
  iterates `int`s and warns about narrowing; write `std::initializer_list<uint8_t>{...}`.
- In a function returning `Result<bool>`, `return {};` returns `false`, not success.
  Return the value explicitly, and prefer an enum to `bool` when the meaning of the
  two values is not obvious at the call site.
- Shell working directories persist between commands in agent sessions. Use absolute
  paths, or `cd` to the repository root at the start of every command.
- EnTT is pinned to the 3.x line on purpose: 4.0 relies on C++20 syntax that MSVC 19.38
  rejects. Check every supported compiler before moving to a new major version of a
  header-only library.
- Editor reference images show the whole interface, so any visible change to a panel
  changes them. That is intended: look at the new image, then approve it (see the
  render-tests skill). They are rendered with logging silenced; do not add anything to
  the default layout that shows machine-specific text.
- Agent tools that run shell commands can mangle a here-document that contains an odd
  number of apostrophes. Write files with the file-writing tool instead of `cat <<EOF`.
- NVRHI's validation layer rejects a second *immediate* command list opened while
  another is open. Anything that records while a frame is being recorded needs
  `CommandListParameters().setEnableImmediateExecution(false)`.
- "The device is idle" does not mean the device has recorded its last submission as
  finished: on at least one driver, polling an event or timer query right after
  `vkDeviceWaitIdle` says "not yet" one time in four. `GraphicsDevice::waitIdle` waits
  for the last submission by name as well; use it rather than NVRHI's `waitForIdle`
  wherever something is polled for afterwards. A test that passed once has not shown
  that it is not flaky - when timing is involved, run it forty times.
- A GPU timer query must not be read while the command list that fills it is still
  executing: the driver can hand out the new start time together with the end time of
  the measurement before, which gives a duration of seconds for a frame of
  microseconds. `GpuTimer` waits for the command list with an event query first.
- cgltf leaves the `scale` of a texture slot at zero when the material has no such
  texture; only its texture-view parser sets the default of one. Read optional glTF
  values only when the thing they belong to is present.
- An exact `u >= 0` test makes a ray aimed at the edge two triangles share miss both.
  `intersectTriangle` counts edges as inside, with a tolerance, and judges "parallel"
  relative to the triangle's size.
- The sine of a float two pi is not zero. Geometry that closes on itself (`Primitives`)
  computes its last column from index zero instead, or the seam is a crack.
- `-0.0f` and `0.0f` compare equal but differ in their bits; code that compares
  vertices by bytes (`meshopt_generateVertexRemap`) must normalize it first.
- Turning a float that is infinite, negative or not a number into an unsigned integer
  is undefined in C++ and left open in shaders: an infinite depth became slice 0.
  Bring the value into range as a float first, then convert
  (`ClusterDepthMapping::sliceOf`, `clusterIndexOf` in `common/Lights.slang`).
- A floor seen at a low angle runs through one pixel for many centimetres, and where
  in the pixel the GPU takes its sample is not exactly the centre: vertices are
  snapped to a sub-pixel grid. A test that predicts such a pixel from its centre fails
  on a steep gradient - the edge of a spot light - by a few steps of 255. Compare with
  the range of what the model gives a little around the centre (`SceneLightingTests`,
  the test with hundreds of lights), or look at the surface head-on.
- Check that a test can fail. The lighting tests were run once with every light's
  bounds shrunk by a sixth, to see them fail, before they were trusted to pass.
- doctest prints a `const char*` that is streamed into a message as `1`. Wrap it:
  `REQUIRE_MESSAGE(found, "no budget for " << std::string(name))`.
- A braced list inside the braced list of a range-for (`for (auto [a, b] : {{1, 2}})`)
  has no type to deduce. Name the type: `std::initializer_list<std::pair<...>>{...}`.
