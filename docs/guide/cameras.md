# Cameras

A camera is an entity with a **Camera** component: a point of view. It looks along the
entity's -Z axis, with its +Y up - the way a light points - so it is aimed by turning its
entity.

## The camera a game is seen through

A game is seen through the first camera of the scene, in the order of the hierarchy, that is
enabled and marked `primary`. Other cameras are for drawing into a picture
([Screens and mirrors](materials.md#screens-and-mirrors)) or for a script to switch to: one
that sets `primary` on another camera and takes it off this one cuts to it.

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
