# Learning from Cordite

Cordite was built to be read. This is a route through it for someone who wants to learn how
a game engine is put together: what to read, in which order, what to look for in each
place, and something to try at each stop. It takes you from the first line of `core` to an
exported game.

Three documents stay at your side the whole way:

- [docs/architecture.md](architecture.md) - how the engine is put together, and under
  "Design decisions" **why** each choice was made, with the alternatives that were turned
  down. Most stops below point at a few of its sections.
- [AGENTS.md](../AGENTS.md) - the rules the code keeps, written for whoever changes it. Every
  rule there is there because something went wrong without it; its "Known pitfalls" are a
  catalogue of real bugs and their causes.
- [.claude/skills](../.claude/skills) - step-by-step walkthroughs of recurring tasks: adding a
  component, a render pass, a script function, a dependency.

The [user guide](guide/README.md) explains what the engine does from the outside; read its
[Getting started](guide/getting-started.md) first if you have not used the editor yet.

## How to read a module

Every public header starts with a `/// @file` block that says what the module part is for,
with an example of its use, and every public function says what it does and how it fails.
Read the header before the source. Then read the tests in `tests/unit/<module>/` - they show
the behavior that is promised, including the edge cases and the failures - and only then the
`.cpp`. Run one suite while you read:

```powershell
./scripts/dev.ps1 build -Target cordite_core_tests
build/windows/bin/RelWithDebInfo/cordite_core_tests.exe -ts=Reflection
```

## 1. The shape of the engine

Read: [README.md](../README.md), architecture.md "Shape of the engine" and "Modules",
`CMakeLists.txt`, `cmake/CorditeModule.cmake`, `cmake/CheckModuleLayering.cmake`.

The engine is a set of static libraries, one per module, layered so that a module uses only
those below it. Look at how `cordite_add_module` declares what a module may depend on, and
how the `module_layering` test reads every `#include` to hold it to that - an architecture
that a test enforces does not erode. Look at `cmake/CorditeDependencies.cmake`: every
third-party library is an archive pinned by its SHA-256 (architecture.md: "Dependencies are
fetched as archives pinned by SHA-256").

Try: add `#include "cordite/renderer/SceneRenderer.h"` to a source of `core`, build, and run
`./scripts/dev.ps1 test -Filter module_layering`.

## 2. Foundations: errors, logs, reflection

Read: `core/Result.h`, `core/Assert.h`, `core/Log.h`, `core/Json.h`, `core/Reflection.h`,
`core/JobSystem.h`; architecture.md "Expected failures return Result, not exceptions",
"Assertions are for programmer errors only", "Input is parsed into a patch before it touches
an object".

Two kinds of failure, kept apart everywhere: what can happen to a correct program - a missing
file, bad input - returns a `Result`; what is a bug asserts. Then the most important file of
the engine: `Reflection.h`. A component is described once with `TypeBuilder` - its
properties, their types, ranges, units, descriptions - and that one description drives scene
files, the editor's Inspector, undo, the automation API and what scripts can read and write.
Note how JSON never reaches an object unchecked: `TypeInfo::parse` validates and returns a
`PropertyPatch`, and applying a patch cannot fail - so an edit either happens whole or not at
all.

Try: in `tests/unit/core/ReflectionTests.cpp`, read "a rejected update leaves the object
exactly as it was", then break `TypeInfo::fromJson` so that it applies values as it reads
them, and watch that test fail.

## 3. Math

Read: `math/Math.h` (the conventions), `math/Transform.h`, `math/Projection.h`,
`math/Frustum.h`, `math/SphericalHarmonics.h`.

Right-handed, +Y up, -Z forward, metres, column vectors; depth is reversed (1 near, 0 far),
which spreads the precision of a depth buffer evenly over distance. `Projection.h` shows the
matrices; the spherical harmonics are what turns an image of the sky into the light it casts
on a matte surface, in nine numbers.

## 4. The world: entities and components

Read: `world/World.h`, `world/Components.h`, `world/ComponentRegistry.h`,
`world/SceneSerializer.h`, `world/Prefab.h`; architecture.md "Entities and components are
stored with EnTT", "Scene files are JSON, strict, and stable", "A prefab is copied where it is
placed".

Components are plain structs; systems live elsewhere and read them. Entities are referred to
by `Uuid` across saves and undo, never by handle. Scene files load strictly - an unknown
property is an error naming the entity - and save byte for byte the same, which makes them
work in version control.

Try: open `samples/showcase/assets/scenes/showcase.scene.json`; every component is explained
in its comments.

## 5. Assets

Read: `assets/AssetDatabase.h`, `assets/AssetManager.h`, `assets/GltfImporter.h`,
`assets/Mesh.h`, `assets/MeshProcessing.h`, `assets/Material.h`, `assets/Image.h`;
architecture.md "An asset's id lives next to the asset, in a .meta file", "glTF is imported
with cgltf, MikkTSpace and meshoptimizer", "The texture slot decides the color space".

Every file is untrusted: look at how `GltfImporter.cpp` checks every count and offset of a
glTF file with arithmetic that cannot overflow before using it, and at the tests that feed it
truncated and lying files. Tangents come from MikkTSpace, the standard every baker uses, so
that normal maps look as they did in the tool that made them.

## 6. Talking to the GPU

Read: `rhi/GraphicsDevice.h`, `shaders/ShaderLibrary.h`, `shaders/ShaderTypes.h`,
`shaders/EngineShaders.h`, `renderer/ShaderManager.h`, `renderer/RenderTarget.h`;
architecture.md "The application owns the Vulkan device, NVRHI wraps it", "Shaders are
written in Slang", "Shader permutations are identified by a content-independent key",
"Validation is part of the test contract".

Vulkan is reached through NVRHI, which tracks resource states and inserts barriers. Shaders
are Slang, compiled to SPIR-V; a permutation is a path, an entry point and sorted defines,
cached on disk by a key a test pins. Every GPU path works without a window - the swapchain only
receives a copy - which is why the renderer is tested headless, with the Vulkan validation
layers on and every message treated as a bug.

## 7. The renderer, one frame

Read `renderer/SceneRenderer.h` and `SceneRenderer.cpp` first: `render()` is the frame, in
this order. Then each step, with its shader in `shaders/`:

| Step | C++ | Shaders | architecture.md |
|------|-----|---------|-----------------|
| What is drawn, in how few draw calls | `MeshDrawList.h`, `MeshPass.h` | `passes/MeshVertex.slang` | "Instances are a vertex stream" |
| Levels of detail | `LevelOfDetail.h`, `MeshProcessing.h` (`generateLods`) | `passes/MeshDebug.slang` | "Levels of detail are made when a mesh is loaded, and chosen by their error on the screen" |
| Shadow maps, in one atlas | `ShadowPlan.h`, `ShadowAtlas.h` | `common/Shadows.slang` | "All shadow maps of a view share one atlas", "Cascades are spheres, moved in whole texels", "A surface is kept out of its own shadow by where it is looked up" |
| Depth pre-pass, then the G-buffer | `SceneTargets.h` | `passes/MeshDepth.slang`, `passes/MeshGBuffer.slang`, `common/GBuffer.slang` | "A depth pre-pass, then a base pass on equal depth", "The G-buffer is four targets and depth" |
| Decals, laid onto the G-buffer | `Decals.h`, `DecalPass.h` | `passes/Decal.slang`, `common/Decals.slang` | "Decals are laid onto the G-buffer" |
| Ambient occlusion | `AmbientOcclusion.h`, `AmbientOcclusionPass.h` | `passes/AmbientOcclusion.slang`, `common/AmbientOcclusion.slang` | "Ambient occlusion is measured from the surface, in slices" |
| Lights, sorted into clusters | `LightClusters.h`, `SceneLights.h` | `common/Lights.slang` | "Lights are found through clusters, built on the CPU", "Brightness is set for artists, computed in radiance" |
| Deferred lighting | `DeferredLightingPass.h` | `passes/DeferredLighting.slang`, `common/SceneLighting.slang`, `common/Brdf.slang` | "Solid surfaces are shaded deferred, translucent ones forward" |
| The environment | `EnvironmentMaps.h`, `EnvironmentLight.h` | `common/Environment.slang`, `passes/EnvironmentPrefilter.slang` | "An environment is one image, and three things are made of it" |
| Fog | `Fog.h` | `common/Fog.slang` | "Fog is a formula in the lighting, not a pass" |
| Translucent surfaces | `MeshDrawList.h` | `passes/MeshForward.slang` | |
| Light shafts | `LightShafts.h`, `LightShaftPass.h` | `passes/LightShafts.slang`, `common/LightShafts.slang` | "Light shafts are the sky around the sun, blurred towards it" |
| Depth of field | `DepthOfFieldPass.h`, `PostProcessSettings.h` | `passes/DepthOfField.slang`, `common/DepthOfField.slang` | "Depth of field is gathered at half the size, from light, before the picture is finished" |
| Finishing the picture | `PostProcessPass.h`, `PostProcessSettings.h` | `passes/Exposure.slang`, `passes/Bloom.slang`, `passes/PostProcess.slang`, `common/ToneMapping.slang`, `passes/AntiAliasing.slang` | "A picture is finished in one pass", "Bloom is a chain of halvings", "Automatic exposure is measured from the bloom", "Edges are smoothed from the finished picture" |

The deepest lesson of the renderer is in how it is tested. A test that can say what a pixel
must be does so - `tests/render/SceneLightingTests.cpp` lights a white, matte wall with lights
whose brightness on it follows from the model, and checks the pixels against numbers worked
out on the CPU; several pieces of arithmetic are written twice, in C++ and in Slang, and the
tests hold one against the other. Reference images are only for what only an eye can judge.

Try: follow [.claude/skills/add-render-pass](../.claude/skills/add-render-pass/SKILL.md) to
add a debug view, and see it in the editor under View > Render view.

## 8. Skeletal animation

Read: `animation/Pose.h`, `animation/AnimationSystem.h`, `renderer/SkinPalettes.h`;
architecture.md "Joints are not entities", "A clip finds its joints by name", "Skinning is a
version of the one vertex shader", "The joint matrices of a view are one buffer".

A pose is made in steps - rest pose, sample a clip, blend - and turned into skinning matrices.
Skinning is a permutation of the one vertex shader every mesh pass uses (the base pass keeps
only pixels whose depth equals what the pre-pass wrote, which holds only while both compute
positions with the same code). `tests/support/SkinnedModelSupport.h` is a bar of three joints
whose every pose can be worked out by hand.

## 9. Physics

Read: `physics/PhysicsWorld.h`, `physics/PhysicsSystem.h`, `physics/CharacterSet.h`;
architecture.md from "Physics is Jolt, behind a world of the engine's own types" to "Time
passes for bodies in steps of one length".

Jolt Physics stays behind `PhysicsWorld`: no engine header shows one of its types. Study how
`PhysicsSystem` keeps entities and bodies in step without either owning the other, how
contacts are turned into begin and end events once per pair, and why a simulation gives the
same bits on every compiler (`PhysicsWorldTests` pins a fingerprint of a toppling pile).

## 10. Sound

Read: `audio/AudioSystem.h`, `assets/Sound.h`; architecture.md "What is heard is said by
components", "An engine opens no sound device unless it is told to".

miniaudio mixes; the engine says what plays. Sound is tested with numbers: played into memory,
with samples that count their own frames, so a test can say which ear heard what, how loud,
from which frame on.

## 11. Scripting

Read: `scripting/ScriptRuntime.h`, `scripting/ScriptSystem.h`, `scripting/ScriptBindings.h`,
`scripting/ScriptComponentApi.cpp`; architecture.md from "Scripts are Lua, bound by hand" to
"Scripts run the same way twice".

The sandbox is the lesson here: limits on instructions, memory and nesting, a build of Lua
without files or the operating system, an environment per chunk where an unknown name is an
error - and for each rule a test in `ScriptRuntimeTests.cpp` that tries to get past it.
Components reach scripts through the reflection of stop 2, so a new component is scriptable
without a line written for it.

Try: [.claude/skills/add-script-function](../.claude/skills/add-script-function/SKILL.md),
then call your function from `samples/showcase/assets/scripts/selftest.lua`.

## 12. The engine and its frame

Read: `engine/Engine.h` and `Engine.cpp`, `renderer/FramePacer.h`,
`engine/SceneChange.h`; architecture.md "A frame", "The engine never gets more than three
frames ahead of the GPU", "The renderer is told the time".

`Engine` owns the subsystems and runs the frame: input, scripts, physics in fixed steps,
animation, transforms, rendering, the interface. With a fixed time step every run draws the
same frames, which is what lets tests compare pictures.

## 13. The editor

Read: `editor/EditorContext.h`, `editor/EditorActions.h`, `editor/EditorCommands.h`,
`editor/CommandRegistry.h`, `editor/UndoStack.h`, `editor/PlayMode.h`,
`editor/EditorApp.h`; architecture.md "The editor", "Undo stores ids and validated patches",
"A scene is played in the world it is edited in".

Four layers: the document; every edit as an undoable action; every action as a named command
with JSON in and out; and the interface on top. Nothing exists only in the interface, which is
why an AI agent can do everything a person can (see the [automation
guide](guide/automation.md)). The interface is Dear ImGui and runs without a window too, so
`tests/render/EditorAppTests.cpp` clicks through menus and compares pictures of the whole
editor.

## 14. Shipping a game

Read: `shader_compiler/EngineShaderCache.h`, `engine/Game.h`, `player/Main.cpp`,
`editor/GameExport.h`, `cmake/CorditeLicenses.cmake`; architecture.md "A game has its shaders
compiled, and no compiler", "A game is a directory", "The notices of the libraries go with the
program".

## 15. Testing as a discipline

Read: `tests/support/TestSupport.h`, `tests/render/SceneTestSupport.h`,
`tests/perf/budgets.json`, `.claude/skills/pre-commit-review/SKILL.md`, and the "Testing"
section of AGENTS.md.

Every change in the history of this repository passed one gate (`./scripts/dev.ps1 verify`):
formatting, two compilers, three configurations, every test, and the linter. Look at how
tests are written to fail when they should - pinned values for anything persisted, numbers
before pictures, timing never trusted without a generous bound - and at the performance
suite, which holds counts to tight budgets and times to loose ones. The commit messages of
`git log` say, for each change, what it does, why, and how it is tested: reading them in order
is the history of the engine, told by the person who made each step.
