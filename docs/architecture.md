# Cordite architecture

This document describes how the engine is put together and why. The working rules
derived from it are in [AGENTS.md](../AGENTS.md).

## Shape of the engine

Cordite is a set of static libraries, one per module, that are linked into two kinds of
executable: the editor, used to build games, and the player, which runs an exported
game and contains no editing code.

```
       cordite-editor (executable)    cordite-player (executable)
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
| `core` | -         | Services every other module needs: logging, assertions, error handling, hashing, UUIDs, JSON and binary serialization, file I/O and noticing that files change, configuration, the job system, memory allocators, timing, platform queries. |
| `math` | `core`    | glm type aliases and conventions; `Transform`, bounding volumes, rays and intersection tests, frustum culling, projection matrices, color conversions, keyframed curves, deterministic random numbers, spherical harmonics, and where a direction is in an image of everything around. |
| `platform` | `core`, `math` | The window system and windows (GLFW), and per-window keyboard, mouse and gamepad state with an injection API for tests and automation. |
| `assets` | `core`, `math` | Assets as data, and where they come from: images and mipmaps; meshes, primitives and mesh processing; materials; the text of scripts; the samples of sounds; prefabs, as far as their files go; render targets and sequences, as their files say them; glTF import and export; the asset database, which knows what exists and under which id; the asset manager, which loads by id. Knows nothing of the GPU. |
| `world` | `core`, `math` | The scene: entities, their components and their parent/child hierarchy, stored with EnTT, and the paths of names that lead to them; the registry of component types known by name; scene files; prefabs - placing one in a world, and making one of what is there. |
| `animation` | `core`, `math`, `assets`, `world` | Movement of skeletons: the functions that make a pose (sampling a clip, blending two poses, the matrices a mesh is skinned with, the bounds of a posed mesh), the `Animator` component, and `AnimationSystem`, which plays the animators of a world and gives each the pose it is in. The shake of a camera: the `CameraShake` component, how it moves a view and how time passes for it. Sequences: the `SequencePlayer` component, and `SequenceSystem`, which plays them into the world. |
| `physics` | `core`, `math`, `assets`, `world` | The simulation of rigid bodies: `PhysicsWorld` holds bodies - shapes, a mass, where each is and how it moves - steps them on in time and answers what a ray hits. Jolt Physics is behind it and shows in none of its headers. The `Collider` and `RigidBody` components say what of a world is simulated, and `PhysicsSystem` keeps the two in step; it is also what makes the shapes of colliders out of mesh assets. |
| `audio` | `core`, `math`, `assets`, `world` | Sound: the `AudioSource` and `AudioListener` components, and `AudioSystem`, which gives the sources of a world voices, places them around whoever listens, mixes them through five buses and plays the result on a device - or, for tests and tools, into memory. miniaudio does the mixing and is seen by nothing outside the module. |
| `scripting` | `core`, `math`, `platform`, `assets`, `world`, `animation`, `physics`, `audio` | Gameplay scripts: `ScriptRuntime` is the Lua state they run in and what keeps a script that is wrong from being more than an error - limits on instructions, memory and nesting, a build of Lua without what reaches the machine, an environment for every chunk - with the catalogue of the functions scripts are given, and the conversion of values between JSON and Lua. The `Script` component names a script for an entity; `ScriptSystem` gives each such entity an instance and calls it, and tells it what its sequences passed and what its body met. What scripts call is in the `Script...Api.cpp` sources, one per subject: entities, the world and its hierarchy and transforms, components (through the reflection), vectors and rotations, the controls of the game, the simulation. |
| `rhi` | `core`, `math`, `platform` | The GPU: Vulkan instance and device creation, the NVRHI device the renderer programs against, the swapchain, and texture readback. The only module that calls Vulkan. |
| `shaders` | `core` | What a shader *is* to the engine: requests and their permutation keys, compiled binaries, the on-disk cache, and the `ShaderLibrary` that resolves a request from memory, disk or a compiler. |
| `shader_compiler` | `core`, `shaders` | The Slang-based implementation of `IShaderCompiler`, and the cache of the engine's shaders compiled ahead that an exported game has instead (`EngineShaderCache`). Kept separate so exported games do not link a compiler. |
| `renderer` | `core`, `math`, `assets`, `world`, `animation`, `rhi`, `shaders` | Drawing: the scene renderer and its passes (depth, G-buffer, deferred lighting, forward, debug views), the GPU copies of assets it draws with, the targets of a view, the components that make an entity visible or make it a light, the lights of a world sorted into the clusters of a view and the cookies they shine through, the environment that surrounds a world made ready to light it, ambient occlusion worked out from the screen; the view a camera gives, debug line drawing, and the `ShaderManager` that turns shader binaries into GPU shader objects. |
| `ui` | `core`, `math`, `platform`, `rhi`, `shaders`, `renderer` | The immediate-mode user interface: the Dear ImGui context, its input, and the renderer that draws it with NVRHI; its typefaces and icons, compiled in, and its themes. |
| `engine` | all of the above but `shader_compiler` | `Engine`: creates and owns the subsystems, the assets, the world and the interface, and runs the frame loop. Also what needs several of the modules below at once: projects, a game made of a project (`Game`, which the player runs), placing a model in a world, checking the assets a scene refers to. |
| `editor` | `engine` and below, `shader_compiler` | The editor: the scene document, undoable actions, the command registry, the panels, `EditorApp`, and making a game of a project (`GameExport`). Not part of exported games. |

### A frame

A frame is bracketed by `Engine::beginFrame()` and `Engine::endFrame()`, and is the same
with and without a window:

1. `beginFrame` polls the window system, which starts a new input frame and delivers
   events, and advances the clock.
2. It acquires the swapchain back buffer (windowed mode only). A minimized window yields
   none; the frame still runs, offscreen, so the program needs no special case.
3. It opens the frame's command list, clears the engine's offscreen frame target and
   starts the interface frame.
4. The program does its work: it changes the world as it sees fit, then calls
   `Engine::updateWorld()`, which runs the scripts of the entities, plays the sequences
   and tells the scripts what those passed, steps the simulation of the world's bodies and puts the entities where their bodies have
   gone, tells the scripts what met and what parted on the way, plays the animations
   on by the time of the frame, poses the skeletons, shakes the cameras on and works
   out where every entity is; it records render passes
   into the command list, and describes its interface with ImGui calls. A game calls
   `Engine::drawWorld()`, which renders the world through its primary camera; the
   editor renders it from the viewport's own camera into the viewport panel instead,
   and updates the world with its clock stopped: a scene that is being worked on is
   shown as it is, and does not play - no script runs, nothing falls - until the
   editor is told to play it, which is the same frame with the clock running.
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

### Assets

A project keeps its assets as ordinary files in an `assets` directory: models (glTF),
textures, materials, scenes, prefabs, scripts and sounds. Two classes stand between
those files and the rest of
the engine. The `AssetDatabase` knows *what exists*: it lists every asset with its id,
type, name and dependencies, without loading anything but the structure of model
files. The `AssetManager` *loads*: given an id it returns the mesh, material, image or
model, shared with everyone else who asked.

```
assets/models/crate.gltf        -> model     4fe72146-...   (id from crate.gltf.meta)
                                   mesh      ebd09deb-...   (derived: model id + "mesh/0")
                                   material  7cde9ddc-...   (derived: model id + "material/0")
assets/textures/planks_normal.png -> texture e4a3a2af-...   (id from its own .meta)
```

A model file is one asset that contains others. Its images are assets inside it when
they are embedded, and the assets of their own files when they are not, so a texture
shared by two models exists once. A model whose meshes are skinned also contains their
skeletons (`#skeleton/0`) and its animations (`#animation/0`), each an asset of its
own: a clip names the joints it moves, and so can move any skeleton that has them.

### Rendering a scene

`SceneRenderer` draws a world as a camera sees it. It walks the entities that have a
`MeshRenderer`, asks `RenderResources` for the GPU copy of each mesh and material
(created on first use from what the asset manager loads) and leaves out what is outside
the view. A `MeshDrawList` puts the rest in order and groups equal draws into instanced
batches, once for all the passes that follow. The picture is built in a `SceneTargets`,
one per view:

```
lights of the world ──> light clusters: which lights reach which part of the view
                    ──> shadow plan: which lights get which shadow maps
what casts shadows ──> shadow pass ──> shadow atlas: depth as each of those lights sees it
solid surfaces ──> depth pre-pass ──> depth
               ──> base pass      ──> G-buffer: base color and lighting channels, normal,
                                               roughness / metallic / specular / occlusion,
                                               emissive
decals + depth + a copy of the normals ──> decal pass ──> G-buffer, laid over: color, normal,
                                                           roughness / metallic / specular, light
depth + normals ──> ambient occlusion, traced ──> ambient occlusion: how much light from all around gets through
image of the sky ──> (when first used) nine spherical harmonics: what it lights matte surfaces with
                 ──> (when first used) a cube map, blurred level by level: what surfaces reflect
G-buffer + depth + clusters + shadow atlas + environment + ambient occlusion ──> deferred lighting ──> scene color (linear, float)
                                                             and the sky, where nothing was drawn;
                                                             all of it seen through the fog
translucent surfaces + clusters + shadow atlas + environment ──> forward pass ──> scene color, blended over it
scene color + depth + the sun ──> light shafts ──> scene color, added to
scene color + depth ──> depth of field ──> scene color, blurred where it is out of focus
scene color ──> bloom levels, down ──> exposure: how many stops the view adds
                              └────> bloom levels, up again
scene color + bloom + exposure + lookup table ──> post-processing ──> the finished picture (sRGB, 8 bit)
the finished picture ──> anti-aliasing ──> display image (sRGB, 8 bit)
                                           ^ overlays: debug lines, gizmos
```

The deferred path is what makes many lights affordable: every solid surface is written
down once, and each light then costs the pixels it reaches, not the geometry. What
cannot be written down that way - a surface through which another shows - is drawn by
the forward pass, with the same shading code.

What all of that produces is light: numbers without an upper limit. The last pass
makes the picture of it that a screen shows - exposed, with a bloom around what is
brighter than white, tone mapped and graded - as the `PostProcess` components of the
scene say. A view that is asked not to finish its picture, and every debug view, skips
that and only encodes the light for a display.

A scene is lit by components: `Light` (directional, point, spot or rect), `AmbientLight`
(a sky above and a ground below, as two colors) and `Environment` (an image of
everything far away, which lights the scene and is its sky). A `Fog` fills its air:
what is far away is seen through it, the sky included. `Decal`s lay materials onto what
is drawn, into the G-buffer, between the base pass and the light. For every view,
`SceneLights` gathers the enabled lights, and a `LightClusterBuilder` sorts the point,
spot and rect lights into a grid of
clusters that fills what the camera sees - 16 columns, 9 rows, 24 slices into the
depth. Each cluster gets the list of the lights that touch it; a pixel works out which
cluster it is in and asks the lights of that list, and the directional lights, which
reach everywhere. The lights, the grid and the lists are three buffers in the binding
set of the view, next to its constants, so that the deferred lighting and the forward
pass read the same ones.

A light that casts shadows gets shadow maps: pictures of the scene's depth as the light
sees it, all of them tiles of one texture, the shadow atlas. The sun - the brightest
directional light that casts shadows - gets up to four, the cascades, each covering a
slice of the view; a spot light one; a point or a rect light six. The maps are drawn before
anything else, with the same vertex shader as every other mesh pass, and the lighting
looks a surface up in them to find out how much of each light reaches it.

A debug view (`RenderDebugView`) replaces the lighting, or the passes altogether, by a
picture of something the renderer works with: a channel of the G-buffer, how many
lights each pixel asks, which cascade of the sun's shadow it is looked up in, how much
of the light from all around gets to each surface, how much of each pixel is fog, the
light without the materials, what the surroundings give every surface to reflect, the
edges of the triangles, how often each pixel is drawn.

### The editor

The editor is four layers, each usable without the ones above it:

```
EditorContext    the document: a world, the selection, the undo history, the scene file
EditorActions    every edit: validated, applied, recorded for undo
EditorCommands   the same edits by name, taking and returning JSON
panels, EditorApp    hierarchy, inspector, viewport, console, assets; the frame loop
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

### JSON has a depth, and data of no fixed shape is checked before it is kept

Text nested ten thousand arrays deep is valid JSON, and the library reads it without
trouble - and then writes it out, copies it and compares it with functions that call
themselves once per level, until the stack ends. Nothing the engine reads is deeper
than a handful of levels, so `parseJson` refuses anything beyond 64 and throws the
rest away as it comes.

Almost all JSON then goes through the reflection, which knows the shape of what it
reads. The exception is a property that is JSON itself (`PropertyType::Json`): the
values a script is given for an entity have whatever shape the script wants. Such a
value is kept as it is, after `checkJsonData` has made sure that it can be written
out again - no deeper than 16, no number that is not finite. The alternative, a
string holding JSON, would have put text full of escaped quotes into scene files
that people read and write by hand.

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

### A shader is compiled for Vulkan, and on Windows for Direct3D 12

The person whose engine this is chose Direct3D 12 as what a Windows machine without Vulkan
draws with, over OpenGL, which NVRHI has no backend for and which would have meant a second
renderer to keep, and over a Vulkan drawn on the CPU, which is slow. So a shader has a
target beside its request: SPIR-V, or DXIL. Slang writes DXIL through DXC, Microsoft's
compiler, pinned as its release and loaded from beside the program - of it only
`dxcompiler.dll`, which signs what it writes: the validator beside it is under terms of its
own, and is not needed since DXC signs by itself. DXIL is of shader model 6.0, on purpose:
the machines this is for are old.

The target is not part of the key, which a test pins: it is beside it, in the name of the
file - `<key>.dxil.cshader` - and in the version of the compiler, which for DXIL names DXC,
so that another DXC makes DXIL anew and SPIR-V stays. The file format has no field for the
target; the code says what it is (its magic number), which kept every cache of SPIR-V made
before readable. A game of Windows ships its shaders in both.

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

### A prefab is copied where it is placed

A prefab is a file with a few entities in it: one root and what hangs under it, written
exactly as a scene writes entities. Placing it creates copies of them with ids of their
own - what refers to an entity of the group refers to its copy - and marks the root
with a `PrefabInstance` component that names the prefab. From then on the copies are
entities like any others: the scene saves them in full, an edit changes them, and
nothing ties them to the file they came from. A prefab that is changed later changes
what is placed from then on.

The other design, in which a scene keeps a reference to the prefab and a list of what
was changed in the instance, is what makes a change to a prefab reach everything that
was placed from it. It is also overrides to keep, to show and to undo, a scene that
cannot be read without its prefabs, and rules for a prefab inside a prefab. None of
that is needed to place a crate twenty times or to have a script spawn one, which is
what prefabs are for here, and none of it can be had by degrees. So it is left out as
a whole, and what is there is small: the format is the scene's, the code that checks
and creates the entities is the scene's - `instantiateEntities`, told to give new ids
and to insist on one root - and a scene file is still read without any other file.

The mark exists so that a scene says what it was made of and an editor can show it.
Nothing reads it to decide what an entity does.

Two modules share the work. The assets module knows the file: its header, its name,
that it holds entities (`PrefabData`, `AssetManager::loadPrefab`). What those entities
mean - which components there are, and whether the values are right for them - is said
when they are placed, by `instantiatePrefab` in the world module, which has the
registry of components to ask. It is not in the `engine` module, where placing a model
is, because scripts place prefabs too (`cordite.world.instantiate`) and the scripting
module is below `engine`.

Making a prefab writes a file, and writing a file is not an edit of the scene: undo
does not take it back. What `prefab.create` changes in the scene is one component - the
entity the prefab was made of is marked as its first instance - and that is what the
history holds.

### The asset browser is a list that is worked out, and a window that draws it

The Assets panel shows the assets of a project as the browsers of Unity and Unreal
Engine do: the folders as a tree, and beside it what the folder chosen there has in it -
its directories, its files, and for a model what is inside the file - as a grid of tiles
or a list. Which rows there are, in which order, which of them are open, which folder is
shown, what a filter leaves of them and which asset is chosen is decided by
`AssetBrowser`, a class without a line of interface code in it. It makes the rows anew
from the asset database whenever it is asked, and keeps no pointer into it, so the rows
are right after the assets were read again. `AssetBrowserPanel` asks for them when the
database or the browser has changed, draws what fits in its window, and turns a double
click into the line `asset.instantiate {...}` for the console to run.

A folder is known by a key, not by its path: what the engine has built in is a folder
of its own, which a project may have a directory of the same name beside. The folder
that was chosen stays chosen while it is gone - a refresh in the middle of a move - and
the nearest folder above it is shown meanwhile, as a chosen asset that is gone stays
chosen. A search looks in the folder shown and below it, as Unity's does once a folder
is chosen: in the top it looks everywhere, and a click on a folder of the tree, which
the search leaves alone, narrows it. Whether the contents are a grid or a list is the
person's, kept with their theme, not the project's.

The split is there for two reasons. What a panel decides can be tested without a
window and without a pixel: the tests of the browser compare lists of rows. And the
command `assets.browse` asks the same object the panel draws, so an agent that wants to
know what a person is looking at is told exactly that, and one that sets a filter sees
it in the next screenshot.

The panel shares its place with the console, and is in front when the editor starts,
as the project browser of Unity is. The console is where the editor says what went
wrong: when something the user asked for fails, it comes forward, and the status bar
at the bottom of the window says the newest message whatever is in front. Errors of
the game that is played do not bring the console forward: a game can log one in every
frame, and the panel behind would be out of reach.

### A project opens with a double click on its file

A project is a directory with one file in it that says so, `project.cordite`, as a
Godot project has its `project.godot`: always the same name, so that the editor finds
it in a directory without a choice to make, and with an extension of its own, which
is what a system knows what opens a file by. Its content is JSON, like every file of
the engine; a project of an earlier version, whose file was `cordite.project.json`,
is refused with what to rename it to.

On Windows the editor registers the type itself, for the person who runs it, when it
is started with a window: there is no installer yet to do it, and a registration in
HKEY_CURRENT_USER asks for no administrator and changes nobody else's files. It
registers only when files of the type do not already open with that very executable,
so the build that was started last is the one a double click starts. Linux and macOS
have no such registry: a .desktop file and a MIME type, or the Info.plist of an
application, are put in place by an installation, which is what packaging is for.

### The editor is laid out after Unity and Unreal Engine

People who come to Cordite know one of the two, and both put the same things in the
same places: the scene's entities at the left, what is selected at the right, the
assets under the view of the scene, play in the middle of a bar at the top, messages
along the bottom. Cordite does the same. The toolbar and the status bar are bars of
ImGui's main viewport (`BeginViewportSideBar`), which take their room from the dock
space, so the panels never cover them. Every button of the toolbar runs a command,
queued like a line of the console, so that what a click does is what a script can
do; what each button does in each state is a list (`documentButtons`, `playButtons`)
that tests hold without drawing it.

### The viewports of an editor share what a picture costs

The brief asks for more than one viewport: the game's view beside the one that is
flown, a render view beside the picture. An editor has up to four, each a panel of its
own.

Decisions:

- **A viewport's own are what it shows; what it costs is the editor's.** Each has its
  camera, the camera it looks through, its render view and its colliders. The quality
  and the gizmo are one for all of them (`ViewportSettings`): the renderer has one atlas
  of shadow maps, which it makes anew whenever its size changes - two viewports at two
  shadow qualities would make 64 or 256 MB anew twice a frame - and the textures are the
  program's already. Unity and Unreal Engine set what the views of an editor may cost
  for the editor too.
- **Numbered places, not a list.** A window is known by its id, which a layout remembers.
  A viewport keeps its number until it is closed, the lowest free one is given next, and
  one that is opened again has the id - and the place - it had. Four, because each is a
  view of a frame, and the views of the frames on their way through the GPU share a
  number of versions of their constants that a `static_assert` adds up.
- **The one used last.** The View menu, F and the commands that name no viewport act on
  it: a person who types a command in the console is looking at one viewport. In a
  batch run it changes only by what a script does - opening one, bringing one to the
  front - so a script is as repeatable with four viewports as with one.
- **Captures once a frame.** The cameras of the scene draw into their targets before the
  first viewport that draws, for all of them, as a game draws them - with the game's
  background, whatever that viewport looks through. Drawn by each, they would be drawn
  four times, their intervals would pass four times as fast, and what they show would
  depend on which viewport came first.
- **Placed by splitting a dock node, before the dock space is laid out.** A viewport
  without a place goes beside another, as Dear ImGui's dock builder splits its node;
  one that was open before goes back to the node Dear ImGui keeps for a window that is
  closed. Views along the axes of the world, without perspective, are not built: the
  cascades of the sun's shadow are slices of what a camera sees from where it is, which
  an orthographic camera far back would spend on empty space.

### The editor's user interface is Dear ImGui

ImGui is the standard immediate-mode UI library for tools. Immediate mode suits an
editor whose panels are generated from reflection data: there is no widget tree to
keep in sync with the scene. The docking branch provides the panel layout. ImGui's own
GLFW backend handles input, clipboard and cursors; the renderer backend is ours,
written against NVRHI like every other pass, because ImGui ships none for it.

Without a window the interface still runs, with the pointer fed from the engine's
input. Reference images of the whole editor and scripted interaction tests depend on
that.

### The interface brings its typefaces, its icons and its themes

The editor is meant to look like a tool of today, flat, after Unity and Unreal Engine.
ImGui's own font, Proggy, is a face for code at small sizes; the interface uses Inter
instead, a typeface drawn for screens, in two weights - the stronger for the names of
sections - and JetBrains Mono, without ligatures, for what is typed and logged, where
ids and numbers have to line up and be shown as they were typed. The icons are
Lucide's, as a font: merged into each typeface, an icon is a character, and goes
wherever text does, with no images to load and no second way of drawing. All three are
pinned like any other dependency and compiled into the `ui` module
(`cmake/CorditeEmbed.cmake`): a program has them wherever it is started from, and a
test cannot pass on one machine and fail on another for a font that was not found. The
four files take under two seconds each to compile, so they are compiled as they are,
with no tool to compress them first. ImGui's own fonts are left out of the build
(`IMGUI_DISABLE_DEFAULT_FONT`).

A theme is a palette of colors named for what they are for - the surface of a panel, a
field, text in passing, the accent - and the sizes that go with them. Both themes are
flat: surfaces differ by a shade, and one warm accent marks what is selected, on or
held. Each color was chosen against the contrast that the guidelines for the web
(WCAG 2) ask of it, and the tests hold it there, so a color can be changed without
anyone checking it by eye against every surface. ImGui's own dark and light styles
are not the start: what a theme does not name would change with every version of
ImGui, and so a theme names all of ImGui's colors, and asserts that it did.

Which theme a person works in is a preference of theirs, kept beside no project, in
the directory their system keeps such things in (`%APPDATA%`, `~/.config`,
`~/Library/Application Support`). A project's layout of panels is kept in the project,
since it is about that project's work; a theme is about the person.

### Third-party assertions report through the engine

EnTT and ImGui check how they are used and, by default, report through the C runtime's
`assert`, which on Windows can open a dialog and hang an unattended run. Both are
configured to call the engine's assertion handler instead (`ExternalAssert.h` for code
that must not include engine headers). Lua checks nothing by default; it is given the
same handler for the checks of its API, which are worth more than they cost wherever
assertions are on. Their checks are then logged, turned into test
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

### An asset's id lives next to the asset, in a `.meta` file

References must survive renaming and moving a file, so they cannot be paths. The id is
stored in a sidecar file (`crate.gltf.meta`) rather than inside the asset, because most
asset files are in formats the engine does not own, and one rule for every file is
simpler than two. The sidecar moves with the file in any file manager or version
control system that moves both.

A file that has no `.meta` yet gets an id *derived from its path*, not a random one. The
same files therefore get the same ids on every machine, which lets a generated project
(and the tests) know its ids in advance; once written, the `.meta` keeps the id
whatever happens to the path. Only if the derived id is taken - by a file that used to
live at that path - is a random one used. A copy that brought its `.meta` along is
given a new id by the editor and ignored by everything else.

### Assets inside a file get ids derived from the file's

The meshes and materials of a glTF file have no files of their own to put a `.meta`
next to. Their ids are a hash of the model's id and their position in the file
(`mesh/2`), which is stable across re-imports and needs no storage. The price is that
reordering meshes inside a model file changes their ids; exporters do not do that
between saves, and names - the alternative - are neither unique nor required in glTF.

### The texture slot decides the color space

Whether a texture holds color (sRGB) or data (linear) is not stored with the texture.
The material slot it is used in decides: base color and emissive are color, normal,
roughness, metallic and occlusion are data. A normal map "imported as color" - the
classic mistake - cannot happen, there is nothing to set. If one image is ever used
both ways, the GPU holds it twice; that is a strange thing to do and cheap to allow.
For the same reason, wrap modes and filtering belong to the material, not the texture.

### glTF is imported with cgltf, MikkTSpace and meshoptimizer

cgltf parses; the engine converts into its own types at once and keeps nothing of
cgltf's. Tangents that a file lacks are computed with MikkTSpace, because that is the
basis normal maps are baked in - any other tangent basis lights a baked normal map
subtly wrong. meshoptimizer welds the vertices MikkTSpace split and orders triangles
and vertices for the GPU. Before any of that, every number in the file is checked
against the data that is really there, with arithmetic that cannot overflow.

### Assets are loaded when first needed, on the calling thread

The first frame that shows an asset loads it, decodes it and sends it to the GPU.
That keeps every caller simple and every run deterministic, at the price of a slow
first frame for a large scene. It is a known limitation, not the final design: the
interfaces (`RenderResources::mesh` may return null, materials fall back to a default)
already allow an asset to be "not there yet", which is what loading in the
background needs. Imported data is not cached on disk yet either; a model is parsed
again on every start.

### Uploads use a command list of their own

Data for new GPU resources is recorded on a separate, short-lived command list and
executed before the frame's. NVRHI keeps the staging memory of everything a command
list has ever uploaded for as long as the list lives; uploading a scene's textures
through the frame's list would pin that much memory for the whole session.

### Instances are a vertex stream

The mesh pass passes each instance's transform to the vertex shader as per-instance
vertex attributes, three rows of a 3x4 matrix, from one buffer written once per view.
Drawing a range of that buffer is an ordinary instanced draw call with a start
instance. The alternative, a structured buffer indexed by the instance id, depends on
whether the shading language adds the start instance to that id - which differs between
Direct3D and Vulkan conventions and between compilers.

### Solid surfaces are shaded deferred, translucent ones forward

The look the engine is after needs many lights - practical lamps, muzzle flashes, rim
lights - on dense, normal-mapped scenes. Shading forward, every light costs every
surface it might touch; shading from a G-buffer, it costs the pixels it reaches.
Blended surfaces cannot be in a G-buffer, which holds one surface per pixel, so they
are drawn forward afterwards. Both paths call the same `shadeSurface`, and a test
draws one surface both ways and compares the pictures.

### The G-buffer is four targets and depth

| Target | Format | Holds | Why this format |
|--------|--------|-------|-----------------|
| 0 | SRGBA8 | Base color; in alpha, the lighting channels | Eight bits are enough when they are spent as the eye sees; alpha is not encoded, so its eight bits hold eight channels exactly |
| 1 | RG16_UNORM | World-space normal, octahedrally mapped | Two channels, the same precision in every direction, no banding on smooth highlights |
| 2 | RGBA8 | Roughness, metallic, specular, occlusion | Four numbers between 0 and 1 |
| 3 | R11G11B10F | Emissive | Light is not limited to 1; no alpha is needed |
| depth | D32 | Reversed depth | The position of a pixel follows from it, so none is stored |

Fourteen bytes a pixel, plus depth. Emissive has a target of its own rather than being
added to the scene color by the base pass, so that it stays a thing that can be looked
at and that later passes can tell from reflected light. The motion vectors the base
pass writes as well are in a target beside the G-buffer, not in it: decals, which
change what a surface is made of, do not change where it went (see "Motion blur is
made of motion vectors" below).

### A depth pre-pass, then a base pass on equal depth

Solid surfaces are drawn twice: first their depth alone, then what they are made of,
only where their depth is exactly the one in the buffer. The material - five texture
reads and a normal-map transform - is then evaluated once per pixel however many
surfaces overlap, masked materials get their holes cut in the cheap pass, and the
depth is complete before anything that wants to read it. The price is that the
geometry is transformed twice.

"Exactly the same depth" is only true when both passes compute positions with the
same code, so every pass that draws meshes shares one vertex shader, and a mesh pass
is a description (`MeshPassDesc`) rather than a class with shaders of its own.

### Lights are found through clusters, built on the CPU

A scene of the kind the engine is for has dozens of lamps in view and may have
hundreds; a pixel is lit by the few whose range it is in. The ways to find those few:

- *Ask every light for every pixel.* Simple, and the cost is lights times pixels.
- *Draw a volume per light* over the G-buffer. Each light costs the pixels it covers,
  but it works for the deferred path only, and every light is a draw call and a
  round of reading the G-buffer.
- *Tiles:* cut the screen into squares and list the lights of each. Works for both
  paths, but a tile that contains a near and a far surface lists the lights of
  everything in between.
- *Clusters:* cut the tiles along the depth as well. A list holds what is near one
  piece of the view, and both the deferred lighting and the forward pass can use it.

The engine uses clusters. The slices get deeper by a constant factor (evenly for a
camera without perspective), because that is how much of the picture things take up,
and the grid stops at 500 metres, beyond which everything shares the last slice.

The lists are built on the CPU, per view: a few hundred lights against a few thousand
boxes is a fraction of a millisecond (the `lights` performance scenario measures it),
the code runs and is tested without a GPU, and the result is plain data that a test
can read. A compute shader would only pay off with many thousands of lights. Two
things keep it cheap: the boxes of the clusters depend on the projection alone and
are kept from frame to frame, and a light is first held against whole rows and
columns of a slice, so that most clusters are never asked about it.

A view holds four directional lights and 1024 point, spot and rect lights. More are left
out, counted in the statistics and reported once; lights that reach nothing in view
do not count. Lights are taken in the order of their entities' ids, which is the same
on every run and after any edit, so that neither the sums nor the choice of what is
left out depend on how components happen to be stored.

### A cookie is a layer of one array, filled on the CPU

A light that shines through a picture - its cookie - is looked up in the same loop over
the lights of a pixel as everything else about it, so every cookie a view uses has to be
in reach of every pixel. They are the layers of one texture array, 256 pixels square: one
binding, whatever lights are in view. Pictures of other sizes are brought to that size
when they are first used, on the CPU, with the filter the mip levels are made with, and
the layers written as uploads. Drawing them into the array on the GPU would be quicker to
write and wrong to run: the array is read by the very frame that would draw into it, and
a texture cannot be both. The array is made with the first cookie, of one layer, and made
twice as large whenever it has no room for the next, sixteen layers at the most - what it
held is copied into the larger one on the GPU, which is a copy and not a draw. So the
memory follows what is used: a scene with one cookie pays for one. A view with no cookie
binds a single white texel.

Which level of a cookie is read is worked out from how large a pixel is where the light
lands, not from the differences between neighbouring pixels: the lights of one pixel are
not those of the next, so the GPU's own derivatives say nothing there. A spot light
throws its cookie as a projector throws a slide, and so can use the arithmetic of a
perspective projection; a directional light lays its out like a texture of the world,
from where its entity is - which is what lets clouds drift without a property for it.

### Brightness is set for artists, computed in radiance

A light has a color and an intensity, and an intensity of 1 lights a white, matte
surface that faces the light to white: at any distance for a directional light, one
metre away for a point or spot light, which thin out with the square of the distance.
That is a unit a person can set by eye and check in a test. `SceneLights` converts it
to what the shaders compute with - a matte surface throws back one part in pi of what
reaches it, so the light is handed on pi times as bright - and the conversion is the
one place to change when photometric units (lux, candela) are added on top.

A point or spot light ends at its `range`: the inverse-square falloff is multiplied by
a window that brings it smoothly to zero there (the one of "Real Shading in Unreal
Engine 4"). A light without an end would be in every cluster's list. A rect light ends
at its range the same way, measured from its middle.

### A rect light is linearly transformed cosines, with the published fit

The brief asks for rect or area lights, if practical, and real-time renderers have one
way of making them practical: linearly transformed cosines (Heitz, Dupuy, Hill and
Neubelt, "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines", 2016).
What a matte surface gets of a polygon of light has a closed form - a sum over its edges
as the surface sees them. What the GGX lobe of a shiny surface reflects of it has none;
but a cosine that a 3 by 3 matrix has stretched and sheared looks like the lobe, and the
polygon taken through the inverse of that matrix is lit by a plain cosine, where the
closed form applies again. The matrices depend on the roughness and the angle of the
view, and are fitted once, against integrals of GGX.

That fit is not made here. It is its authors' own, ltc_code, pinned like every other
dependency (docs/third-party.md) and fitted to the very GGX of `Brdf.slang`, with
Smith's height-correlated visibility; two tables of 64 by 64 texels are all of it the
engine holds, compiled in and given to every view in half floats. Fitting them anew
would be writing and validating an optimisation over thousands of lobes to arrive at the
same numbers. The alternative that needs no table - one direction picked inside the light
to shade with, a "representative point" (Karis, 2013) - is cheaper and wrong where it is
seen: the shape of the highlight of a wide light on a rough surface, and how much light
there is in it.

`RectLights.h` is the arithmetic on the CPU, and the tests hold it to closed forms for
what is matte and to GGX added up over the rectangle for the lobe: within an eighth,
mostly within a fiftieth, seen at up to sixty degrees from the normal. What the fit gives
up is at grazing angles, where the cosine that stands in for the lobe reaches below the
horizon: of a sky full of light it reflects a tenth less at 75 degrees, a seventh less at
85. For that reason the rectangle is cut at the surface's horizon before it goes through
the matrix, and again at the cosine's after - where no light can come from. The shaders
do the same arithmetic, and the pictures are held to it.

A rect light's intensity is a point light's: from far along the way it shines, it lights
as a point light of the same intensity in its middle would, and less at a slant, its face
seen narrower; the face is as bright as its color over its area. So a point light can
become a rect light without the scene being lit anew, and a larger panel spreads its
light rather than giving more of it - where an intensity that said how bright the face
is would make every small light dark. Its range, its shadows and what it throws into a
fog all go from its middle, which keeps the clusters, the shadow plan and the fog's
arithmetic as they are for a point light: the range never ends short of its corners, and
what it throws into a fog is the light of its face times the solid angle the face fills
there. Its shadows are those of a point light in its middle: as sharp, and six maps of
which about half of what they show is behind the face, where nothing is lit - the price
of keeping them a point light's. Shadows as soft as a light of that size casts
(percentage-closer soft shadows) and a picture on the face (a textured rect light, which
needs prefiltered levels of the picture) are not built.

### Lighting channels travel with the instance

Every mesh renderer and every light is in some of eight lighting channels, and a
light shines on a surface when they share one - for a rim light that picks out one
character, or a lamp that must not light the room next door until there are shadows
to stop it. The channels of a mesh renderer are per-instance data, next to the
transform, so that two things that differ in nothing else still share a draw call.
The base pass writes them into the alpha of the first G-buffer target, which was
unused, and the forward pass has them at hand; `shadeSurface` compares them with the
light's. Ambient light does not ask for channels.

### A scene without lights is black, except in the editor

A game shows what its scene says, and a scene without lights says darkness. An
editor showing a scene that is being started would show nothing at all, so a view
can ask for stand-ins (`SceneRenderSettings::defaultLighting`): a key light and
ambient light, used only while the world has no light component whatsoever. One
light, even switched off, ends that - a scene that was made dark stays dark.

### An environment is one image, and three things are made of it

What surrounds a scene - a sky with its sun, a horizon, a ground - is an asset like
any other: one image of everything around, twice as wide as high, the way HDR images
of skies come (Poly Haven's, for one). There is no cube map asset, no tool to run
before a sky can be used, and nothing baked that could be out of date. The renderer
makes what it needs of the image the first time a view is lit by it (`EnvironmentMaps`):

- **Nine numbers per color for matte surfaces.** A matte surface adds up the light of
  the whole half of the world it faces, and however rough the light is, that sum is
  smooth: spherical harmonics up to the second band hold it to within a few percent
  (Ramamoorthi and Hanrahan, 2001). Nine vectors in the constants of the view replace
  a second cube map and a read from it. They are worked out on the CPU
  (`projectEnvironment`), from every pixel of the image by the part of the sphere it
  covers, so that a sun of one pixel among millions is not skipped.
- **A cube map for reflections, blurred more from level to level.** A mirror reflects
  one direction, a rough surface a blur around it, the wider the rougher. Each of the
  six levels holds the image blurred for one roughness, and a pixel reads the level of
  its own; how much of that light a surface throws back is the fitted curve the
  ambient light uses already (the "split sum" of Karis, 2013, without its lookup
  table). The levels are drawn on the GPU from the image itself - not from the level
  above, which would add blur to blur - with reads spread the way a surface of that
  roughness has its facets, each taken from the mip level of the image whose pixels
  are as large as what the read stands for (Krivanek and Colbert, 2008). Three dozen
  small passes, once: too little to show in the time the first frame of the showcase
  takes on a desktop GPU.
- **The image itself for the sky.** It is sharper than the cube map, and it is already
  on the GPU. The deferred lighting draws it where the depth buffer says nothing was
  drawn, so there is no sky pass and no sky geometry, and translucent surfaces blend
  over it like over anything else. Which mip level a pixel reads follows from how far
  the view turns from one pixel to the next, not from how fast the place in the image
  changes - that jumps where the left and right edges of the image meet.

Cube maps are kept while views ask for them and given back a while after the last one
did (an image of a sky may be tens of megabytes). When the asset changes, the
environment is made anew.

### The environment lights everything, and nothing stands in its way

The light of an environment reaches every surface, inside a room as in a field: there
is nothing yet that knows how much of the sky a point can see. What dims it is what a
material says about itself - its occlusion texture - and, from the milestone of the
screen-space effects on, ambient occlusion. That is the usual order of things, and the
reason it is one function, `shadeSurroundings`, that everything from all around goes
through.

A world has one environment - of several that are enabled, the one whose entity has
the lowest id, which is the same on every run - while lights and ambient lights add
up. It can be turned about the vertical axis only: that is what lines a sky up with a
sun, and it keeps the horizon level.

### Images of light are half floats on the GPU, averaged plainly

A floating-point image is light, whatever it is used for. It is sent to the GPU as
16-bit floats: every GPU can filter those, which the Vulkan specification does not
promise for 32-bit ones, and they take half the memory. What a half float cannot
hold - more than 65504 - is cut off there; a sun is still sixty thousand times white.

The mip levels of such an image are plain averages. The sharper filter that 8-bit
textures get overshoots beside a pixel thousands of times brighter than its
neighbours, and leaves a ring of less than no light around a sun, which the blurred
levels of a cube map would then spread.

The same limit is why whatever writes scene color cuts off at 65504 itself
(`storableLight`): what a GPU stores for more is its own choice, and the ones that
store infinity turn it into "not a number" in the first pass that blurs.

### Ambient occlusion is measured from the surface, in slices

Light from all around reaches a point from the half of the world it faces, less what
stands in the way. How much less is worked out from the depth buffer, the way of
"ground truth ambient occlusion" (Jimenez et al., 2016): the half sphere above a point
is cut into slices that all contain the line to the viewer; in each, the depth buffer
is walked both ways and the steepest thing met is the horizon of that side; what comes
in between two horizons, weighted by how squarely it falls on the surface, has a
closed form. That gives numbers that mean something - at the foot of a wall a floor
gets half its light, beside a ramp half of one plus a cosine - which is what the
tests hold it to, and why it was chosen over the older ways of counting how many
random points around a pixel lie behind the depth buffer.

Three things are this engine's own:

- **What is met is measured from the plane of the surface**, not from the direction to
  the viewer. The depth buffer can only be read at the middles of pixels, and those
  are seldom in the slice that is being walked. Taken as if they were, a floor seen
  at a low angle finds itself in its own light - a step sideways on the screen is a
  long way along such a floor - and no bias mends it. Measured by how far it rises
  above the surface's plane, in the slice it really is in, a pixel of the same plane
  rises by nothing, wherever it is. Where what is met does lie in the slice, the two
  ways give the same number.
- **What gets through is divided by what an open sky would give through the same
  slices.** A pixel asks two or three slices of the endless many, and an open slice
  lets through more the further the normal leans in it; only all of them together
  average one. Without the division an open, tilted surface would flicker around one.
- **A second pass makes a value for every pixel**, from what the pixels around found
  on the same surface: each pixel looks in other directions than its neighbours, and
  at the lower qualities only one in four looks at all. "The same surface" is judged
  by the depth the pixel's own surface would have there, so that the darkness under a
  crate does not creep up its side.

Only light from all around is dimmed - ambient lights and the environment. The light
of a lamp is kept away by a shadow or not at all; dimming it in corners as well is how
the first games with this effect got their dirty look, and can be had by raising the
intensity. What it looks like - intensity, radius, bias - belongs to the look of a
scene and is set with its PostProcess components; what it costs is one of three
qualities of a view, with the shadows and the finishing of the picture.

### A probe is a picture the renderer takes of its own scene

An environment lights a scene from far away, and nothing in the scene keeps its light
out: inside a room the walls are lit by the sky, and shiny things reflect a sky they
cannot see. An environment probe is the cure the engines of the time had: a picture of
everything around a point, taken from the scene itself, that stands in for the
environment inside a box. Its harmonics light matte surfaces with the light of the
walls around them; its cube, blurred level by level as an environment's is, is what
shiny ones reflect - looked up where the reflection meets the box, so that a floor in a
room reflects the wall before it rather than a wall infinitely far away (box
projection). Probes give light that has bounced once, and nothing that moves.

Decisions:

- **Taken by the renderer, not baked to files.** A probe is drawn with six views of
  the renderer itself when its scene is first drawn - a few milliseconds, once - and
  again when it moves, when the assets are read again, and when the editor asks. A file
  would be one more thing to keep in step with the scene it was baked from, and a
  scene would look wrong between a change and the next bake; drawn on the spot, it is
  what the scene is. A game pays for it in its first frames, one probe a frame: a frame
  that took several pictures would stand out, and their views would not fit beside those
  of the frames on their way through the GPU (`kMaxViewsInFlight`). Not on a timer, and
  not by scripts: six views whenever something liked would be a cost nobody sees coming.
- **One bounce.** The views that take a probe are lit by no probe: a picture that saw
  probes would see the last picture, and light would build up from picture to picture
  - differently for every order in which they are taken.
- **The harmonics are worked out on the GPU.** The picture never comes back to the CPU:
  a compute pass weighs every texel by the part of the sphere it covers and adds up, in
  the same arithmetic `SphericalHarmonics9` has for an image.
- **The reflections of all probes are one array of cube maps,** which the shaders that
  light every surface read with a probe's index. That needs `imageCubeArray` of the
  device, which every GPU the engine runs on has; the alternative - a version of every
  lighting shader without probe reflections, for devices without it - would double the
  shaders an exported game ships for hardware it will not meet. The engine requires it
  instead, and says so when a device lacks it.
- **Order is by priority and then by id,** never by how components are stored, and a
  surface takes from each probe what the probe's weight leaves of what the probes before
  it left: a small probe of a higher priority can be laid inside a large one, and what
  the probes leave goes to the environment.

### A reflection found on the screen replaces the surroundings' exactly

A shiny surface reflects pictures taken from far away - the environment - or at a point of the
scene - a probe: never what stands beside it now. Reflections from the screen follow each
reflection through the depth buffer of the picture that has just been lit, and where it meets
something, the light of that pixel is what the surface reflects.

Decisions:

- **After the lighting, in the picture of the same frame.** The engines of the time followed
  reflections into the picture of the frame before, moved to where it would be now: a history
  the view keeps, a frame of lag, and nothing at all in the frame after a cut. Here they are
  followed into the light the lighting has just worked out, before translucent surfaces are
  drawn: two runs give the same picture, and a test can say what a pixel must be. What is
  given up is what the lighting has not drawn yet - translucent surfaces are not reflected -
  and the reflections that the surfaces met have of their own, which they had in the frame
  before and have not yet.
- **Replaced, not added.** The lighting has reflected the surroundings already; the second
  pass takes back what it added and puts what was met in its place, weighed the same way and
  seen through the same fog. That holds to the bit only because both read one surface
  (`surfaceAt`, `common/GBufferSurface.slang`) through one function (`surroundingLight`), for
  which the lighting's arithmetic was split in two - the light for the diffuse color and the
  light that is reflected - before anything is weighed. Where nothing is met, the pixel is
  copied, and a test holds it against a picture drawn without.
- **Sharp, and so for smooth surfaces only.** A blur of what is met, wider for rougher
  surfaces, needs a pass for every roughness or a chain of smaller pictures; without one, a
  rough surface would show a sharp reflection beside the blurred one of its surroundings.
  What the screen gives fades out with roughness instead, up to `reflectionMaxRoughness`.
- **Even steps across the picture, refined by halving.** The simplest walk that finds where a
  floor reflects a wall to within a pixel (M. McGuire and M. Mara's, without the pyramid of
  depths). It costs the quality's steps for every pixel, for one pixel in four below the high
  quality.
- **Set out a pixel's width above the surface.** The depth buffer holds a surface as it is at
  the middles of its pixels. A reflection that grazes the floor it leaves found the floor's
  own pixels in front of it, half a pixel's width away - at any angle, which is why the
  offset is a pixel's width in metres and not a distance chosen once.

### Textures keep to a budget by one bias for all of them

The brief asks for textures streamed in, or at the least for budgets of texture memory that
can be predicted. Streaming - each texture as sharp as it is seen large, its levels loaded as
the camera comes near - is a system of its own: what is seen how large, loading that keeps
up with the camera, and memory that changes from frame to frame. What is built is the
budget, and it is meant to be predicted.

Decisions:

- **Two bounds: the quality's and the project's.** The quality of the program caps the
  largest side a texture keeps (1024, 2048, 4096 texels), so what one texture costs is
  known before it is loaded; the project's budget caps what all of them take together.
  The cap is the program's and not a view's: the views of a program share one set of
  textures, and a texture cannot be two sizes at once.
- **One bias for all the textures, set by all of them.** Past the budget every texture
  drops the same number of levels - the fewest at which the textures known so far fit.
  Dropping levels of each texture as it comes would be determined, but not predictable:
  two players at the same place would see different textures blurred, depending on what
  each looked at first, and the editor and the game would differ. The bias follows from
  the set of textures, not from their order; a test asks for the same textures in two
  orders and holds them to the same sizes.
- **Applied between frames, by dropping everything.** A change takes effect at the start
  of the next frame, where the resources already drop everything when the assets are
  read again: the interface may hold a pointer to a texture until its frame is drawn,
  and what is counted of the GPU's memory stays exact. What was made of the textures -
  the pictures of probes - is made again with them.
- **What is read texel by texel keeps its size.** A lookup table of colors is not a
  picture: smaller, it is no table. It is neither limited nor counted.

### Fog is a formula in the lighting, not a pass

Fog takes light away on its way to the viewer and puts its own color in its place:
after a way with an optical depth of t - the density of the fog added up along it -
e to the minus t of the light is left. For a fog that is the same everywhere, t is the
density times the distance. A fog that lies on the ground is denser below than above,
and if its density falls by the same factor with every metre of height, the sum along
a straight way still has a closed form: the difference of the densities at its two
ends, over how many e-ths lie between them. That is "exponential height fog", the fog
of the games this engine takes its look from, and it costs a pixel one exponential
and a division - no pass, no texture, no steps along a ray.

It is applied where light is computed: at the end of `shadeSurface`, for the surface
that is being shaded, and in the lighting pass for the pixels in which nothing was
drawn, where the sky or the background is infinitely far away. Doing it there, and
not in a pass over the finished scene color, is what puts translucent surfaces into
the fog correctly: a pane of glass five metres away is seen through five metres of
fog, the wall behind it through fifteen, and blending the one over the other gives
exactly what the light does on its way - a test works that out both ways. A pass over
the picture would only know the depth of the wall.

Three things are decided differently from the textbook form:

- **The fog stops getting denser below its height.** Taken as it stands, the formula
  doubles the density every few metres downwards without end: a cellar under a misty
  yard would be solid, and the numbers overflow soon after. Here `density` is the
  densest the fog gets, at its height and everywhere below; a way that crosses the
  height is the sum of a part in even fog and a part in thinning fog. Every exponent
  is then at most zero, and nothing has to be clamped.
- **The height is where the entity is.** A fog is moved up and down like anything
  else in a scene, by hand, by a parent, by a script.
- **The fog's own color is not lit.** It has the color it is given, plus a second
  color towards the sun - the brightest directional light - that falls off with a power
  of the cosine. A sun that is seen through haze has a glow around it, and that is most
  of what light in fog looks like outdoors. Lamps that light the fog around them, and
  shafts of light between the shadows inside a fog, need the fog sampled along the
  ray; that is a different effect, and adds to this one (next section). The light
  shafts of the post-processing come from the sky around the sun, blurred towards it in
  the picture (below), and need no fog.

### The fog's light is a volume over the view, worked out by compute shaders

A fog lit by lights (`FogComponent::volumetric`) gets what each light throws
towards the viewer from every point of the air it reaches, through its shadows and its
cookie: the beam of a lamp, the glow of a lantern, the sun between the beams of a
roof. Along a line of sight that is the integral of what the lights throw at each
point, times the light the fog there takes away from what is behind (`VolumetricFog.h`
writes it out). Two ways to work it out were weighed:

- **March along each pixel's line of sight**, in a pass over the picture after the
  lighting, at a lower resolution and blurred. It needs nothing new, and gives sharp
  beams. But it knows the depth of the solid surfaces only: drawn after the
  translucent ones, it lays the beam behind a pane of glass over the pane; drawn
  before them, the pane hides the beam in front of it. And the rule of this renderer is
  that light is worked out in one place, `shadeSurface`, for solid and translucent
  surfaces alike, which a test holds by drawing one surface both ways.
- **A volume laid over the view** ("froxels", Hillaire and Wronski's volumetric fog,
  2014-2015): a grid of boxes - so many columns and rows over the picture, so many
  slices deep - that holds what is thrown towards the viewer in front of the end of
  each box. `shadeSurface` reads it where the surface is, whichever pass draws the
  surface, and the lighting reads it in front of the sky. That is the choice.

It is the first work of the renderer done by compute shaders (`ComputePass`): one pass
asks the lights at the middle of every box - those of the box's cluster, which the
lights of the view are sorted into anyway - and one walks every column from the camera
out and adds up. Three decisions in it:

- **How much each box counts is the closed form of the fog**, not its density times
  its depth: the fog's amount at the box's far end less that at its near end, as
  `Fog.slang` works it out, start distance and largest opacity included. The volume
  then dims what lies behind it exactly as the fog does, and where what the lights
  throw is the same all through a box - a sun that nothing shadows - the sum is
  exact, whatever the slices. A test holds a picture of that against the closed form to
  what a half float can hold.
- **Slices deepen as the square of the way through them**, so that most of them are
  near the camera, where a metre is much of the picture: half of them lie in the first
  quarter of the depth. The grid ends at `volumetricDistance`; beyond it, only
  directional lights light the fog, in the closed form and as if nothing stood in
  their way - a sun lights all the air there is, and a grid that ended its light would
  leave the distance unlit.
- **Nothing is kept from one frame to the next.** Engines that smooth the volume over
  frames get finer beams from a coarser grid, at the price of light that trails behind
  what moves; this renderer blends no picture with the one before it (it has no
  temporal anti-aliasing either), so the grid is as fine as the view's quality pays for,
  and a beam thinner than a box is not seen whole.

The sky is behind all the fog there is. A line of sight that climbs comes out of a
fog that thins out, and the sky shows through the more the steeper the line is; a
level one never comes out, so the horizon is the color of the fog. A fog without
falloff hides the sky altogether, which is what an endless even fog does; `maxOpacity`
is there for the scenes that want their sky back.

### Fullscreen passes write every pixel

The lighting pass used to `discard` the pixels in which nothing was drawn, so that
they kept the color the targets were cleared to. With fog the reason to discard
became "nothing was drawn here, and the view has neither a sky nor fog" - one
condition for the pixel and two that are the same for all pixels - and on one GPU
driver whole blocks of 8 by 8 pixels were thrown away whenever one pixel in them was:
the edges of everything in front of the background, and small things altogether. The
shader was right; the driver's compiler was not. Rather than find a spelling of the
condition that it gets right, the passes that fill the screen now write the
background color themselves, which the view tells them, and discard nothing. A pass
that cannot throw pixels away cannot throw the wrong ones away.

### All shadow maps of a view share one atlas

A texture for every light that casts shadows would mean an array of textures in the
shaders, or a draw call's worth of bindings for each light. One atlas is one texture
and one sampler in the binding set of the view, whatever the lights are, and a light's
maps are rectangles in it that a small buffer describes. Sizes are powers of two and
handed out by cutting squares in four, so tiles never overlap and never leave a gap
that could not be used; a light can ask for half or twice the usual size, and when
the atlas is full the lights farthest from the camera get smaller maps and then none.
The sun is served first, and so could take everything: four cascades at twice the
usual size are the whole atlas. It gets three quarters at most. If its cascades would
take more, the far ones are halved - their texels are looked at from farthest away -
until they fit, so that asking more for the sun never costs a lamp its shadow.

The atlas is drawn anew for every view of every frame. Keeping the maps of lights and
things that did not move is the obvious next step, and is not taken yet: it needs to
know what moved, which nothing in the engine tracks so far.

The quality of shadows is a setting of the view (`SceneRenderSettings::shadowQuality`):
the size of the atlas and of the maps in it, and how far the filter reaches. Lower
qualities make the same shadows coarser, not different ones. The atlas is four bytes a
texel - 16, 64 or 256 MiB - which is most of what shadows cost in memory, so the
statistics report it, and switching shadows off gives it back.

### Depth in a shadow map is the conventional way round

Every depth buffer of a view is reversed - 1 at the near plane, 0 far away - for the
precision that gives. Shadow maps are not: they are read through a comparison sampler,
which answers "is this depth smaller than what is stored?", and NVRHI fixes that
question for Vulkan. With 0 at the light, "smaller" is "nearer the light", which is
"lit". The alternative, comparing by hand in the shader, gives up the four-texel
blend the hardware does for each lookup. The loss of precision does not matter here:
a directional light's map is linear in depth either way, and the maps of point and
spot lights end at the light's range, a few metres away.

### A surface is kept out of its own shadow by where it is looked up

A shadow map knows the depth at the middle of each texel; a surface that the light
strikes at an angle is nearer than that on one side of a texel and farther on the
other, and without help shadows itself in stripes. The usual help is a depth bias
when the map is drawn, which is a state of the pipeline: the same for every light, or
a pipeline for every setting. Here the map is drawn as it is, and the surface is
looked up a little off: towards the light, and along its normal by more the flatter
the light strikes it, both measured in texels of the map and set per light.

The filter that softens the edge of a shadow reads the map at several places around
the point. Read at the same depth, each place further out would need a larger offset -
the first version did, and shadows under a low sun ended a hand's breadth short. The
places are therefore taken beside the point on the point's own plane, each as far
from the light as a flat surface would be there. A flat surface then agrees with the
map at every one of them, and the offsets only have to cover the one texel the
hardware blends across.

All of this is about where the surface is, so it is done with the surface's own
normal - that of the triangle - and not with the one the lighting uses, which a normal
map has bent. The first version used the bent one, and the grooves between the tiles
of a floor lay in shadows of their own making under a low sun. A blended surface has
its triangle's normal at hand when it is drawn. The G-buffer does not store a second
normal: deferred lighting works it out from the depth buffer, from where the surface
is in the pixels to the side and above or below - of each pair, the one nearer in
depth, since at the edge of an object the other shows something else. That is four
more reads of the depth buffer for every pixel, which a view without shadow maps
does not make.

### Cascades are spheres, moved in whole texels

A cascade has to cover a slice of what the camera sees. Fitted tightly, its size
would change as the camera turns, and its texels would slide over the world as the
camera moves: shadow edges would crawl with every step. A sphere around the slice is
as large whichever way the camera looks, so a texel always covers the same piece of
the world, and the map is moved by whole texels in the light's own frame, so a point
of the world stays in the same place within its texel. That wastes some of the map -
the sphere is larger than the slice - and buys shadows that hold still.

Where two cascades meet, the shaders go over from one to the next across the last
tenth of the nearer one, and each cascade's map starts that much earlier, so that no
line shows.

### A picture is finished in one pass, after everything is lit

The renderer works out light, which has no upper limit: a lamp is a thousand times as
bright as the wall beside it. A screen shows colors between black and white. What
turns the one into the other - exposure, bloom, tone mapping, grading - is done once,
at the end, by `PostProcessPass`: one pass over the picture for everything that
happens to a pixel by itself, and before it the few small passes that need the picture
around a pixel, for the bloom and for measuring how bright the picture is.

The order is that of a camera: what the lens does to the light (aberration, bloom, a
vignette), how much of it is let in (exposure, white balance), what the film or sensor
makes of it (the tone mapper), and what is done to the developed picture (saturation,
contrast, lift, gamma and gain, a lookup table, grain). Everything up to the tone
mapper is arithmetic on light; everything after it is arithmetic on the colors of a
display. A setting belongs where a photographer would look for it.

The numbers are a `PostProcessSettings`. A scene sets them with `PostProcess`
components, which add where they apply: everywhere, in a box, or for the camera they
are on. A view starts from the settings as they are when nothing is said - a filmic
curve, a little bloom, nothing else - and lays the components that apply to it over
that, by priority. Numbers blend; what cannot be blended, such as the tone mapper,
changes over half way through a blend.

Two tone mappers are offered, and none: J. Hable's filmic curve, which is what gives
games of the late 2000s their soft contrast and slow highlights and is what a view
has when nothing is said; the look of ACES in K. Narkowicz's short form, with more
contrast and more color; and "linear", which cuts off what is brighter than white.
Both curves are scaled so that a mid-grey of 0.18 stays where it is: changing the curve
changes what becomes of shadows and highlights, not how bright the picture is.

### Grading is done on the finished colors, and a lookup table is a strip of slices

Saturation, contrast, lift, gamma and gain are applied after the tone mapper, to
colors between black and white, and so is the lookup table. Grading the light before
the curve is the more modern way, and the more exact one for large changes; but then a
lookup table has to be made by a tool that knows the engine's curve. After the curve, a
table is something any image editor can make: take a screenshot with the neutral table
pasted into a corner, change the colors until the picture is right, cut the table out
again. That is how the games this engine takes its look from were graded.

The table is an image of N * N by N pixels: N slices side by side, red running across
each, green down it, blue from slice to slice (256 by 16 for N = 16). It is read as
plain numbers - the colors in it are those of a display already - with two reads, one
in each of the two slices on either side of a color's blue.

### Automatic exposure is measured from the bloom

A view with automatic exposure has to know how bright its picture is, as an eye would
judge it: by the average of the logarithms, in which a small glaring lamp counts for
its size and not for its brightness. Measuring that needs the whole picture, reduced to
one number without skipping pixels - which is what the bloom does anyway on its way
down. So the levels of the bloom carry, in their alpha, the logarithm of the brightness
of the picture (of all of it, not only of what blooms), and one pass averages the
smallest level. Nothing is read back to the CPU: the result is a texture of one texel,
which the last pass reads.

The exposure moves towards what was measured a part of the way in every frame, faster
towards a brighter picture than towards a darker one, as eyes do. That makes it the one
thing in the renderer that lasts from frame to frame, and it lasts where the view
does: in its `SceneTargets`. A view that is new, or has changed its size, takes what it
measures at once.

### Bloom is a chain of halvings, and bright pixels are tamed where it starts

The bloom is the picture made smaller five times or so, each time by half and with a
blur, and then added up again from the smallest, each level spread a little into the
next larger one (J. Jimenez, 2014). That gives a narrow glow and a wide one at once, at
the cost of a few passes over very few pixels. `bloomScatter` says how much of each
smaller level goes into the larger: the result is an average, not a sum, so the bloom
of an even picture is that picture - which is something a test can state.

Where the chain starts, single pixels can be thousands of times as bright as their
neighbours: the glint of a highlight on metal. Averaged plainly, such a pixel becomes a
blot that flickers whenever it moves. The first pass therefore weights each of its
reads by one over one plus its brightness, within each group of four that it averages:
a glint among dark pixels adds little, a lamp that fills the group blooms in full.

### Edges are smoothed from the finished picture

A picture made of pixels shows a slanted edge as a staircase. The ways to mend that
differ in what they need. Drawing every pixel several times (multisampling) needs a
G-buffer several times the size and lighting that knows about it. Adding up frames
(temporal anti-aliasing) needs to know how every pixel has moved since the last one,
and gets it wrong where it cannot. Looking for the staircases in the finished picture
needs the picture, and nothing else: it is one pass, it cannot lag behind anything,
and nothing in the renderer has to know of it. That is what is here - "fast
approximate anti-aliasing" (T. Lottes, 2011), the way of the engines this one takes
its look from - and what moving pixels need can be added beside it when the renderer
has their motion.

It finds edges by contrast, decides whether an edge runs along the rows or the
columns, follows it both ways to where it steps to the next row, and blends each pixel
with its neighbour across the edge by as much as the edge must cover of it: nearly
half at the end of a run, nothing in its middle. What it does to a bar at eight angles
is held against how much of each pixel the bar really covers: a third to a half of
what was wrong goes. What it cannot do is said by tests as well: at exactly 45
degrees, where every run is one pixel long, it does little; a line one pixel wide
loses a third of its light; near the end of an edge that runs along the pixels, the
end is taken for a step.

Three things follow from where it stands in the frame:

- **It works on the colors of a display**, after the tone mapping. Contrast is what
  an eye sees, and light without an upper limit has edges that no blend can smooth:
  half way between a lamp and a wall is still a lamp.
- **The final pass draws into an image of its own** when the picture is to be
  smoothed, and writes the brightness of each pixel into its alpha, which is all the
  search for edges reads. The image belongs to the view's targets and is given back
  when the view is no longer smoothed.
- **Film grain comes after it.** Grain is contrast from pixel to pixel: smoothed, it
  would be blurred away where it is strong and kept where it is weak. The noise that
  keeps slow gradients from banding stays where it was, before the smoothing: it is a
  level at the most, far below what counts as an edge.

The algorithm is written out in the shader rather than taken from its author's
header, which is a file of macros for a dozen compilers of 2011 and none for Slang;
what there is to get wrong in it - up for down, the side of the edge, the end of a
run - is what the tests turn the picture round for.

### Depth of field is gathered at half the size, from light, before the picture is finished

A lens is sharp at one distance; in front of it and behind, a point becomes a circle.
The controls are those of the engines this one takes its look from: a distance that is
sharp, how far around it everything stays sharp, over how many metres the blur grows
beyond that, how much of the most blur there is in front and behind, and how wide the
most blur is. They are numbers a person can set by looking, not the aperture and focal
length of a camera.

How blurred a pixel is follows from its depth alone, so it is worked out where the
depth is. The blur itself is gathered: every texel of an image half the size of the
picture is made of the texels around it whose own circle reaches it. Scattering - every
pixel drawn as a circle of its own size - is the other way, and the slower one, and
needs no less care at edges. At half the size a blur costs a quarter, and what is
blurred has no detail to lose.

It is done to the light, before the picture is finished, so that a lamp out of focus is
still a lamp: blurred after the tone mapping, a highlight would be greyed into its
surroundings, and bloom and exposure would see a sharp picture that is then blurred.

What makes it hard is edges, where something near and something far meet, and each
rule of the gather is there for one of them. A block of four pixels takes the blur of
its nearest, so that what is in front grows over what is behind. A tap is the texel it
falls in, never filtered between texels: filtered, how blurred it is would be a
mixture across the edge, and something sharp in front would glow into the blur behind
it. What is behind a texel counts as far as the texel's own blur reaches, and no
further than twice that: something sharp keeps its edge, something blurred lets what is
behind it show through its own. And the blur is laid over each pixel from the texels of
the half-sized image that are blurred like the pixel, or the texels that hold an edge -
half of the one thing, half of the other - would draw a fringe around everything sharp
against a blurred sky. Tests hold each edge, and where nothing is out of focus, that
the picture is left as it was, to the bit.

What it does not do: the circles are discs of even light, not the shapes of a lens's
blades; a translucent surface is not in the depth buffer and is blurred as what is
behind it; and a blur of a texel or two is made of few texels, which shows less as a
blur than as a softening.

### Motion blur is made of motion vectors

A camera's shutter is open for a while, and what moves meanwhile is smeared along the
way it went. To smear it, a picture has to know for every pixel how far, and which
way, what it shows went since the picture before: its motion vector. The base pass
works them out with the positions, from where every vertex is and where it was - the
placement of its entity then, the pose of its figure then, the camera then - and writes
them beside the G-buffer. That is why the base pass and the depth pre-pass are both
compiled with `MOTION`: the base pass keeps only the depth the pre-pass wrote, and the
two must run the same code. What did not move goes through the same arithmetic twice
and comes out the same to the bit, so its motion is exactly nothing - which is what
lets a still picture stay sharp to the last bit, not merely nearly.

Where things were is remembered by the view, in its targets, by the ids of entities:
the view is what knows when it was last drawn. A capture that draws one picture a second
blurs a second's motion; a view whose camera cut to another shot - another entity, a
jump, a sharp turn or zoom - remembers nothing, and its next picture is still. So is a
picture drawn when no time has passed since the last: what changed in between - an edit
in the editor, a second picture of the same moment - happened in no time, which is a
jump, not a motion. Only the
camera's motion is worked out without the base pass: the sky, which nothing draws, moves
as the camera turns and not as it goes, since it is infinitely far away - its directions
are projected as points that far, with a fourth number of zero.

The blur is the reconstruction filter of McGuire, Hennessy, Bukowski and Osman (2012),
which engines of the look this one takes after made common. Each pixel is blurred along
the longest motion near it, which tiles of the picture find first - so that a still
background next to a moving thing is reached by its blur - and it takes in a point of
that line as far as what is in front moves over what is behind, what is behind shows
through what moves, and what moves alike reaches each other. Gathering along one line
from a copy of the picture is cheap, and the depth keeps a sharp thing in front sharp
against a blurred background, and the reverse. The points are spread a little
differently from pixel to pixel, which turns the steps a long blur would show between
them into grain; the noise that spreads them is part of the arithmetic written down
twice, so tests can hold the GPU's picture against the CPU's, pixel for pixel.

It is done to the light, after the depth of field and before bloom and exposure, as the
depth of field is, and for the same reason. The editor's own camera does not blur: it
moves to look around, and a person who flies through a scene wants to see it. Through
the game's camera the editor blurs only while the game's time passes, as the game would.

What it does not do: a translucent surface has no motion of its own and blurs as what is
behind it; a blur is at most a fortieth of the picture either way, which a very fast
motion is cut down to; and something that moved further than that in one frame -
teleported - is blurred for one frame at the longest, unless the camera cut.

### Light shafts are the sky around the sun, blurred towards it

The look the engine is after has the sun streaming past what stands in its way. Light
scattered by the air towards the viewer would be worked out properly by following every
line of sight through the shadows of the sun - a volume, and a pass that marches through
it. What is done instead is the way of the engines this one takes its look from: the
sky around the sun is blurred along the lines towards the sun's point in the picture, and
what was drawn in front of the sky blocks it on the way, which is where the shadows in
the shafts come from (K. Mitchell, 2007). It needs the picture, the depth and where the
sun is - no shadow maps, no volume - and it is right where it is seen most, looking
towards the sun past the edge of something.

What feeds the blur is the sun's own light, as the Light component gives it, and the part
of the sky that is brighter than a threshold, both as much as the pixel looks at the sun:
a scene with a plain background has shafts from its sun, and one with an image of the sky
gets the glare of the sun in the image as well. It is held to four times white: a sun in a
photograph of the sky is tens of thousands of times as bright as white, and blurred over
the picture it would be all there is. The cone around the sun is fixed, about twelve
degrees to half, so that bright clouds away from the sun do not send shafts in towards it;
a setting for it would be one more number with no look behind it.

They are added to the light of the scene before the depth of field, which then blurs them
with the rest; and they are not in the light of anything, so nothing is lit by them and
no shadow map is read. Without perspective every line of sight is parallel, and there is
no point in the picture for them to come from: orthographic views have none. Nor do views
that have the sun behind them; towards ninety degrees they fade, so that turning a camera
does not make them jump.

### Scene captures are views drawn before the views that show them

A security monitor shows what a camera of the scene sees; a mirror, what a camera behind it
would. Both are a view of the world - shadows, lighting, finishing and all - drawn into a
picture that a material then shows. Such a picture is an asset, a render target, so that the
SceneCapture that draws into it and the material that shows it can name the same one; the
file says only its size, and the picture is the renderer's.

Every capture is drawn as a view of its own, before the views of the frame - the game's, the
editor's viewport - which may show it, and then copied into the texture of its render target.
The copy comes after: a capture that sees its own screen reads the picture of the frame
before, and no texture is read while it is drawn into. The texture is sRGB, because what is
copied is a finished picture in the colors of a display; read through a material's color
slot it is light again, and in a slot of numbers it means nothing and is refused. A frame
draws four captures at the most, by the ids of their entities, so that the views of the few
frames on their way through the GPU stay within the versions their constant buffers have;
a capture with an interval keeps its picture until the interval has passed.

### Levels of detail are made when a mesh is loaded, and chosen by their error on the screen

A mesh far away costs as many triangles as one up close, and most of them land on the
same few pixels. So each mesh comes with coarser versions of itself, made by
meshoptimizer's simplifier as the mesh is loaded - there is no import step to keep them
in, and a mesh of a hundred thousand triangles is simplified in milliseconds. Every
level aims at half the triangles of the one before and is simplified from the mesh
itself, not from the level before, so that errors do not add up; every submesh is
simplified on its own, with its open edges locked, so that two materials never part
along their seam.

The simplifier measures how far each level's surface is from the mesh's, and that
number - not a distance or a size on the screen picked by hand - decides where a level
is used: the coarsest whose error, where the mesh is nearest to the camera, looks no
larger than a pixel, or two, or half of one, by the quality of the view. A level that
looks the same to within a pixel can be swapped in without anyone seeing it happen, at
any resolution; a camera inside a mesh's bounds sees it at its finest, and a surface that
is flat loses nothing to simplification and is drawn at its coarsest wherever it is.

One level is chosen for each mesh and view, before anything is drawn, and every pass of
the view uses it, shadows included: a mesh casts the shadow of the triangles that are
drawn, so it is not shaded by a silhouette it does not have.

### Decals are laid onto the G-buffer

Grime, cracks and markings change what a surface is - its color, how rough it is, its
bumps - not what light arrives at it, so they are laid onto the G-buffer, between the base
pass that fills it and everything that reads it: the ambient occlusion and the lighting
then treat them as part of the surface, and a decal costs what it covers on the screen,
not a mesh of its own cut to the shape of what it lands on. Each decal is the box of its
entity, drawn from inside and over everything, so that a camera in the box still sees
it; a pixel of the box finds the surface by the depth buffer and lays the material on
where that surface is inside the box and faces back along the projection, fading as it
turns away. Translucent surfaces are not in the G-buffer, and get none.

How much of the material is laid on goes by its alpha mode, as for a mesh: a decal
material is an ordinary material, and one that is opaque covers all of its box. Color,
roughness, metal and specular are blended by the hardware; what the alpha of those
targets holds - the light channels, the occlusion - is kept by the blend, and light the
material gives off is added. The normals are another matter: they are kept as points of
an unfolded octahedron, and blending two such points gives neither normal. So a decal
bends the normal in its shader and writes the result, reading the normals from a copy -
a target cannot be read while it is drawn into. Made once, the copy would hand every
decal the normals of the base pass, and a decal over another would undo the bumps of the
first; so it is made again before a decal whose box meets the box of one that bent normals
since the last copy. Boxes in the world, not rectangles on the screen: two decals whose
boxes do not meet cannot both cover one point of a surface. A decal whose material has no
normal map writes no normals and makes no copy stale.

The level of a texture to read is chosen by how far the image moves from one pixel to the
next, and the usual way to know that - the difference to the neighbouring pixels - jumps
wherever the neighbour shows something else, which in a decal is every edge of what it
lands on. Taken at the pixel's own depth instead, it is too small at a slant: on a floor
seen at a few degrees a step on the screen is a long way along the floor, and the first
level of the texture is read where a small one belongs, which shimmers. The step is
therefore measured where the line of sight of the next pixel meets the plane of the image
through the point the pixel shows.

### Not finishing a picture is a quality

`PostProcessQuality` runs from low to high like the quality of shadows, and like it
has "off": the light as it was worked out, encoded for a display, with what is brighter
than white cut off. That is what tests draw when they say what a pixel must be, what
debug views always are, and what the editor shows when asked to (`viewport.quality`).
From low to high the look stays the same; what gives is how finely it is worked out -
where the bloom starts and how far it reaches, and whether the lens adds grain and
fringes. `RenderQuality` is one word for both qualities, for the options of a game.

### The renderer is told the time

Getting used to light takes time, and film grain changes with it. The renderer does not
look at a clock for either: how long it has been since a view was last drawn, and what
time it is, are part of the settings it is drawn with. The engine fills them in from
its own clock - which runs on a fixed step when it is told to - so a test, a
screenshot taken by a script and a reference image come out the same on every run.

### Debug views are part of the renderer

Looking at a single channel is how rendering bugs are found - the first time the
occlusion view was looked at, it showed the floor's occlusion texture stretched over
the whole floor. The views are therefore not editor code that pokes at the renderer's
textures, but settings of the renderer (`SceneRenderSettings::debugView`) that a game,
a test and the editor all reach the same way, and the tests of the G-buffer read it
through them.

### The engine never gets more than three frames ahead of the GPU

Recording a frame is usually quicker than drawing it. With a window, waiting for the
display holds the loop back; without one - tests, automation, a build server - nothing
does, and a loop would queue frames without limit, each holding on to its buffers.
`FramePacer` bounds that: when three frames are waiting, handing over a fourth waits
for the oldest. Three keeps the GPU busy without making what the user does show up
late, and it gives everything that is "one per frame in flight" - timer queries,
versions of constant buffers - a number to be sized by.

### What a frame costs is measured by the engine itself

`Engine::statistics()` reports, for the last frame: the time the program worked on it,
the time the GPU worked on it, what the scene renderer drew in all its views, and what
it holds on the GPU. The editor shows these numbers, the `editor.statistics` command
returns them, and the performance tests hold them against budgets - one source for all
three, so that a person, an agent and a test argue about the same numbers.

Counts and sizes have tight budgets, because they are the same on every machine and a
change in them is a change in the code. Times have loose ones: a test that fails
because the machine was busy teaches people to ignore it. Times are compared by
running `dev.ps1 perf` before and after a change instead.

The time on the GPU comes from a timer query that is read only after the GPU has
finished the command list it is in. Read earlier, a driver may combine the start time
of the new measurement with the end time of the one before.

### Joints are not entities

A skeleton is an asset, and an entity that is animated has one component, the
`Animator`: which skeleton, which clip, where in the clip. `AnimationSystem` works the
pose out from that every frame, into a `SkeletonPoseComponent` that exists only while
the program runs.

The other way - an entity for every joint, as glTF has a node for each - was not taken,
for three reasons. Playing an animation would change the transforms of entities, which
are what a scene file saves and what the editor's undo history watches: every frame of
a clip would be an edit. A crowd of a hundred figures of sixty joints would be six
thousand entities whose world matrices are worked out, sorted through and saved, for
nothing anyone looks at. And a pose would be spread over the registry instead of lying
in one array, which is how the GPU needs it.

What it costs: something that hangs on a joint - a weapon in a hand - cannot simply be
made a child of it. That needs a component that names a joint, and is not built yet.

### A clip finds its joints by name

A track of an `AnimationData` names the joint it moves; `bindTracks` turns the names
into indices once for a clip and a skeleton, and `AnimationSystem` keeps the result. So
a clip made for one figure moves every figure whose joints are called the same, which
is what makes a library of clips possible, and a track for a joint a skeleton does not
have is passed over instead of being an error. Indices would have tied every clip to
the one file it came from.

### An editor poses, a game plays

`AnimationSystem::update` moves the clocks of the animators on and poses;
`evaluate` only poses, and writes nothing to an animator - it does not even bring a
time that lies beyond its clip into the clip, but reads it as a clock that ran there
would stand. The time of an animator is a property like any other: a person sets it to
look through a clip, the edit is in the undo history, and the scene file gets what was
set. A system that tidied the number up afterwards would change the document behind
the back of everything that keeps track of it.

The engine has one call for both, `Engine::updateWorld(WorldClock)`: `runFrame()` and
games call it with a running clock, the editor with a stopped one. When the editor
learns to play a scene, it will play a copy of it.

### A model with a skeleton is placed with one animator for each skeleton

`instantiateModel` makes an entity for every node of a model, except its joints. For
each skeleton it makes one entity with an `Animator`, directly under the instance, and
puts the meshes skinned to that skeleton under it with no transform of their own:
glTF places a skinned mesh by its joints alone, and its joints in the model, so where
the node of such a mesh is in the file does not matter. A figure of several meshes is
then animated in one place, by one clock.

The animator is given the skeleton and no clip. glTF has no notion of the clip a model
starts with, and the first of a file is as often a walk as an idle; a figure that
stands as it was modelled until it is told what to do is the one result nobody has to
undo.

### Skinning is a version of the one vertex shader

A skinned mesh is drawn by `passes/MeshVertex.slang` compiled with `SKINNED`: the
matrices of the up to four joints a vertex follows are blended by their weights into
one, the matrix of the instance is multiplied with it, and from there on the shader
does what it does for every mesh. Normals need nothing of their own: they are
transformed by the cofactors of whatever matrix places the vertex, which is right for
a joint that stretches what follows it as for one that only turns it.

It is a version of the same file because the base pass keeps the pixels whose depth
equals what the depth pre-pass wrote, and that holds only while both passes work
positions out with the same code; a pass draws a skinned mesh with the skinned version
in all its steps. That a skeleton at rest changes nothing is tested to the bit: its
matrices are the identity, and multiplying by one and adding nothing leave a float as
it is.

How a vertex follows its joints is a vertex buffer of its own, beside the vertices, so
that a mesh without a skin pays nothing and every pass reads the same vertices.
Skinning on the CPU was not considered for long: it would send every vertex of every
figure to the GPU every frame.

One thing is not handled: a joint that mirrors what follows it turns its triangles the
other way round, and the pipeline goes by the instance alone. An instance that is
mirrored as a whole is drawn correctly.

### The joint matrices of a view are one buffer, and an instance says where its own begin

`SkinPalettes` holds the matrices of every pose a view is drawn in, one pose after
another, in a structured buffer that is a binding set of its own. An instance carries
the index of its first matrix, as it carries its lighting channels. The alternative -
a buffer or a binding set for each figure - would make every figure a draw call of its
own; this way a crowd of one mesh is one draw call in every pass, each figure in its
own pose, and a pose that several meshes share is sent once.

The matrices are in the space the mesh was modelled in, so the buffer is the same for
the camera's view and for every shadow map of it, and is filled once for all of them.

### A skinned mesh is drawn in the pose of what it hangs under

The renderer takes the pose from the entity of the mesh, or from the nearest entity
above it that has one. A figure is then one entity with an `Animator`, and its meshes -
body, armour, hair, each a mesh of its own skinned to the same skeleton, as modelling
programs export them - are entities below it that need nothing but a `MeshRenderer`.
The other ways were an animator on every mesh, which game code would have to keep in
step, or a reference from the mesh to the entity that poses it, which is one more thing
to set and to get wrong.

The pose moves the vertices where the mesh was modelled; after that the entity of the
mesh places the result, like any mesh. A skinned mesh below a figure with another
skeleton needs an animator of its own; one whose pose has fewer joints than its
vertices follow is drawn as it was modelled, and the log says so once.

### A posed mesh is bounded by the boxes of its joints

What is in view, and what a light sees, is decided by boxes. The box of a mesh as it
was modelled says nothing about a raised arm, so a skinned mesh keeps a box for every
joint, around the vertices that follow it (`jointBounds`, worked out once). A vertex in
a pose is somewhere between the places its joints would each take it to, so the boxes,
each moved by its joint, enclose the mesh (`skinnedBounds`). It costs a box per joint
and a matrix times a box per joint and frame, and it is never too small - only too
large, where a vertex follows a joint by a little.

### The editor skins on the CPU what it has to find

Telling what the pointer is over means intersecting a ray with triangles, and for a
skinned mesh the triangles are where the pose has put them - which only the vertex
shader knows. The editor works that out again (`PosedMesh`): for a click, every vertex
of every skinned mesh once; for a frame, the vertices of the meshes that are selected,
to put a box around them. Reading the picked entity back from the GPU would be exact
for free, but it needs a target of ids, a pass to fill it and a frame's wait for the
answer; picking is rare, and the CPU's answer is there at once and is the same in a
test without a device. The rule for which pose a mesh is in is one function,
`findSkeletonPose`, that the renderer and the editor both call.

### A sequence is a file of tracks, and a component plays it into the world

Cinematics and camera moves are sequences: an asset with a duration and tracks of keys -
where an entity is, a property of a component, which camera the game is seen through,
moments that scripts are told of - played by a `SequencePlayer` on an entity. Tracks
name what they move by paths of names, from that entity or from the roots of the scene,
as an animation clip names joints: a sequence is then made once and played on any rig of
entities with those names, and a scene owes it nothing until it plays. Ids would tie a
sequence to one scene, and its file to a table of bindings in every scene that plays it.

What a sequence moves is written into the world through the reflection, as an edit is:
any property a component describes can be keyed, with the same checks - ranges, kinds,
what is read-only. That is also why the editor does not play sequences while a scene is
edited: an animator writes a pose nobody saves, a sequence writes the document. Scrubbing
through a sequence in the editor would need a copy of the world to play it in, which is
what playing the scene is.

A cut is not a change of the `primary` of cameras, which would outlast the sequence and
have to be undone; it is the player's to say while it plays (`SequencePlayer::camera`),
and the camera a game is seen through is that cut where there is one
(`findGameCamera`). When the sequence stops, by its end or by a script, the cut goes with
it and the game's own camera is there again.

Events are passed from where the time was, included, up to where it got, excluded: every
moment of a sequence belongs to one update, and an event to the update whose time passes
it - also the one at the very beginning. A long frame passes the events of a sequence that
loops once at the most, as timers do, rather than flooding the scripts.

### A camera shakes its view by its trauma, with noise

A `CameraShake` is driven the way Squirrel Eiserloh described at GDC 2016: what happens
gives the camera trauma, which falls with time, and the camera is moved by its square -
so that shakes of different sizes look different, not only larger. The movement is Perlin
noise of a phase, another row of it for each of the six ways a camera can move: sines of a
few frequencies read as a wobble, and shakes that keep time with each other as a sway.
The noise is glm's, which the engine has already; the rows lie between those of the
lattice, where gradient noise is not nothing at every whole turn - or the camera would
come back to rest, all six ways at once, at a steady beat.

The shake moves the view, not the entity. Moving the entity would move whatever goes by
it - a ray a script aims from the camera, the listener on it - and would be a change of
the world that physics and the editor's history notice. So it is applied where the view
of a camera's entity is made, in one function (`makeCameraView` of an entity) that the
game, scene captures and the editor's view through the game's camera all call. A scene
holds how its cameras shake; that one does, and where in its shaking it is, is the
game's (`transient`), so a scene begins at rest and the editor never shows a shake it
cannot undo.

The noise is of a phase that moves on by the frequency, not of the time times the
frequency: a script that changes the frequency while the camera shakes would make the
second jump. The phase goes round after a period in which the noise repeats, so it
keeps the digits that a frame moves it by however long a game runs.

### Physics is Jolt, behind a world of the engine's own types

Rigid bodies, collision detection and a character that walks on slopes and steps are
years of work and are not written here. Jolt Physics does them: it is under the MIT
license, is maintained and released regularly, builds from source with CMake alone on
all three platforms, has a character controller of its own, and is deterministic by
design. Bullet has not kept pace, and PhysX is a far larger build for a feature set a
game of this kind does not need.

`PhysicsWorld` is all of it the engine sees: ids, descriptions made of the engine's
vectors, and functions. The cost is a layer of forwarding; what it buys is that
nothing outside one module is compiled against Jolt - which makes builds faster,
keeps Jolt's macros and conventions in one place, and would let the library be
replaced.

### Physics is built for the processor every machine has, and gives the same results on all of them

Jolt would build itself for AVX2 and fused multiply-add. The rest of the engine is
built for the baseline of x86-64, and a game must not start on an older machine only
to stop where its physics begins - so Jolt is built for the baseline too. It is also
built in the mode that gives the same results with every compiler and on every
platform: a simulation can then be repeated, which tests rely on, replays and
networked games will, and bug reports do. Both cost some speed. A test holds the build
to it with the fingerprint of a small simulation, which MSVC and clang-cl agree on in
all configurations.

Jolt's jobs are run on the calling thread for now. Simulations on several threads
are deterministic in Jolt as well, and running its jobs on the engine's job system is
the next step when scenes need it.

### The world says what is simulated, and the simulation is not kept in the world

A `Collider` is a shape and a `RigidBody` a way of moving; both are plain data that
scenes hold, the inspector shows and commands set. The bodies made of them belong to
`PhysicsSystem`, in a table by entity, and nothing of them is a component - no handle,
no cached shape. A world can then be saved, copied, undone and loaded without anything
of the simulation to carry along or to repair, and a world that is emptied leaves the
system with bodies that no entity asks for, which it removes at its next update. The
cost is that every update walks the hierarchy and compares what each body is made of
with what it was made of; that is cheap beside the simulation, and it is what lets any
edit of any property simply take effect.

One piece of the simulation does show in the world: the speeds of a rigid body, as
properties that are not saved. A script reads and sets them like anything else.

### A shape of a mesh is made when a collider first asks for it

A collider can be the hull around a mesh or the triangles of one. Jolt has to make
something of the mesh first - find the hull, sort the triangles into a tree - and
what it makes is kept as *geometry*, under an id, in the physics world. Shapes refer
to geometry and scale it; any number of bodies can be made of the same.

The physics system makes geometry the first time it meets a collider of a mesh and a
shape, and keeps it for all that follow, until the assets are refreshed. That it is
made then, and not when the mesh is imported, is the simple way and not the fast one:
the first frame of a scene with many large mesh colliders waits for all of them. What
Jolt makes can be written to a file, and the place for it is the cache of derived
data beside the shaders; it is not done yet.

The world of bodies knows nothing of assets: it takes points and triangles. Mesh
assets are the system's affair, which is why the `physics` module depends on
`assets` and `PhysicsWorld` does not include a header of it.

A convex hull is what a dynamic body is made of: it has an inside, and so a mass.
The triangles of a mesh are a surface, and only for what the simulation does not
move. No shape is made of a mesh by itself - a collider names its mesh, which need
not be the one its entity is drawn with, and should often be a simpler one.

### What touches is told, once for each pair, after the step

Games want to know when two things meet and when they part, and when something
enters a place. The simulation tells that after each step, as a list - `PhysicsWorld`
of bodies, `PhysicsSystem` of entities - and not by calling anybody from the middle of
a step, when bodies are locked and nothing may be changed. A script that destroys
what it is told about is then no trouble.

What Jolt reports is finer and stranger than that: every pair of shapes, every step,
and "removed" when a body falls asleep. The engine keeps the pairs of bodies that
touch and says of each that it began and that it ended, once. Two that have both
stopped moving touch on: a crate that sleeps on the floor has not left it.

A trigger - a collider that stops nothing and only notices - is a sensor body of its
own, moved by hand to follow its entity. It is not a property of a shape in a body,
because the library has sensors by the body; and it is not static, because a sensor
that stands still does not notice what has fallen asleep in it, which is exactly
what a game asks a trigger most often: is it still there?

### A joint is a component of the entity that is held

A `Joint` names another entity, and holds the body its own entity is part of to the
body of that one - or to the world. It is on the thing that hangs, swings or slides,
which is where one looks for what holds it; and it names the other by id, like every
reference from one entity to another, so that a copy of the two holds to the copy.

A joint is made of how its two bodies stand to each other at one moment: that is
what a hinge is turned against, where its limits count from, and how long a spring
is. That moment is the first update in which the simulation can make the joint -
the beginning of the game, for what is in a scene - and not the moment of some later
making: a body is made anew whenever what it is made of changes, and its joints
with it, and a door must not forget where shut is because it was made heavier while
it stood open. Nor because a script gave it other limits: whatever a `Joint` is
given later is said of that same moment. So the system keeps how the two entities
stood, as the matrix from the space of the one to that of the other, and makes the
joint of it each time, in the spaces of whatever bodies the entities are part of
then. Only a joint that is told to hold to another entity begins again - there is
nothing else it could be said of - and so does one that could not be made for a
while: what was held by nothing has gone where nothing held it, and is held there.

Angles are degrees in the component, as everywhere a person types them, and radians
in the simulation. One pair of numbers serves all kinds - degrees for a hinge, metres
for a slider and a distance - because a joint is one kind at a time, and three pairs
of which two are dead would be worse to read than one that says what it is.

### A character is moved by asking what is in its way, not by forces

What walks is not a rigid body. A capsule that the simulation moves falls over
unless it is kept from turning, slides down every slope unless friction is tuned
against it, is stopped by a kerb, and is thrown about by whatever hits it; each of
these can be worked against, and each such fix shows somewhere else. A character
here is Jolt's `CharacterVirtual` instead: a shape that is moved by a speed it is
given, as far as queries against the world let it go - along the ground, up slopes
it can walk on, up steps, around what stands still. It stands where it is put, and
what it does is the same every time.

The price is that the simulation does not know of it: a character is in no broad
phase. So each has a kinematic body inside it, a little smaller than itself, that
Jolt keeps where the character is - which is what a ray hits, what a trigger
notices, and what a crate that falls on the character comes to rest on. What the
character runs into it has to meet itself, with the force it has; its body would
push with all the force there is.

Jolt moves a character and leaves it to its user how fast: what gravity does, what
a jump is, how much there is to steer by in the air. That is decided once, in the
simulation (`CharacterSet::step`), and not by every game: it needs what the last
step found under the character's feet, in every step, and a frame may be two steps
or none. A game says where a character is to walk and when it is to jump.

### A character controller is told what to do, and says what happened

A `CharacterController` is data like every component: how large the character is and
what it can climb, which a scene holds; what it is told - where to walk, to jump -
which a game writes every frame; and what the simulation says of it - how fast it
moved, what it stands on - which a game reads. There is no function to call on a
character: a script, an editor command and a test all set the same properties, and
what the reflection knows of them - their ranges, that the last group is read-only -
holds for all three.

A jump is a request, not a state, and a frame is not a step: the update that passes
a jump on takes it from the component, and the simulation makes it in the step
that follows, whenever that is. Asked for once it is tried once.

The simulation says where a character is, and nothing about which way it faces: a
capsule that stands upright has no front. So the entity keeps its rotation, and a
game turns it as it likes - towards where it walks, or towards what it aims at -
without the simulation taking that for the entity having been put somewhere.

An entity is a character or a body, not both: the capsule of a character is its
own, and a collider on the same entity would be a second thing in the same place.
The same goes for a collider hanging on it that no rigid body claims. Neither is
silently dropped: the system says so once, and `scene.validate` finds it before
anything runs.

### Controls have names, and a file says what moves them

Code that asks whether the space bar is down has decided three things at once: that
the game has a jump, that a key makes it, and which key. The first is the game's;
the other two are the player's, and the machine's - a gamepad has no space bar. So
a game asks for *actions* and *axes* by name, and a file of the project - the input
map - says which keys, buttons and sticks those are. Rebinding is editing that
file; a second device is another line in it.

The map is data and the state is worked out from it and from the raw input once a
frame, in one place (`InputActions`): nothing subscribes, nothing is called back.
What was pressed in a frame is a list that anything may read during that frame -
which is what a script wants as events, and what a test can compare.

An axis takes the binding that is moved furthest rather than the sum of all. Sums
are what make a character run twice as fast with a stick and a key together, and
they turn a pointer and a stick on the same axis into a fight between two units.

The map is no asset with an id: nothing in a scene refers to it. The project names
it by path, as it names its start scene.

### Scripts are Lua, bound by hand

Gameplay scripts are written in Lua 5.4: small enough to read in an afternoon, built
from two dozen C files with nothing but a compiler, and the language most people who
have scripted a game have already used. LuaJIT is faster and was not chosen: it is
Lua 5.1, it has no releases to pin, and its build is not a matter of compiling files.

The engine talks to Lua through Lua's own C API rather than through a binding
library such as sol2. Such a library earns its keep where hundreds of C++ classes
are exposed one method at a time. Here components are reached through the
reflection - one description of a type already drives scene files, the inspector
and the automation API, and it drives scripts as well - so what is left to bind by
hand is a few dozen functions. Against that stands a large body of templates that
would have to compile on three compilers, two of which no machine at hand can run.

Values cross between the two sides as JSON (`ScriptValue.h`), because that is what
the reflection reads and writes. It is not the fastest way to hand a number to a
script, and what scripts do every frame - moving an entity - gets functions of its
own.

### Lua is compiled as C++

Lua reports an error by leaving the function it is in - with `longjmp` when it is
built as C, by throwing when it is built as C++. A function of the engine that a
script calls is C++: it has strings, vectors and locks on its stack, and a
`longjmp` over them destroys none. So Lua's sources are compiled as C++ (they are
written to be), an error is an exception that never leaves Lua's own protected
calls, and every destructor on the way runs. The price is that Lua's headers must
not be read as C anywhere: `cordite/scripting/Lua.h` is the one place that includes
them, and a test keeps it so.

Lua takes anything thrown through it for an error of its own, without knowing what
it is. What the engine throws inside - an assertion under the test harness, a
container out of memory - is therefore caught where Lua calls the engine and raised
again as a Lua error with the text it had.

### A script cannot be more than an error

A script is written by whoever makes the game, changed while the game runs, and
wrong as often as any other code. The brief asks that a script error never crash the
engine; `ScriptRuntime` takes that to mean every way a script can go wrong, not only
the ones Lua reports by itself:

- A script that does not end. Lua calls back every thousand instructions, and a call
  into the scripts that has used up its ten million is ended with an error. Counting
  is not free - the interpreter asks before every instruction whether it is time,
  which costs code that only computes about four tenths more time (0.52 s against
  0.73 s for ninety million instructions) - so it can be turned off, by a game that
  is finished and would rather be fast than stoppable.
- A script that catches that error and carries on. `pcall`, `xpcall` and
  `coroutine.resume` are replaced by versions that pass it on. There was one more
  way: the handler given to `xpcall` runs while the error is still on its way, and
  Lua calls no hook while one is raising an error - a handler that loops was never
  stopped. It is not asked about this error.
- A script that takes all memory. Lua allocates through a function of the engine,
  which counts and refuses past a limit; Lua turns the refusal into an error.
- A script that calls itself without end. From script to script that ends at the
  limit of Lua's own stack. Through C - `pcall`, a metamethod, a library function
  that calls back - it uses the stack of the program, where running out is not an
  error. Lua stops at 200 such levels, which is safe when Lua is optimized; an
  unoptimized MSVC build spends 6.6 kB on a level and has 1 MB. The limit is 64, the
  same in every build so that a script does not work in one and fail in another,
  and a test measures how much stack the deepest nesting takes.
- A finalizer. Lua runs `__gc` whenever it collects, with the hook switched off, so
  scripts cannot set one.
- Reaching the machine. The libraries for files, the operating system, loading code
  and debugging are not compiled into the engine at all, and compiled chunks are not
  loaded: Lua does not verify them.

What this does not hold is said in `ScriptRuntime.h`: one call of a library function
is one instruction however long it takes, and scripts share the library tables.

### A script is a table of functions, and every entity has a table of its own

A script file returns a table - `start`, `update`, `destroy`, and the `properties` an
entity may set - and the engine makes one more table for every entity that runs it,
whose metatable leads to the script's. That is the oldest way there is of having
objects in Lua, and it is chosen for what follows from it: the state of an entity is
in a place of its own without the script doing anything for it, a script is an
ordinary Lua module that can be read without knowing the engine, and loading a script
again is putting another table behind the same metatable - every instance has the new
functions at once, and none has lost what it knew.

Properties are declared in the script, with the values they have when nobody says
otherwise, and a `Script` component may only give values to what is declared, of the
same kind. A name that is written wrongly in a scene is then an error that names it,
rather than a value nobody reads. Every instance gets a copy of its own: a table
among the properties that one entity changes is not the table of the next.

### The scripts follow the components, and nothing listens

`ScriptSystem` does what `PhysicsSystem` does: once an update it walks the world,
compares what the `Script` components ask for with the instances it has, and makes,
ends and drops instances until the two agree. It does not listen for components
being added and removed. A listener would be called in the middle of whatever else
the engine is doing - loading a scene, destroying a hierarchy - and would run script
code there, which can do anything. Comparing once an update costs a walk through
the hierarchy, and buys that scripts run at one known point of a frame and nowhere
else.

The price is what `destroy` can promise. It is called while the entity is still
there, which the system can only see to for endings it brings about itself: a
component that is gone or says something else, an entity that a script asked to
have destroyed - which is why scripts ask, and the system destroys after they have
all run - and the clearing of everything before another scene is loaded. An entity
that other code destroys is simply gone at the next update, and its instance with it.

An error in a function of an instance stops that instance and no other. Calling it
again every frame would say the same thing sixty times a second about a table that
is half changed; what stopped is said once, kept where the editor and an agent can
ask for it, and run again when the script has another text.

### Scripts reach components through the reflection

A script reads and writes components by the names of their properties:
`lamp:get("Light", "intensity")`, `lamp:set("Light", { intensity = 4, color = { 1, 0.5, 0 } })`.
Behind it is the one description every component has (`TypeBuilder`), which already
drives scene files, the inspector, undo and the automation API - so a component is
scriptable the moment it is described, and a script is held to the rules a person
in the editor is held to: a write goes through `TypeInfo::parseEdit`, which checks
kinds and ranges, refuses what is read-only, and makes a patch only when all of it
is right. A change that is refused has changed nothing.

The other way is a binding per component: a `Light` object for scripts with an
`intensity` field, written or generated for every type. It reads a little nicer
(`light.intensity = 4`) and is faster, since nothing passes through JSON. It was not
chosen because it is a second description of every component that has to be kept
in step with the first, and because the speed is not needed where it is paid: a
script changes a handful of properties in a frame, and what it does in every line -
moving and turning entities - has functions of its own that touch the transform
directly (`entity:setPosition`). A function is written for what is no property, and
for nothing else.

Values take the shape a script works with on the way: a `Vec3` property is a vector
with arithmetic, a `Quat` a rotation, and a property that refers to an entity is the
entity itself, or `nil` when there is none - a script never handles the id.

One rule is the script's own. What people write most is read, change, write back:

```lua
local light = lamp:get("Light")
light.intensity = light.intensity * 2
lamp:set("Light", light)
```

The table that comes back holds everything, and some of it may not be written: a
character's `velocity` is read-only, and an edit that names it is refused whatever
value it gives. So `set` first leaves out what has the value it had, and only what
is left is an edit. That makes writing back what was read a change of nothing, which
is what it looks like, and keeps the refusal for the script that really tries.

### Scripts know the controls by name, and the scripting module knows the input map

A script asks `cordite.input.isDown("jump")` and `cordite.input.axis("moveX")`: the
actions and axes of the project's input map, never a key. The one difference from
`InputActions`, which C++ asks, is what an unknown name is. To C++ it is a control
that is never pressed, so that a game runs with a map that lacks something. To a
script it is an error that lists the names there are: a script is where names are
typed by hand, and "nothing happens when I press it" is the hardest thing to find.

Presses and releases also arrive as calls - `input(action, pressed)`, between the
starts and the updates of a frame - because what happens once (a jump, a shot) is
awkward to write as a question asked every frame, and what lasts (walking) is
awkward as an event.

The scripting module depends on `platform` for this, on `physics` for what
scripts ask the simulation, and on `audio` for what they play. The other way - scripting declares small interfaces,
the engine module implements them over the real systems - would keep the module
smaller, but the functions scripts call could then only be tested against
stand-ins, with the real thing tested in the engine suite, which needs a GPU. As it
is, the tests of the script API press real keys into a real `InputActions` and cast
rays at real bodies, without a window.

### Both are told what met, each from its own side

When two bodies begin to touch, the simulation says so once, of a first and a
second entity. Scripts hear it twice: the script of each is called with the other
entity, and the `normal` it is given is the way from itself to the other. A script
is written from the point of view of one entity, and should not have to ask whether
it was the first or the second - which is an accident of the order bodies were
made in. The same holds for a trigger: the entity that has it and the one that
walked in are both told, with the same function.

The telling happens right after the step, not with the scripts' update of the next
frame: then what is said is still true when it is heard, and a script that reacts
by changing a component - a light, a name - has done so before the frame is drawn.
The other entity may still be gone by the time its name is said, because the
simulation says that two bodies parted when one of them was destroyed; a script
gets the value it knew the entity by, so that what it kept under that value can be
found and dropped, and `isValid()` says the rest.

### Timers and tasks belong to the script that made them

A script asks for something to happen later - `cordite.time.after(2, self.close)` -
and does not say for whom: it is for the entity whose script the engine was calling
at that moment. That is what makes the common case need no bookkeeping. The door
that is destroyed, switched off or given another script takes its timers along, and
a function that would otherwise be called for an entity that is no longer there, or
no longer what it was, is not called at all. The function is handed the table of
the script, so that a method can be given as it is.

A task - `cordite.time.run(function() ... cordite.time.wait(0.5) ... end)` - is a
Lua coroutine that the engine resumes. Lua has coroutines, and a script could
schedule its own; the engine does it because the hard part is not resuming but
ending: knowing whose a coroutine is, and dropping it with its owner. Tasks start
at once rather than in the next frame, so that what a task does before its first
wait has happened when `run` returns - a script that starts one and then asks what
it set finds it set.

Time is the clock of the scripts, which only passes in updates, so timers are as
repeatable as everything else scripts do. A timer that repeats keeps time - it is
due an interval after it was due, not after it was called - and it does not catch
up: after a frame that took a second, a timer of ten a second is called once. A game
that falls behind should not fall further behind by doing ten frames' work in one.

### A scene is changed between frames, after it has been checked

`cordite.scene.load("scenes/cellar.scene.json")` changes nothing where it is called:
the script that calls it is running in the scene it wants gone, and so may a dozen
others be in the same frame. The request is kept, the frame comes to its end as any
other, and the scene changes where the next one begins.

The change itself (`changeScene`, in the engine module, without anything of the
renderer in it so that it can be tested without a GPU) is ordered by what can go
wrong. First the file is read and put into a world of its own: a scene with a
mistake in it is found before anything of the running one is touched, and the game
goes on where it was, with the reason in the log. Then the scripts of the old scene
end, each with its `destroy`, while the entities they belong to still exist. Then
the bodies and the poses are forgotten - they are kept by entity, and the entities
of the next scene are given the handles the old ones had. Only then is the world
given the new scene.

Scripts keep nothing across the change: the system is cleared, its clock with it.
What a game wants to carry from one scene to the next has no place yet, and will
need one.

### The showcase tests the script API from inside the game

Every function scripts are given has unit tests, which run it in a small world
built for the purpose. They cannot show that it works where a game uses it: with
the components the engine really has, in a scene of some size, in a frame in which
the simulation runs as well. So the showcase carries a script, `selftest.lua`, that
calls every function in the first frames of the game and compares what comes back
with what should - and hands every component of every entity to Lua and takes it
back. A script that stops is a problem of the scene, which the tests of the
showcase and of the engine fail for; the `samples` test also fails for a function
that no script of the showcase has called, by its name, and for a component that
is not what it was once the scripts have run.

It found its first defect on the day it was written: handing back the properties
of a script reordered them, because the engine's JSON keeps the order of the file
and compared two objects in order, and a Lua table has none. No unit test had
data whose names were not in alphabetical order.

### Vectors are tables, and scripts compute in doubles

A vector is a Lua table with `x`, `y` and `z`, and the ones the engine makes have a
metatable with the arithmetic. The alternative is a userdata that wraps a `Vec3`:
less memory, no hash lookups. A table was chosen because it is what a script can
write down (`{ 0, 1, 0 }` and `{ x = 0, y = 1, z = 0 }` are both vectors wherever one is
expected, which is how the properties of a script hold them, as plain data) and
take apart (`way.y = 0`), and because it keeps Lua's numbers: a table holds doubles,
a `Vec3` single floats.

That last point decides how numbers cross. The arithmetic of vectors and rotations
is done in doubles, so what a script computes is as exact as anything else in Lua.
A float of the engine goes to Lua as the double that is written the shortest and
still is that float - the 0.1 a person typed into a scene, not 0.10000000149011612,
which is the same float and would make every `print` of a position unreadable and
every comparison with a literal false. On the way into the engine a number is
rounded to the float nearest to it, and refused when that is not finite.

Vectors are values. What a script is given is a copy; changing `position.x` moves
nothing until the vector is handed back. A vector that wrote through to its entity
would be a reference to a component, kept by a script for as long as it likes - the
one thing the world does not allow anybody.

### A chunk has its own globals, and names must exist

In Lua a name that was never assigned reads as `nil`, and assigning to a name
without `local` makes a global. Both are the commonest mistakes there are - a typing
error that shows up three calls later as "attempt to index a nil value", a counter
that two scripts turn out to share - and both are silent. So every chunk runs in an
environment of its own, whose globals no other chunk sees; reading a name that is
not there is an error that says which; and once a chunk has run it cannot make new
globals, so that the `local` someone forgot inside a function is found the first
time the function runs.

### Scripts run the same way twice

Lua seeds two things from the clock and from where the state happens to lie in
memory: the hashes of its strings, which decide the order in which `pairs()` walks
a table, and `math.random()`. A game whose scripts behave differently from one run
to the next cannot be tested and cannot be played back. Both seeds are fixed. The
random hash seed is a defence against keys chosen by an attacker, which the scripts
of a game are not.

### A body of several shapes is an entity with colliders below it

A rigid body takes its own collider and those of the entities below it, down to the
next rigid body. A table is a top with four legs hanging under it, each placed, turned
and scaled by its own transform - which is how everything else in a scene is put
together, and so needs no editor of its own. The other way, a list of shapes in one
component, would need one.

A body has no scale; its shapes are as large as the scales above them, multiplied.
They are worked out from the transforms the entities hold, never from where things
are in the world: taken apart, the matrix of a body that turns gives a scale and a
rotation that differ in the last digit from frame to frame, and a body is made anew
whenever what it is made of changes.

### While time passes the simulation places dynamic bodies; while it does not, the world does

In a game the simulation has the say over a dynamic body: after every update its
entity is where the body is. Two things can put the body elsewhere - a new transform
given to its entity, and nothing else. What the entity hangs under does not: a crate
that stands on a platform and hangs under it in the scene would otherwise be moved
twice, by its parent and by the friction, and slide off. Its entity is put back where
the body is instead, also in a frame without a step, and also while the body sleeps.

In an editor, where no time passes, it is the other way round: the scene is the
truth, every body is where its entity is however the entity came to be there, and
the system writes nothing - a document must not change because it was looked at.

Whether an entity was moved is told exactly: its own transform and the matrix of
what it hangs under are kept from the last time body and entity were brought in
line, and compared number for number. Nothing is decided by how far apart an entity
and its body are in the world, because taking a world matrix apart and putting it
together again does not give the same numbers, and under a parent that is turned and
scaled unevenly not even nearly: a tolerance would wake bodies that nobody touched.

A kinematic body is moved through steps and never put somewhere while time passes.
On a display faster than the simulation most frames have no step; putting the body
ahead in those would leave only the last frame's part of the way for it to push
anything along.

### Time passes for bodies in steps of one length

A frame takes what it takes; the simulation takes steps of a sixtieth of a second, as
many as have come together, and at most eight to a frame. Steps of one length are what
keep stacks standing and make a simulation repeatable. What is shown is the state
after the last whole step: nothing is moved between two steps, so on a display that
does not run at the step's rate bodies move a little unevenly. Showing a state
between the last two steps would smooth that, at the price of showing everything one
step late; it is not done yet.

### Sound files are read by miniaudio, and a sound is decoded whole

miniaudio is what reads sound files: one C file that decodes WAV, FLAC and MP3, mixes,
places sound in space and plays it on all three platforms, with nothing to install -
what a machine has to play sound with is looked for when the program runs. Its own
CMake file is not used: it searches the machine for codecs and libraries to build
against, and a build of Cordite must be the same wherever it runs. Its resource
manager is compiled out, because it reads files by name on a thread of its own; here
the asset manager reads files, and what plays a sound is handed samples.

A sound asset is its samples (`SoundData`): decoded once, all of it, when it is
loaded, and kept. That suits effects, which are short and played again and again,
and it makes playing a sound a matter of reading memory. It costs memory for music -
a minute of stereo at 48 kHz is 23 MB - which is why there is a limit
(`kMaxSoundFrames`), and why decoding as a sound plays is the thing to add when a
game needs long music.

Ogg Vorbis is not read. miniaudio reads it through stb_vorbis, whose record with
files that are broken on purpose is poor, and files are untrusted here; the
reference decoder would be two more libraries. FLAC and MP3 cover what Vorbis is
used for.

What a file says of its own length decides nothing. The decoder is asked for a few
thousand frames at a time and what it hands over is counted, so a file of fifty
bytes that claims four thousand million is a sound of a few frames, and one that
really is too long is refused, not cut.

### A scene is played in the world it is edited in, and read back when it stops

The editor has one world, the engine's. Playing a scene does not copy it into a
second one: the same world is updated with its clock running, so everything that
shows a scene - the viewport, the hierarchy, the inspector, every command that asks
about an entity - shows the game without knowing that it is one. What makes this
safe is done at the two ends. When playing begins the scene is written out, as a
file would have it, and kept; when it ends, whatever ran for the world is ended
(`endScene`: scripts are told, bodies, poses and sounds are forgotten) and the
world is read back from what was kept. The scene after playing is the scene before
it, to the byte, because it went through the one path that files go through - and
"files do not churn" is tested.

A second world to play in would let the scene be edited while its game runs. It
would also need every system and every panel to be told which world it is looking
at, and that is a great deal of code for a rare wish.

The entities of the scene are others afterwards - new handles, the same ids. The
editor already refers to entities by id wherever something lasts: the selection,
the history. Both survive playing for that reason, without a line written for it.

While a scene plays the first viewport looks through the camera of the game, as the game
itself would draw it, because that is what playing is for; it can be told to look
through the editor's camera again, to fly around a game that runs, and the other
viewports go on looking through what they looked through.

### What is edited while a scene plays goes with the game

Editing is not switched off while a scene plays: moving a light to see what it does
to a game that runs is half the point. But those edits are made to a world that
will be thrown away, so they must not be recorded where the edits of the scene are
- undoing one of them later would change entities that never had it. While a scene
plays the document has another, empty history, which goes when playing ends; the
history of the scene is set aside and comes back untouched, and so does the answer
to "are there unsaved changes". Saving is refused meanwhile, because what would be
written is a game half-way through.

### What is heard is said by components, and the world is silent until asked

A sound in a scene is an `AudioSource` on an entity: which sound, how loud, on which
bus, whether it is heard from where the entity is. Like every component it is plain
data, and `AudioSystem` is what reads it - each update it gives the sources that
say they are playing a voice, in the order of the hierarchy, and takes the voice
from those that no longer do. `playing` is the whole interface: a scene file says
with it what sounds from the start, a script or an editor sets it to play
something, and the system sets it back when a sound that does not loop has ended.
Nothing has to be called, so nothing can be forgotten when an entity is disabled,
copied or undone.

What belongs to no entity - a shot, a step - is `AudioSystem::play`, which returns
a number to stop it by. It is deliberately the smaller interface: whatever should
be saved with a scene or seen in an inspector is a component.

### Scripts play what belongs to no entity; the rest is a component

`cordite.audio` is small on purpose: `play`, `stop`, `isPlaying`, and the volumes of
the buses. There is no function to play the sound of an entity, because that is
already there - an `AudioSource` is reached like every component, and
`entity:set("AudioSource", "playing", true)` plays it. A second way to do the
same thing would be a second thing to keep right.

`play` takes a table of options and refuses a name it does not know. Options are
where a script is most easily wrong without anyone noticing - `{volumee = 0.2}`
would otherwise be a sound at full volume - so a name that is no option is an
error that lists the options. What is no mistake of the script is no error: when
every voice is taken `play` returns nil, and `stop` and `isPlaying` take nil, so
that a script need not ask before it stops what it may never have started.

### An engine opens no sound device unless it is told to

Where the mix goes is chosen when the audio system is made, and the default is
`Offline`: no device, no thread, and as much sound mixed into memory as `update` is
told time has passed. That is the right default for most of what runs an engine
here - tests, tools, the editor in batch mode, machines without sound - and it
makes sound something a test can state numbers about: the same world gives the
same samples every run. A game asks for `Device`. A machine that has none gets a
device that plays to nobody at the speed of a real one, because a game must not
behave differently - sounds that never end, timers that depend on them - for want
of a loudspeaker.

### Buses are a fixed set

Effects, music, ambience, voice and interface: the sliders of a settings menu.
Every source is on one, each has a volume, and a volume for everything sits on
top. A project cannot add buses of its own, route one into another or put an
effect on one; that would be a mixer with a file of its own, as the controls of a
project have one, and nothing has needed it yet. The names are part of scene
files and of the script API.

### The mixer's rules for space are taken as they are

How a sound is heard from a place - how much of it in which ear, how fast it fades
with distance, how gains are smoothed when something moves - is miniaudio's
spatializer, not something the engine computes. Its rules are simple and a little
unusual (a source straight ahead is half as loud in each ear; an ear never hears
less than a fifth), and they are written down in `AudioSystem.h` and pinned by
tests, so that a new version of the library that hears differently is noticed.
What is left out is left out on purpose for now: no Doppler effect - sources and
the listener have no velocity - no cones, no reverberation, nothing in the way of a
sound.

### Settings are the player's file, not the project's

What a person chooses about a game they play - the window, vsync, the quality of the
pictures, the volumes - is theirs: it is kept where their system keeps such things, under
the name of the game, and not beside the game, which an export replaces and which may sit
where nobody may write. It is a file, read as strictly as a scene and written with every
member, rather than a menu: a game has no interface of its own yet, and a file the game
writes the first time is something a person can find and change, and that a menu can later
read and write. The command line wins over it for one start and is never written into it,
so that a test or a shortcut that asks for a window of some size does not change what the
person chose; and a game run without a window reads none at all unless it is told which.

### A preview is beside the world, not on a bus of it

The Assets panel plays a sound for whoever is at the editor while the scene is not
played - when everything of the world is held. A bus would not do for it: buses are
the world's, turned down by a game's settings and by its scripts, and held with it.
So a preview has a group of its own beside the one that everything of the world
plays into, and the volume of the listener, which used to be on everything, went
onto the world's group with the rest of what turns the world down. Only the volume
of everything is on top of both. A preview is one sound at a time, not a voice of
the world's, and `pause` mixes it offline - which is how the tests hear it while the
world is held.

### Enum names use `nameOf`, not `toString`

doctest stringifies values by calling an unqualified `toString`, which finds any
overload in the value's namespace by argument-dependent lookup and then fails to
compile because the return type is not doctest's string. Using a different verb avoids
the collision without wrapping every enum in the tests.

### A game has its shaders compiled, and no compiler

The engine compiles a shader the first time it is asked for, and keeps it in a cache -
which is what an editor wants, where shaders are edited while it runs. A game that is
handed to people wants neither the compiler nor the sources: Slang is a library of tens of
megabytes, compiling every permutation takes seconds that a player would wait through at
the start, and a shader that does not compile on a player's machine is a bug nobody can
fix there. So the engine does not link the compiler at all. Whoever has one gives it to
the engine (`EngineDesc::shaderCompiler`, which the editor fills in); an engine without
one reads every shader from its cache, trusting what is there - no sources to compare
with, no compiler version to check - and fails with the name of a shader it does not
find.

What a game needs in that cache is known before it runs: the engine names every
permutation of its shaders that it can ask for (`engineShaderRequests`), and a test drives
the renderer through every debug view and quality and holds what it asked for to that list,
both ways. An export compiles the list (`compileEngineShaders`) into the cache of the game.

### A game is a directory: its program, its project and its assets, as they are

An exported game is the project as the editor has it - the project file, the asset
directory with the `.meta` files that hold the ids - beside a program that plays it,
`cordite-player` under the name of the game. The assets are not packed into an archive of
a format of their own: the game reads them with the code the editor reads them with, which
the tests already hold, and a second reader of a second format would be one more thing to
get wrong. Handing the game out is putting the directory into whatever archive a
distribution channel takes.

The player links the engine and nothing of the editor. It needs no file of the build
either: the C runtime is linked in, Vulkan is loaded from the system when the program
starts - on macOS, which has none, from the bundle, see below - the shaders are compiled in
its cache. A test exports the showcase and plays it
from where it was put.

An export writes, last, a record that it made the directory (`.cordite/export.json`), with
the name of the program. Exporting into the same directory again - which is what people do
while they work on a game - replaces what the record names and nothing else: a file that
somebody put beside the game stays, and a directory that holds anything without such a
record is refused, so that an export never deletes what it did not write. The record is
read as anything in a file is: a program that is no plain name in the directory makes it no
record.

On macOS a program people start from the Finder is an application bundle, so there the game
is one (`GameLayout::Bundle`): the program in `Contents/MacOS`, the `Info.plist` that tells
the system what the program is called and what the game is known by, and the very directory
of a game - project, assets, cache, notices - in `Contents/Resources`, where the program
looks for its game when it is in a bundle (`gameDirectoryOf`). The destination is the bundle
itself rather than a directory that holds one: then the checks of a destination, the record
and what is replaced work as for a directory, where the record is in `Resources`. The record
says how the game was laid out, and an export of the other layout refuses to replace it - a
bundle taken for a directory would have its `Contents` left behind. Whether a program is in
a bundle is told by the shape of its path (`applicationBundleOf`), and the layout can be asked
for on any system, so that every part of a bundle is made and checked by the tests on Windows;
what only a Mac can tell - that it starts from the Finder - has not been tried.

macOS has no Vulkan, so a bundle brings one: MoltenVK, in `Contents/Frameworks`, which the
program loads by its path, directly - MoltenVK is a Vulkan implementation that an application
may link without the Vulkan loader, as its own guide describes. The loader would add a manifest
of the driver, the search for it in the bundle (which depends on its version) and nothing a
game uses: validation layers are for development, which on a Mac uses the loader of the
Vulkan SDK. The library is the one MoltenVK's publisher builds, pinned for macOS alone, since
building it takes Xcode and scripts of its own; without it beside the player an export still
makes the bundle, and says that it draws with the system's Vulkan.

### The notices of the libraries go with the program

Nearly every library the engine is made of - MIT, BSD, zlib, Apache, the OFL of the
typefaces - asks for its notice to accompany a copy. The build copies them into `licenses/`
beside the programs, with the list of them (`cmake/CorditeLicenses.cmake`), as it copies the
shaders, and an export ships that directory with the game. Two libraries keep their notices
in a header rather than a file, Lua and MikkTSpace; those are in `cmake/licenses/` as the
headers word them. A library that the engine starts to link adds its notice in the same
change; configuring fails when a notice that the list names is not where it says.

### The editor and the player have no console of their own

On Windows a program is built either for the console or for windows, and one built for the
console opens a console window as soon as it is started from Explorer - beside a game that
somebody double-clicked, or beside an editor opened by a project file. Both programs are
built for windows (`WINDOWED`), with `main` as their entry point all the same. Such a
program writes nowhere unless it is given somewhere: the pipes and files that tests,
scripts and Git Bash give it stay where they are, and what would go to a console goes to
the terminal that started it, if any (`attachToParentConsole`). A failure that a person
could otherwise only read in a log - a game that cannot start because the machine has no
Vulkan driver - is said in a dialog (`showErrorDialog`), which a headless run never opens.

The price is PowerShell, which waits for such a program only while it reads what the
program writes: `dev.ps1 run` reads it, and a person who starts one by hand pipes it into
`Out-Host`.

### The editor notices changed files by looking at them

Scripts are edited in a text editor, and textures in a paint program; the editor reads
them again when they change, so that what is played is what was saved - a script of a
scene that is being played goes on with its new text, keeping what its instances hold.
How it notices is by looking: every half second a thread of its own lists the files of
the assets, with their sizes and the times they were written, and compares (`DirectoryWatcher`).
The notifications each system offers differ from one another, need a library to be
used alike (efsw, for one), miss what happens on network drives, and come in bursts that
have to be gathered anyway; a look at a few thousand files costs milliseconds, off the
thread that draws, and is the same everywhere, which the tests rely on.

What changes is answered with what the Refresh button does: everything is read again.
That is simple and right, and slow for a large project, where the next step is to read
again only the files that changed. What the editor writes itself - a scene it saves - and
what a refresh writes - the .meta files of new assets - is taken for no change, or every
save would read the assets again. A batch run and a test do not watch: they see the files
as their own commands leave them.

### An editor works on one project, and begins again for another

An engine is made for the assets and the cache of one project: the asset database, the
shader cache, every system that holds what it loaded from them. Opening another project in
the same editor would mean taking all of that apart and putting it together again, which
is what making a new engine does anyway, and a second way of doing it would be one more
thing to keep right. So File > New project and Open project close the editor, and the
program that runs it - `editor/Main.cpp` - makes another on the project that was asked
for, in the same process: a new window, the same person at it. A batch run goes on with
the frames it has left, so that a script can make a project and look at it.

A new project is not empty. It has a scene to begin with - a camera that looks at a box on
a floor, under a sun and a sky - made of the engine's own components and built-in assets,
written by the engine's own serializer, so that it is right by construction and plays and
exports as it is. Making one writes over nothing: a directory that holds a project, or a
scene where the new one would go, is refused, and assets that are there already become the
project's.

### The File menu asks the system for paths

Where the editor needs a path - a scene to open, where to save one, a project, where a game
goes - it shows the dialog of the system it runs on, through nativefiledialog-extended:
Explorer's on Windows, the open and save panels of macOS, and on Linux the file chooser that
xdg-desktop-portal shows for the desktop in use. A dialog drawn by the editor itself would
work the same everywhere and could be tested, but it would be one more thing to keep right,
and it would not know the places a person keeps - favorites, network places, the folders
of the desktop - which is what people look for in such a dialog.

A dialog of the system holds the thread that shows it until it is answered, so the editor
shows it between two frames, where nothing is half drawn, and does with the answer what the
menu did before: a scene is opened or saved, a project made, opened or exported by its
command. What each item asks for is worked out by a function of its own and held by tests;
the editor reaches the dialogs through an interface, which a test answers itself, since no
test can wait for a person. An editor without a window has no dialogs, and says so: a batch
run and an agent use the commands that take paths. The library is linked into the editor
alone, so no exported game carries it.
