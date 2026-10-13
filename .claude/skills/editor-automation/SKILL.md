---
name: editor-automation
description: Drive the Cordite editor without a person, and extend what can be driven. Use when building or changing a scene from a script, taking screenshots of the editor or the viewport, checking what the editor shows after a change, adding an editor command or an undoable action, or when an editor feature exists only as a button.
---

# Editor automation

Everything the editor can do is a named command with JSON parameters and a JSON result.
Commands are the same code path the menus use, so they are undoable and tested.

## Run a script

A script is a JSON array of steps:

```json
[
    {"command": "scene.open", "params": {"path": "scenes/showcase.scene.json"}},
    {"command": "entity.create", "params": {"name": "Probe", "parent": "Stage",
                                            "components": {"Transform": {"position": [0, 1, 2]}}}},
    {"command": "asset.instantiate", "params": {"asset": "models/crate.gltf", "parent": "Probe"}},
    {"command": "selection.set", "params": {"entities": ["Probe"]}},
    {"command": "viewport.frame", "params": {"entity": "Probe", "radius": 3}},
    {"command": "editor.screenshot", "params": {"path": "build/shots/probe.png"}}
]
```

```powershell
./scripts/dev.ps1 run -Target cordite-editor -- --headless --width 1280 --height 720 `
    --project samples/showcase --exec script.json --exec-output build/outcome.json
```

- `--headless` needs no desktop. Add `--validation` to have the GPU validation layers
  check the run.
- `--project <directory>` gives the editor a project: its assets, and its start scene,
  which is opened before the script runs. Without a project only the built-in assets
  exist.
- `--scene <file>` opens that scene instead of the project's start scene.
- Without `--exec-output` the outcome is written to standard output; logs always go to
  standard error.
- `--frames N` renders N more frames after the script; `--screenshot <file>` saves the
  last one.
- `--fixed-time-step 0.0166667` makes every frame take a sixtieth of a second, whatever
  the clock says. Use it when a screenshot is to be compared with another: automatic
  exposure and film grain depend on the time.

Exit status: `0` success, `1` the editor could not run, `2` the validation layers
reported something, `3` a script step failed.

The outcome lists every step that ran:

```json
{
    "ok": false,
    "results": [
        {"command": "scene.open", "ok": true,
         "result": {"name": "Showcase", "path": "samples/showcase/assets/scenes/showcase.scene.json",
                    "unsavedChanges": false, "entityCount": 29}},
        {"command": "entity.create", "ok": false,
         "error": {"code": "NotFound", "message": "entity.create: no entity is named 'Stag'"}}
    ]
}
```

A failed step ends the script; the steps after it do not run.

## Find out what exists

Do not guess command or property names; ask the editor.

| Command | Returns |
|---------|---------|
| `editor.commands` | Every command with its description and parameters |
| `component.types` | Every component type with its properties: type, range, unit, options, description |
| `scene.describe` | The hierarchy: ids, paths, enabled state, component names |
| `entity.get` | Everything about one entity, including all property values |
| `editor.state` | Scene file, unsaved changes, what undo and redo would do, the selection |
| `asset.list` | Every asset with its id, type, name and path; optionally of one type |
| `asset.get` | One asset: dependencies, import warnings, and its content (size, properties, nodes) |
| `scene.validate` | References to assets that are missing or of the wrong kind, and colliders that no shape or body can be made of |
| `editor.statistics` | What the last frame cost: CPU and GPU time, draw calls, triangles, lights, shadow maps, memory on the GPU |
| `physics.raycast` | What a ray hits first - or, with a `radius`, a ball moved along it: the entity, the distance, the point and the normal |
| `physics.overlap` | The entities whose bodies reach into a ball (`radius`) or a box (`size`, `rotation`) |
| `play.status` | Whether the scene is being played (`stopped`, `playing`, `paused`), for how many frames, and what keeps scripts of it from running |
| `script.api` | What gameplay scripts are written against: every function a script can call and every function the engine calls in one, each with its signature and what it does; `prefix` narrows the functions to a library (`"cordite.physics."`) |

## Refer to entities

Wherever a command takes an entity, it accepts:

- its id: `"10000000-0000-4000-8000-000000000002"` - always unambiguous;
- its path of names from a root: `"Stage/Crate"` (or `"/Stage"` for a root);
- its name: `"Crate"` - only if exactly one entity has that name.

A name or path that matches several entities is an error that lists the candidates with
their ids. Commands that create or change an entity return its `id` and `path`; keep the
id when the entity will be referred to again.

## Use assets

Components refer to assets by id. Get the id from `asset.list` (or `asset.get`), then
set it like any other property:

```json
[
    {"command": "asset.list", "params": {"type": "mesh"}},
    {"command": "entity.create", "params": {"name": "Block", "components": {
        "MeshRenderer": {"mesh": "0a56c2f1-21e0-819d-8bac-567d1e1c4345",
                         "materials": ["9e35de7e-da93-8003-824c-d50e66fb2ad4"]}}}},
    {"command": "scene.validate"}
]
```

- Commands that take an *asset* (`asset.get`, `asset.instantiate`) accept its id or its
  path as `asset.list` shows it: `"models/crate.gltf"`, `"models/crate.gltf#mesh/0"`,
  `"builtin#mesh/cube"`. Properties of components take ids only.
- `asset.instantiate` places a whole model: an entity per part, under one new root that
  it returns. Move that root to place the model. A model with a skeleton also gets an
  entity named `Skeleton` with an `Animator`; to animate it, set the animator's `clip`
  to one of the model's animations (`asset.list` with `"type": "animation"`), and its
  `time` to the moment of the clip to look at - the editor shows a pose, it does not
  play (skill: `work-with-assets`).
- `asset.instantiate` places a prefab the same way: copies of its entities, under the
  root it returns, which a `PrefabInstance` component marks. `prefab.create` makes a
  prefab of an entity and everything below it: `{"entity": "Yard/Cart", "path":
  "prefabs/cart.prefab.json"}`. It writes a file, which undo does not take back
  (skill: `work-with-assets`).
- The built-in meshes (cube, sphere, plane, cylinder, cone, capsule, torus) have the
  same ids in every project. `materials` takes one entry per submesh (`asset.get` tells
  how many a mesh has); a missing or null entry is drawn with the default material.
- Physics is components: a `Collider` gives an entity a shape (`shape`: `box`,
  `sphere`, `capsule`, or - made of the mesh asset its `mesh` names - `convexHull`
  and `mesh`), a `RigidBody` makes it a body (`motion`: `static`, `kinematic`,
  `dynamic`). The triangles of a mesh (`mesh`) are only for bodies that are not
  dynamic; `scene.validate` says where that, or a missing mesh, is in the way. A
  collider with `"trigger": true` stops nothing and notices what moves into it; the
  viewport draws it in orange, and rays and overlaps do not find it.
  `viewport.show` with `{"colliders": true}` draws every collider in the viewport.
  `input.map` returns the controls of the project - its actions and axes, and what
  moves them; `input.state` what each was doing in the last frame; `input.inject`
  does what a hand does: `{"key": "Space", "down": true}`, `{"mouseButton": "Left",
  "down": false}`, `{"mouseMove": [x, y]}`, `{"scroll": [x, y]}`, `{"gamepad": 0,
  "buttons": ["A"], "axes": {"LeftX": 0.5}}`. What is injected is seen by the next
  frame (`editor.runFrames`), and a key stays down until it is let go of.
  `physics.raycast` and `physics.overlap` ask what is where - in the scene as it is,
  without a frame having to go by first; they name the entity that has the body.
- A `Joint` holds the body its entity is part of to that of the entity `connectedTo`
  names by id (none: to the world): `type` is `fixed`, `ball`, `hinge`, `slider` or
  `distance`; `anchor` and `axis` are in the entity's own space; `minimum` and
  `maximum` are degrees for a hinge and metres otherwise. It holds the two entities
  as they stand when the game begins, and whatever it is given later is said of
  that moment. The viewport draws it with the colliders, and `scene.validate` says
  where one will hold nothing.
- A `CharacterController` makes its entity a character: an upright capsule of
  `radius` and `height` with its soles where the entity is, that walks up ground of
  up to `maxSlope` degrees and steps of up to `stepHeight`. In a game it is walked
  by setting `movement` (metres per second, in the world) and `jump` (a speed,
  taken when it is passed on), and says `velocity`, `ground` (`onGround`,
  `onSteepGround`, `inAir`), `groundNormal` and `groundEntity` - none of which a
  scene holds, and the last four of which cannot be set. Its entity has no
  `RigidBody` and no `Collider` but a trigger. The viewport draws its capsule with
  the colliders.
  The editor does not simulate until the scene is played (see "Play the scene"):
  nothing falls, and `editor.statistics` reports the bodies there are and no steps.
- A new material is a `.material.json` file written into the project's `assets`
  directory, followed by `asset.refresh` (skill: `work-with-assets`).
- Run `scene.validate` after editing references; a reference to nothing draws nothing
  and says so only in the log.

## Light a scene

A scene shows what its lights light. The viewport lights a scene that has no light
component at all, so that it can be worked on; a game does not, and renders such a
scene black. Lights are components like any other:

```json
[
    {"command": "entity.create", "params": {"name": "Sun", "components": {
        "Transform": {"rotation": [-0.40762199, 0.3640026, 0.181485, 0.81756322]},
        "Light": {"type": "directional", "color": [1, 0.95, 0.87], "intensity": 0.6,
                  "castsShadows": true, "shadowDistance": 40}}}},
    {"command": "entity.create", "params": {"name": "Ambient", "components": {
        "AmbientLight": {"skyColor": [0.26, 0.31, 0.4], "groundColor": [0.1, 0.09, 0.08]}}}},
    {"command": "entity.create", "params": {"name": "Sky", "components": {
        "Environment": {"texture": "0c40bfdd-8a6c-8590-95e3-3128fea948a7", "intensity": 1,
                        "rotationDegrees": -132, "visible": true}}}},
    {"command": "entity.create", "params": {"name": "Lamp", "components": {
        "Transform": {"position": [0, 3, 0], "rotation": [-0.70710677, 0, 0, 0.70710677]},
        "Light": {"type": "spot", "intensity": 4, "range": 6,
                  "innerConeDegrees": 25, "outerConeDegrees": 40, "castsShadows": true}}}},
    {"command": "entity.create", "params": {"name": "Haze", "components": {
        "Transform": {"position": [0, 0, 0]},
        "Fog": {"color": [0.2, 0.185, 0.17], "density": 0.02, "heightFalloff": 0.25,
                "sunColor": [0.7, 0.42, 0.2], "sunExponent": 6}}}}
]
```

- A light shines from where its entity is. A directional and a spot light shine along
  the entity's -Z axis: the rotations above point the sun 53 degrees down and the lamp
  straight down. `component.types` describes every property.
- `intensity` 1 lights a white, matte surface that faces the light to white: at any
  distance for a directional light, one metre away for a point or spot light, which
  thin out with the square of the distance and end at their `range`.
- An `Environment` is an image of everything far away - an `.hdr` image of a sky,
  twice as wide as high, named by the id `asset.list {"type": "texture"}` gives it. It
  is the sky of the scene, lights matte surfaces and is what shiny ones reflect.
  `intensity` scales it; `rotationDegrees` turns it about the vertical axis,
  counter-clockwise seen from above. The middle of the image is seen along -Z: to
  bring a sun that is there to where a directional light shines from, turn the
  image by that light's turn about the vertical axis, less 180 degrees. Nothing
  blocks its light, a world uses one (of several, the one whose entity has the lowest
  id), and `"visible": false` keeps its light and leaves the background as it was.
- A `Fog` lets what is far away fade into its `color`, the sky included. `density` is
  the part of the light that is lost with every metre (0.02: a fifth after ten
  metres) where the fog is densest: at the height of its entity and below. Above, it
  thins out to about a third every 1 / `heightFalloff` metres; with 0 it is the same
  at every height and hides the sky. Nothing lights it: give it the color of the
  sky's horizon, and a darker one in a dark scene. `sunColor` is added towards the
  brightest directional light. A world uses one fog, the one whose entity has the
  lowest id. Look at it with `viewport.debugView {"view": "fog"}`.
- `channels` on a light and `lightChannels` on a mesh renderer are numbers with one
  bit per lighting channel (1, 2, 4 ... 128). A light shines on a mesh when they share
  a bit; left alone, everything is in channel 1.
- A light throws shadows when `castsShadows` is set; a mesh renderer throws them unless
  its own `castsShadows` is switched off, and what is drawn with a blended material
  never does. Of several directional lights only the brightest casts shadows. A
  point light costs six shadow maps, a spot light one, the sun `shadowCascades` (1 to
  4) that share its `shadowDistance`. Stripes on lit surfaces: raise `shadowBias` or
  `shadowNormalOffset` a little. Shadows that start away from what casts them: lower
  them.
- `viewport.quality {"preset": "low"}` sets how much the picture in the viewport may
  cost, by one word (`low`, `medium`, `high`); `{"shadows": "off"}`,
  `{"effects": "off"}` and `{"postProcess": "off"}` set the shadows, what is worked
  out from the screen (ambient occlusion) and the finishing of the picture alone
  (`off`, `low`, `medium`, `high`). It is the editor's view, not a setting of the
  scene. With `postProcess` off the viewport shows the light as the renderer worked it
  out - no exposure, no bloom, no curve, no grade - which is the picture to judge
  materials and lights by. `{"antiAliasing": "off"}` leaves the steps of slanted
  edges as they are and `"fxaa"` smooths them (the default); no preset changes it.
  Turn it off to look at single pixels: it blends what is on an edge with its
  neighbour.
- How the picture is finished is a component, `PostProcess`: on any entity it applies
  everywhere (`"global": true`) or in the box the entity's transform makes of a cube
  of one metre; on a camera it applies to that camera's views. `component.types` lists
  its settings with their ranges. To make a scene brighter, raise `exposure` (stops:
  +1 doubles the light); to let a view set its own, `"exposureMode": "automatic"`.
- `viewport.debugView {"view": "lightCount"}` shows how many point and spot lights
  each pixel has to ask, `{"view": "shadowCascades"}` which cascade of the sun's shadow
  each pixel is looked up in, and `editor.statistics` how many lights are in view, how
  many were left out because a view holds only so many, how many shadow maps were
  drawn, how many lights found no room for theirs, and how much memory the atlas they
  share takes (`shadows.atlasBytes`: 16, 64 or 256 MiB at low, medium or high quality).
  `environment.bytes` is the memory of the cube maps made of environments (4 MiB
  each), `environment.made` how many the frame had to make: one in the first frame an
  environment is seen in, none after.

## Play the scene

The editor shows a scene without running it. To see what a scene *does* - its scripts,
its simulation, its sounds - play it:

```json
[
    {"command": "play.start"},
    {"command": "input.inject", "params": {"key": "Space", "down": true}},
    {"command": "editor.runFrames", "params": {"count": 30}},
    {"command": "entity.get", "params": {"entity": "Stage/Sentry"}},
    {"command": "play.status"},
    {"command": "editor.screenshot", "params": {"path": "played.png", "viewportOnly": true}},
    {"command": "play.stop"}
]
```

- Time passes for the game only in frames that are run: `editor.runFrames`. Run the
  editor with `--fixed-time-step 0.0166667` and every frame is a sixtieth of a second,
  whatever the machine: the same script then plays the same game every time.
- `play.pause` holds the game, `play.resume` lets it go on, and `play.step {"frames":
  N}` lets a held game pass exactly N frames and runs them - for looking at what
  happens one frame at a time.
- The viewport shows the game through its own camera while it plays, so a screenshot
  with `viewportOnly` is the player's picture. `viewport.show {"camera": "editor"}`
  looks at the running game from where the editor's camera is instead.
- Everything that asks about the scene asks about the game while it plays:
  `entity.get`, `component.get`, `physics.raycast`, `editor.statistics` (which then
  reports scripts, steps of the simulation and voices), screenshots. `input.inject`
  presses what the game's controls are bound to.
- `play.status` lists `scriptProblems`: a script that stopped with an error says so
  there, with its entity and its line. Look at it after playing something new.
- Edits are allowed while it plays, and are gone when it stops - with everything the
  game did. `play.stop` puts the scene back exactly as `play.start` found it. To
  keep a change, make it when the scene is stopped.
- `scene.save`, `scene.open` and `scene.new` fail while the scene plays: stop first.

## See the result

- `editor.screenshot {"path": ...}` saves the whole editor window after letting the
  layout settle.
- `editor.screenshot {"path": ..., "viewportOnly": true, "width": W, "height": H}`
  saves the scene alone, at any size.
- `viewport.lookAt {"position": [x,y,z], "target": [x,y,z]}` and
  `viewport.frame {"entity": ...}` place the viewport camera first.
- `viewport.show {"camera": "game"}` looks through the camera the game renders from
  - the first enabled primary `Camera` of the scene - and draws nothing an editor
  adds: what a player would see. `{"camera": "editor"}` goes back. A scene that is
  played is shown through the game's camera by itself.
- `viewport.debugView {"view": "normals"}` makes the viewport show something the
  renderer works with instead of the lit picture: `baseColor`, `normals`, `roughness`,
  `metallic`, `specular`, `occlusion`, `emissive`, `depth`, `lightCount`,
  `shadowCascades`, `ambientOcclusion`, `fog`, `lightingOnly`, `reflections`,
  `wireframe`, `overdraw`; `none`
  brings the picture back. When a surface looks wrong, a screenshot of the right view
  says whether the material, the normals or the light is at fault. `reflections` makes
  a mirror of every surface: it shows what the environment gives them to reflect, and
  which way round it is. `ambientOcclusion` shows how much of the light from all
  around gets to each surface: white is all of it. If corners are too dark or not dark
  enough, the numbers to change are on the scene's `PostProcess` component
  (`ambientOcclusionIntensity`, `ambientOcclusionRadius`, `ambientOcclusionBias`).
  `fog` shows how much of each pixel is fog: white where nothing shows through. A
  scene that is too hazy, or not hazy enough, has a `Fog` component to change
  (`density`, `heightFalloff`, and the height of its entity).

Then read the image file. Looking at the picture is the only way to know that a change
is visible and looks right; a passing command only says that the data changed.

## Measure

`editor.statistics` returns what the last frame cost. Run a few frames first
(`editor.runFrames`): the frame that loads a scene is not typical, and the time the
GPU took arrives a few frames late (`gpuMilliseconds` is null until then, and on
adapters that cannot time their work). The counts - `drawCalls`, `instances`,
`triangles`, `lights`, `shadows`, `meshBytes`, `textureBytes` - are exact and the same
on every machine, which makes them the numbers to compare before and after a change to
a scene.
`editor.showPanel {"panel": "statistics"}` opens the same numbers as a window, for
screenshots.

## Try commands by hand

The Console panel of the interactive editor runs the same commands: type the name,
optionally followed by the parameters as a JSON object.

```
entity.create {"name": "Lamp", "parent": "Stage"}
editor.undo
```

## Add a command

A command is a thin JSON front for an `EditorActions` function. If the edit does not
exist as an action yet, add the action first.

### 1. The action (`engine/cordite/editor/EditorActions.h/.cpp`)

An action validates, applies the change, and pushes one entry on the history:

```cpp
Status EditorActions::setEntityEnabled(Entity entity, bool enabled) {
    CORDITE_TRY(requireValid(m_context, entity));
    World& world = m_context.world();
    if (world.isEnabled(entity) == enabled) {
        return {};                                   // not an edit: record nothing
    }
    world.setEnabled(entity, enabled);

    EditorContext* context = &m_context;
    const Uuid id = world.id(entity);                // ids, never handles
    const auto apply = [context, id](bool value) -> Status {
        CORDITE_TRY_ASSIGN(const Entity target, findEntity(*context, id));
        context->world().setEnabled(target, value);
        return {};
    };
    m_context.history().push({.label = ..., .undo = [apply, enabled] { return apply(!enabled); },
                              .redo = [apply, enabled] { return apply(enabled); }});
    return {};
}
```

- Validate everything before changing anything. A failed action changes nothing and
  records nothing.
- Capture entity `Uuid`s and validated `PropertyPatch`es or serialized entities in the
  undo and redo functions. Never capture an `Entity`, a component pointer or a
  reference into the world.
- An action that changes nothing pushes nothing.
- Redo must reproduce the edit exactly, ids included, so that later history entries
  still find what they refer to.

Test it in `tests/unit/editor/EditorActionsTests.cpp`: the change, `checkUndoRedo`, each
way it can fail with `checkUntouched`, and add it to the "whole session" test.

### 2. The command (`engine/cordite/editor/EditorCommands.cpp`)

```cpp
CORDITE_TRY(commands.add({
    .name = "entity.setEnabled",
    .description = "Enables or disables an entity. A disabled entity, and everything below it, is ignored.",
    .parameters = {{"entity", Type::String, kEntityReference}, {"enabled", Type::Boolean, "The new state."}},
    .handler = [&context](const Json& parameters) -> Result<Json> {
        CORDITE_TRY_ASSIGN(const Entity entity, entityParameter(context, parameters, "entity"));
        CORDITE_TRY(EditorActions(context).setEntityEnabled(entity, parameters.at("enabled").get<bool>()));
        return describeEntity(context, entity);
    },
}));
```

- Names are `group.verb` in dotted lowercase words. Group by what the command acts on:
  `scene`, `entity`, `component`, `asset`, `selection`, `viewport`, `editor`. Commands
  about assets are in `AssetCommands.cpp`.
- Declare every parameter with its type, a description and whether it is required. The
  registry checks them before the handler runs, so the handler may assume declared
  types.
- The description and the parameter descriptions are all an agent knows about the
  command. Say what it does and what it returns.
- Return something a caller can use next (the entity's `id` and `path`), or null.
- A command that needs a running editor (frames, the viewport camera, the window) is
  registered in `EditorApp::registerApplicationCommands` instead.

Test it in `tests/unit/editor/EditorCommandsTests.cpp`: add the name to the catalogue
list, exercise it, and add its failure cases to the "mistakes" table.

### 3. The interface

Only now add the menu item, button or shortcut, calling the same action and reporting
failure with `reportIfFailed`. An edit requested while a panel is iterating over the
world (the hierarchy tree) is deferred until the iteration is over.

## Checklist for an editor change

- The edit is an `EditorActions` function with undo, and has a command.
- `editor.commands` describes it well enough to use without reading the source.
- Unit tests cover the action and the command; `./scripts/dev.ps1 test -Filter editor`.
- A headless run with a script shows the feature working:
  `./scripts/dev.ps1 test -Filter editor_smoke`, or your own script with a screenshot.
- If the default editor layout looks different, the reference image
  `editor-showcase-1280x720` was looked at and approved (skill: `render-tests`).
