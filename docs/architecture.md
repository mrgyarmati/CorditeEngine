# Cordite architecture

This document describes how the engine is put together and why. The working rules
derived from it are in [AGENTS.md](../AGENTS.md).

## Shape of the engine

Cordite is a set of static libraries, one per module, that are linked into two kinds of
executable: the editor, used to build games, and the runtime, which runs an exported
game and contains no editing code.

```
            editor (executable)          runtime (executable)
                      \                    /
                       +---- engine modules (static libraries) ----+
                       |  engine                                   |
                       |  ui, renderer, world, ...                 |
                       |  math                                     |
                       |  core                                     |
                       +-------------------------------------------+
```

Modules form a strict layering: a module may use only the modules it lists as
dependencies, and their dependencies. The `module_layering` test enforces this by
scanning `#include` lines, so an accidental upward dependency fails the build instead
of slowly turning the code base into a ball of mud.

### Modules

| Module | Depends on | Responsibility |
|--------|-----------|----------------|
| `core` | -         | Services every other module needs: logging, assertions, error handling, hashing, UUIDs, JSON and binary serialization, file I/O, configuration, the job system, memory allocators, timing, platform queries. |
| `math` | `core`    | glm type aliases and conventions; `Transform`, bounding volumes, rays and intersection tests, frustum culling, projection matrices, color conversions, keyframed curves, deterministic random numbers. |
| `platform` | `core`, `math` | The window system and windows (GLFW), and per-window keyboard, mouse and gamepad state with an injection API for tests and automation. |
| `assets` | `core`, `math` | CPU-side asset data. Currently images: decoding, PNG encoding, pixel access and tolerance-based comparison. |
| `world` | `core`, `math` | The scene: entities, their components and their parent/child hierarchy, stored with EnTT; the registry of component types known by name; scene files. |
| `rhi` | `core`, `math`, `platform` | The GPU: Vulkan instance and device creation, the NVRHI device the renderer programs against, the swapchain, and texture readback. The only module that calls Vulkan. |
| `shaders` | `core` | What a shader *is* to the engine: requests and their permutation keys, compiled binaries, the on-disk cache, and the `ShaderLibrary` that resolves a request from memory, disk or a compiler. |
| `shader_compiler` | `core`, `shaders` | The Slang-based implementation of `IShaderCompiler`. Kept separate so exported games do not link a compiler. |
| `renderer` | `core`, `math`, `world`, `rhi`, `shaders` | Render passes, render targets, the view a camera gives, debug line drawing, and the `ShaderManager` that turns shader binaries into GPU shader objects. |
| `ui` | `core`, `math`, `platform`, `rhi`, `shaders`, `renderer` | The immediate-mode user interface: the Dear ImGui context, its input, and the renderer that draws it with NVRHI. |
| `engine` | all of the above | `Engine`: creates and owns the subsystems, the world and the interface, and runs the frame loop. |
| `editor` | `engine` and below | The editor: the scene document, undoable actions, the command registry, the panels, and `EditorApp`. Not part of exported games. |

Further modules (physics, audio, scripting) are added by the later milestones and will be
listed here as they land.

### A frame

A frame is bracketed by `Engine::beginFrame()` and `Engine::endFrame()`, and is the same
with and without a window:

1. `beginFrame` polls the window system, which starts a new input frame and delivers
   events, and advances the clock.
2. It acquires the swapchain back buffer (windowed mode only). A minimized window yields
   none; the frame still runs, offscreen, so the program needs no special case.
3. It opens the frame's command list, clears the engine's offscreen frame target and
   starts the interface frame.
4. The program does its work: it updates the world, records render passes into the
   command list, and describes its interface with ImGui calls. The editor renders the
   scene into the viewport panel's own target here; a game renders into the frame target.
5. `endFrame` draws the interface over the frame target, copies the frame target to the
   back buffer and presents (windowed mode only), and releases GPU resources whose last
   use has finished.

Rendering to an offscreen target first is what makes the engine testable and
automatable: the headless path is not a special case, it is the main path minus the
copy. Screenshots, reference-image tests and editor automation all read the frame target.
In headless mode input comes from what is injected into `Engine::input()`, and the
interface is laid out and drawn exactly as on screen.

### The scene

A `World` holds entities. An entity is an id; everything else about it is in components,
which are plain structs. Four of them every entity has: `EntityInfo` (a permanent `Uuid`,
a name, an enabled flag), `Transform` (placement relative to the parent), `Hierarchy`
(parent and ordered children) and `WorldTransform` (the derived placement in the world).
Systems - rendering now; physics, audio and scripts later - iterate the components they
need and know nothing of each other.

Each component type is described once through reflection (`TypeBuilder` in
`core/Reflection.h`): the names, types, ranges and meaning of its properties. That
description is what saves and loads scenes, fills the editor inspector, makes edits
undoable and tells an AI agent what it can set. Component types are registered by name
in a `ComponentRegistry`, which is an ordinary object owned by the engine, not a global.

### The editor

The editor is four layers, each usable without the ones above it:

```
EditorContext    the document: a world, the selection, the undo history, the scene file
EditorActions    every edit: validated, applied, recorded for undo
EditorCommands   the same edits by name, taking and returning JSON
panels, EditorApp    hierarchy, inspector, viewport, console; the frame loop
```

A person clicking in a panel, a script passed with `--exec` and a line typed in the
console all end up in the same `EditorActions` function. That is what "fully controllable
by AI agents" rests on: there is no feature that exists only as a button.

## Conventions

- **Coordinates:** right-handed, +Y up, metres. Identity rotation faces -Z.
- **Matrices:** column-major, column vectors, `world = parent * local`.
- **Clip space:** Direct3D-style, x and y in [-1, 1] with +Y up, depth in [0, 1].
  NVRHI flips the Vulkan viewport so the same matrices work on every backend.
- **Depth:** reversed (near = 1, far = 0) for scene rendering, for uniform precision.
- **Angles:** radians in code, degrees in files and user interfaces.
- **Color:** linear RGB in all math; sRGB only at storage and display boundaries.
- **Text:** UTF-8 everywhere, including file paths in data.
- **Identity:** assets and entities are identified by `Uuid`, never by path or name.

## Design decisions

Each entry records a choice that is not obvious from the code, and why it was made.

### Dependencies are fetched as archives pinned by SHA-256

CMake `FetchContent` downloads each dependency from a fixed URL and verifies its hash.
Git branches and tags are mutable, so tracking them would make builds unreproducible
and would let a compromised upstream change what we compile. Archives are also much
faster to fetch than full git histories. Git submodules were rejected because they
need an extra checkout step that is easy to forget, and vcpkg/Conan because they add a
tool every developer and CI machine must install first.

### The build needs only a C++ compiler, CMake and Ninja

No Python, no package manager, no SDK installers. `scripts/bootstrap.ps1` fetches CMake
and Ninja on Windows machines that only have Visual Studio. Dependencies whose build
needs extra tools are avoided.

### The C runtime is linked statically on Windows

Exported games must run on a clean machine without installing the Visual C++
redistributable. Every dependency is built from source with the same setting, so there
is exactly one runtime in the process.

### Expected failures return `Result`, not exceptions

`Result<T>` (an alias of `tl::expected`) makes failure part of a function's signature,
so a caller cannot forget that loading a file may fail. Exceptions remain enabled
because several dependencies use them, but they are caught at the boundary where they
arise and converted. `std::expected` was not used because it needs C++23, which the
three supported standard libraries do not yet implement equally.

### Assertions are for programmer errors only

`CORDITE_ASSERT` disappears in `Release`. Anything that can happen because of data or
user input is an expected failure and returns a `Result`. The assertion handler is
replaceable; tests install one that throws, so failure paths are testable without
killing the test process.

### Logging goes through a small facade over spdlog

Engine code logs with `CORDITE_LOG_*` and never includes spdlog. The facade adds what
the engine needs on top of spdlog's sinks: a category per message, a bounded in-memory
history for the editor console and the automation API, and listeners. Console output
goes to stderr so that stdout stays free for machine-readable output from tools.

### The job system wraps enkiTS

enkiTS is a small, widely used task scheduler. It is hidden behind `JobSystem` with a
pimpl so the rest of the engine depends on three functions (`parallelFor`, `submit`,
`wait`) rather than on a scheduler library. An exception escaping a job is captured and
rethrown to whoever waits, or logged if nobody does, so a failing job is never silent.

### Custom allocators instead of `std::pmr`

`LinearAllocator` and `PoolAllocator` are a few dozen lines each and cover the two
patterns the engine needs: per-frame scratch memory and many same-sized objects.
`std::pmr` would do the same, but its implementation lives in the standard library
binary on macOS and would raise the minimum supported macOS version.

### JSON keeps key order

`Json` is `nlohmann::ordered_json`. Scene, material and settings files are edited by
people and agents and stored in version control, so their layout must be stable and
readable: fields appear in the order the engine writes them rather than alphabetically.

### Deterministic random numbers

`Random` implements PCG32 directly instead of using `<random>`, whose distributions
produce different values on each standard library. Gameplay, procedural content and
image-comparison tests need the same sequence on every platform. The implementation is
verified against the reference generator's published output.

### Vulkan 1.3 is required

NVRHI's Vulkan backend uses dynamic rendering, synchronization2 and timeline semaphores.
All three are core in Vulkan 1.3, which every current desktop driver, MoltenVK and the
lavapipe software rasterizer support. Adapters that lack them are rejected with a
message saying what is missing.

### The application owns the Vulkan device, NVRHI wraps it

NVRHI does not create instances, devices or swapchains; `GraphicsDevice` and `Swapchain`
do, and hand the handles to NVRHI. NVRHI is built with the dynamic Vulkan-Hpp dispatcher
and, as a static library, expects the application to provide and initialize it.

### One GraphicsDevice, one Engine per process

That dispatcher is a single global table of Vulkan entry points, shared by NVRHI and the
`rhi` module. It holds the functions of one instance and one device: extension functions
are only present when that instance or device enabled the extension. Two devices with
different configurations (say, a windowed one and a headless one) would overwrite each
other's entries and crash on the first call that the other did not enable. So
`GraphicsDevice::create` refuses to create a second device while one exists, which also
lets the dispatcher resolve device functions straight to the driver.

GLFW is likewise a process-global library, so `WindowSystem`, and with it `Engine`,
exists at most once at a time. Devices and engines can be created one after another.

### Validation is part of the test contract

Devices created with `validation = true` enable the Khronos validation layer (when
installed) and NVRHI's own validation layer. Everything either reports is recorded on
the `GraphicsDevice`, and every GPU test asserts that the record is empty. A dedicated
test triggers a deliberate error to prove the mechanism detects one. The validation
layers come from a Vulkan SDK that `scripts/bootstrap.ps1 -WithVulkanSdk` installs
inside the repository in copy-only mode, so no system-wide install is needed.

### The swapchain is 8-bit UNORM, not sRGB

The final pass writes display-encoded values itself, and user interface is blended in
display space, as every UI toolkit expects. An sRGB swapchain format would apply the
encoding a second time. Linear HDR rendering happens in offscreen targets.

### Swapchain image layouts are left to NVRHI

A swapchain image may only change layout while it is acquired, and starts out undefined.
NVRHI's state tracker already treats a texture with `keepInitialState` as undefined
until its first use, so the swapchain declares its images with `initialState = Present`
and does nothing else. `present()` additionally requests the Present state, which is free
when the frame rendered into the image and makes an untouched image presentable.

### Shaders are written in Slang and compiled with the official prebuilt compiler

Slang is the shading language the NVRHI ecosystem is built around, compiles HLSL-style
source to SPIR-V on all three platforms, and reports dependencies, which the cache needs.
It is consumed as the official release binaries, pinned by hash: building it from source
would dominate the engine's build time. glslang was rejected because its build needs
Python. The compiler lives in its own module (`shader_compiler`) behind the
`IShaderCompiler` interface so that an exported game links none of it.

Compiler options are passed in `slangc` command-line syntax and parsed by Slang's own
`parseCommandLineArguments`, which avoids depending on the numeric option identifiers of
one particular Slang release.

### Shader bindings use HLSL registers with NVRHI's default offsets

A shader declares `register(t0)`, `register(s0)`, `register(b0)`; the compiler shifts
them to Vulkan bindings 0, 128 and 256 (and 384 for `u`), which is what NVRHI's binding
layouts assume by default. C++ and shader therefore use the same small slot numbers per
resource kind, and the code would carry over to a Direct3D backend unchanged.

### Shader permutations are identified by a content-independent key

A `ShaderRequest` (path, entry point, stage, sorted defines) hashes to a 128-bit key that
names its cache file. The key deliberately excludes file contents and the compiler
version: those are stored *in* the entry and checked on load. One permutation therefore
has exactly one cache file, which is replaced when it goes stale, instead of leaving an
orphan behind for every edit. A test pins the key of a known request, because changing
the key function would silently invalidate every cache.

### Entities and components are stored with EnTT

EnTT is the de-facto standard C++ entity-component store: sparse sets with dense,
cache-friendly iteration and no code generation. `World` wraps it to add what a scene
needs and EnTT leaves open: permanent ids, names, an ordered hierarchy and world
transforms. The registry is still exposed, because iterating views is the whole point.

The 3.x line is used rather than 4.0, which needs C++20 syntax that MSVC 19.38 - still
the default toolset of many Visual Studio 2022 installations - does not accept. EnTT is
configured by macros that must be identical in every translation unit, so it is only
ever included through `cordite/world/Entt.h`, and a test enforces that.

### A component is described once, by hand

Reflection is a table built in code (`TypeBuilder<T>().property(...)`), not generated
by a tool and not hidden in macros. C++ has no reflection of its own yet; a code
generator would add a build step and a parser, and macro-based schemes make the struct
itself unreadable. The table costs one line per field and buys a place to put what
no generator could infer: ranges, units, descriptions for tooltips and for agents,
and whether a field is read-only or runtime state.

### Input is parsed into a patch before it touches an object

`TypeInfo::parse` turns JSON into a `PropertyPatch` - a list of already validated
writes - or fails as a whole. Applying a patch cannot fail. Scene loading and editor
commands use that to validate everything they were given before they change anything,
so a bad file or a bad request leaves the world exactly as it was. The alternative,
applying as you go and rolling back on error, needs every setter to be reversible and
goes wrong in the cases nobody tested.

Saved data and edits are parsed with different rules: a file may contain a read-only
property (it was saved, after all) but not runtime state; an edit is the reverse.

### The world stays where it is

`World` cannot be moved or swapped. Loading a scene validates the file into a plan,
then clears the existing world and fills it again, rather than building a new world
and replacing the old one. Systems can therefore keep a reference to the world and
their listeners on its registry across scene changes, and they see every component
destroyed and every new one constructed - complete, not default-initialized and then
patched. When an entity is destroyed its children go first, and while its components
are being destroyed it still has its identity, transform and place in the hierarchy.

### Scene files are JSON, strict, and stable

Scenes are JSON so that people and agents can read, diff and write them. Two rules keep
that practical. Loading is strict: an unknown component, property or member is an error
naming the entity, because silently dropping a misspelled field loses work. And files
do not churn: loading and saving again writes the same bytes. Floats are written in
their shortest exact form (`0.1`, not `0.10000000149011612`) and read back to the
identical value, and quaternions that are already unit length are not renormalized.

### The editor's user interface is Dear ImGui

ImGui is the standard immediate-mode UI library for tools. Immediate mode suits an
editor whose panels are generated from reflection data: there is no widget tree to
keep in sync with the scene. The docking branch provides the panel layout. ImGui's own
GLFW backend handles input, clipboard and cursors; the renderer backend is ours,
written against NVRHI like every other pass, because ImGui ships none for it.

Without a window the interface still runs, with the pointer fed from the engine's
input. Reference images of the whole editor and scripted interaction tests depend on
that.

### Third-party assertions report through the engine

EnTT and ImGui check how they are used and, by default, report through the C runtime's
`assert`, which on Windows can open a dialog and hang an unattended run. Both are
configured to call the engine's assertion handler instead (`ExternalAssert.h` for code
that must not include engine headers). Their checks are then logged, turned into test
failures by the test harness, and compiled out of `Release` together with ours.

### Undo stores ids and validated patches

A history entry is a pair of functions, one to take an edit back and one to make it
again. They capture entity `Uuid`s and `PropertyPatch`es or serialized entities - never
handles or pointers into the world, which do not survive the entity being deleted and
restored by another entry. Deleting is undone by re-instantiating a snapshot under the
original ids, which keeps every later entry valid.

Interactive edits (dragging a value in the inspector) are applied to the component
directly while the control is held and become a single history entry when it is
released.

### Enum names use `nameOf`, not `toString`

doctest stringifies values by calling an unqualified `toString`, which finds any
overload in the value's namespace by argument-dependent lookup and then fails to
compile because the return type is not doctest's string. Using a different verb avoids
the collision without wrapping every enum in the tests.
