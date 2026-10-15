# Cordite

Cordite is a 3D game engine for Windows, macOS and Linux, written in C++20 and rendered
with Vulkan through [NVRHI](https://github.com/NVIDIA-RTX/NVRHI). Its default look is
the gritty, high-contrast, normal-mapped style of late-2000s console games: deferred
shading, deep shadows, strong speculars, bloom and filmic post-processing.

The full product brief is [AIGameEngine.md](AIGameEngine.md).

## Status

Cordite is under active development. Milestones follow the brief; the ones without a
number are what its list of milestones leaves out and its requirements ask for:

| # | Milestone | State |
|---|-----------|-------|
| 1 | Engine foundation: build system, core, math, tests, tooling | Done |
| 2 | Window and renderer skeleton: GLFW, Vulkan via NVRHI, swapchain, Slang shaders | Done |
| 3 | Scene and ECS: entities, components, hierarchy, scene files, editor with viewport and command automation | Done |
| 4 | Static mesh rendering: projects and asset ids, glTF import, textures, materials, instanced forward pass | Done |
| 5 | Deferred G-buffer: depth pre-pass, base pass, deferred lighting, forward translucency, debug views | Done |
| 6 | Lighting: directional, point and spot lights, ambient light, clustered light culling, lighting channels | Done |
| 7 | Shadows: shadow maps in an atlas for directional, point and spot lights, cascades for the sun, soft filtered edges | Done |
| 8 | HDR and post-processing: exposure by hand or automatic, bloom, filmic and ACES tone mapping, color grading with lookup tables, vignette, grain, quality presets | Done |
| - | Image-based lighting: HDR images of the sky as environments - the sky itself, its light on matte surfaces, its reflection on shiny ones | Done |
| 9 | Screen-space effects: ambient occlusion from the screen, fog, anti-aliasing (FXAA). Screen-space reflections, which the brief marks optional, are listed below | Done |
| - | Skeletal meshes and animation: skinned glTF models, skeletons and animation clips are imported, exported and listed as assets; an `Animator` component plays clips and blends two of them into a pose (`animation` module); skinned meshes are drawn in those poses on the GPU, instanced, with shadows and culling that follow the pose; the engine plays the animators of a world, placing a model sets its skeletons up, and the showcase has a hoist whose cable swings | Done |
| 10 | Physics and gameplay: rigid bodies, colliders, a character controller, input and scripting. So far: rigid bodies over Jolt Physics (`physics` module) - `Collider` and `RigidBody` components make boxes, spheres, capsules, the convex hulls around meshes, the triangles of meshes and bodies of several shapes that fall, collide and rest, in steps of a fixed length; kinematic bodies that push; raycasts, sphere casts and overlap queries, also as editor commands; triggers, and events for what begins and stops touching; `Joint` components that hold bodies together (fixed, ball, hinge, slider, distance); `CharacterController` components that make entities characters, which walk, jump, climb slopes and steps, ride platforms, push what is in their way and enter triggers; the editor draws colliders, joints and characters and does not simulate. Controls by name: an input map per project binds actions and axes to keys, mouse and gamepads (`InputMap`, `Engine::actions()`), with editor commands that show them and press them. Scripting has begun: `ScriptRuntime` (`scripting` module) runs Lua 5.4 with limits on instructions, memory and nesting, without the libraries that reach the machine, and with an environment for every chunk in which a name that was never given a value is an error. Scripts are assets (`.lua`) that a `Script` component puts on an entity: `ScriptSystem` gives each such entity an instance with its own state and the properties the component sets, calls its `start`, `update` and `destroy`, stops what raises an error and says so once, and loads scripts again when their files change. Scripts find, make and destroy entities, move them in their hierarchy, place and turn them (`cordite.Vec3` and `cordite.Quat`, with arithmetic), read and change every component through its reflected properties, checked as the editor checks them, and ask for the time, the ids of assets and the controls of the input map, by name (`cordite.input`, and `input(action, pressed)` for what is pressed and released); a script walks the showcase's sentry up and down, and another (`selftest.lua`) calls every function scripts are given in the running game and checks the answers. Whoever plays the showcase steers the sentry and makes it jump. Scripts ask the simulation what a ray or a ball hits and what is within reach, read and change gravity, and push bodies (`cordite.physics`, `entity:addImpulse`), and are told what meets what and what walks into a trigger (`collisionBegan`, `triggerEntered` and their ends): in the showcase a ball hops when "fire" is pressed and counts its landings, and a light is lit while the sentry stands at its post. Timers and tasks belong to the script that made them (`cordite.time.after`, `every`, `cancel`; `run` and `wait` for functions that take their time): the clock ticks with one, and a lamp flickers. A script asks for another scene (`cordite.scene.load`), which the engine loads between two frames once it has checked it: the showcase has an annex to travel to, and back. What scripts take of a frame is measured and budgeted, and the editor command `script.api` returns everything a script can call and everything the engine calls in one, in words. Scripts are heard: an `AudioSource` is played by setting its `playing`, and `cordite.audio` plays sounds that belong to no entity and sets the volumes of the buses. Scripts place prefabs with `cordite.world.instantiate` | Done |
| - | Audio: sounds are assets (WAV, FLAC, MP3); `AudioSource` components are heard from where their entities are by the entity with the `AudioListener` - quieter with distance, by an inverse or a linear rolloff, and from their side - or the same in both ears; five buses (effects, music, ambience, voice, interface) with volumes of their own; sounds without an entity; voices are limited, and what is heard stops while time does (`audio` module, over miniaudio). Mixed into memory unless a device is asked for, which is how it is tested. Scripts play sounds and set volumes (`cordite.audio`). What it does not do yet is listed below | Done |
| 11 | Editor maturity: asset browser, prefabs, undo and redo, play-in-editor, automation. So far: every edit is undoable and every action a command (since milestone 3); a scene is played in the editor where it is edited - Play, Pause, Step and Stop in the Play menu, `play.start` ... `play.stop` for scripts and agents - with its scripts, its simulation and its sounds running, and is put back exactly as it was when it stops; prefabs - an entity with everything below it is made one (`prefab.create`, Entity > Make prefab), placed as often as it is wanted (`asset.instantiate`, Assets > Place prefab) and placed by scripts (`cordite.world.instantiate`), each time as copies that a later change to the prefab does not reach; an asset browser - the Assets panel lists the assets of the project as a tree of directories, files and what is inside files, narrowed down by type and by text, a model or a prefab is placed by a double click or dragged onto an entity of the hierarchy, and any asset is dragged onto a property of the inspector that takes one of its kind (`assets.browse` says and sets what the panel lists). Still to come: renaming and moving assets and previews of them in the browser, gizmos in the viewport, copy and paste | In progress |
| - | A modern editor: a flat interface with typefaces and icons of its own and a layout after Unity and Unreal Engine, themes to choose from, and projects that open in the editor when their file is double-clicked | Planned |
| 12 | Packaging and export | Planned |

What animation does not do yet: more than two clips at once (layers, blend trees, a
state machine), motion of the figure taken from a clip (root motion), things that hang
on a joint and follow it, morph targets, inverse kinematics, and events at times of a
clip. The editor shows an animation at the time its animator holds and does not play
it.

What sound does not do yet: it has been measured, not listened to - the tests state
what is mixed in numbers, the device of one machine was opened once, at no volume, and
no person has heard the showcase. A sound is decoded whole when it is loaded, which is
right for effects and costs memory for music (a minute of stereo is 23 MB); Ogg Vorbis
is not read. There is no Doppler effect, no reverberation, nothing in the way of a
sound, no cones; the buses are a fixed five, without effects and without routing; one
listener hears; the device is the machine's default. The editor plays the sounds of a
scene when it plays the scene, and does not play a single sound on request.

What the brief asks of the renderer that no milestone names, and that is not built
yet: screen-space reflections, decals, depth of field, motion blur (and the motion
vectors it and a temporal anti-aliasing need), light shafts and lens flares, rect
lights, light cookies, lights that light the fog, local reflection and ambient probes,
baked lighting, levels of detail, texture streaming, render-to-texture and more than
one viewport in the editor. They follow once the milestones above stand.

## Building

### Windows

Requires Visual Studio 2022 with the "Desktop development with C++" workload.

```powershell
./scripts/bootstrap.ps1     # once: downloads pinned CMake and Ninja into .tools/
./scripts/dev.ps1 build
./scripts/dev.ps1 test
```

### Linux (Ubuntu 24.04+) and macOS

```bash
# Ubuntu: sudo apt install build-essential cmake ninja-build xorg-dev libwayland-dev libxkbcommon-dev
# macOS:  brew install cmake ninja
cmake --preset linux        # or: macos
cmake --build --preset linux-dev
ctest --preset linux-dev
```

All third-party code is downloaded at configure time from archives pinned by SHA-256;
see [docs/third-party.md](docs/third-party.md).

## Running

A Vulkan 1.3 capable GPU and driver are required.

```powershell
./scripts/dev.ps1 run -Target cordite-editor -- --project samples/showcase
```

![The editor showing the showcase scene](tests/render/references/editor-showcase-1280x720.png)

The picture is one of the reference images the rendering tests compare against. It
shows the showcase project, [samples/showcase](samples/showcase), which uses every
feature the engine has so far: here a sky that lights the scene, a sun, a street lamp
(a spot light) and a wall light, all three casting shadows, and a light that shines on
the ball alone.

In the viewport, hold the right mouse button to look around and fly with W A S D, Q and
E; hold the middle button to slide; turn the wheel to move forwards and backwards.

F5 plays the scene where it stands: its scripts run, its bodies fall, its sounds are
heard, the viewport shows it through the camera of the game (View > Game camera
switches between that and the editor's own), and the controls of the project reach the
game - in the showcase W A S D walk
the sentry and Space makes it jump, the left mouse button makes the ball hop, and T
travels to the annex. F6 holds the game and lets it go on, F10 lets one frame of a held
game pass, and Shift+F5 stops it: the scene is then exactly what it was before,
whatever the game did to it.

## Projects and assets

A game is a project: a directory with a `cordite.project.json` and an `assets`
directory holding models (glTF 2.0: `.gltf`, `.glb` - with their skeletons and
animations, when they are skinned), textures (PNG, JPEG, TGA, BMP,
HDR), materials (`.material.json`), scenes (`.scene.json`), prefabs (`.prefab.json`), scripts (`.lua`) and sounds (WAV, FLAC, MP3).
Every asset has an id
that survives renaming and moving - it is kept in a `.meta` file next to the asset -
and scenes refer to assets by id only. Meshes for blocking out a scene (cube, sphere,
plane, cylinder, cone, capsule, torus) are built in.

The showcase's own textures, models and sounds are generated from code by a small tool, so
the repository contains no third-party artwork:

```powershell
./scripts/dev.ps1 run -Target cordite-sample-assets -- samples/showcase/assets
```

## Lighting

A scene is lit by components. `Light` is a directional, a point or a spot light; it
shines from where its entity is, along the entity's -Z axis. `AmbientLight` is light
from all around, from a sky above and a ground below. An intensity of 1 lights a white,
matte surface that faces the light to white - at any distance for a directional light,
one metre away for a point or a spot light, which thin out with the square of the
distance and end at their range.

`Environment` surrounds the scene with an image of everything far away: an HDR image
of a sky, twice as wide as high, such as those of [Poly Haven](https://polyhaven.com/hdris).
Put the `.hdr` file among the project's assets and name it as the component's
`texture`. It is the sky that is seen, it lights matte surfaces with all it shows, and
shiny surfaces reflect it - sharply or blurred, as rough as they are. `intensity`
scales its light, `rotationDegrees` turns it about the vertical axis (to line its sun
up with a directional light that casts the shadows), and without `visible` it lights
the scene and stays unseen. The "Reflections" view of the editor makes a mirror of
every surface, to show what there is to reflect.

A view can hold hundreds of point and spot lights: they are sorted into a grid of
clusters, and a pixel only asks the lights near it. Every mesh and every light is in
some of eight lighting channels, and a light shines on a mesh when the two share one.
A scene without lights is black in a game; the editor lights it until it has lights of
its own. The "Light count" view of the editor shows how many lights each pixel asks.

Where light from all around - ambient light, an environment - does not get to, it is
darker: in corners, under things, at the foot of a wall. That is ambient occlusion,
worked out from the picture itself. How strong it is, how far around a point things
are looked for and how much they must rise above a surface to count are set with the
rest of the look of a scene (`PostProcess`: `ambientOcclusionIntensity`, `...Radius`,
`...Bias`); how finely it is worked out is the screen-effects quality of the view. The
"Ambient occlusion" view of the editor shows it alone.

A scene can have fog (`Fog`): what is far away fades into its color, the sky included.
It is at its densest at the height of its entity and below, and thins out above, so
that it can lie on the ground while what stands tall rises out of it; towards the sun
it glows in a second color. `density` is the part of the light that is lost with
every metre, `heightFalloff` how fast it thins out; the "Fog" view of the editor shows
how much of each pixel is fog.

The steps that slanted edges have in a picture made of pixels are smoothed in the
finished picture (FXAA): one more pass, which needs nothing of the scene. It is a
switch of the view - View > Quality > Anti-aliasing in the editor,
`SceneRenderSettings::antiAliasing` in code - and on unless it is turned off.

A light casts shadows when it is told to (`castsShadows`): the sun through up to four
cascades that reach as far as its shadow distance, a spot light through one shadow map,
a point light through six. All of them share one atlas, whose size - and with it how
sharp the shadows are - is the shadow quality of the view: off, low, medium or high.
Meshes throw shadows unless told not to; what lets light through throws none.

## The finished picture

The renderer works out light, which has no upper limit; what a screen shows of it is
set by `PostProcess` components. One says how the picture is exposed (by a fixed number
of stops, or automatically, the way an eye gets used to light), how much of what is
brighter than white blooms, which curve brings the light into the range of a screen
(`filmic`, `aces` or none), and how the result is graded: white balance, contrast,
saturation, lift / gamma / gain, a lookup table made in any image editor, a vignette,
chromatic aberration and film grain.

A component applies everywhere, or in and around a box - walk into a cave and the
picture changes with it - or, on a camera, to what is seen through that camera.
Several are laid over one another by priority. A scene that has none gets a filmic
curve and a little bloom. How finely all of it is worked out is a quality like that of
the shadows, and one word - low, medium, high - sets both.

## Automation

Everything the editor can do is a named command that takes and returns JSON, and the
editor runs without a window. A script is a list of commands:

```json
[
    {"command": "entity.create", "params": {"name": "Eye", "components": {"Camera": {}}}},
    {"command": "component.set", "params": {"entity": "Eye", "component": "Transform",
                                            "properties": {"position": [0, 1.7, 4]}}},
    {"command": "asset.instantiate", "params": {"asset": "models/crate.gltf"}},
    {"command": "editor.screenshot", "params": {"path": "out/eye.png"}},
    {"command": "scene.save", "params": {"path": "out/eye.scene.json"}}
]
```

```powershell
./scripts/dev.ps1 run -Target cordite-editor -- --headless --project samples/showcase --exec script.json
```

The outcome of every step is written to standard output as JSON, and the exit status
says whether the script ran to the end. `editor.commands` lists every command with its
parameters, `component.types` describes every component and property, and `asset.list`
shows every asset; that is what an AI agent reads to find out what it can do. The same commands can be typed into the
console panel of the interactive editor. `--help` lists every command-line option.

## Documentation

- [AGENTS.md](AGENTS.md) - how to work in this repository: commands, coding standards,
  testing and review rules. Written for both people and AI agents.
- [docs/architecture.md](docs/architecture.md) - module map, conventions and the
  reasoning behind the main design decisions.
- [docs/third-party.md](docs/third-party.md) - every library the engine uses, with
  versions and licenses.
- [.claude/skills](.claude/skills) - step-by-step guides for recurring tasks: building
  and testing, adding a component, a render pass, a dependency, an editor command or
  a function for scripts, and working with assets.
- Public APIs are documented in their headers under `engine/cordite/`, each with a
  usage example.
