# Cordite - guide for agents and contributors

Cordite is a 3D game engine for Windows, macOS and Linux (Ubuntu 24.04+), written in
C++20 and rendered with Vulkan through NVRHI. Its default look is the gritty,
high-contrast, normal-mapped style of late-2000s console games. The visual and rendering
brief is [AIGameEngine.md](AIGameEngine.md); where the engine goes next is
[docs/GAME_ENGINE_PLAN.md](docs/GAME_ENGINE_PLAN.md), with its audit and backlog in
[docs/architecture/](docs/architecture/CORDITE_AUDIT.md); the generic coding standard is
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
./scripts/dev.ps1 run -Target cordite-editor -- --new-project out/my-game --name "My Game"   # a new project, with a scene to begin with
./scripts/dev.ps1 run -Target cordite-editor -- --headless --validation --project samples/showcase --exec tests/data/editor-smoke.json --frames 3 --screenshot out.png
./scripts/dev.ps1 run -Target cordite-sample-assets -- samples/showcase/assets   # regenerate the showcase's textures, models and sounds
./scripts/dev.ps1 run -Target cordite-editor -- --project samples/showcase --export out/Showcase   # make a game of a project
./scripts/dev.ps1 run -Target cordite-player -- --game out/Showcase   # play it (or run out/Showcase/Showcase.exe)
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
# Ubuntu 24.04: sudo apt install build-essential cmake ninja-build xorg-dev libwayland-dev libxkbcommon-dev libdbus-1-dev
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
player/                The cordite-player executable: the program of a game (a thin main around Game)
tools/                 Development tools, such as the generator of the showcase's textures, models and sounds
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
| `core` | -         | Logging, assertions, `Result`, hashing, UUIDs, JSON, reflection, binary streams, file I/O, noticing that files change, config, job system, allocators, timing |
| `math` | `core`    | glm aliases, `Transform`, bounds, rays, frustum, projections, color, curves, random, spherical harmonics, directions in images of everything around, reflection of math types |
| `platform` | `core`, `math` | Windows (GLFW), keyboard / mouse / gamepad input, and the controls a game has by name: `InputMap`, `InputActions` |
| `assets` | `core`, `math` | Assets as data: images and mipmaps, meshes - rigid and skinned - and what prepares them, their levels of detail among it, skeletons, animation clips, materials, the text of scripts, the samples of sounds, prefabs as their files have them, render targets, sequences, glTF import and export, the asset database (ids, `.meta` files) and the asset manager (loading by id) |
| `world` | `core`, `math` | Entities, components and their hierarchy (EnTT), the paths of names that lead to them, the component registry, scene files, prefabs: placing one in a world and making one of what is there |
| `animation` | `core`, `math`, `assets`, `world` | Poses of skeletons and what makes them: sampling clips, blending, the matrices a mesh is skinned with and the bounds of a posed mesh; the `Animator` component; `AnimationSystem`, which plays the animators of a world; the shake of a camera: the `CameraShake` component, how it moves the view (`cameraShakeOffset`) and `shakeCameras`, which lets time pass for it; sequences played: the `SequencePlayer` component and `SequenceSystem`, which puts what their tracks say into the world |
| `physics` | `core`, `math`, `assets`, `world` | `PhysicsWorld`: rigid bodies, their shapes - boxes, balls, capsules, the hulls around sets of points and surfaces of triangles - joints between them, the characters that walk among them, stepping, contact events and queries, over Jolt Physics; the `Collider`, `RigidBody`, `Joint` and `CharacterController` components; `PhysicsSystem`, which keeps the entities of a world and the bodies, joints and characters of a simulation in step and makes shapes of mesh assets |
| `audio` | `core`, `math`, `assets`, `world` | What is heard: the `AudioSource` and `AudioListener` components, and `AudioSystem`, which plays the sources of a world from where they are - voices, five buses with volumes of their own, sound without an entity - over miniaudio, onto a device or into memory |
| `scripting` | `core`, `math`, `platform`, `assets`, `world`, `animation`, `physics`, `audio` | `ScriptRuntime`: the Lua state that gameplay scripts run in and the fence around it - limits on instructions, memory and nesting, the libraries a script may have, an environment for every chunk, the catalogue of the functions scripts are given - and values on their way between JSON and Lua; the `Script` component; `ScriptSystem`, which gives every entity with a script an instance of it and calls its `start`, `update` and `destroy` and tells it what its sequences and its bodies did; the functions scripts are given: entities and their hierarchy, transforms, components through the reflection, vectors and rotations, the time, the ids of assets, the controls, the simulation |
| `rhi` | `core`, `math`, `platform` | The device of Vulkan or, on Windows, Direct3D 12 (offscreen only, so far), the swapchain (Vulkan) and texture readback, over NVRHI |
| `shaders` | `core` | Shader requests and permutation keys, disk cache, `ShaderLibrary` |
| `shader_compiler` | `core`, `shaders` | Slang to SPIR-V compiler, and the cache of the engine's shaders compiled ahead that an exported game ships (`EngineShaderCache.h`); not linked into exported games |
| `renderer` | `core`, `math`, `assets`, `world`, `animation`, `rhi`, `shaders` | The scene renderer and what it is made of: GPU copies of assets, the mesh passes, the G-buffer and its lighting, the lights of a world and the clusters they are sorted into, what a rectangle of light gives a surface (rect lights), their cookies, their shadow maps and the atlas those share, what finishes the picture (exposure, bloom, tone mapping, grading), the environment (the sky and what it lights), environment probes (pictures of the scene that light it in the environment's place), ambient occlusion and reflections from the screen, fog and what lights throw into it, decals, levels of detail, scene captures, the debug views, scene targets, the `MeshRenderer`, `Light`, `AmbientLight`, `Environment`, `EnvironmentProbe`, `Fog`, `Decal`, `SceneCapture` and `PostProcess` components; camera views, debug lines, GPU timing, GPU shader management |
| `ui` | `core`, `math`, `platform`, `rhi`, `shaders`, `renderer` | The immediate-mode interface: the Dear ImGui context and its NVRHI renderer; its typefaces and icons, compiled in; its themes, and the widgets it draws the same way everywhere |
| `engine` | all of the above but `shader_compiler` | `Engine`: owns the subsystems, the assets, the world and the interface, and runs the frame loop; projects; a game made of a project (`Game`); placing models; checking the assets a scene uses |
| `editor` | `engine` and below, `shader_compiler` | The editor: the scene document, undoable actions, scene and asset commands, panels, `EditorApp`; making a game of a project (`GameExport`) (not linked into exported games) |

`cordite-editor` (in `editor/`) is a thin `main` around `EditorApp`. Everything else,
including what tests need to drive the editor, is in the `editor` module.
`cordite-player` (in `player/`) is a thin `main` around `Game` (`engine/Game.h`): a
project's start scene played with its controls, by an engine without a shader compiler,
on the engine's shaders compiled ahead into the project's cache. It links nothing of the
editor.

**A game is exported** (`project.export`, File > Export game, `cordite-editor --project P
--export DIR`; `exportGame` in `editor/GameExport.h`): its program - `cordite-player`
under the name of the game - the project file, the assets with their .meta files, the
engine's shaders compiled into the game's cache, `licenses/`, and last a record that an
export made the directory (`.cordite/export.json`). Exported there again, what the record
names is replaced and nothing else; a directory that holds what no export made is
refused. The editor and `--export` compile the shaders into the project's own cache
(`GameExportDesc::shaderCache`, `.cordite/shaders`, where the editor keeps its own) and
copy the game's permutations from there: exported again, the showcase took 8 seconds, every
shader compiled anew for two targets, and takes one. `exportGame` without it compiles into
the game, and writes nothing outside it. `licenses/` is the notice of every library the
program is made of, which the build copies beside the player
(`cmake/CorditeLicenses.cmake`; Lua and MikkTSpace keep theirs in a header, so theirs are
in `cmake/licenses/`): a library that the engine starts
to link adds its notice there in the same change. The game runs on the kind of system
that exported it, with the player of the editor's build: a game to be shipped is exported
by a `Release` build. `export_smoke` exports the showcase and plays it from where it was
put, with nothing of the build beside it. On macOS a game is an application bundle
(`GameLayout::Bundle`): the destination is `<name>.app` itself - the editor adds the `.app`
(`gameDestinationFor`) - with the program in `Contents/MacOS`, `Contents/Info.plist`
(`bundleInfoPlist`, the identifier the project's `bundleIdentifier`), the Vulkan it draws
with in `Contents/Frameworks` - MoltenVK, pinned for macOS and copied beside the player by
the build, which the program of a bundle loads directly (`kBundledVulkanLibrary`,
`bundledLibrary`) - and everything else in `Contents/Resources`, which is where the program
looks for its game (`gameDirectoryOf`); the record says how a game is laid out, and a game
laid out one way is not replaced the other way. All of it but loading MoltenVK is paths and
text, tested on every system with the layout asked for; a bundle has not been run on a Mac.

**A game that someone plays has their settings** (`engine/GameSettings.h`): the whole
screen or a window and its size, vsync, the quality of the pictures, the volumes - in
`settings.json` under the name of the game (`gameFileName`) in the player's configuration
directory, read as strictly as a scene. `Game::create` reads the file it is given
(`GameDesc::settingsFile`): none there yet, it plays with the defaults and writes them,
for the player to change; one that cannot be read is said, left alone and replaced by the
defaults for that start. What the command line says (`GameDesc::overrides`) wins for one
start and is never written. The defaults are an engine's own, so a game without settings
and one with a fresh file are the same game. `cordite-player` uses the game's own file
only with a window: a run nobody watches - a test, `--headless` - must not change what a
person chose, and names a file of its own with `--settings` when it wants one.

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
serialized as `[x, y, z, w]`; four numbers from outside are checked with `isRotation`
and brought to unit length with `normalizeRotation`, not with `glm::normalize` (see
[Known pitfalls](#known-pitfalls)). Details are in `engine/cordite/math/Math.h`.

## Rendering and shaders

NVRHI is the rendering interface; engine code outside the `rhi` module never calls
Vulkan or Direct3D directly. Step-by-step guide for adding a pass:
[add-render-pass](.claude/skills/add-render-pass/SKILL.md).

- **A device draws with Vulkan or, on Windows, Direct3D 12** (`GraphicsDeviceDesc::backend`;
  `Auto` is Vulkan). What both have is `GraphicsDevice`; what one API has is a backend of
  the `rhi` module (`DeviceBackend.h`: `vulkan/VulkanDevice.cpp`, `d3d12/D3D12Device.cpp`),
  whose objects outlive the NVRHI device made of them. Direct3D 12 asks for feature level
  11_0 and shader model 6.0, and has WARP ("Microsoft Basic Render Driver") as its last
  adapter, as Vulkan has lavapipe. So far an engine draws with Direct3D 12 without a window,
  when told to (`EngineDesc::graphicsBackend`), and nothing chooses it by itself: a window
  (`Swapchain::create` refuses one) and the choice of the API come next. The shaders are
  of the device's kind (`shaderTargetFor`; `ShaderManager` asserts it). Platform code stays
  in `.cpp` files: the header declares `createD3D12Device` everywhere, and
  `GraphicsDevice.cpp` calls it only on Windows.
- **A vertex attribute's name is the semantic Direct3D 12 matches**, and a name that ends
  in a digit is none it takes: what a shader numbers (`TEXCOORD0`, `TEXCOORD1`) is one
  attribute of as many elements (`setArraySize`), laid out one after the other - which
  Vulkan reads from locations one after the other, as it read separate attributes.

- **Every GPU code path must be reachable headless.** A feature that only works with a
  window cannot be tested automatically or driven by an agent. Render to an offscreen
  target first; the swapchain only ever receives a copy.
- **Validation must stay silent.** Tests create devices with `validation = true` and end
  with `checkNoValidationIssues`. A validation message is a bug, not noise; fix the
  cause. Never filter a message out to make a test pass.
- **Shaders are Slang**, in `shaders/`. Matrices are column-major (`mul(matrix, vector)`
  matches glm's `matrix * vector`). Resources use HLSL registers with NVRHI's default
  Vulkan offsets: `tN` -> N, `sN` -> 128 + N, `bN` -> 256 + N, `uN` -> 384 + N.
- **A shader is compiled to a target** (`ShaderTarget`): SPIR-V for Vulkan, and on Windows
  DXIL for Direct3D 12 - of shader model 6.0, the least there is, since the machines
  without Vulkan that it is for are old ones. DXIL is written by DXC, which Slang loads
  from beside itself (`dxcompiler.dll`, pinned, copied there by the build); DXC signs what
  it writes, and Direct3D 12 refuses what is unsigned (`isDxil`). A cache keeps each
  permutation once for each target, under the key of its request - DXIL as
  `<key>.dxil.cshader` beside the SPIR-V - and its file says nothing of the target: the code
  says it itself, so caches of SPIR-V made before stay good. The version of a compiler is
  per target (`IShaderCompiler::version`), and DXC's is a part of DXIL's: another DXC
  makes the DXIL anew and leaves the SPIR-V. A game of Windows ships both
  (`gameShaderTargets()`).
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
  order: shadow maps, depth pre-pass, base pass (fills the G-buffer), decals, ambient
  occlusion, what lights throw into the fog, deferred lighting, reflections from the
  screen, forward pass, light shafts, depth of field, post-processing. Solid surfaces - opaque and masked - go through the G-buffer; only
  translucent ones are drawn forward. Front faces wind counter-clockwise; depth is reversed. Draws that share
  mesh, submesh and material are instanced, and sorted by the serial numbers of their
  GPU resources, never by address, so that two runs issue the same draw calls in the
  same order (`MeshDrawList`).
- **A mesh is drawn at one level of detail in a view** (`LevelOfDetail.h`): the coarsest
  whose error looks no larger than `SceneRenderSettings::lodErrorPixels` where the mesh's
  bounding sphere is nearest to the camera - a pixel at medium quality, two at low, half
  of one at high; nought draws everything at its finest. All of a mesh's submeshes, and
  its shadows, are drawn at that level: a mesh casts the shadow of what is drawn. The
  levels follow the mesh in its index buffer (`GpuMesh::rangeOf`); a draw and a batch
  carry their level, so instances at different levels are different draw calls. The
  levels of detail view colors each mesh by its level.
- **A pass that draws meshes is a `MeshPassDesc`**, not a new class: `MeshPass` owns
  what such passes share. All of them use the one vertex shader in
  `passes/MeshVertex.slang`. That is not a convenience: the base pass keeps only pixels
  whose depth *equals* what the depth pre-pass wrote, which holds only while both
  compute positions with the same code. Never give a mesh pass a vertex shader of its
  own.
- **A skinned mesh is drawn by that same shader, compiled with `SKINNED`** - a
  permutation, not a second shader, for the reason above. How its vertices follow
  joints is a third vertex buffer (`GpuMesh::skinBuffer`, slot 2), the joint matrices
  a third binding set (register space 2, `SkinPalettes`), and only the pipelines of
  skinned batches (`kMeshVariantSkinned`) have either. The matrix of the joints a
  vertex follows is put before the matrix of its instance, and everything after that
  - positions, normals, tangents - is the code of every mesh. The attributes of
  `MeshVertexInput` are listed in the same order in `MeshPass.cpp`; the two of the
  skin come last, so that the others have the same places in both versions.
- **The depth pre-pass and the base pass are compiled with `MOTION`, both of them**
  (`MeshPassDesc::motion`), for the reason above: the base pass writes motion vectors,
  and the pre-pass must run the very code that works out positions there. `MOTION`
  adds where the instance was when the view was last drawn - three more rows of its
  matrix, after `PARAMETERS` and before the skin - and works out where each vertex was
  with the same arithmetic as where it is: what did not move has a motion of exactly
  nothing.
- **The joint matrices of a view are one buffer** (`SkinPalettes`): every pose that
  something is drawn in adds its matrices once, and an instance carries where they
  begin (`MeshDraw::palette`, `parameters.y` of the instance) - and where those of the
  pose it was in when the view was last drawn begin, where that was another
  (`MeshDraw::previousPalette`, `parameters.w`). So figures in poses of
  their own are still instances of one mesh, in one draw call - shadows included.
  **A palette must never point past its pose:** `SceneRenderer::poseFor` gives a draw
  one only when the pose has a matrix for every joint the mesh follows
  (`GpuMesh::jointBounds.size()`); the shader would otherwise read the joints of the
  next figure.
- **A skinned mesh is drawn in the pose of its own entity, or of the nearest entity
  above it that has one** (`SkeletonPoseComponent`): the meshes of a figure hang
  under what animates it. The pose moves the vertices where the mesh was modelled;
  the entity of the mesh then places the result like any other mesh. Without a pose
  it is drawn as it was modelled, as a rigid mesh.
- **What a pose moves is seen where it is now.** Culling and shadows use
  `skinnedBounds`: the box of what follows each joint (`jointBounds`, made once when
  the mesh goes to the GPU), moved by that joint. It has room to spare, never too
  little. Matrices that are not made of numbers are replaced before they reach the
  GPU or the bounds.
- **Binding sets:** register space 0 is the view's (`ViewBindings`: the constants of
  `common/View.slang` at `b0`, the lights of `common/Lights.slang` at `t0` to `t2`,
  the shadow maps of `common/Shadows.slang` at `t3`, `t4` and `s0`, the
  environment of `common/Environment.slang` at `t5`, `t6`, `s1` and `s2`, the
  cookies of the lights at `t7` and `s3`, what lights throw into the fog
  (`common/VolumetricFog.slang`) at `t8` and `s4`, the environment probes at `t9`
  to `t11`, and the fit that rect lights are worked out with (`common/RectLights.slang`)
  at `t12` and `t13`, read with `s1`), space 1 the
  material's in a mesh pass (`materials/Material.slang`; call `evaluateSurface`, never
  sample material textures directly) or the G-buffer's in a pass that reads it
  (`GBufferBindings`: its four targets, the depth, the ambient occlusion at `t5` and
  the motion vectors at `t6`).
  The C++ side of the constants is `ViewConstants.h`.
- **The G-buffer layout is written down twice**: `common/GBuffer.slang` and
  `GBufferTarget` in `SceneTargets.h`. Change both, and only through
  `encodeGBuffer`/`decodeGBuffer`; `SceneRendererTests` checks every channel through
  the debug views.
- **Motion vectors say how far each pixel moved since the view was last drawn**
  (`MotionVectors.h`, `common/Motion.slang`), in parts of the picture, from where it
  was to where it is. The base pass writes them for solid surfaces into a target of
  their own beside the G-buffer (`SceneTargets::motion()`: decals do not change them),
  from where each entity placed its mesh then, the pose each figure was in then, and the
  view-projection then (`previousViewProjection` of the view's constants). A view
  remembers all of that in its targets (`MotionHistory`), by the ids of the entities,
  and forgets it when its camera cuts to another shot - another entity, a jump, a sharp
  turn or zoom (`isCameraCut`) - and when it changes its size: then nothing moved.
  Where nothing was drawn the sky is seen, which moves as the camera turns and not as
  it goes (`motionOfSky`). Translucent surfaces have the motion of what is behind them.
- **A scene capture draws a camera of the scene into a render target**
  (`SceneRenderer::renderCaptures`, called by `Engine::drawWorld` before its view, and by
  the first of the editor's viewports that draws in a frame before its own, with the
  game's background whatever that viewport looks through - once, for all of them:
  `ViewportFrame`): each `SceneCapture` is a view of its own, through the
  `Camera` of its entity with the aspect of its target, finished like a game's and then
  copied into the target - so one that sees its own picture sees the frame before, and
  nothing is read while it is written. A frame draws `kMaxSceneCaptures` at the most, by
  the ids of their entities; one waits out its `interval`; their targets go when they
  are no longer found. A render target is one texture of `RenderResources` for as long
  as the assets are not refreshed, black until it is drawn into, in sRGB: a finished
  picture, which a material shows as its color or its light and refuses as numbers.
  The capture's camera is never primary (`findRenderProblems`).
- **Decals are laid onto the G-buffer** (`DecalPass`, `passes/Decal.slang`), after the
  base pass and before anything reads it. Each is the box of its entity, drawn from
  inside without the depth test; a pixel finds the surface the depth says is there, and
  lays the decal's material on where that is inside the box and faces back along the
  projection (`Decals.h`, written down twice with `common/Decals.slang`: change both).
  Color, roughness, metal and specular are blended by how much of the material there is -
  by its alpha mode, as for a mesh - and its light is added; what alpha holds in the
  targets, the light channels and the occlusion, is kept by the blend. The normals are
  numbers that cannot be blended: a decal bends them in its shader, reading a copy
  (`SceneTargets::prepareDecalNormals`) made before the first decal and again before
  one whose box meets that of a decal since then that bent them; a material without a
  normal map writes no normals. How far the image moves from one pixel to the next is
  measured where the line of sight of the next pixel meets the plane of the image, never
  from the neighbours' depth, which may be something else - and at a slant, that is the
  difference between the right level of the textures and shimmer (`DecalTests` holds
  stripes seen at five degrees). A box that mirrors is drawn mirrored back, or its far
  faces would be culled. Translucent surfaces get none; what decals did is seen in the
  views of the G-buffer.
- **Light is computed in one place**, `shadeSurface` (`common/SceneLighting.slang`),
  which the deferred lighting and the forward pass both call: what leaves a surface
  towards the viewer, and what the fog between the two makes of it. Tests render the
  same surface both ways and compare the pictures; a lighting feature that only one
  path has fails them.
- **A scene is lit by components:** `Light` (directional, point, spot or rect),
  `AmbientLight` and `Environment` (`RenderComponents.h`). A light shines from where
  its entity is, along the entity's -Z axis. An intensity of 1 lights a white, matte
  surface that faces the light to white - at any distance for a directional light,
  one metre away for a point or a spot light, and as a point light in its middle does
  for a rect light seen from far along the way it shines. ("White" is a brightness of
  one in the scene; what a screen shows of it is for the tone mapper to say.)
  `SceneLights` turns that into what shaders compute with (times pi); nothing else
  converts brightness. A world without light components is black, as a game shows it;
  only an editor view asks for stand-ins (`SceneRenderSettings::defaultLighting`).
- **A spot or directional light can shine through a cookie** (`LightComponent::cookie`,
  `LightCookies.h`): a picture whose colors filter it - thrown across a spot light's
  cone as a slide by a projector, seen from behind the light; laid out over the world by
  a directional light, repeated every `cookieSize` metres from where its entity is. A
  point or a rect light has none (`findRenderProblems`). Cookies are layers of one
  texture array of `RenderResources` (`cookieLayer`), `kLightCookieSize` square with
  their mip levels, brought to that size on the CPU and written into their layers as
  uploads - never drawn into, which a texture the frame reads cannot be. The array has
  a layer for each cookie in use, rounded up to a power of two: it is made twice as
  large when it has no room, what it held copied over on the GPU. An image of light, and
  one more than `kMaxLightCookies`, is said once and the light shines without; a view
  binds a single white texel while no light has a cookie. Where on a cookie a point is
  is written down twice, in `LightCookies.cpp` and in `cookieFilter` of
  `common/Lights.slang`: change both. The level of the cookie's pictures is worked out
  from how large a pixel is there (`pixelFootprint`), not from the neighbouring pixels,
  which may be shading other lights. A light without a cookie reads nothing:
  `LightCookieTests` holds a light next to one whose cookie is black.
- **Point, spot and rect lights are found through clusters** (`LightClusters.h`,
  `common/Lights.slang`): a grid of 16 x 9 x 24 boxes over what the camera sees, each
  with the list of the lights that touch it, built on the CPU for every view. The size
  of the grid, the layout of a light and the arithmetic that turns a pixel into a
  cluster are written down twice, in those two files; change both.
  `SceneLightingTests` holds a floor under hundreds of lights against the sum worked
  out on the CPU, so a light that is missing from a list fails it.
- **A light ends at its range**, and is culled by it. Whatever is added to how a point,
  spot or rect light falls off must still be zero at the range - a rect light's measured
  from its middle - or the light is cut off along the borders of clusters.
- **A rect light is worked out with linearly transformed cosines** (`RectLights.h`,
  `common/RectLights.slang`, `shadeRectLight` of `common/Lights.slang`): what it gives a
  matte surface is the closed form of the cosine over what of its rectangle is above the
  surface's horizon; what the GGX lobe reflects of it is the same closed form after the
  rectangle has been taken through the inverse of a matrix that turns a cosine into the
  lobe - the published fit of ltc_code, two tables of 64 by 64 that `ViewBindings` makes
  in half floats and writes in its first `update`. The rectangle is cut at the surface's
  horizon before it goes through the matrix and at the cosine's after. Its face is as
  bright as its color over its area, so that from far along the way it shines it lights
  what a point light of the same intensity in its middle does. Its range, its shadows -
  six maps - and what it throws into a fog (the light of its face times the solid angle
  it fills, `rectLightSolidAngle`) go from its middle, and the range never ends short of
  its corners (`makeSceneLight`). A surface that faces away from the middle may still see
  part of the face: `shadeLight` asks about a rect light before what it asks of a point
  light. The arithmetic is written down twice, in `RectLights.cpp` and in those shaders:
  `RectLightsTests` holds the first to closed forms and to GGX added up over the
  rectangle, and `SceneLightingTests` the pictures to it - a matte wall to the light
  added up patch by patch, shiny walls to the fit at a slant and at a grazing angle, a
  light through the wall. Change both.
- **Lighting channels** are eight bits on every mesh renderer and every light; a light
  shines on a surface when they share one. They reach the shaders as per-instance data
  (`MeshDrawList`) and, for the deferred path, in the alpha of G-buffer target 0.
  Ambient light does not ask for them.
- **Lights are taken in the order of their entities' ids**, and ambient lights add up:
  neither a sum nor the choice of which lights are left out when a view has too many
  (`kMaxDirectionalLights`, `kMaxLocalLights`) may depend on how components are stored.
  Where one light has to be picked for a person to see - the sun whose shadows are
  drawn - it is picked by something they can set: the brightest. Environments have
  nothing of the kind to go by: of several, the one whose entity has the lowest id
  is the world's.
- **Shadows are maps in one atlas.** `planShadows` (`ShadowPlan.h`, plain arithmetic,
  tested without a GPU) gives every light of a view that casts shadows its maps - up
  to four cascades for the sun, one for a spot light, six for a point or a rect light -
  and each a tile of the `ShadowAtlas`, which is drawn anew for every view. A light that
  finds no room gets smaller maps, then none, and is counted
  (`SceneRenderStatistics::lightsWithoutShadows`); the sun, which is served first,
  leaves a quarter of the atlas to the others. Planning runs for every view of every
  frame and allocates nothing: keep it so.
- **Depth in a shadow map is not reversed:** 0 at the light, 1 at the far end. NVRHI's
  comparison sampler passes what is smaller than the stored depth and cannot be told
  otherwise. `MeshPassDesc::conventionalDepth` is for this pass alone; every other
  depth buffer of the engine is reversed.
- **The shadow pass has a binding set of its own**, with the constants alone. The
  view's set holds the atlas as something to read, which a texture cannot be while it
  is drawn into.
- **A surface is kept out of its own shadow by where it is looked up**, not by biasing
  the depth that is drawn (`shadowLookupPosition` in `common/Shadows.slang`): a little
  towards the light and along the normal, in texels of the map, per light. The filter
  that softens the edges reads the map beside the point *on the point's own plane*,
  so a wider filter needs no larger offset. Offsets cost: they make shadows end short.
  `SceneShadowTests` holds both ends - no stripes on a floor lit at three degrees, and
  a shadow that begins at the foot of what casts it.
- **Shadows ask where a surface is, light asks which way it faces.** A normal map
  changes the second and not the first, so a `Surface` has two normals: `normal` for
  light, `geometricNormal` - the triangle's own - for anything that is looked up in a
  shadow map. The forward pass has it from the triangle; the G-buffer has no room for
  it, so deferred lighting works it out from the depth of the pixels around
  (`geometricNormalAt`), and only in views that have shadow maps at all.
- **Cascades do not shimmer** because each is a sphere around its slice of the view,
  as large whichever way the camera looks, moved in whole texels of its map. Keep both
  when changing `cascadeView`; a test moves the camera about and checks them.
- **An environment is one image, used three ways** (`EnvironmentMaps`,
  `common/Environment.slang`): nine spherical harmonics light matte surfaces; a cube
  map blurred level by level - one level for each step of roughness, drawn on the GPU
  the first time the image is used - is what surfaces reflect; and the image itself is
  the sky, which the deferred lighting draws where the depth says nothing was drawn.
  A view without an environment binds `EnvironmentMaps::none()` and an intensity of
  zero. Nothing in a scene blocks the light of its environment; everything that comes
  from all around goes through `shadeSurroundings`, where environment probes stand in
  for the environment inside their boxes.
- **An environment probe is a picture of the scene that lights it in the environment's
  place** (`EnvironmentProbeComponent`, `EnvironmentProbes.h`): taken from the middle of
  its entity's box by six views of `probeFaceView()`, unfinished and lit by no probe -
  light bounces once - and of that come a cube blurred level by level for what is
  reflected (`EnvironmentMaps::prefilterCube`, from levels made on the GPU) and nine
  spherical harmonics for what matte surfaces get, projected by a compute pass. A cube
  map's faces are left-handed: a camera's picture is turned left to right as it is
  copied in (`copyFace`), and `EnvironmentProbeTests` holds a mark on every wall of a
  room to where it must be. The pictures are taken by `SceneRenderer::renderCaptures`,
  before any view: when a probe is new, when its entity has moved (compared exactly),
  when the assets are read again and when asked (`probes.capture`) - never on a timer
  or by a script - and at most `kMaxProbeCapturesPerFrame` in a frame, because each is
  six views of the few that share `kMaxViewsInFlight`: the views of the frames that wait
  for the GPU and of the one being recorded, which a `static_assert` in
  `SceneRenderer.cpp` adds up, so that a number raised past what fits does not compile.
  NVRHI says a buffer has too few versions only when the GPU falls that far behind,
  which no test can count on. Nothing is written back to the
  world. A view takes the probes whose boxes it sees, the highest priority first and
  then by id, at most `kMaxViewProbes`; a surface takes from each what its weight there
  leaves of what the ones before it left, and from the environment the rest - both for
  matte light and for reflections, which are looked up where they meet the box
  (`probeReflectionDirection`, written down twice with `common/Environment.slang`, like
  `probeWeight`). The reflections of every probe are one array of cube maps, which is
  why the renderer requires `imageCubeArray` of the device (`GraphicsDevice.cpp`): the
  shaders that light every surface read it. The picture and its levels are written only
  between views - the set of a view holds both.
- **Which direction is where in an image of everything around is written down twice**:
  `equirectUv` in `math/Equirectangular.h` and in `common/Math.slang`. The middle of the
  image is -Z, +X a quarter of the width to its right, the top row +Y.
  `EnvironmentTests` holds every direction of the sky, and of mirror balls of several
  roughnesses seen from six sides, against the first: a face of the cube map that is
  the wrong way round does not pass.
- **A pass that samples the depth buffer cannot draw into a framebuffer that has it
  attached.** Use `SceneTargets::lightingFramebuffer()`, which has color only.
- **The renderer works out light; `PostProcessPass` makes a picture of it.** Scene
  color has no upper limit. The final pass exposes it, adds the bloom, tone maps and
  grades it, in the order `PostProcessSettings.h` lists. The curves and the arithmetic
  are written down twice - in `PostProcessSettings.cpp` and in
  `common/ToneMapping.slang` - and `PostProcessTests` holds the pictures of the one
  against the numbers of the other: change both. A scene sets the numbers with
  `PostProcess` components (`ScenePostProcess.h`): global ones, boxes, and a camera's
  own, laid over one another by priority.
- **Depth of field blurs the light of the scene, before it is finished**
  (`DepthOfFieldPass`, `passes/DepthOfField.slang`), so that bloom and exposure see the
  blur: three passes - the picture and how blurred each block of four pixels is, at half
  the size, taking the nearest of the four; that blurred, each texel by the texels around
  it whose own blur reaches it; and the blur laid over the scene color by blending. A
  tap is read as the texel it falls in, never filtered between texels: filtered, how
  blurred it is mixes across the edge of something, and a sharp thing in front glows
  into the blur behind it (`DepthOfFieldTests` has both edges). What lies behind a
  texel counts as far as the texel's own blur reaches and no further than twice that:
  a sharp thing in front keeps its edge, a blurred one lets what is behind show through
  its own. The blur is laid over each pixel from the texels that are blurred like it
  (a bilateral upsampling): the texels along an edge are made of both sides, and
  filtered into the blur beside them they would draw a fringe around everything sharp
  against a blurred sky. Where neither a pixel's own blur nor what reaches it from in
  front says anything, the blending leaves the picture exactly as it was, and a test
  holds it to the bit. How blurred a depth is -
  `focusBlur`, along the way the camera looks, nothing drawn being as far as can be -
  is written down twice, in `PostProcessSettings.cpp` and in `common/DepthOfField.slang`:
  change both. It is part of finishing: a picture that is not finished has none, and a
  scene without `nearBlur` or `farBlur` pays nothing for it. Translucent surfaces are not
  in the depth buffer, and are blurred as what is behind them.
- **Motion blur smears what moved along its motion vectors, after the depth of field and
  before the rest of finishing** (`MotionBlurPass`, `passes/MotionBlur.slang`): the
  reconstruction filter of McGuire et al. Three passes - the longest blur of each tile of
  the picture (a tile is the longest blur there may be, a fortieth of the height:
  `motionBlurRadius`), the longest of each tile and the tiles around it, and every pixel
  taken from points along that, from a copy of the picture made before. A point counts
  as far as what is in front moves over what is behind, what is behind shows through
  what moves in front of it, and what moves alike reaches each other (`motionBlurWeight`).
  The arithmetic is written down twice, in `MotionBlur.cpp` and in
  `common/MotionBlur.slang`, and `MotionBlurTests` holds the GPU's picture of a moving
  bar against the first, pixel for pixel: change both. Where nothing near a pixel moved
  by half a pixel it keeps its color to the bit; a picture that goes on from none - the
  first of a view, the first after a cut - is not blurred at all. How much is the
  scene's (`PostProcess.motionBlur`, the share of the time the shutter is open); whether
  at all is the view's (`SceneRenderSettings::motionBlur`): a game's view and a scene
  capture blur, the editor's own camera does not, and the editor's view through the
  game's camera only while the game's time passes (`ViewportPanel::render`, given the
  play clock). A picture drawn when no time has passed (`deltaSeconds` 0) has nothing
  moving in it: what changed since was an edit or a jump, not a motion.
- **Light shafts are the sky around the sun, blurred towards it** (`LightShaftPass`,
  `passes/LightShafts.slang`), and added to the light of the scene before the depth of
  field: what a pixel where nothing was drawn gives them is the sun's own color and what
  of the sky is brighter than a threshold, as much as the pixel looks at the sun (a cone
  of about twelve degrees, `lightShaftSource`), at most four times white - a sun in an
  image of the sky is tens of thousands of times as bright, and spread over the picture
  it would be all there is. Everything that was drawn gives nothing, and is what casts
  the shadows in them. Three passes blur that towards the sun's point in the picture,
  eight taps each, eight times as far apart as in the pass before, and a fifth adds it.
  The sun is `SceneLights::sun()` - the brightest directional light, the one the fog
  glows towards - and its point `sunScreenPoint`, which projects the direction, not a
  point far away. With the sun behind the view, or a view without perspective, there are
  none, and no pass is drawn; towards ninety degrees they fade (`lightShaftFade`). The
  arithmetic is written down twice, in `LightShafts.cpp` and `common/LightShafts.slang`:
  change both.
- **A test that says what a pixel must be draws a picture that is not finished**
  (`PostProcessQuality::Off`, which `SceneFixture::flatSettings()` asks for): the light
  as it was worked out, encoded and nothing else. Debug views are never finished
  either. What finishing does is tested on its own, on flat fields of known light -
  and with `AntiAliasing::Off`, wherever a picture has edges in it.
- **Anti-aliasing is the last step of finishing a picture** (`AntiAliasing::Fxaa`,
  `passes/AntiAliasing.slang`), and one more pass of `PostProcessPass`: the final pass
  then draws into an image of the view's own (`SceneTargets::aliased()`), with the
  brightness of each pixel in alpha, and the smoothing writes the display image. It
  works on the colors of a display, after the tone mapping, and film grain is laid
  over its result (`withGrain`), never under it: every grain would be an edge. Its
  tests hold a slanted bar against how much of each pixel the bar covers, and against
  itself turned round (`AntiAliasingTests`). It is a switch of the view, like the
  debug view; how far it follows edges goes by the `PostProcessQuality`.
- **What lasts from one frame to the next belongs to the view**, which is its
  `SceneTargets`: the exposure a view has got used to is kept there, and so is what it
  remembers of its last picture for the motion of the next (`MotionHistory`); both are
  gone when the view changes its size. What a pass keeps for a view - `PostProcessPass` its
  binding sets - goes by `SceneTargets::serial()`, and is dropped when that has not
  been drawn for a while. That is for what costs nothing to keep. An image as large as
  the view is one of the view's targets, made when it is first needed
  (`SceneTargets::prepareTracedOcclusion`): kept by a pass, the images of every size a
  viewport went through while it was dragged larger would wait there to be dropped.
- **Ambient occlusion is worked out from the screen** (`AmbientOcclusionPass`), between
  the base pass and the lighting, and dims the light from all around - ambient lights
  and the environment - and nothing else: it multiplies the occlusion of the surface
  that `shadeSurroundings` takes into account. Its arithmetic is written down twice,
  in `AmbientOcclusion.cpp` and in `common/AmbientOcclusion.slang`; tests hold the
  first against cases with a known answer, and the pictures against the first with a
  wall given as a formula (`Reference` in `AmbientOcclusionTests.cpp`). What stands in
  the light is measured by how far it rises above the plane of the surface
  (`riseAboveSurface`), never from the direction to the viewer alone: a flat surface
  must come out with all its light, at any angle, without a bias to hide behind.
  What it looks like is the scene's to say (`PostProcess`: intensity, radius, bias);
  what it costs is the view's (`ScreenEffectsQuality`). Surfaces drawn forward get
  none: they are not in the depth buffer.
- **What a reflection meets on the screen takes the place of what the surroundings
  would have it reflect** (`ScreenReflectionPass`, after the lighting and before the
  forward pass): the scene color is copied, the reflection of each pixel is followed
  through the depth buffer in even steps across the picture (`traceScreenReflection`),
  and where it meets something the light the lighting gave that pixel replaces the
  reflected part of `surroundingLight` - the sky and ground of the ambient lights, the
  environment, the probes - weighed by `surroundingsReflectance` and the surface's
  occlusion, and seen through the fog between the viewer and the surface, as the
  lighting weighed what it replaces. That holds to the bit only because both read one
  surface (`surfaceAt` in `common/GBufferSurface.slang`) through the same functions:
  never work out the surroundings a second way. Where nothing is met, the pixel is
  copied unchanged, and a test holds that to the bit. The walk sets out a pixel's
  width above the surface - the depth buffer holds a surface at the middles of its
  pixels, which a reflection that grazes it would seem to pass behind - and what lies
  outside the picture, or beyond the far plane, is met by nothing. What is met is as
  sharp as a mirror's reflection and is not blurred for rough surfaces, so surfaces
  rougher than `reflectionMaxRoughness` take nothing from it; trust also fades at the
  borders of the picture and towards `reflectionMaxDistance`. The arithmetic is written
  down twice, in `ScreenReflections.cpp` and `common/ScreenReflections.slang`: tests
  hold the first against geometry and the pictures against both. Where in the picture
  a reflection met something is kept in 16 bits: half floats put it pixels off at the
  far side of a large picture. Below the high quality the reflection of a block's first
  pixel serves the four pixels of the block, each with its own roughness - no upsampling
  that keeps to edges, as the ambient occlusion has - so the border of a smooth surface
  whose block starts on a rough pixel has a pixel without one. What it looks like is the scene's (`PostProcess`), what
  it costs the view's (`ScreenEffectsQuality`); probes are taken without it
  (`SceneRenderSettings::screenReflections`), and surfaces drawn forward get none. A view
  in which nothing can be smoother than that - no solid material, no decal
  (`GpuMaterial::leastRoughness`, where a texture of roughness counts as smooth) - draws
  none and pays nothing (`SceneRenderer::hasSurfaceSmootherThan`).
- **Fog is in the way of everything a viewer sees** (`Fog.h`, `common/Fog.slang`).
  `shadeSurface` ends with it, so solid and translucent surfaces are in it alike; the
  lighting pass lays it over the sky and the background, where nothing was drawn. It
  is a closed form - a fog at its densest up to the height of its entity, thinning
  out above by the same factor every metre, with a glow towards the sun - and costs no
  pass and no texture. The arithmetic is written down twice, in `Fog.cpp` and in
  `common/Fog.slang`: unit tests hold the first against the density added up along a
  way, step by step, and `FogTests` holds pictures of boxes of known colors against
  the first. Change both. Without perspective every line of sight sets out from the
  plane the camera is in (`viewerOf`, `viewerAt` in `common/View.slang`), not from
  the camera. A world has one `Fog` in use, as it has one environment; `SceneLights`
  picks it and its sun, the brightest directional light.
- **Lights light a fog that says so** (`FogComponent::volumetric`; `VolumetricFog.h`,
  `VolumetricFogPass`, `common/VolumetricFog.slang`): each light throws its `fogColor` -
  its color times the `volumetricIntensity` of its component - into the air it reaches,
  with its shadows and its cookie, besides the fog's own color. It is worked out for a
  grid of boxes over the view (`FogVolumeGrid`, as fine as the view's
  `ScreenEffectsQuality`; none at `Off`), from the camera to `volumetricDistance`, in
  slices that get deeper as the square of the way: the first of two compute passes asks
  every light of a box's cluster at its middle, with one read of its shadow map; the
  second adds that up along each column, each box by how much the fog's amount grows
  across it - the closed form of `Fog.slang`, start distance and largest opacity
  included - so that the volume dims what lies behind it exactly as the fog does, and a
  sun that nothing shadows comes out exact. `shadeSurface` reads the sums in front of a
  surface, and the lighting in front of the sky, between the ends of the slices; beyond
  the grid only directional lights add theirs, in the closed form, as if nothing stood
  in their way. A translucent surface is lit in front as a solid one is: that is why it
  is a volume and not a march along each pixel, which would lay the beam behind a pane of
  glass over the pane. The arithmetic is written down twice - in `VolumetricFog.cpp` and
  in `common/FogVolume.slang`, `common/VolumetricFog.slang` and the two passes - and
  `FogLightTests` holds the volume's texels and the pictures against the first: change
  both. The second pass binds the view's constants alone (`ViewBindings::constants()`):
  the view's set holds the volume it writes. A beam thinner than a box is not seen
  whole; nothing of the light is kept from one frame to the next.
- **A pass that computes is a `ComputePass`** (`ComputePass.h`): a compute shader, its
  layouts, and a dispatch of so many thread groups (`groupsFor`). What it writes is a
  texture made with `isUAV`, written in the shader through an `RWTexture` that names its
  format (`[format("rgba16f")]`), so that writing it needs no feature of the device for
  writing without a format.
- **A fullscreen pass writes every pixel.** Where nothing was drawn, the lighting and
  the debug views write the background (`backgroundColor` in `ViewConstants`); they
  do not `discard` (see the pitfall on discarding for what is true of every pixel).
- **The renderer has no clock.** What takes time (exposure getting used to light) or
  changes with it (film grain) is told how much has passed and what time it is, in
  `SceneRenderSettings`. `Engine::drawWorld` fills that in; with a fixed time step
  (`EngineDesc::fixedTimeStep`, `cordite-editor --fixed-time-step`) every run draws
  the same frames, which is what tests and reference images rely on.
- **Every renderer feature gets a debug view** (`RenderDebugView`) when there is
  something to look at: a channel, a mask, a count. A view of the G-buffer is a `case`
  in `passes/DebugView.slang`; one that is not drawn from the G-buffer is drawn by the
  pass it is about, in a mode of its own - lighting only and reflections by the lighting,
  the focus by the depth of field, over the lit picture, and the light shafts and the
  reflections from the screen by theirs, alone over black. The editor's menu and `viewport.debugView` list them all
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
  before they are sorted or sent to the GPU. Whatever writes scene color ends in
  `storableLight` (`common/SceneLighting.slang`): more light than a half float holds -
  a sun in an image of the sky, times the brightness of the environment - is kept as
  infinity by some GPUs, and the bloom makes "not a number" of that.
- **A view is perspective or orthographic** (`CameraView::orthographic`). Whatever
  needs the direction to the viewer must ask: with perspective it points at the
  camera's position, without it is the opposite of the camera's direction, everywhere.
- **At most a few frames wait for the GPU** (`FramePacer`, called by `Engine::endFrame`).
  Without a window nothing else holds a render loop back, and frames that pile up use
  up the versions of every per-view constant buffer. Code that submits frames of its
  own in a loop paces them the same way.
- **Shader permutations** are `ShaderRequest`s: path, entry point, stage and sorted
  defines. Their key is deterministic and pinned by a test; changing how it is computed
  orphans every shader cache. Every permutation the engine asks for is listed in
  `engineShaderRequests()` (`shaders/EngineShaders.cpp`): an exported game ships a
  cache of exactly those and no compiler. `EngineShadersTests` compiles them all
  without a GPU, and a test drives the editor through every debug view, quality and a
  played scene and fails for a permutation that was asked for and is not listed - or
  is listed and was not asked for (`ShaderLibrary::requested()`).
- **An engine compiles shaders only when it is given a compiler** (`EngineDesc::
  shaderCompiler`: `makeSlangShaderCompiler`, which the editor and the tests pass). Without
  one it needs no sources and reads every shader from its cache, trusted as it is - the
  cache an exported game ships (`compileEngineShaders`). The `engine` module does not
  depend on `shader_compiler`, and `module_layering` keeps it so. The editor keeps what it
  compiles in the project's `.cordite/shaders`, checked against the sources when read:
  with a cache in memory alone it compiled every shader each time a project opened - three
  seconds of the showcase's three and a half, which a test now holds to none.
- **Swapchain back buffers** are 8-bit UNORM; the final pass writes display-encoded
  (sRGB) values itself. Do not render linear values straight to the back buffer. A
  finished picture is dithered by a level at the most, so that slow gradients show no
  bands; the dither dies away towards black and white, which stay what they are.
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
  inspector, undo, the automation API and what gameplay scripts read and write. A field
  that is not described cannot be saved, edited or scripted; describe every field, with
  a `description` and, for numbers, a range.
  A component that is "some settings, and where they apply" can be a struct built on
  the struct of the settings (`PostProcessComponent : PostProcessSettings`): the
  builder describes inherited members like the struct's own.
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
- **A property may be JSON of no fixed shape** (`PropertyType::Json`, a member of type
  `Json`): an object that is kept as it is, for data only its user understands - the
  values a script is given for one entity. It is nested at most 16 deep and holds
  only finite numbers (`checkJsonData`), an edit replaces the whole of it, and the
  inspector shows it as text that becomes the value once it is an object. Nothing in
  it is followed: an id of an entity in there still names the original after the
  entity was duplicated. What the engine itself reads is never kept this way: that
  gets fields, with types and ranges.
- **Refer to entities by `Uuid`** across frames, saves and undo. `Entity` handles die
  with their entity, and references to components are invalidated when any entity is
  destroyed or any component of that type is added or removed. Hold the `Entity`,
  fetch the component again.
- **Change the hierarchy only through `World`** (`createEntity`, `destroyEntity`,
  `setParent`); iterate components through `world.registry()`.
- **Include EnTT only through `cordite/world/Entt.h`**, which configures it identically
  everywhere. The `module_layering` test rejects a direct `<entt/...>` include.
- **Files do not churn.** Loading a scene and saving it again produces the same bytes.
  Floats written to a file go through `jsonFromFloat`; nothing is "improved" on load.
- **Scene loading is strict.** An unknown component, property or member is an error
  that names the entity, not something to skip.
- **A prefab is entities in a file, and placing it copies them.** A `.prefab.json`
  holds a group of entities with one root, written as a scene writes them. The assets
  know it as what its file says (`assets/Prefab.h`); the world places it
  (`world/Prefab.h`: `instantiatePrefab`, and `serializePrefabEntities` for making
  one) - there and not in `engine`, because scripts place prefabs too and the
  scripting module is below it. The copies get ids of their own, what refers to an
  entity of the group refers to its copy, and the root gets a `PrefabInstance` that
  names the prefab. That component is a mark and nothing more: what was placed is
  ordinary entities, saved in full with their scene, and a prefab that is changed
  later does not change them. Nothing may read the mark to decide what an entity
  does. Keeping instances up to date with their prefab - overrides, prefabs inside
  prefabs that stay tied - would be a design of its own, not an addition to this one
  ([why](docs/architecture.md#a-prefab-is-copied-where-it-is-placed)).
- **The showcase uses everything.** The scene of `samples/showcase` must contain every
  component type and every named option that a scene can hold - not those of
  `transient` properties, which only a running game has - and use every asset and
  every material option of the project, and its scripts must call every function
  that scripts are given and have every function the engine calls in a script,
  with something in the scene that calls it; the `samples` test fails otherwise.
  Adding a feature includes showing it there.

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
- **A skinned mesh says how its vertices follow joints beside its vertices**
  (`MeshData::skin`: four joints and four weights a vertex, one entry for every vertex
  or none at all). Whatever reorders, splits or merges vertices carries it along
  (`MeshProcessing.cpp`): two vertices are one only when they follow the same joints.
  Weights are brought to one in all on the way in (`normalizeSkinWeights`), and
  `validateMesh` holds them to it.
- **A mesh comes with its levels of detail**, made as it is loaded (`generateLods`, by
  meshoptimizer's simplifier): up to four coarser versions over the same vertices, each
  aiming at half the triangles of the one before and simplified from the mesh itself,
  each with its error - how far its surface is from the mesh's, at the most, in the
  mesh's own units. Every submesh is simplified on its own with its open edges kept in
  place, so no gap opens between two of them. Levels index the vertices as they are:
  whatever changes the vertices or the submeshes drops them, so they are made last, by
  the asset manager - an importer, and a glTF file, have none. A mesh of fewer than 256
  triangles gets none.
- **A skeleton keeps its joints in the order of its file** (`SkeletonData`), because
  that is the order vertices name them in: a joint may come before the one it hangs
  on, and `jointOrder()` is the order to work a pose out in. What stands between a
  joint and its parent joint in a file and is no joint - an armature, placed and
  scaled - is the joint's `parentOffset`, kept as a matrix so that nothing of it is
  lost. **An animation moves joints by name** (`AnimationData`): a clip fits every
  skeleton that has joints of those names. Rotations are curves of four numbers, each
  key the same way round as the one before it.
- **The slot decides the color space, not the texture.** Base color and emissive are
  sRGB, everything else is data. There is no per-texture setting to get wrong. A
  light's cookie is a color as well: it filters light by its colors. A
  color lookup table (`PostProcess.colorLut`) is data too: an image of N * N by N
  pixels - N slices side by side, red across each, green down it, blue from slice to
  slice - that holds the colors of a display as an image editor has them.
- **An image of floating-point numbers is light** (`.hdr`), whatever uses it. It goes
  to the GPU as half floats - every GPU can filter those - with what is more than
  65504 cut off, and its mip levels are plain averages: the sharper filter that 8-bit
  images get leaves a ring of less than no light around a sun. As an environment, an
  image of 8-bit colors is sRGB.
- **What textures take on the GPU is bounded twice**: by the quality of the program - the
  largest side a texture keeps, 1024, 2048 or 4096 texels (`textureLimitFor` in
  `SceneRenderer.h`, `Engine::setRenderQuality`, which a game's settings and the
  editor's quality presets set: every view of a program shares its textures, so this is
  the program's, never a view's) - and by the budget of the project
  (`textureBudgetMegabytes` in `project.cordite`): past it every texture is kept the same
  number of levels smaller, the fewest at which all the textures known so far fit
  (`TextureBudget.h`), and a texture that could not be made is not among them. Not
  one texture at a time as they come: then how sharp a texture is would depend on what
  was looked at first, and a test asks for the same textures in two orders and holds
  them to the same sizes. A change applies at the start of the next frame
  (`RenderResources::beginFrame`, which the engine calls at the start of every frame),
  never in the middle of one - the interface may hold a pointer to a texture - and drops
  everything, so that what is counted stays right; what was made of the textures - the
  pictures of probes - is made anew by `RenderResources::revision`. What is read texel
  by texel - a lookup table of colors - keeps its size and is not counted
  (`dataTexture`). Textures are not streamed by how large they are seen.
- **Sampling belongs to the material:** wrap modes, filter, tiling. A texture is pixels.
  All textures of a material share one layout (`uvChannel`, `uvScale`, `uvOffset`); the
  occlusion texture is the one exception, and only when `occlusionUvChannel` gives it
  the other set of coordinates, which is then used as it is.
- **FBX and OBJ files are read by ufbx** (`FbxImporter.h`) into the same `GltfDocument`
  as glTF files, brought to +Y up and metres and to glTF's texture coordinates, so
  everything after the import treats a model alike. Skins and animations of FBX files
  are not imported yet; what is left out is in `warnings`. An OBJ file's MTL files are
  read only from inside the asset directory, and images are named relative to the file,
  as in glTF.
- **Files are untrusted.** Every count and offset of a glTF file is checked before use,
  with arithmetic that cannot overflow, before cgltf's own validation runs (which
  can be made to overflow). A mesh is limited by `kMaxMeshVertices` and
  `kMaxMeshIndices`, and so is a file as a whole (`GltfImportOptions`): any number of
  meshes and images may point at the same few bytes, so every use is counted, before
  it is read. Numbers are made finite on the way in (`1e999` is valid JSON). Files a
  glTF refers to are read only from inside the asset directory.
- **A sound is its samples** (`SoundData`): floats in one or two channels, at the rate
  of its file, decoded whole when it is loaded - by miniaudio, from WAV, FLAC or MP3 -
  and kept. What a file says of its own length decides nothing: `decodeSound` counts
  what the decoder hands over and refuses what is longer than `kMaxSoundFrames`.
  Whatever is given a `SoundData` that code made checks it with `validateSound`.
  miniaudio is included in `.cpp` files only, and compiled with the definitions in
  `cmake/CorditeDependencies.cmake` - which every includer has to see, so a new one
  is added there, never with a `#define` before the include.
- **A render target is a file that says the size of a picture** (`.target.json`,
  `RenderTarget.h`), read as strictly as a material and checked when it is indexed. The
  picture itself is the renderer's (a scene capture draws it); the asset is what a
  `SceneCapture` and a material name, and what makes both point at the same one. It is
  written by hand: there is no creating one in the editor.
- **A sequence is a file of tracks** (`.sequence.json`, `Sequence.h`): a duration, and
  tracks of four kinds - transforms, properties of components, cuts to cameras, events -
  each a list of keys at times within the duration, each after the one before. It is
  read as strictly as a material, checked when it is indexed, bounded in its bytes, its
  tracks, its keys and its text, and written by hand: there is no timeline in the editor.
  What its tracks name is a path of entity names (`world/EntityPath.h`), found when it
  plays: the file says nothing a scene has to agree with until then.
- **A broken asset never stops a scene.** The database lists problems
  (`AssetDatabase::problems`), loading returns an error, and the renderer reports once
  and draws the rest. `scene.validate` and `findSceneProblems` find dangling references.
- **What an asset needs, and what needs it, is in the database**: `AssetInfo::dependencies`
  (a material its textures and its parent, a model what it holds, a scene and a prefab
  what their entities refer to) and `AssetDatabase::dependents` the other way round,
  where the file an asset is in is no user of it. What entities refer to only their
  components can tell, which the `assets` module does not know: whoever makes a
  database passes `AssetDatabaseDesc::readReferences`, and an engine passes
  `assetReferenceReader` (`engine/SceneAssets.h`). That reads the JSON as it stands, so
  a scene that would not load still says what it uses - which is when it is asked,
  before something is deleted. A scene the editor saves is read again on its own
  (`rereadDependencies`), because a refresh reloads every asset; `revision()` does not
  change then, so nothing may keep dependencies by it. What a script loads while it
  runs is known to nothing until it runs.
- **A file moves with its .meta** (`asset.move`, `editor/AssetMove.h`): it keeps its id,
  so whatever refers to it by id follows it, and so do the open scene's file and the
  project's start scene. A file that had no .meta gets one that keeps the id it had.
  What names a file by where it is does not follow: `moveAsset` refuses to leave a glTF
  file's buffers and images behind in another directory (`GltfDocument::externalFiles`)
  and to take a texture away from the glTF file that names it, and lists the lines of
  scripts, scenes and prefabs, and the values in the open scene, that still name the old
  path: those are changed by hand. It takes the id by value - the assets are read again
  on the way, and an `AssetInfo` does not outlive that. A directory moves the same way
  (`asset.moveDirectory`, `moveAssetDirectory`), whole, in one rename: the assets in it
  without a .meta get one first, a glTF file must still find what it names wherever the
  move leaves the two - one may move without the other where the way between them stays
  the same - and the project's start scene and input map follow it.
- **A file from elsewhere is brought in by a copy** (`asset.import`,
  `editor/AssetImport.h`, or dropped onto the editor's window - into the folder the
  browser shows, `importDirectory`): a glTF file brings what it names by where it is,
  to the same places beside it, and a .meta beside a file stays behind - in this
  project the file is new. Everything is checked before the first copy, and a copy
  that fails takes the others back.
- **A picture of an asset is drawn by the scene renderer** (`AssetPreview`,
  `asset.preview`), in a world of its own: a mesh as it is, a material on a ball, a
  model or a prefab as placed, whole, in an editor's stand-in light and the finishing
  of `PostProcessSettings` at its defaults - so it looks like what the renderer makes
  of the asset, and changes with it. It is drawn with the shadow quality of the
  viewports: the renderer has one atlas, and makes it anew whenever its size changes.
  A texture is its own picture. A preview draws when what it is asked for changes,
  and remembers a failure, so that it can be asked in every frame.
- **The showcase's textures, models and sounds are generated**, by `tools/sample-assets`.
  Change the generator, run it, look at the result, commit both. Its materials, its
  prefab and its scene are written by hand, with comments: they are the reference for
  the file formats. What the scene has placed from the prefab is written out in the
  scene, as every instance is, and a test keeps the two the same: change the prefab,
  and change its instances in the scene with it.
  A new file gets its `.meta` file - its id - when the project is next opened in the
  editor (`cordite-editor --headless --project samples/showcase --frames 1`); the ids
  of what is inside it are listed by `asset.get`.

## Animation

- **Joints are not entities.** A skeleton is an asset. Where its joints are at a moment
  is worked out from an `Animator` into a `SkeletonPoseComponent`, which is not in the
  component registry: it is not saved, not shown and not undone. Playing an animation
  therefore changes nothing a scene file holds but the animator's own clocks, and a
  crowd costs a pose each, not an entity for every joint.
- **A pose is made in steps** (`animation/Pose.h`): `restPose`, then `sampleClip` over
  it - which changes of a joint only what the clip has keys for - then `blendPoses`;
  `jointMatrices` puts the joints in the model, parents first (`jointOrder`), and
  `skinningMatrices` is that times the inverse bind matrix. A clip is bound to a
  skeleton once, by the names of its joints (`bindTracks`), not for every frame.
- **A rotation between two keys is on the arc between them** (`sampleRotation`), as
  glTF asks: a third of the time is a third of the angle. `Curve<Vec4>::evaluate`
  goes the straight way through four numbers, which is neither a rotation nor at an
  even speed; only a cubic spline is taken from it, and brought back to unit length.
- **`AnimationSystem::update` lets time pass, `evaluate` only poses.** A program that
  shows a scene without running it calls `evaluate`, which changes nothing an animator
  holds - not even a time beyond its clip, which it reads as a clock that ran there
  would stand. A clock is written only when it moves, so nothing changes a scene
  behind the back of the editor's undo history.
- **The end of a clip is a time it has**, also when it loops (`clipTime`): a clock set
  to the duration shows the last pose, not the first. Only a time beyond the ends goes
  round.
- **What an animator cannot use is reported once** and poses nothing or the rest pose;
  numbers that are none - a speed, a weight, a time - count as nothing. Skeletons and
  clips are held until `AssetDatabase::revision()` changes.
- **The engine plays, the editor poses.** `Engine::updateWorld(WorldClock)` is the one
  place the animation of a frame happens, before the world transforms are worked out:
  with a running clock (`runFrame`, a game) the animators play on by the time of the
  frame, with a stopped one (`EditorApp`) they are only posed. A program that draws
  a world itself calls it instead of `World::updateWorldTransforms()`, once a frame.
- **Placing a model lays it out for animation** (`instantiateModel`): no entities for
  joints (`ModelNode::joint`), one entity with an `Animator` for each skeleton
  ("Skeleton"), directly under the instance, and the skinned meshes under it, unmoved
  - a skinned mesh is placed by its joints, wherever its node is in the file. What
  hangs on a joint in a file (a sword in a hand) is placed where the joint rests and
  does not follow a pose yet; the log says so.
- **One rule says which pose a mesh is drawn in**, `findSkeletonPose`: the renderer
  draws by it and the editor picks by it. The editor works the pose out a second
  time, on the CPU (`editor/PosedMesh.h`), under the renderer's conditions - a mesh
  that is not skinned, has no pose, or follows more joints than its pose has is where
  it was modelled - so that what is clicked is what is seen. A selected animator shows
  its skeleton: joints are no entities, and that is the one place they are seen.
- **Animation is tested with numbers.** `tests/support/SkinnedModelSupport.h` is a bar
  of three joints whose every pose can be worked out by hand: half way through "Bend"
  its middle joint is turned 45 degrees. `tests/render/SkinningTests.cpp` says which
  pixels the bent bar covers, and that a bar at rest is drawn bit for bit like the
  mesh without a pose.
- **A sequence is played while time passes, and writes the world** (`SequencePlayer`,
  `SequenceSystem`): `Engine::updateWorld` with a running clock plays the sequences
  after the scripts, which start them by setting `playing`, and before the simulation,
  which sets out from where they put things. Unlike an animator, a sequence writes
  components - transforms, and any property the reflection describes, through
  `TypeInfo::parseEdit`, held to its range - so a stopped clock, an editor's, plays
  nothing and poses nothing: that would change the document behind its history.
  Players are walked in the order of the hierarchy, so of two that set the same thing
  the later has the last word. A player that plays puts what its tracks say at its time
  into the world in every update; one that does not loop stops itself at its end, as
  its last keys leave things, and nothing that is not playing writes anything. Whole
  numbers and yes or no step from key to key whatever the track says. The events of an
  update are those from where the time was, included, to where it got, excluded - and
  at an end, those at the end: an event is passed once, and a long frame passes every
  event of a sequence that loops once at the most. A time a script sets is where the
  sequence goes on from, and what it was set past is not told. The paths of a track
  set out from the player's entity, or from the roots when they begin with '/'. What
  cannot be played - a path that leads nowhere, a component or property there is
  none of, values of the wrong kind, a read-only property - is said once and plays
  nothing; the other tracks play. A transform track moves an entity with a kinematic
  body, which follows; a dynamic one would be put back every frame, against the
  simulation.
- **A cut is the player's to say, and the game's camera follows it**: the system writes
  the camera a sequence's camera track is at into `SequencePlayer::camera` (transient,
  read-only), and none where nothing plays; `findGameCamera` (`renderer/CameraView.h`)
  is that camera, of the first player in the order of the hierarchy, or else the primary
  one - for `Engine::drawWorld` and the editor's view through the game's camera alike.
  No `primary` is changed, so a cut ends with its sequence and leaves nothing to undo.
  The listener does not follow a cut: what is heard is heard by the `AudioListener`.
  When the scene ends, so does what the system holds (`endScene`).
- **A camera shakes by its trauma, and only its view does** (`CameraShake`,
  `animation/CameraShake.h`). What a scene holds is how the camera shakes - how far,
  how fast, how soon it is still again; the trauma that makes it shake and where in its
  shaking it is are `transient`, a game's: a script raises the trauma when something
  happens, and `shakeCameras` - called by `Engine::updateWorld` with a running clock,
  after the scripts and the animators - lets it fall and moves the phase on. The view is
  moved by trauma squared times Perlin noise of the phase (`cameraShakeOffset`, glm's
  noise, a row of its own for each of the six ways), in the camera's own space and in
  metres whatever the entity's scale. It is applied where the view of a camera's entity
  is made, `makeCameraView(world, entity, aspect)` (`renderer/CameraView.h`), which
  `Engine::drawWorld`, scene captures and the editor's view through the game's camera
  all use: never build what is seen through a scene camera from its world matrix. The
  lines the editor draws for a camera are the one exception, on purpose: they show
  where the entity is, and are made from its matrix. The entity stays where it is, and
  so do its rays and its listener; a shake at rest changes the view not by a bit. The noise goes round after `kCameraShakePeriod` turns without a
  jump, so that the phase keeps the digits a frame moves it by.

## Physics

- **Jolt stays behind `PhysicsWorld`.** No header of the engine shows a type of it, and
  only sources of the `physics` module include `<Jolt/...>` - after
  `cordite/physics/JoltSupport.h`, which brings in the `Jolt/Jolt.h` that every header
  of Jolt needs before it and that clang-format would otherwise sort behind them.
- **Jolt takes numbers for granted.** Everything that reaches it is checked first:
  sizes with volume, masses of more than nothing, positions and rotations made of
  numbers. What can come from a file or a script is refused with an error
  (`createBody`); what only code can get wrong is asserted and then ignored.
- **A simulation is repeatable.** The same bodies, made in the same order and stepped
  by the same times, end up in the same places to the bit - with every compiler, in
  every configuration. `PhysicsWorldTests` pins a fingerprint of a toppling pile;
  when it changes, Jolt or how it is built has changed (`CorditeDependencies.cmake`:
  no instruction sets beyond the baseline, no fused multiply-add), and that is a
  decision to make knowingly. Steps are of one fixed length.
- **A step that runs out of room is reported, not asserted** (`PhysicsStatistics::
  overflowedSteps`, one warning): how many bodies touch depends on the scene, not on
  the code. It is the one assertion of Jolt that is let pass; all others are reported
  as the engine's own.
- **Jolt is built without what it would look for on the machine**: its simulations on
  the GPU want a shader compiler, and are switched off.
- **Entities say what there is, `PhysicsSystem` makes bodies of it.** A `Collider`
  alone stands still; a `RigidBody` is one body made of its own collider and of the
  colliders below it that have no rigid body of their own. Nothing of the simulation
  is kept in the world but the speeds of a rigid body, which are `transient`: bodies
  are the system's, and are found again by entity (`bodyOf`, `entityOf`).
- **Who moves whom, while time passes** (`PhysicsSystem::update`): the simulation
  moves a dynamic body and its entity follows; a kinematic body follows its entity,
  through the steps of the frame, and pushes what is in its way; a static body is
  where its entity is. A dynamic body is put elsewhere by giving its entity another
  transform. What the entity hangs under does not carry the body along: after every
  update - with a step or without - the entity is where its body is, in the world.
  Carried along, a crate that stands on the platform it hangs under would be moved
  twice, once by its parent and once by the friction.
- **While no time passes** (`synchronize`) **the world alone has the say**: every body
  is where its entity is, however the entity came to be there, and nothing is
  written to the world - an editor's document is not the simulation's to change.
- **That an entity was moved is told exactly, never within a tolerance:** its own
  transform, and the matrix of what it hangs under, are compared with what they were
  when body and entity were last brought in line. A tolerance on where things are
  in the world would either take rounding for a movement and keep waking bodies, or
  take a movement for rounding. Where a tolerance is used - bodies that stand still -
  it is against where the body *is*, not against where its entity was a frame ago,
  or an entity that creeps is never followed.
- **A kinematic body is moved in steps only.** A frame without a step - most frames,
  on a fast display - leaves it where it is, and the next step takes it all the way.
  Put ahead at once it would be inside what it is to push. Its speeds follow from
  how its entity is moved; only a dynamic body takes the speeds its `RigidBody` is
  given.
- **A body is turned as `Transform::fromMatrix` finds its entity turned**, and that
  function puts a mirroring into the X scale. How large an entity is in its body
  must be worked out to match (`foldedScale`), and the rotation written back must
  leave the entity's own scale as it is (`rotationOf`): otherwise the collider of
  an entity mirrored along Y or Z is on the wrong side of it, and the entity turns
  over the first time its body moves.
- **What is not made of numbers stops at the system.** An entity whose transform is
  none leaves its body where it was and is named once in the log; a place that
  cannot be worked out - under something scaled to nothing - is not written.
  `PhysicsWorld` asserts on such values, and a script must not be able to get there.
- **What a body is made of must be the same numbers every frame**, because a body is
  made anew when they change. Shapes are therefore worked out from the entities' own
  transforms - the scales multiplied down the hierarchy, the transforms from the body
  to its colliders - and never from world matrices taken apart, which differ in the
  last digit as a body turns: that remade every turning body every frame, and a test
  tumbles a body for two seconds to keep it from coming back.
- **The engine simulates, the editor does not.** `Engine::updateWorld` steps the
  simulation with a running clock, in steps of a fixed length (`FixedTimestep`), and
  only brings bodies in line with a stopped one - so that rays still hit what is in a
  scene that is being edited, and nothing in it falls.
- **Colliders are seen in the editor** (`viewport.show`, View > Colliders; those of
  what is selected and of what hangs below it, always), drawn from the components by
  the same rules of scale the physics uses. A new shape needs its lines there too.
  Those made of meshes are drawn from what the physics has made of the mesh
  (`ColliderOutlines`, `PhysicsWorld::geometryEdges`), not from the mesh: what is
  seen is what things collide with. A trigger is drawn in a color of its own.
- **A shape of a mesh is geometry, prepared once.** `PhysicsWorld::createConvexHull`
  and `createTriangleMesh` make it and hand out a `GeometryId`; a `ShapeDesc` holds
  that id and a scale, never points - what a body is made of is compared every frame.
  `PhysicsSystem` makes geometry of a mesh asset when a collider first asks for it,
  keeps it by mesh and shape for every collider at every scale, and forgets it all
  when `AssetDatabase::revision()` changes. A collider of which no shape can be made
  is left out of its body and named once; `findPhysicsProblems`, which
  `scene.validate` reports, finds what can be told without making anything.
- **What touches is said after a step, not asked for**: `PhysicsWorld::contactEvents`,
  and for entities `PhysicsSystem::events` after every update - began, ended, entered
  a trigger, left it. Jolt speaks of two bodies once for every pair of their shapes
  that meet, in every step, and calls a contact removed when a body falls asleep;
  `ContactRecorder` makes of that what is wanted: each pair of bodies once, and two
  that sleep against each other touch on. It does not listen to `OnContactRemoved`
  at all - two have parted when a step passes without word of them although one is
  awake. Events are sorted by the places of the bodies, since Jolt hears of contacts
  in the order its jobs happen to run.
- **A trigger is a sensor: a kinematic body of its own, in a layer of its own.**
  Kinematic, because a sensor that stands still loses what falls asleep in it; of
  its own, because Jolt has sensors by the body, not by the shape - so a collider
  with `trigger` never joins the body it hangs under, and an entity can have a body
  and a trigger (`bodyOf`, `triggerOf`). In its layer it is checked against what
  moves and nothing else: not against what stands still, which would be told of in
  every step, and not by rays, casts and overlaps, which look for what can be run
  into.
- **A body that is made anew is another body to the simulation**, which says that
  the old one has stopped touching things and the new one has begun to.
  `PhysicsSystem` takes both out where they are about the same two entities
  (`dropEchoesOfRemaking`): changing a mass must not sound like an impact.
- **A joint is measured by the body that is joined against what it is joined to**:
  how far a hinge is turned, how far a slider has slid (`JointDesc::first` against
  `second`). Jolt measures its second body against its first, and wants the one that
  does not move to be the first - so `makeConstraint` hands it the two the other way
  round. A test with limits that are not the same both ways pins the direction.
  The points and axes of a joint are given in the space of each body, and turned
  into Jolt's, which are against the centre of mass.
- **A `Joint` component holds its two entities as they stood when it first did.**
  That is found once, in the first update that can make the joint, and kept by the
  system as one matrix: from the space of the entity to that of the entity it is
  held to (`JointRecord::restToConnected`). The anchor, the axis and the limits of
  the component are all said of that moment, and the joint is made of that matrix
  every time it is made again - a body was made anew, an entity came to be part of
  another body, the component was given other values - never of where things are
  just then. Made of where things are, a door that is given another mass or other
  limits while it stands open would count them from there, and a spring that is
  stretched would take that for its length. Only another entity to hold to begins
  again. An editor, in which no time passes, has no joints: what will not work is
  found by `findPhysicsProblems`.
- **A point of an entity is turned into the space of its body by way of the
  hierarchy** (`entityToBody` in `PhysicsSystem.cpp`), not by where the body is in
  the simulation: a kinematic body is a frame behind its entity until the step has
  moved it, and a joint made of where it is just then is a frame's way off. Where
  bodies are is asked only when two are joined for the first time, to find how they
  stand to each other at one moment.
- **A joint holds on to its bodies.** `destroyBody` takes the joints of a body away
  first, and their ids are then no joint's; whoever keeps a `JointId` asks
  `isValid`. What a joint held is woken when the joint goes, and when it is made:
  a sleeping body does not feel that it is held, or let go of.
- **Bodies that are joined do not collide with each other** unless the joint says so
  (`JointDesc::collide`): every body is a collision group of its own, and
  `JoinedPairFilter` keeps the pairs that joints hold, counted, since two bodies may
  be held by several. A hinge or a slider that is limited to where it is and does
  not give is refused - Jolt asserts on it, and it is a fixed joint.
- **A sleeping body notices nothing by itself.** Jolt looks at one only when a body
  that is awake comes near - and a body that is destroyed, put somewhere else or
  standing still never is. So `destroyBody`, `setPlacement` and the making of a
  static body wake what sleeps where the body was and where it is
  (`wakeWhatIsIn`): without that a crate stays asleep in the air when the shelf it
  lay on is gone. Anything new that takes a body away, or puts one down, must do
  the same.
- **What is where is asked of `PhysicsWorld`**: `raycast`, `sphereCast` - a ball
  moved along a ray - and `overlapSphere`/`overlapBox`. Rays are exact; what is done
  with a shape meets a box whose edges are five hundredths of a metre round
  (`kBoxEdgeRadius`), so tests of casts at edges need that much tolerance. Overlaps
  are sorted by the place of the body in the world, never by address or by what
  Jolt happens to find first. In the editor the commands `physics.raycast` and
  `physics.overlap` synchronize first, so the answer is about the scene as it is.
- **The triangles of a mesh have no inside**, and so no mass: a body of them is
  static or kinematic, never dynamic (`createBody` refuses; a kinematic one is given
  any mass, which Jolt asks for and never uses). They stop what comes from the side
  they face - counter-clockwise, like everything the engine draws - and a ray, a
  ball moved along one and an overlap find them from either side. What is to be
  moved by the simulation is a convex hull.
- **A mesh made for drawing has two vertices wherever a seam or a sharp edge runs.**
  A surface made of it is welded first (`createTriangleMesh` does, with Jolt's own
  `Indexify`): triangles that are not known to share an edge are taken for the end
  of the surface there, and what slides across it catches on every one.
- **A character is no body.** It is Jolt's `CharacterVirtual`: a capsule that is
  moved through the world by asking what is in its way, in `PhysicsWorld::step`
  before the bodies, in the order the characters were made. It does not fall over,
  bounce or slide by itself, which is what one wants of something that walks. All
  of it is in `CharacterSet` (`CharacterSet.h`, for the module alone).
- **How a character's speed comes about is the engine's to say**, in
  `CharacterSet::step` - Jolt moves a character at the speed it is given, and gives
  none. A character that stands has the speed of what it stands on and of its walk,
  and is *not* pulled at by gravity: pulled at, it creeps down every slope it
  stands on, and drifts downhill when it walks across one. One in the air falls, and
  goes sideways as it left the ground as far as `airControl` leaves `walk` no say -
  a share of the two speeds, not a pull towards one, so that it does not depend on
  the length of a step. Tests pin each of these; take one out and see which fails.
- **Jolt leaves the speed of a character as it was given**, whatever stopped the
  character. `CharacterState::velocity` is therefore worked out from where the
  character got to; and the speed it rises or falls with is taken from it where it
  did not rise or fall at all, or it hangs under a ceiling until gravity has used
  its jump up.
- **Jolt keeps the shape of a character a little away from everything**
  (`kCharacterPadding`), and puts it that far above where the character is. The
  shape is made smaller by as much all round, with its lowest point where the
  character is: then what is kept out of the world is as large as was asked for,
  and the soles are on the ground. Make it smaller *and* raise it, and the
  character stands two centimetres deep in the floor.
- **A body stands for each character** (`characterBody`): kinematic, a tenth
  smaller, kept where the character is by Jolt. It is what rays find, what
  triggers notice and what contact events name - a character itself is in no broad
  phase. It is the character's: `destroyBody` refuses it, and placing it or giving
  it a speed is ignored.
- **A `CharacterController` makes its entity a character, and nothing else.** The
  entity is where the character's soles are - the simulation writes its position,
  and leaves how it is turned and how large it is to whoever walks it; putting the
  entity somewhere else puts the character there, and turning it does nothing. What
  the component is told (`movement`, `jump`) and what the simulation says of it
  (`velocity`, `ground`, ...) are `transient`: no scene holds them. `jump` is taken
  from the component by the update that passes it on, so that a jump asked for once
  is tried once, also in a frame without a step.
- **A character is no body, and has none of its own.** An entity with a `RigidBody`,
  or a `Collider` that is no trigger, is that body and no character; and a collider
  that hangs on a character without being part of a rigid body is left out - it
  would stand still, be carried about, and be in the character's way wherever that
  goes. Both are reported once by the system, and found by `findPhysicsProblems`
  without running anything. Triggers and rigid bodies may hang on a character.
- **Bodies and characters are written back in one walk**, each after what it hangs
  under (`PhysicsSystem::writeBack`, by the `place` each had in the hierarchy): an
  entity is placed against its parent, which may be of either kind and may itself
  just have been moved.
- **A trigger says nothing of what it hangs on**: the body or the character of its
  own entity, or of an entity above it, is in it for as long as it is there, and
  `PhysicsSystem::events` leaves that out.
- **What lies on a character rests on its body, and is nothing to the character**
  (`CarriedBodyFilter`). Between a contact that presses down and ground that
  presses up Jolt lets a character go nowhere: a crate on its head would pin it to
  the spot. And Jolt puts that body where the character has gone without waking
  anything, so `PhysicsWorld::step` wakes what rested where it was - or what
  slept on a character that left in one step would go on sleeping in the air.
- **Jolt keeps a shape against its centre of mass.** The points of a hull and the
  bounds of any shape are relative to it; add `GetCenterOfMass()` to be back in the
  space the geometry was given in. Where a body *is* (`position`) already accounts
  for it. And around points that lie in one plane Jolt makes a hull without
  complaint that has no volume - `createConvexHull` refuses it, or a body of it
  would have a mass of nothing spread over nothing.

## Audio

- **A source says, the system plays.** `AudioSource` is plain data: which sound, on
  which bus, how loud, whether it has a place. `AudioSystem::update` gives every
  enabled source that is `playing` a voice and keeps the voice like the source. The
  one thing it writes back is `playing = false`, when a sound that does not loop has
  ended - so a scene file says what plays when the scene starts, and setting
  `playing` is how anything is played. Sound without an entity is
  `AudioSystem::play`.
- **miniaudio stays inside `AudioSystem.cpp`** (and `assets/Sound.cpp`, which decodes).
  Mixing, placing in space and the devices are its; the engine's types never show
  its own. What it is told is told once: `apply` sends a voice only what changed,
  because miniaudio smooths what it is told and begins again when told twice.
- **Where the mix goes is chosen when the system is made** (`AudioOutput`), and the
  default is nowhere. `Offline` has no device and no thread: `update` mixes as many
  frames as it is told time has passed, the same every run, and `mixed()` is what
  came out - which is how tests hear, and why an `Engine` that is not asked for a
  device opens none. `Device` is what a game asks for; a machine without sound
  gets `Silent`, a device that plays to nobody in real time, and a warning. No test
  opens a real device.
- **Sound is tested with numbers.** A sound whose samples are all 0.5, or count their
  own frames, played into memory, says exactly what was heard: which ear, how loud,
  from which frame on (`AudioSystemTests`). What miniaudio computes about a place -
  a distance - it computes to a part in a thousand, and it slides to where a source
  has moved over several hundredths of a second: compare with `about()` and listen
  with `settled()`. A sound in two channels or without a place is exact, at once.
- **How a place is heard is miniaudio's rule**, written down in `AudioSystem.h` and
  pinned by the tests: a source straight ahead is half as loud in both ears, one at
  a side as loud as it gets in that ear and a fifth of it in the other. Distance
  is the rolloff's: `inverse` halves at twice `minDistance` and never reaches
  nothing, `linear` reaches it at `maxDistance`.
- **While time does not pass nothing of the world is heard.** `Engine::updateWorld`
  with a stopped clock calls `AudioSystem::pause`, which holds every voice of the
  world where it is; a scene that is replaced has its voices stopped (`changeScene`,
  after the scripts have ended). That is why an editor is silent until it plays its
  scene.
- **A preview is for whoever is at the editor**, not for the world
  (`AudioSystem::preview`; `asset.listen`; Play beside the rows of the Assets panel):
  a sound of the assets, once, without a place, in a group of its own beside the
  world's. What holds, ends and turns down the world - `pause`, `clear`, the buses,
  the volume of a listener, which is why that is set on the world's group and not on
  everything - leaves it alone; only the volume of everything applies, and it is no
  voice of the world's. `pause` takes the time of the frame, and an `Offline` system
  mixes that much of a preview while the world is held.
- **Scripts have two ways to be heard, as C++ has.** What an entity sounds like is its
  `AudioSource`: `entity:set("AudioSource", "playing", true)` plays it, and no
  function was added for that. `cordite.audio.play` is for what belongs to no entity;
  it gives nil, not an error, when no voice is free - too much sound is nothing to
  stop a script for - and an error for what is a mistake: a sound that is none, an
  option that is spelled wrong. The showcase shows both in `hop.lua`.
- **A voice is a limited thing** (`AudioSystemDesc::maxVoices`). Sources get theirs in
  the order of the hierarchy, and those that find none wait and are counted;
  `play` fails. A sound that cannot be loaded is tried once, said once, and not
  read again until the assets are refreshed.
- **The sounds of the showcase are generated**, like its textures
  (`tools/sample-assets`): written as formulas, begun and ended at nothing, and
  where they loop made of whole cycles. Nobody can listen in a test run, so look at
  what was generated in numbers before committing it - peak, first and last
  samples, the step across the seam of a loop.

## Input

- **A game asks about its controls by name.** `InputMap` (`platform/InputMap.h`) says
  which keys, buttons and sticks press an *action* ("jump") and move an *axis*
  ("moveX"); `InputActions` works both out of the `Input` of the frame. Game code
  asks `Engine::actions()`, and scripts `cordite.input`, which is the same thing -
  never `Input` for a key. `Input` is for what has no name in a game: the editor's
  own interface.
- **The controls of a project are one file**, named by `inputMap` in
  `project.cordite` and read as strictly as a scene: a member that is not
  known, a key that has no such name or a binding that names two controls is an
  error that says which action, and which binding. A file is written with
  everything in it, so that reading and writing it changes nothing.
- **Controls are worked out once a frame**, in `Engine::beginFrame`, after the events
  of the frame are in: everything that asks during a frame gets the same answer.
- **A tap counts.** A key that goes down and comes up between two frames was
  pressed and released - `Input` remembers both, and so does an action. A gamepad
  only says what it is like now.
- **A map that was just taken does not know how things were**: what is held then was
  not pressed for it. Only a key or a mouse button that goes down in its first frame
  was, because those say so themselves.
- **An axis has the value of the binding that is moved furthest**, not the sum: a
  stick and the keys beside it do not add up to twice the speed.
- **Whatever a hand can do, `input.inject` can**, between frames - which is how tests
  and agents press keys. It reaches `Engine::input()`, and without a window the
  pointer of the editor's interface follows it as well - its keys do not: a shortcut
  of Dear ImGui (W, E, R, Ctrl+Z) is pressed in a test with `ImGuiIO::AddKeyEvent`, by
  `pressShortcut` (`tests/render/EditorTestSupport.h`) - which holds the key a person
  holds for Ctrl, `controlKey()`: on a Mac Dear ImGui takes Cmd for Ctrl, and swaps the
  two as they come in. That includes dropping files onto
  the window from a file manager (`Input::droppedFiles()`, for one frame), which the
  editor imports.
- **The showcase binds every kind of control**: the `samples` test fails for a kind
  of binding that its input map does not use.

## Scripting

Gameplay scripts are Lua 5.4: `.lua` assets that a `Script` component puts on an
entity, run by `ScriptSystem` (`scripting/ScriptSystem.h`) in a `ScriptRuntime`
(`scripting/ScriptRuntime.h`). The scripts under `samples/showcase/assets/scripts/`
are the reference for how one is written: `clock.lua` for what a script is made of,
`patrol.lua` for one that moves something, `selftest.lua` for every function there
is, called once and checked. Scripts reach entities and their hierarchy, transforms,
every component through its properties, vectors and rotations, the time and the ids
of assets, the controls of the game, and the simulation - rays, what is within
reach, gravity, pushes - which tells them what meets what, they have timers and
tasks, they ask for another scene, they start sequences and are told what those pass,
and they are heard: an entity's `AudioSource` is a component like any other, and
`cordite.audio` plays what belongs to no entity and sets how loud the buses are.
Step-by-step guide for giving scripts a function:
[add-script-function](.claude/skills/add-script-function/SKILL.md).

- **A script returns a table**: the functions the engine calls - `start`,
  `update(deltaSeconds)`, `destroy` - and `properties`, what a `Script` component
  may give other values of the same kind. Every entity with the script has a table
  of its own, `self`, with `entity` and `properties` in it; the functions are found
  through it.
- **Instances follow the components; nothing listens.** Every update the system walks
  the hierarchy and brings its instances in line with the `Script` components it
  finds, as the physics does with bodies: a component that says something else -
  another script, other properties - gets another instance. `start` and `update` are
  called in the order of the hierarchy, and nothing of an entity that is switched
  off.
- **Scripts run while time passes**: in `Engine::updateWorld()` with a running clock,
  before the simulation, so that what a script decides in a frame is what the
  bodies do in it. An editor, whose clock is stopped, runs none.
- **An update is four walks**: `start` for what has not started,
  `input(action, pressed)` for every action that was pressed or released in the
  frame, the timers and tasks that are due, `update(deltaSeconds)` - three through
  the hierarchy, and the timers in the order they were made. Each asks again whether
  an instance still runs (`Impl::running`): the call before may have stopped it or
  switched its entity off - or on, and nothing of an instance is called before its
  `start` has been through.
- **A timer or a task belongs to the instance whose script was being called when it
  was made** (`Impl::calling`): it is called for that instance, waits while the
  instance does not run and goes when the instance ends - also when the same entity
  gets another, and when its `start` fails: the start that is tried again asks
  again. Made where no script of an entity is being called - while a file is
  loaded - it is an error. A script may have 1024 waiting, which is what tells the
  one that makes a timer in every frame.
- **What a walk makes waits for the next**: a timer made by a timer, with no time
  to pass, is not called in the same update, or `after(0, again)` would never end.
  What repeats is due again an interval after it was *due*, and a long frame calls
  it once, not once for every interval it spanned.
- **A task is a coroutine the system resumes**; `cordite.time.wait` yields how long,
  and is refused anywhere else - in a coroutine the script resumes itself nobody
  would wake it. A task runs at once as far as its first wait, inside the call that
  started it, so an error there is an error of that call; later it stops the
  instance, with the place the task was at.
- **What is cancelled or done is marked, and let go of in a sweep**
  (`Impl::sweepTimers`) once no script runs: the task that cancels itself is still
  running when it asks, and Lua's registry is what keeps its coroutine from being
  collected under it.
- **What a sequence passes is told to the scripts of its player at once**
  (`ScriptSystem::tellSequenceEvents`, given `SequenceSystem::events()`): right after
  the sequences have played, which is after the scripts' update and before the
  simulation - so that what a script does about an event is in the step of the same
  frame. `sequenceEvent(name)` is called for the entity whose `SequencePlayer` played
  the sequence, in the order of the events.
- **After the simulation has moved on, scripts are told what met and what parted**
  (`ScriptSystem::tellContacts`, given `PhysicsSystem::events()`):
  `collisionBegan(other, contact)`, `collisionEnded(other)`,
  `triggerEntered(other, contact)`, `triggerLeft(other)`. Both entities of an event
  hear of it, the first and then the second, each with the other - which may be gone
  by then, and is still the value the script knew it by - and to the second the
  `normal` of the contact points the other way: it is always the way from the one
  that is told to the other. What parts has no contact to tell of.
- **The functions the engine calls in a script are one list** (`Callback` and
  `kCallbacks` in `ScriptSystem.cpp`), each with how a script writes it and what it
  is for. Being on it is what makes a function found when a script is loaded -
  which is also what refuses `input = 5` - counted, and said to whoever asks
  (`ScriptSystem::callbacks()`). A new one is an entry there, the place that calls
  it, its lines in `ScriptSystem.h`, and a script of the showcase that has it: the
  `samples` test runs the showcase with its simulation and names every function on
  the list that was never called.
- **What scripts are written against can be asked for**: the editor command
  `script.api` returns the functions with their signatures and descriptions and the
  callbacks with theirs - the catalogue itself, so it cannot be out of date. That is
  why the words in a `ScriptFunction` and in `kCallbacks` are written for someone
  who has not read the engine.
- **What the scripts take of a frame is measured** (`FrameStatistics::
  scriptMilliseconds`: their update, and what they are told after the simulation),
  shown under View > Statistics, returned by `editor.statistics`, and budgeted in
  the performance suite at its middle and at its worst.
- **Scripts ask about controls by the names of the input map**, and a name the map
  does not have is an error that lists the ones it has - where `InputActions`
  itself answers "never pressed". A typing mistake in a script, or a control that
  was renamed in the map, is found the first time the line runs.
- **Whoever runs scripts gives them what they ask about** (`ScriptContext`): the
  world, the assets, the controls. The engine passes its own; a test that runs the
  scripts of a project gives them the project's input map, as a game does. A world
  that is given no controls tells a script that asks so - it does not pretend that
  nothing is pressed.
- **A script does not destroy; it asks.** `entity:destroy()` is
  `ScriptSystem::destroyLater()`: the entity goes once the scripts of the update
  have run, after the instances in it had their `destroy`, children first. An
  entity that anything else destroys takes its instance along without a call.
- **An error stops the instance it happened in**, and is said once: in the log and in
  `ScriptSystem::problems()`, with the entity, the file and the line. A script that
  cannot be loaded is one problem however many entities name it.
- **Scripts are loaded again when the assets are refreshed** and their text is another.
  Instances keep their tables and get the new functions - one metatable per script
  leads them there - and those that an error stopped are given another go. Text
  that cannot be loaded leaves the old running.
- **An entity is one value to scripts**: the same value every time for the same
  entity (`pushEntity`), with `cordite.Entity` as its methods. It outlives its
  entity; `checkEntity` raises for one that is gone, and `isValid()` is how a script
  asks first. A function that scripts call finds the world through `checkContext`,
  which is only there during an update (`scripting/ScriptBindings.h`).
- **A reference to a component does not outlive a call into a script**, nor the
  loading of one: what runs there may add and remove components. Copy what is
  needed first.
- **Components reach scripts through the reflection, and only through it.**
  `entity:get`, `set`, `add` and `remove` (`ScriptComponentApi.cpp`) read and write
  what `TypeBuilder` describes, and every write goes through `TypeInfo::parseEdit`:
  the kinds, the ranges, what is read-only and the all-or-nothing of an edit hold
  for a script as they do for the editor. A new component, and a new property, is
  scriptable without a line written for it. On the way a `Vec3` becomes a
  `cordite.Vec3`, a `Quat` a `cordite.Quat` and a reference to an entity the entity.
  What a script changes is not undoable: scripts run in a game, not in a document.
- **Writing back what was read changes nothing.** `set` leaves out what has the value
  it had before it asks `parseEdit`, so a script can read a component, change one
  thing in the table and hand all of it back - whatever else is in there: a read-only
  property, which is only an attempt with another value, or free-form data, of which
  no Lua table can hold a member that holds nothing or the order of the members:
  both would otherwise be lost to a script that never touched them. "The value it
  had" is therefore judged as a script sees values (`asScriptsHandBack`), not as
  JSON compares them.
- **A function is written for what is no property**: a question to a system, an
  action, a computation. `entity:position()` and its like exist because a script
  moves things in every other line, not because `get("Transform")` could not.
- **A vector is a table with `x`, `y` and `z`**, a rotation one with `x`, `y`, `z`
  and `w`. Those the engine makes have `cordite.Vec3` and `cordite.Quat` as their
  metatables; wherever one is expected a plain table will do, with the names or with
  the numbers in a row (`{0, 1, 0}`). They are values: what a script is handed is a
  copy, and changing it changes nothing until it is given back. They are read
  without asking a metatable (`readNumbers`), so that nothing of a script runs in
  the middle of a function of the engine.
- **Scripts compute in doubles; the engine keeps floats.** The arithmetic of
  vectors and rotations stays in doubles from end to end. A float crosses into Lua
  as the double that is written the shortest (`scriptNumber`, `pushVec3`,
  `pushQuat`) - 0.1, not 0.10000000149011612 - which is the number the reflection
  writes into files. On the way back `checkVec3` and `checkQuat` refuse what is not
  finite as a float: no script puts an entity at "not a number".
- **A list of entities is in the order of the hierarchy**, never in that of a
  registry: what a script does to "the first" is the same on every run. What the
  simulation finds (`overlapSphere`) is in the simulation's order, which is the
  same every time as well.
- **To a script a body is the entity that has it.** A ray names the entity it hit,
  an overlap lists entities, and what is pushed is an entity (`entity:addImpulse`).
  A body whose entity is gone - destroyed since the simulation last looked - is no
  entity: `nil` in a hit, and not in a list.
- **Scripts see the simulation as the last frame left it.** They run before it
  moves on, so an entity a script has moved in this frame is found where it was,
  and one that was only just given a collider has no body until the next frame -
  there are none at all in the frame a game starts with. A function that needs a
  body says so when there is none; it does not wait, and it does not do nothing.
- **Velocities are properties, pushes are functions.** How fast a body moves is in
  its `RigidBody` and set through `entity:set`; a force, an impulse and a torque are
  not states of anything, and have functions.
- **A scene changes between two frames, and only after it has been checked.** A
  script asks (`cordite.scene.load`, which is `ScriptSystem::requestScene`); whoever
  owns the world takes the request when the frame is over and calls `changeScene`
  (`engine/SceneChange.h`) - the engine does in `beginFrame`. That reads the file and
  tries it on a world of its own first, so that a scene that is wrong costs the one
  that runs nothing; then the scripts end while their entities are still there to
  say `destroy` to, bodies and poses are forgotten - the new entities may have the
  very handles the old ones had - and the scene is put into the world. Anything else
  that is kept per entity of a played world has to be let go of there. The clock of
  the scripts starts again with the scene.
- **What scripts are given is pinned by tests**: every function has a signature and
  a description (the catalogue, `ScriptRuntime::functions()`), and what each library
  holds is listed, or counted, in the tests of that library. A function that is
  added or taken away changes a test.
- **The showcase tests the script API from inside the game.** Its
  `scripts/selftest.lua` calls every function scripts are given and compares what
  comes back with what should, in the first frames of the running scene, with real
  components and the simulation beside it; an answer that is not as expected stops
  it, which is a problem of the scene and fails the `samples` and engine tests. The
  `samples` test also names every function no script of the showcase has called -
  `cordite.log.warn` and `error` excepted, which a scene where nothing is wrong
  must not call - and checks that the scripts left every component of the scene as
  it was, to the digit: the self-test hands each of them to Lua and takes it back.
  A new function is called and checked there in the commit that adds it.

- **A script that is wrong is an error with a message, never the end of the
  program.** That is what the runtime is for, and each of the rules below has a test
  that tries to get past it (`ScriptRuntimeTests`). A change to the runtime that makes
  one of those tests hang has opened the hole the test is there for.
- **Lua's API is used inside `ScriptRuntime::protect()`, or in a function that a
  script calls - nowhere else.** An error raised anywhere else has nowhere to go, and
  Lua ends the program. Out of memory is such an error, and a script can bring it
  about whenever it likes.
- **Lua is built as C++**, so that an error leaves the engine's functions the way an
  exception does and their destructors run. Include it through
  `cordite/scripting/Lua.h` only - never `lua.hpp`, which declares everything a C
  function; the `module_layering` test rejects a direct include. Its compile-time
  settings are in `scripting/LuaConfig.h`, which `lua.h` includes itself.
- **Take what is needed out of the engine first, then call the script.** An error
  unwinds whatever is on the stack; work of the engine that is half done when a
  script is called stays half done.
- **A call into the scripts has three limits.** Instructions: ten million for the
  outermost `protect()` and everything inside it; counting them costs a script that
  only computes about four tenths more time, and `instructionLimit` 0 turns it off.
  Memory: 256 MiB for all scripts together. Nesting: 64 calls inside each other that
  go through C - `pcall`, a metamethod, a function of the engine that calls back -
  because each takes up to 6.6 kB of the program's stack in an unoptimized build and
  a Windows main thread has 1 MB. A test measures what the deepest nesting takes, and
  fails long before a crash would.
- **The error that stops a script which does not end cannot be caught by it.**
  `pcall`, `xpcall` and `coroutine.resume` are the runtime's own and pass it on, and
  `xpcall` does not ask the script's handler about it. Whatever else is ever given to
  scripts that catches errors has to pass it on as well.
- **Scripts have no finalizers.** Lua counts no instructions while `__gc` runs, nor
  while an error that the counter raised is on its way; script code must not run in
  either place.
- **What a script cannot reach is not in the build**: files, the operating system,
  loading of libraries, the debug library. `load`, `dofile`, `loadfile`,
  `collectgarbage` and `string.dump` are taken out, and only text is compiled.
- **Every chunk has an environment of its own**, and a name that was never given a
  value is a mistake: reading one is an error that names it, and so is assigning to
  one after the chunk has run. `rawget(_G, name)` and `rawset(_G, name, value)` are
  for whoever means it.
- **Scripts run the same way twice**: the seed of Lua's string hashes is fixed, so
  `pairs()` walks tables of names in the same order every run, and `math.random()`
  starts from the same seed. The first numbers are pinned by a test. And the same on
  every machine: where Lua would lean on the C library of the platform - numbers in
  hexadecimal - it is told to use its own code (`LuaConfig.h`).
- **A function for scripts is registered with `ScriptRuntime::registerLibrary`**,
  with its signature and a sentence that says what it does: the runtime keeps the
  catalogue, and counts the calls. It may raise Lua errors and throw what derives
  from `std::exception`; a script sees both as errors. Arguments are checked with
  Lua's own `luaL_check...`, whose messages name the function and the argument.
- **Values cross as JSON** (`scripting/ScriptValue.h`), which is how the engine
  describes its data anyway. `null` is `nil`, but `cordite.null` inside an array; a
  table is an array when its keys are 1 to n and an object when they are names, with
  its members sorted; anything else is refused with where in the value it is.
  Whatever else reads a table of a script takes the same view of it
  (`scriptListLength`): a table with names in it is never the list of what happens
  to stand at its positions.
- **A table of a script may hold itself.** Whatever follows tables into each other
  counts how deep it is and stops (`kMaxScriptValueDepth`); the types of the engine
  do not bound it - a list of lists says "a table" of every depth.

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
- **What is copied is a snapshot, kept by the document** (`EditorContext::clipboard`):
  the entities as `serializeEntities` writes them, which outlives what it was taken from
  and the scene, and is pasted with ids of its own (`EditorActions::pasteEntities`).
  It is the editor's, not the system's: a test or an agent that copies changes nothing
  outside the editor.
- **Everything a person can do is a command.** A new editor feature is an action, a
  command and their tests first, and interface second. `editor.commands` returns the
  catalogue an agent reads, so every command and parameter needs a real description
  (a test rejects empty ones).
- **Undo takes back what was changed in the scene, not a file that was written.**
  `prefab.create` writes a prefab and reads the assets again, and neither is undone:
  what undo takes back is the mark it put on the entity. A command that writes a file
  among the assets says so in its description, keeps the path inside the asset
  directory (`relativePathWithin`), refuses to write over a file unless it is told
  to, and is refused while the scene is played.
- **History entries hold ids and validated patches**, never `Entity` handles, component
  pointers or references into the world.
- **A panel that shows more than what is there is a model and a window.** What the
  Assets panel shows - the tree of folders, the folder shown and what it has in it,
  the filter, what is open, what is chosen, the list or the grid - is `AssetBrowser`:
  no ImGui in it, tested without a window, and asked by the command `assets.browse`,
  so that what a person sees is what a script is told. `AssetBrowserPanel` draws it
  and turns clicks into command lines (`placeAssetCommand`), which it hands on as the
  console does. Build the next panel the same way: what it decides is tested where no
  pixel is involved.
- **The Assets panel is laid out as the browsers of Unity and Unreal Engine are**: the
  tree of folders at the left (`folders()`), what the folder chosen there has in it
  beside it (`contents()`) - its directories, then its files, what is inside a file
  after it once its arrow opens it - as a grid of tiles or a list, with the way to the
  folder above, each step a button back up (`folderSteps`). A folder is a key
  (`AssetBrowserRow::key`), not a path: what the engine has built in is a folder
  (`kBuiltinFolder`) beside a project's directory that may have its name. A search
  looks in the folder shown and below it, and lists what it finds side by side; the
  tree is not narrowed by it. A folder that is gone - moved, emptied - shows the
  nearest above it while it is gone (`shownFolder()`), as a chosen asset stays chosen.
  Choosing an asset from outside the panel shows the folder it is in (`reveal`). The
  grid or the list is a person's choice, kept in the editor's preferences
  (`EditorPreferences::assetView`), and a batch run has the grid. A double click on
  what can be placed - a model, a prefab, a mesh (`canBePlaced`) - places it; a
  directory is gone into by one.
- **What is dragged from the Assets panel goes where it is dropped** (`AssetDrop.h`).
  What can be placed hangs under the entity of a row of the hierarchy it is dropped on,
  is a root below the rows, and onto a viewport is a root at the point under the pointer:
  where the line of sight first meets a mesh as it is drawn (`raycastMeshes`), or else
  the ground grid, as far as the grid is drawn, or else as far ahead as the camera
  looks - its origin there, not its bounds. A material dropped onto a mesh in a
  viewport goes to the submesh under the pointer, the one entry of the `MeshRenderer`'s
  materials; it is not taken where there is no mesh, nor by the hierarchy, whose rows
  say nothing of which part. A drop is a line the viewport queues
  (`viewportDropCommand`: `asset.instantiate` with a `position`, `component.set`), so
  that what a hand does by dragging an agent does by name; a viewport that looks
  through the game's camera takes none. A mesh placed on its own is drawn as its model
  draws it (`materialsOfMesh`).
- **What the user asked for and did not get is said in the console, and the console
  comes to the front for it.** A failed edit is logged with `reportIfFailed`, a
  failed line by the console - under the two categories `reportsFailedAction` knows -
  and `EditorApp` then brings the console in front of the assets, with which it shares
  its place. An error of the game that runs does not: a game may log one in every
  frame. The newest message of any kind is in the status bar, whatever is in front.
- **The editor is laid out after Unity and Unreal Engine** (`EditorToolbar.h`): a
  toolbar under the menu - save, undo and redo at the left, play, pause and step in
  the middle, play a Stop button while the scene plays - and a status bar - the newest
  message, whether the scene plays, how many entities it has and how many are
  selected. A button is described first
  (`ToolbarButton`: its icon, what it says, the command it queues, whether it can be
  clicked), which is what the tests hold, and then drawn. Panels have icons on their
  tabs, and rows of the hierarchy and of the assets what they are (`EditorIcons.h`).
- **A window is known by what follows `###` in its name** (`windowName`, `kWindowId`
  of each panel), so that its tab can show an icon. ImGui hashes only what follows the
  `###`: the id is also the name the window had before it had an icon, and a layout
  remembered then still places it. A window given another title gets another id, and
  no place in the layouts people have.
- **A project opens with a double click on its file**, `project.cordite` (JSON, like
  every file of the engine). The editor takes a project file by itself on its command
  line, and, started with a window, makes files of `.cordite` open with itself for the
  person who runs it (`FileAssociation.h`; on Windows HKEY_CURRENT_USER\Software\Classes)
  - the build that was started last is the one they open with. A batch run leaves the
  system alone, and tests register types only under keys of their own, which they
  delete. Linux and macOS learn of types from what an installation puts in place;
  packaging is to do that.
- **Commands run between frames.** A command may render frames itself (screenshots);
  lines typed in the console are queued and run before the next frame for that reason.
  A menu item or a shortcut that must not happen in the middle of a frame - starting
  and stopping play - queues its command the same way.
- **A scene is played where it is edited, and put back afterwards** (`PlayMode`,
  `play.start` ... `play.stop`). Playing is the same world with its clock running:
  `EditorApp::runFrame` asks `PlayMode::clock()` what to tell `Engine::updateWorld`.
  `EditorContext::beginPlay` remembers the scene as JSON and sets the history aside;
  stopping ends what runs (`Engine::endScene`) and then reads that JSON back
  (`endPlay`) - so entities have other handles afterwards and the ids they always
  had, which is why nothing in the editor may hold a handle across a frame. What
  the game or an edit did meanwhile is gone, with the history those edits had; a
  scene that is being played is not saved, opened over or made anew. A new piece
  of state that belongs to a played world - something a system holds for it - is
  ended in `endScene`, or it outlives the game.
- **The gizmo moves what is selected** (`Gizmo.h`, `ViewportPanel`): translate, rotate or
  scale (W, E, R, or the toolbar), along the axes of the world or of the entity (X;
  scale always along the entity's own), drawn on the entity selected last, moving every
  selected entity that does not hang under another selected one. Where its handles are,
  which one the pointer is over and what a drag does is arithmetic in `Gizmo.h`, tested
  without an interface; the viewport writes the transforms directly while a handle is
  held and makes one step of the history of it when it is let go
  (`EditorActions::recordComponentEdits`). A handle that is let go of where it was taken
  hold of has changed nothing - not a last digit - and is no step. A drag is an edit of
  `Transform`, which `component.set` makes as well; `viewport.gizmo` says and sets the mode.
- **A viewport looks through the editor's camera or the game's**
  (`ViewportCamera`, `viewport.show {"camera": "game"}`). The game's is the camera the
  game is seen through (`findGameCamera`: the cut of a sequence that plays, or else the
  first enabled primary camera of the scene), drawn as `Engine::drawWorld` draws it -
  the game's background, no default light - and with nothing over it: no grid, no
  markers, no picking, no flying. Playing switches the first viewport to it, and stopping
  switches it back; the others keep their cameras. What an editor adds to the picture
  belongs to the editor's view only.
- **An editor has up to four viewports** (`kMaxViewports`; `viewport.open`,
  `viewport.close`, `viewport.list`; View > Open viewport), each a `ViewportPanel` with a
  number and a window of its own: `###Viewport` for the first, which is always there and
  keeps the place layouts remember for it, `###Viewport 2` and on for the others. Its own
  are its camera - which starts where that of the one used last is - the camera it looks
  through, its render view and its colliders. What a picture costs is not: the quality
  and the gizmo are the editor's (`ViewportSettings`), because the renderer has one shadow
  atlas, made anew whenever its size changes, and the textures are the program's. The
  View menu, F and the commands that name no viewport act on the one used last
  (`activeViewport`): whose window had the focus last - Dear ImGui gives it for a click of
  the left button, the viewport for the others - or that was opened or brought to the
  front (`editor.focusPanel`, "viewport2"); after it is closed, the first. A viewport
  without a place in the layout is split off another before it is first drawn - the
  second to the right of the first, the third below it, the fourth below the second - and
  one that is opened again goes back to where it was: Dear ImGui keeps the dock node of a
  window that is closed, empty and hidden. A viewport that is closed lets go first: a
  drag of its gizmo is a step of the history, and the pointer is unlocked
  (`ViewportPanel::release`).
- **Batch mode is the test and agent entry point:** `cordite-editor --headless --project
  P --exec script.json --frames N --screenshot out.png`. Exit status: 0 success, 1
  could not run, 2 GPU validation issues, 3 a script step failed. The script's outcome
  goes to standard output as JSON, logs to standard error. With `--fixed-time-step
  0.0166667` two runs give the same picture, down to the film grain - and a scene
  that a script plays (`play.start`, `editor.runFrames`) the same game, frame for
  frame. Without a window the editor opens no sound device: what would be heard is
  mixed into memory.
- **The editor works on one project; another is opened by beginning again.** The engine
  is made for the assets and the cache of one project, so File > New project and Open
  project (`project.new`, `project.open`) close the editor (`EditorApp::projectToOpen`),
  and `editor/Main.cpp` makes another on that project - with the frames of a batch run
  that are left, without its script. Both are refused while the scene is played, and the
  commands while it has changes that are not saved, unless told `discardChanges`: a script
  cannot answer the question a person is asked first. `project.open` also refuses a
  project whose controls cannot be read, while this editor is there to say so; a start
  scene that cannot be read keeps no project from opening - the next editor opens it with an
  empty scene, and says why in the console. A scene asked for by name (`--scene`,
  `EditorAppDesc::scene`) that cannot be read is no editor. A new project
  (`createStarterProject`, `editor/NewProject.h`) has a start scene made of the engine's own
  components and built-in assets: a camera, a sun, a sky, a floor and a box, so that it
  plays and exports as it is; `new_project_smoke` makes one of each kind, exports one and
  plays it.
- **What leaves a scene with changes that are not saved asks first** (`EditorApp::leave`):
  File > New scene, Open scene, New project, Open project, Exit and Ctrl+O. The question -
  Save, Don't save, Cancel; Enter saves, Escape cancels - is a modal window of the
  editor's own (`###UnsavedChanges`), not of the system, so that a headless test sees and
  answers it (`unsavedChangesQuestion`, `answerUnsavedChanges`). Save writes the scene to
  its file, or asks for one first, and goes on only once it is written; Don't save tells
  the project command it runs to `discardChanges`. Closing the window does not ask yet.
- **The File menu asks for paths in the system's own dialogs** (`editor/FileDialogs.h`,
  over nativefiledialog-extended; on Linux xdg-desktop-portal, over D-Bus), never in a
  field to type a path into. A dialog holds the program until it is answered, so it is
  shown between frames (`EditorApp::askForPath`, `showPathDialog`), and what it is
  answered with is done as the menu always did it - a scene opened or saved, a project
  made, opened or exported by its command. What each item asks for is a request worked
  out by a function of its own (`openSceneRequest` ...), held by tests to what the menu
  needs; the editor gets its dialogs through the `FileDialogs` interface, which a test
  answers itself (`EditorAppDesc::fileDialogs`) - no test shows a dialog of the system.
  An editor without a window has none: its menu asks for no path and says so, and the
  commands that take paths are the way of a batch run. The library is the editor's
  alone; no exported game links it.
- **The editor and the player open no console window on Windows**: they are programs of
  the Windows subsystem (`WINDOWED` in `cordite_add_executable`), with `main` as their
  entry point all the same. What they write goes where they are given - the pipes of
  CTest, Git Bash and scripts - or, started from a terminal with nothing redirected, to
  that terminal (`attachToParentConsole`). What stops them before a person could see it
  in a log is said in a dialog (`showErrorDialog`), which a headless run never opens.
- **The editor reads the assets again when another program changes their files**
  (`EditorAppDesc::watchAssets`, on unless it is headless): a `DirectoryWatcher` looks at
  them on a thread of its own, and a file that was added, removed or written queues
  `asset.refresh` before the next frame - which is also how a scene that is played gets
  the new text of a script. A file whose name ends in .tmp is on its way to being
  written - an atomic write renames such a sibling - and is not looked at, or a look
  between the two takes it for a file that came and went. What the editor writes itself
  (`EditorContext::saveCount`) or has just read again (`AssetDatabase::revision`, with the
  .meta files that writes) is taken for no change. A refresh reads everything again, as
  the button does. Batch runs and tests leave it off: they see the files as their
  commands leave them.
- **The interface is Dear ImGui**, reached through the `ui` module. It runs without a
  window too, with the pointer fed from `Engine::input()`, so interface code is covered
  by headless tests.
- **The interface has a look of its own** (`ui/UiTheme.h`, `UiFonts.h`, `UiIcons.h`):
  Inter for what is read, JetBrains Mono for what is typed and logged, the icons of
  Lucide merged into both, and a flat theme, dark or light. The fonts are compiled into
  the module (`cordite_embed_file`), so nothing is looked for on the disk. A color is
  the theme's, never a number in a panel: `UiSystem::palette()` has those ImGui has no
  name for - warnings, errors - and what is drawn over the picture of a scene is in the
  overlay colors, the same in every theme (`overlayText`, `UiWidgets.h`). `UiThemeTests`
  holds every color to the contrast it needs against what it is drawn on, and the
  theme names every color ImGui has: one that a new version of ImGui adds is an
  assertion until it is given a value. An icon is a code point of the pinned Lucide,
  held to it by `UiIconsTests`: look them up again when Lucide is upgraded.
- **A theme changes between frames** (`UiSystem::setTheme`, which waits for the next
  frame when it is called during one). ImGui pops what was pushed onto its style back
  to what it was, and a style replaced in the middle of a frame would keep colors of
  the old theme. The View menu queues `editor.theme` for that reason.
- **What a person chose about the editor is theirs, not a project's**: the theme and
  how the Assets panel shows a folder, and the projects they opened last, are kept
  in `editor-preferences.json` in their configuration directory (`EditorPreferences`,
  `userConfigurationDirectory`). A batch run reads and writes none, and is drawn in the
  default theme with the grid, so its pictures do not depend on who runs it. The recent
  projects (File > Open recent, `project.recent`) are absolute paths, newest first, each
  once - one directory reached two ways is one (`sameProjectDirectory`) - and ten at the
  most; one that is gone is greyed out until it is taken out of the list.

## Testing

Framework: doctest. One executable per module (`cordite_<module>_tests`), registered
with CTest under the module name. See the skills
[build-and-test](.claude/skills/build-and-test/SKILL.md) and
[render-tests](.claude/skills/render-tests/SKILL.md).

- Test files are named `<Thing>Tests.cpp` and live in `tests/unit/<module>/`.
- Some suites are not tied to one module: `samples` keeps the showcase project loading
  and complete, `editor_smoke` runs the real editor executable with a command script,
  `module_layering` checks the include rules, `document_links` that every relative link
  of the Markdown of the repository leads to a file that is there, and `test_sources`
  and `assets_listed` keep the tests themselves in order (see the next point but one).
- Rendering is tested with numbers before pictures. A test that can state what a pixel
  must be does so: `SceneRendererTests` with flat emissive materials, an orthographic
  camera and no light; `SceneLightingTests` with a white, matte wall and lights whose
  brightness on it follows from the documented model; `PostProcessTests` with fields
  of known light and the functions of `PostProcessSettings.h`. All use the fixture in
  `tests/render/SceneTestSupport.h`. Reference images are for what only an eye can
  judge.
- glTF files for tests are built in the test (`GltfTestSupport.h`) or written by the
  exporter, never checked in as binaries. Sound files likewise: `encodeWav` writes
  them, and the kinds nothing here can write - FLAC, MP3 - are put together byte by
  byte (`SoundTests.cpp`).
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
  been run (or a system-wide Vulkan SDK is installed), and their devices of Direct3D 12
  with its debug layer when Windows has the optional feature "Graphics Tools" (`dism
  /online /add-capability /capabilityname:Tools.Graphics.DirectX~~~~0.0.1.0`, as an
  administrator). A test that makes a mistake to hear the layer report it says so and
  checks nothing more where the layer is missing.
- **The GPU tests draw with what `CORDITE_GRAPHICS` names** (`testGraphics()`): Vulkan
  unless it is `d3d12` or `warp`. Every device and engine of the tests is made with it, but
  those of the tests that are about one API - Vulkan's adapter, the defaults, a window -
  which say which. No CTest runs the suite on Direct3D 12 yet: 14 of its tests do not pass
  there so far - eight reference pictures, three of fog, the exact light of open ground,
  the console of the editor, and a readback of a mip level, which passes alone and fails in
  a whole run.
- **What a device is known to fail is a list, checked both ways**
  (`tests/render/expected-failures/<name>.txt`, chosen with
  `-DCORDITE_EXPECTED_GPU_FAILURES=<name>`; CI's Linux job has `lavapipe`): its cases are
  left out of `render`, and each runs alone as `render_expected_failure_<n>`, which must
  fail - a case that passes, or a line that names no case, fails it. A line is a pattern
  of doctest's `-tc` without a comma. A case goes on a list with what is known of why,
  and the ticket to find out (P1-13); never to make a change pass.
- **A shader is compiled once in a run of the GPU tests** (`sharedCacheDirectory()` in
  `RenderTestSupport.h`): the `RenderContext` of every test and the engines and editors of
  the fixtures (`headlessDesc()`) share one cache for the run. Compiled anew for every test,
  the shaders were nine tenths of what a test on the GPU took, and a feature that made the
  lighting larger made every test slower. A test that counts compilations, or needs an
  engine without a cache, gives it a cache directory of its own or clears it.
- Rendered frames are compared with reference images in `tests/render/references/`
  within a small tolerance. A new or changed reference image is reviewed by looking at
  it before it is committed; never approve one unseen.
- **The reference pages of the user guide are written by the engine** (`docs/reference/`:
  components, the scripting API, the editor's commands, the names of controls;
  `editor/ReferenceDocs.h`). The `ReferenceDocs` test writes them again and fails, naming
  the first line that differs, when a file is not what the engine writes - so a new
  component, property, script function, command or key changes a page in the same commit:
  run the test with `CORDITE_UPDATE_REFERENCES=1`, read the change, and commit it.
- **Performance is tested against budgets.** The `performance` suite (labels `gpu` and
  `perf`) runs scenarios - the showcase as a game would run it, a field under six
  hundred lights - and checks what each costs against `tests/perf/budgets.json`. CTest
  runs it alone (`RUN_SERIAL`), so that what it measures is not the other suites. Counts
  and sizes - draw calls, triangles, lights and the entries of their lists, bytes on
  the GPU - are the same everywhere and have tight budgets: a change that exceeds one
  raises it in the same commit, with a reason, or has a mistake. Times have budgets
  many times what a desktop GPU needs and are checked in optimized builds only, because
  a test must not fail for a busy machine. The GPU's time is not checked on a software
  adapter (lavapipe, WARP): that is how fast a CPU rasterizes. To see what a change did
  to them, run `./scripts/dev.ps1 perf` before and after: it runs the suite alone and
  prints every scenario's run next to the one before. The numbers come from
  `Engine::statistics()`, which the editor shows under View > Statistics and returns from
  `editor.statistics`.
- Windows opened by tests are hidden (`visible = false`) or use the headless window
  system, so the suite never disturbs the desktop and runs on machines without one.
- Only one `Engine`, one `WindowSystem` and one `GraphicsDevice` can exist at a time;
  tests create them one after another, never side by side.
- Get the value of a `Result` in a test with `test::unwrap(...)`, never with `*result`:
  dereferencing an error is undefined behavior and turns a clean failure into a crash.
- **The message of an assertion is built when the assertion passes, too**, if doctest
  is asked to list what passes (`-s`). It must be safe to build then: write
  `REQUIRE_MESSAGE(result.has_value(), test::errorOf(result))`, never
  `result.error()`, which is undefined for a value (the `test_sources` test rejects
  it); and nothing in a message may assert, which is why `CHECK_NEAR` evaluates its
  arguments once, before the assertion. The `assets_listed` test runs one suite with
  `-s` to keep that way of running the tests alive.

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
   (`AGENTS.md`, `docs/` - the user guide in `docs/guide` too - header comments, skills).

`verify` lints only changed files. Before a commit that touches widely included
headers, also run the full `./scripts/dev.ps1 tidy`.

Commit messages: an imperative subject of at most 72 characters, a blank line, then a
body explaining what changed and why. One logical change per commit.

## Known pitfalls

- PowerShell 5.1: do not append `2>&1` to native commands; it turns stderr lines into
  errors and hides the real output.
- PowerShell waits for the editor and the player - programs without a console of their
  own - only while it reads what they write: `& cordite-editor.exe ... | Out-Host`, or
  `dev.ps1 run`, which does that. Without it the command returns at once, with no exit
  status. Git Bash, CTest and CMake's `execute_process` wait anyway.
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
- `CORDITE_NON_MOVABLE` deletes copying as well; with `CORDITE_NON_COPYABLE` beside it
  the copy operations are declared twice, which MSVC refuses.
- A function in an unnamed namespace hides every function of the same name in the
  namespace around it, whatever its parameters: a local `toJolt(BodyId)` makes the
  `toJolt(Vec3)` of a header unreachable. Give local helpers names of their own.
- EnTT is pinned to the 3.x line on purpose: 4.0 relies on C++20 syntax that MSVC 19.38
  rejects. Check every supported compiler before moving to a new major version of a
  header-only library.
- Editor reference images show the whole interface, so any visible change to a panel
  changes them. That is intended: look at the new image, then approve it (see the
  render-tests skill). They are rendered with logging silenced; do not add anything to
  the default layout that shows machine-specific text. The console panel shows the
  log's history, which outlives a test: a test that takes a picture of the whole
  window empties it first (`createEditorForPicture`), or passes alone and fails in
  the suite. A reference image may differ in a fifth of a percent of its pixels, and
  a new tab or a line of text is less than that: such a test still passes when the
  interface has changed, and when the panel that was asked for is not the one in
  front. Make the image anew after every change to the interface, and look at it for
  what the test is about. And two pictures of an editor with a project in it are not
  the same a few frames apart - the exposure of the scene settles - so compare two
  captures for equality only in an editor without one.
- Agent tools that run shell commands can mangle a here-document that contains an odd
  number of apostrophes. Write files with the file-writing tool instead of `cat <<EOF`.
- ImGui draws a selected row and the header of a collapsing section in the same colors
  (`ImGuiCol_Header`). The themes give them the accent, for what is selected; a section
  is begun with `sectionHeader` (`UiWidgets.h`), which draws it in the shades of a
  field. An `ImGui::CollapsingHeader` of its own looks selected.
- With perspective, the point of an axis nearest to the pointer's ray is not under the
  pointer once the pointer moves across the axis as well as along it: a handle dragged
  that way lags behind, by four percent in the first test of the gizmo. The pointer is
  slid onto the axis as it is seen first (`pointOnAxis` in `Gizmo.cpp`). And a plane seen
  edge on is a line on the screen, whose "inside" is everything on it - a test of a point
  in a quadrilateral asks for its area first.
- Typefaces have glyphs of their own where the icons are: JetBrains Mono has the
  symbols of Powerline in the private use area, one of them on Lucide's icon for
  copying. `addInterfaceFonts` keeps that area for the icons
  (`ImFontConfig::GlyphExcludeRanges`); a typeface merged without it shows a padlock
  where a button says "copy".
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
- `cgltf_validate` refuses some lies before the importer's own checks see them - an
  attribute with another count than the positions, an animation without a value for
  each key - with one message for all ("it does not follow the glTF 2.0
  specification"). A test of such a file expects that message; the importer's own
  check stays, for the day cgltf stops making it.
- Bringing a rotation of a file to unit length changes its last bit when it was of
  unit length already, as nearly as a float can say - and then a file that is written
  and read comes back different. The importer leaves alone what is within a
  hundred-thousandth of one.
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
- `glm::normalize` can change the last digits of a quaternion that is of unit length
  already: a rotation that goes through it on its way into a component may come out
  of the next file different from how it went in. `normalizeRotation`
  (`math/Math.h`) leaves such a rotation bit for bit. And numbers a float can hold may
  have squares it cannot - `1e30` - so that their length is infinite and dividing by
  it gives zeros: `isRotation` says no to those, as to zeros and to what is no number.
- Multiplying matrices in another order gives another last digit:
  `World::computeWorldMatrix` works from the entity upwards, `updateWorldTransforms`
  from the root downwards, and for an entity three deep the two can differ. Compare a
  matrix exactly only with one worked out the same way (`PhysicsSystem` uses
  `computeWorldMatrix` for both sides).
- No 8-bit number is one half. A "flat" normal map of (128, 128, 255) tilts every
  normal by a third of a degree, which showed as a floor seven percent too dark or too
  bright under a low sun, depending on where the sun stood. The default normal
  texture is in half floats for that reason; a default that is to mean "nothing"
  must decode to exactly nothing.
- A texture cannot be read by a binding set while it is the target being drawn into,
  even by shaders that never touch it. A pass that draws into something other passes
  read gets a binding set without it (`ShadowAtlas`).
- A texel of a shadow map with perspective grows with the depth along the way the map
  looks, not with the distance to the light. Measured by the distance, the offsets and
  the filter come out eleven times too wide at the edge of a wide spot light, which
  no test of lights that shine straight at a surface shows.
- A weighted average only tames an outlier among what it averages. Weighting the five
  boxes of the bloom's first pass by their brightness did nothing for a single
  glaring pixel in the middle: it was in all five. The weights go on the four reads
  inside each box. A test with one pixel ten thousand times white found it.
- A loop that submits command lists without ever waiting for the GPU - a test that
  draws two hundred frames and reads none back - runs out of versions of every
  volatile constant buffer, which NVRHI reports as an error. Wait
  (`GraphicsDevice::waitIdle`) every frame or so, as a frame loop does.
- A test that says "the first light" must say first by what: entity ids are random,
  and lights come in their order. A rule that depends on it passes or fails by the
  run.
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
- doctest's `-tc` and `-ts` split their filters at commas, and test names here are
  sentences: a fragment that spans a comma selects nothing, and a run that says
  `test cases: 0` has tested nothing - a mutant checked that way "survives". Filter by a
  fragment without a comma.
- doctest prints a `const char*` that is streamed into a message as `1`. Wrap it:
  `REQUIRE_MESSAGE(found, "no budget for " << std::string(name))`.
- A braced list inside the braced list of a range-for (`for (auto [a, b] : {{1, 2}})`)
  has no type to deduce. Name the type: `std::initializer_list<std::pair<...>>{...}`.
- The arc tangent of nothing over nothing is undefined in a shader, and straight up has
  no place round the horizon. `equirectUv` asks before it divides the circle.
- What a GPU stores for a value too large for a half-float target is its own choice:
  the largest half float on some, infinity on others. A test of it cannot fail on a GPU
  of the first kind - the one this was written on is one - so that test passing does
  not show that the clamp in the shader was not needed.
- A sum over an image that takes one sample at the middle of each pixel makes a
  spotlight of an image of one pixel, which is an even sky. `projectEnvironment` brings
  every image to a grid of its own first, by area.
- Blurring an environment with few, wide reads (filtered importance sampling) blurs it
  more than asked: at 64 reads the roughest level of the cube map let several percent
  of the light from below the horizon through to what faces up. The small levels
  can afford 1024 reads, and take them (`sampleCountOf`).
- stb_image_write compiles its `.hdr` writer only with stdio enabled:
  `STBI_WRITE_NO_STDIO` removes it, callbacks and all.
- An NVRHI command list keeps alive what it writes to, copies from or draws with - but
  not a texture it only clears. One that is cleared in a command list of its own and
  released before the GPU has run that list is destroyed in use, which validation
  reports and a test that draws a few frames first never sees. Fill such a texture
  with `writeTexture`. The test fixtures take their GPU objects down without waiting
  and check validation after, to catch the next one. The same holds for the targets of
  a view: a frame clears only what it then draws into (`SceneTargets::clear` and
  `clearGBuffer`), or a viewport that changes size - for a picture of another size -
  destroys what the frame only cleared; the wireframe and overdraw views did, and
  `SceneRendererTests` replaces the targets after every debug view to keep it so.
- A depth buffer is read at the middles of pixels, which are seldom where a walk
  across the screen wants them. Ambient occlusion that takes them as if they were
  finds a floor seen at a low angle standing in its own light - by four percent at six
  degrees, with noise - because a step to the side on the screen is a long way along
  such a floor. No bias fixes that. Measure what was found from the surface's own
  plane instead (`riseAboveSurface`), which is nothing for anything on the plane.
- With nothing in the way, a slice of the ambient occlusion does not let through 1
  but more the further the normal leans (`openSliceVisibility`); only all slices
  together average 1. A pixel asks two or three: divide what gets through by what an
  open sky would give through the same slices.
- A test's own geometry can be wrong. The first reference for a wall forgot that a
  plane cuts a slice on both sides of the viewer, and "proved" the arithmetic wrong
  by a fifth. When a model and a formula disagree, check the test's drawing of the
  scene before either.
- `discard` for a reason every pixel shares ("this view has no sky and no fog"),
  nested in one that a single pixel has ("nothing was drawn here"), made one driver
  (AMD, Radeon RX 9070 XT) throw away whole blocks of 8 by 8 pixels for the sake of
  one pixel in them: things in front of the background lost their edges, and small
  ones were gone. The SPIR-V was right; the same condition written as two nested
  `if`s happened to work, which is no way to write shaders. The fullscreen passes
  write the background instead, and `discard` is left to mesh passes, for what is
  true of one pixel: its alpha.
- An exponential height fog grows without limit below its own height, and "e to the
  minus the height" overflows a float long before a scene is unreasonable. Here the
  fog stops getting denser below the height it is given (`Fog.h`), so every exponent
  is at most zero. The usual closed form also divides by the change of height along
  the way, which is nothing for a level one: `fogMeanDecayOver` takes the series there.
- Lua takes whatever is thrown through it for an error of a script - also the
  exception of a failed `REQUIRE`. Inside `ScriptRuntime::protect()` and in a function
  that a script calls, a test uses `CHECK`, never `REQUIRE` or `test::unwrap`.
- A C function that Lua calls through another C function has no name and no line in
  what it says about its arguments (`bad argument #1 to '?'`). A replacement for one
  of Lua's own functions checks the arguments itself, where Lua can still tell which
  call of the script it was. Functions given to scripts with `registerLibrary` are
  found by the name of their library instead, which is put where Lua looks.
- `luaL_error` and `lua_error` do not return: they leave the function the way `throw`
  does. What the function holds at that moment is destroyed only because Lua is built
  as C++ here - and for the same reason neither may be called from a destructor or
  from a function that is `noexcept`.
- JSON text that is nested more than 64 deep is refused by `parseJson`
  (`kMaxJsonNesting`). The parser itself would take any depth in its stride, but
  writing a value out, copying it and comparing it all follow the nesting by calling
  themselves. A `Json` that code puts together can still be deeper: check what is
  kept with `checkJsonData`, and never walk a `Json` of unknown origin with a
  function that calls itself without counting.
- An ImGui widget gets its keyboard shortcuts - Ctrl+A in a text field - a frame
  after it became active, not in the same one. A test that types into the interface
  waits that frame; see `PropertyEditorTests`.
- `ImGui::SetScrollHereY` in a region that is laid out for the first time - the frame an
  editor starts in, or the one a panel first shows - scrolls by what that frame thought
  was there, and the scroll is gone by the next: the reference picture of the assets
  showed the chosen tile out of sight that way, while a batch run that drew frames first
  was right. The Assets panel scrolls to an asset chosen from outside in two frames
  (`kRevealFrames` in `AssetBrowserPanel.cpp`); something that must be scrolled to in
  the first frames of a panel does the same.
- `ImGui::Shortcut` gives a key to a shortcut from the frame after the one it was asked
  for in: what it asks is the route of the next frame. A shortcut that is asked for only
  while something allows it loses the first press after a frame in which it was not -
  the gizmo's E, pressed right after the camera was flown, and Ctrl+D right after a click
  that selected something. Ask for every shortcut in every frame, then decide whether to
  act (`EditorApp::handleShortcuts`) - never where what comes before can leave the asking
  out: on the right of `&&` or `||`, or in an `else if`.
- A JSON object that has a key twice is refused by `parseJson`. The library behind it
  keeps one of the two values and says nothing, which is how a budget for the joints
  of the physics went unchecked: it had the name of another. Text that is put
  together of pieces - a material of a test with one member replaced - has to
  replace the member, not write it a second time.
- MSVC 19.38 stops with an internal compiler error (C1001) at a range-for over
  `std::initializer_list<T>{{.member = ...}, {...}}` when `T` holds a vector:
  designated initializers without the name of their type, inside a braced list. Make
  it a `const std::vector<T>` of `T{.member = ...}` first, and loop over that.
- nativefiledialog-extended builds its support for Wayland from the Wayland protocols, a git
  submodule that the archive of a tag does not carry: `NFD_WAYLAND` is off, or the build
  of Linux stops. What that loses is the parent of a dialog on a Wayland session; and the
  editor gives none on Linux at all (`SystemWindow`), since the headers of X11 define
  `Status` and `None` over the engine's names. A filter by a double extension
  (`scene.json`) is given its last one on macOS (`filterSpec`): the system types a file by
  its last extension, and "scene.json" is the type of no file there.
- MSVC's incremental linker sometimes stops with LNK1163 ("invalid selection for COMDAT
  section") on objects that are fine: the same target links when it is built again,
  because the failed link took its `.ilk` with it and the next one starts from nothing.
  Build the target once more before looking for the cause in the code, and run a
  `verify` that failed that way again from the start.
- MSVC links static libraries in any order; GNU ld reads each once, in the order of the
  link line, and keeps only what something before it asked for. A library that uses
  another without saying so in CMake therefore links on Windows and fails on Linux with
  undefined references - NVRHI's backends did, until `CorditeDependencies.cmake` said
  that they use `nvrhi`. A change to what links what is not tested until Linux has built
  it, and Windows builds alone do not show what MSVC's headers include for each other:
  `std::ranges::count` compiled there without `<algorithm>`.
- AddressSanitizer intercepts `dlopen`, and a library opened by name is then searched for as
  if the sanitizer had asked, without the RUNPATH of the library that did: Slang finds its
  compiler of SPIR-V beside itself that way, and found nothing in the `linux-sanitize`
  build. Its tests are told where Slang is (`LD_LIBRARY_PATH`, `tests/CMakeLists.txt`), and
  are given four times as long, as instrumented code needs.
- `doctest::Approx(x).epsilon(e)` allows a difference of `e` times one plus the larger of
  the two numbers: its scale is one unless it is told otherwise. For numbers below one
  that is an absolute tolerance of about `e`, not a share: `Approx(0.35).epsilon(0.1)`
  passed 0.49, and a test that was there to catch a missing factor of 0.7 did not. Write
  the tolerance itself - `CHECK(std::abs(a - b) < 0.03f)` - or say `.scale(0)`.
- Which of two numbers a GPU's `min()` and `max()` give when one is not a number is left
  open (SPIR-V's `FMin`), where C++'s `std::min` and a test such as `!(x >= 0)` follow
  rules of their own: code that is safe on the CPU because every comparison with such a
  number is false may light a pixel on the GPU. What is sent to the GPU is made of numbers
  first - the matrix of a probe's box of no size is replaced (`gpuProbeOf`), not left to
  the test in the shader.
- A test of a perfectly matte surface cannot fail for anything that is reflected: with
  `specular` 0, f0 is nothing, and whatever the reflection is worked out from is
  multiplied by it. The two tables of rect lights bound the other way round passed every
  test of a white wall. What reflects is tested on glossy and metal surfaces - and seen
  at a grazing angle, where Schlick's factor gives most of it to the reflectance at
  grazing angles: at a slant, that part is a hundred-thousandth, and leaving it out
  passed as well.
