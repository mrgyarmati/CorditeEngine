# Cameras

A camera is an entity with a **Camera** component: a point of view. It looks along the
entity's -Z axis, with its +Y up - the way a light points - so it is aimed by turning its
entity.

## The camera a game is seen through

A game is seen through the first camera of the scene, in the order of the hierarchy, that is
enabled and marked `primary` - unless a sequence that plays has cut to another
([Cinematic sequences](#cinematic-sequences)). Other cameras are for those cuts, for
drawing into a picture ([Screens and mirrors](materials.md#screens-and-mirrors)) or for a
script to switch to: one that sets `primary` on another camera and takes it off this one
cuts to it.

| Property | Meaning |
|----------|---------|
| `projection` | `perspective`, as an eye sees, or `orthographic`: everything as large as it is, wherever it is - a map, a view from the side. |
| `verticalFovDegrees` | How much of the world a perspective camera sees from top to bottom, in degrees. Less is a longer lens. |
| `orthographicHeight` | How many metres from top to bottom an orthographic camera sees. |
| `nearPlane`, `farPlane` | What is nearer than the one or further than the other is not seen, in metres. |
| `primary` | Whether the game may be seen through it. |

In the editor, View > Game camera shows what the game's camera sees, and so does playing the
scene. How the picture is finished - exposure, bloom, color, depth of field, motion blur - is
what the PostProcess components of the scene say, and a PostProcess component on the camera's
own entity is the look of that camera alone
([Post-processing](lighting.md#post-processing)).

## Camera shake

A camera with a **CameraShake** component shakes when something happens to it: an explosion
nearby, a hard landing, a blow. What happens gives the camera **trauma**, from 0 to 1; the
camera shakes by trauma squared - a little is hardly seen, a lot is violent - and the trauma
dies down by itself. The shake is smooth noise, not a wobble: it never repeats itself, and
it moves the camera along and about each of its axes in a way of its own.

| Property | Meaning |
|----------|---------|
| `trauma` | How much has happened to the camera, from nothing to the most there can be. Not saved: a game raises it, and it falls by `decay` every second. |
| `decay` | How much trauma goes away every second: at 1, the most there can be is gone after a second. |
| `maxOffset` | The furthest the camera is moved along its own axes - right, up and back - at a trauma of 1, in metres. |
| `maxRotation` | The furthest it is turned about them at a trauma of 1, in degrees: about X it looks up and down, about Y from side to side, about Z it rolls. |
| `frequency` | How fast it shakes: about how many times a second it changes its way. |
| `phase` | Where in its shaking the camera is. Not saved; it moves on by itself. |

Turning shakes a camera more than moving it does, and a camera moved too far may look into
a wall: a few degrees and a few centimetres are a lot. A script raises the trauma where
something happens:

```lua
-- On a barrel that explodes: every camera that can shake is shaken, the nearer the more.
function Barrel:explode()
    for _, camera in ipairs(cordite.world.withComponent("CameraShake")) do
        local distance = camera:worldPosition():distance(self.entity:worldPosition())
        local trauma = camera:get("CameraShake", "trauma") + math.max(1.0 - distance / 20.0, 0.0)
        camera:set("CameraShake", "trauma", math.min(trauma, 1.0))
    end
end
```

The shake moves what is seen and nothing else. The camera's entity stays where it is, and so
does whatever goes by it: a ray that a script casts from it, the sound that a listener on it
hears. A scene capture through a camera that shakes shakes too, and with motion blur on a
shaking camera blurs, as a real one would. While the scene is edited nothing shakes; played,
the game's camera in the viewport shakes as the game's would, and paused it holds where it
is. In the showcase the main camera shakes when the ball comes down hard
(`scripts/hop.lua`).

## Cinematic sequences

A **sequence** is what happens in a scene over a few seconds, on a timeline: a camera that
flies round the stage, a cut to it and back, a light that flares, a door that swings, a
moment a script is told of. It is an asset - a file whose name ends in `.sequence.json` -
and a **SequencePlayer** component plays it on an entity.

| Property | Meaning |
|----------|---------|
| `sequence` | The sequence to play. |
| `time` | Where in it the player is, in seconds. Set it to go on from somewhere else; what it skips is not told. |
| `speed` | 1 as the sequence was made, 2 twice as fast, 0 holds it. |
| `loop` | Start again at the end; without, it stops there. |
| `playing` | Whether it plays. One that does not loop stops itself at its end. |
| `camera` | The camera the sequence has cut to now. Not saved, and set by the sequence alone. |

A sequence file has a `duration` and a list of `tracks`, each a list of `keys` at times
from 0 to the duration:

```json
{
    "format": "cordite.sequence",
    "version": 1,
    "duration": 6.0,
    "tracks": [
        {"kind": "camera", "keys": [{"time": 0.0, "camera": "Cinema camera"},
                                    {"time": 5.5, "camera": null}]},
        {"kind": "transform", "target": "Cinema camera", "interpolation": "smooth",
         "lookAt": "/Stage/Sentry",
         "keys": [{"time": 0.0, "position": [6, 1.2, 1]}, {"time": 6.0, "position": [-5, 3, 1.5]}]},
        {"kind": "property", "target": "/Stage/Lamp", "component": "Light", "property": "intensity",
         "keys": [{"time": 1.0, "value": 0.0}, {"time": 2.0, "value": 6.0}]},
        {"kind": "event", "keys": [{"time": 3.0, "name": "explode"}]}
    ]
}
```

- **camera** - which camera the game is seen through from each key on: a path to an entity
  with a Camera, or `null` for the game's own. When the sequence stops, so does its cut.
- **transform** - where an entity is (`position`), how it is turned (`rotation`, as
  `[x, y, z, w]` like a scene file) and how large (`scale`), against what it hangs under.
  Each key has any of the three. `lookAt` turns the entity to face another all the time,
  wherever either goes.
- **property** - any property of a component, by the names the Inspector shows: a number,
  two to four numbers (a vector, a color), or `true` and `false`. What a key gives is
  checked as an edit is: a value outside the property's range is held to it.
- **event** - a moment the scripts of the entity that plays the sequence are told of,
  by its name: `sequenceEvent(name)`.

How a track goes from one key to the next is its `interpolation`: `linear` (what it does
when it says nothing) goes straight; `smooth` curves through every key without corners;
`step` holds each key until the next. Whole numbers and `true`/`false` always step.

A `target` is a path of entity names, as the editor's commands take them: from the entity
that plays the sequence - `"Cinema camera"` is a child of it, `""` is the entity itself -
or, beginning with `/`, from the roots of the scene. A sequence names entities by their
names, so the same one can be played by any entity with a rig of the same names below it.
What it names and cannot find, or cannot set, is said once in the log, and the rest plays.

A script starts a sequence by setting `playing`, and hears its events:

```lua
local Cinematic = {}

function Cinematic:input(action, pressed)
    if action == "cinematic" and pressed then
        self.entity:set("SequencePlayer", { time = 0.0, playing = true })
    end
end

function Cinematic:sequenceEvent(name)
    if name == "explode" then
        cordite.log.info("boom")
    end
end

return Cinematic
```

Sequences play while the game's time passes. The editor shows a scene as it is, and does not
play them while you edit; press Play to see one. A sequence leaves what it moved where its
last keys put it. An entity it moves should have no dynamic RigidBody: the simulation and the
sequence would both move it. A kinematic one follows.

What is heard is heard by the AudioListener, wherever a cut goes.

## Camera animation

A camera is animated by a sequence like anything else: a transform track for where it is
and how it is turned, a property track for its `verticalFovDegrees` - a zoom - and, where
it is not the game's own camera, a camera track to cut to it while it flies. `lookAt` is
what makes this simple: key where the camera goes, and it keeps what it is to look at in
view; only a camera that should look somewhere else needs keys of `rotation`. A shake on
top - a CameraShake on the same entity, which a script gives trauma at an event - moves the
view, not what the sequence keyed.

In the showcase, **C** (or **X** on a gamepad) plays `sequences/flyby.sequence.json`: the
view cuts to a camera that flies round the stage looking at the sentry, closes its lens in,
and shakes when the light on the wall flares, then cuts back to the game's camera
(`scripts/cinematic.lua`).
