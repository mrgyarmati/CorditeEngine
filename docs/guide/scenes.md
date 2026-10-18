# Scenes, entities and components

A **scene** is a list of **entities**. An entity is little more than a name and an id; what
it is - a lamp, a crate, the player - comes from its **components**: plain sets of values that
say what the entity is, which the engine's systems read. A lamp is an entity with a
MeshRenderer (the post), a Light (the bulb) and maybe an AudioSource (its hum).

## The hierarchy

Entities hang under one another. A child moves, turns and scales with its parent: put the
light under the lamp post and moving the post moves the light. Drag an entity onto another in
the Hierarchy to put it under that one: it stays where it is in the world, and from then on
moves with its new parent.

An entity that is not **enabled** - with everything below it - is ignored by every system: it
is not drawn, not simulated, its scripts do not run.

## Components

| Component | What it makes of an entity |
|-----------|----------------------------|
| **Transform** | Where it is, how it is turned and how large, against what it hangs under. Every entity has one. |
| **Camera** | A viewpoint. A game renders from the first enabled camera marked `primary`. See [Cameras](cameras.md). |
| **CameraShake** | Shakes the view through the entity's Camera when a game gives it trauma. See [Cameras](cameras.md#camera-shake). |
| **SequencePlayer** | Plays a sequence: a camera that flies, a cut to it, lights and properties that change over time, moments scripts are told of. See [Cameras](cameras.md#cinematic-sequences). |
| **MeshRenderer** | Something drawn: a mesh and its materials. See [Materials](materials.md). |
| **Light**, **AmbientLight** | Light. See [Light, shadows and the finished picture](lighting.md). |
| **Environment** | The sky, and the light from all around. |
| **Fog** | Haze between the viewer and everything seen. |
| **Decal** | A material laid onto the surfaces inside the entity's box: grime, cracks, markings. See [Materials](materials.md#decals). |
| **SceneCapture** | Draws what the entity's Camera sees into a render target, for a monitor or a mirror to show. See [Materials](materials.md#screens-and-mirrors). |
| **PostProcess** | How the picture is finished - exposure, bloom, color - everywhere, in a volume, or for one camera. |
| **Collider**, **RigidBody**, **Joint**, **CharacterController** | Physics. See [Physics](physics.md). |
| **AudioSource**, **AudioListener** | Sound. See [Sound](audio.md). |
| **Animator** | A skeleton, posed by animation clips. See [Animation](animation.md). |
| **Script** | Lua that runs for the entity. See [Scripting](scripting.md). |
| **PrefabInstance** | Says which prefab an entity was placed from. See [Prefabs](prefabs.md). |

The [component reference](../reference/components.md) lists every property of every
component, with its type, range and what it does. The Inspector shows the same, and so do
`component.types` and the scripting functions `get` and `set`.

Values are checked wherever they come from - the Inspector, a script, a command, a scene
file: a number outside its range or a value of the wrong kind is refused with a message, and
nothing changes.

## Transforms and units

Positions are in metres, rotations are shown in degrees, and +Y is up. An entity looks along
its -Z axis: a camera and a light point that way. In files, rotations are quaternions written
`[x, y, z, w]`.

## Scene files

A scene is saved as JSON (`.scene.json`), with comments allowed, and is meant to be read:
the showcase's scene, `samples/showcase/assets/scenes/showcase.scene.json`, explains every
component it uses. Loading is strict - an unknown component or property is an error that
names the entity - and saving a scene that was loaded gives the same bytes back, so scene
files work well in version control.

## The showcase

`samples/showcase` is a project that uses everything the engine has: every component, every
option a scene can hold, every function scripts can call. Open it in the editor, play it,
and take it apart - it is the best example of how each piece is meant to be used.
