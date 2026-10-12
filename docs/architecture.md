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
| `math` | `core`    | glm type aliases and conventions; `Transform`, bounding volumes, rays and intersection tests, frustum culling, projection matrices, color conversions, keyframed curves, deterministic random numbers, spherical harmonics, and where a direction is in an image of everything around. |
| `platform` | `core`, `math` | The window system and windows (GLFW), and per-window keyboard, mouse and gamepad state with an injection API for tests and automation. |
| `assets` | `core`, `math` | Assets as data, and where they come from: images and mipmaps; meshes, primitives and mesh processing; materials; glTF import and export; the asset database, which knows what exists and under which id; the asset manager, which loads by id. Knows nothing of the GPU. |
| `world` | `core`, `math` | The scene: entities, their components and their parent/child hierarchy, stored with EnTT; the registry of component types known by name; scene files. |
| `animation` | `core`, `math`, `assets`, `world` | Movement of skeletons: the functions that make a pose (sampling a clip, blending two poses, the matrices a mesh is skinned with, the bounds of a posed mesh), the `Animator` component, and `AnimationSystem`, which plays the animators of a world and gives each the pose it is in. |
| `physics` | `core`, `math`, `world` | The simulation of rigid bodies: `PhysicsWorld` holds bodies - shapes, a mass, where each is and how it moves - steps them on in time and answers what a ray hits. Jolt Physics is behind it and shows in none of its headers. The `Collider` and `RigidBody` components say what of a world is simulated, and `PhysicsSystem` keeps the two in step. |
| `rhi` | `core`, `math`, `platform` | The GPU: Vulkan instance and device creation, the NVRHI device the renderer programs against, the swapchain, and texture readback. The only module that calls Vulkan. |
| `shaders` | `core` | What a shader *is* to the engine: requests and their permutation keys, compiled binaries, the on-disk cache, and the `ShaderLibrary` that resolves a request from memory, disk or a compiler. |
| `shader_compiler` | `core`, `shaders` | The Slang-based implementation of `IShaderCompiler`. Kept separate so exported games do not link a compiler. |
| `renderer` | `core`, `math`, `assets`, `world`, `animation`, `rhi`, `shaders` | Drawing: the scene renderer and its passes (depth, G-buffer, deferred lighting, forward, debug views), the GPU copies of assets it draws with, the targets of a view, the components that make an entity visible or make it a light, the lights of a world sorted into the clusters of a view, the environment that surrounds a world made ready to light it, ambient occlusion worked out from the screen; the view a camera gives, debug line drawing, and the `ShaderManager` that turns shader binaries into GPU shader objects. |
| `ui` | `core`, `math`, `platform`, `rhi`, `shaders`, `renderer` | The immediate-mode user interface: the Dear ImGui context, its input, and the renderer that draws it with NVRHI. |
| `engine` | all of the above | `Engine`: creates and owns the subsystems, the assets, the world and the interface, and runs the frame loop. Also what needs several of the modules below at once: projects, placing a model in a world, checking the assets a scene refers to. |
| `editor` | `engine` and below | The editor: the scene document, undoable actions, the command registry, the panels, and `EditorApp`. Not part of exported games. |

Further modules (audio, scripting) are added by the later milestones and will be
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
4. The program does its work: it changes the world as it sees fit, then calls
   `Engine::updateWorld()`, which steps the simulation of the world's bodies and
   puts the entities where their bodies have gone, plays the animations on by the
   time of the frame, poses the skeletons and works out where every entity is; it records render passes
   into the command list, and describes its interface with ImGui calls. A game calls
   `Engine::drawWorld()`, which renders the world through its primary camera; the
   editor renders it from the viewport's own camera into the viewport panel instead,
   and updates the world with its clock stopped: a scene that is being worked on is
   shown as it is, and does not play.
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
textures, materials and scenes. Two classes stand between those files and the rest of
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
depth + normals ──> ambient occlusion, traced ──> ambient occlusion: how much light from all around gets through
image of the sky ──> (when first used) nine spherical harmonics: what it lights matte surfaces with
                 ──> (when first used) a cube map, blurred level by level: what surfaces reflect
G-buffer + depth + clusters + shadow atlas + environment + ambient occlusion ──> deferred lighting ──> scene color (linear, float)
                                                             and the sky, where nothing was drawn;
                                                             all of it seen through the fog
translucent surfaces + clusters + shadow atlas + environment ──> forward pass ──> scene color, blended over it
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

A scene is lit by components: `Light` (directional, point or spot), `AmbientLight`
(a sky above and a ground below, as two colors) and `Environment` (an image of
everything far away, which lights the scene and is its sky). A `Fog` fills its air:
what is far away is seen through it, the sky included. For every view,
`SceneLights` gathers the enabled ones, and a `LightClusterBuilder` sorts the point
and spot lights into a grid of
clusters that fills what the camera sees - 16 columns, 9 rows, 24 slices into the
depth. Each cluster gets the list of the lights that touch it; a pixel works out which
cluster it is in and asks the lights of that list, and the directional lights, which
reach everywhere. The lights, the grid and the lists are three buffers in the binding
set of the view, next to its constants, so that the deferred lighting and the forward
pass read the same ones.

A light that casts shadows gets shadow maps: pictures of the scene's depth as the light
sees it, all of them tiles of one texture, the shadow atlas. The sun - the brightest
directional light that casts shadows - gets up to four, the cascades, each covering a
slice of the view; a spot light one; a point light six. The maps are drawn before
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
at and that later passes can tell from reflected light. Motion vectors will join when
something needs them (temporal anti-aliasing, motion blur); nothing does yet.

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

A view holds four directional lights and 1024 point and spot lights. More are left
out, counted in the statistics and reported once; lights that reach nothing in view
do not count. Lights are taken in the order of their entities' ids, which is the same
on every run and after any edit, so that neither the sums nor the choice of what is
left out depend on how components happen to be stored.

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
Engine 4"). A light without an end would be in every cluster's list.

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
- **The fog is not lit.** It has the color it is given, plus a second color towards
  the sun - the brightest directional light - that falls off with a power of the
  cosine. A sun that is seen through haze has a glow around it, and that is most of
  what light in fog looks like outdoors. Lamps that light the fog around them, and
  shafts of light between shadows, need the fog sampled along the ray; that is a
  different effect, and when it comes it adds to this one.

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

### Enum names use `nameOf`, not `toString`

doctest stringifies values by calling an unqualified `toString`, which finds any
overload in the value's namespace by argument-dependent lookup and then fails to
compile because the return type is not doctest's string. Using a different verb avoids
the collision without wrapping every enum in the tests.
