# Components

Every component an entity can have, with every property: the kind of value it holds, its default, the values it takes and what it means. Written by the engine (`componentReference`, editor/ReferenceDocs.h) and held to it by a test: do not change it by hand. Properties marked *not saved* are what a running game says, and are not in scene files; *read only* ones are saved, but cannot be edited.

## AmbientLight

Light from all around, from a sky above and a ground below. Several add up.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `skyColor` | color | `[0.26,0.31,0.4]` | at least 0.0 | Linear color of the light from above |
| `groundColor` | color | `[0.1,0.09,0.08]` | at least 0.0 | Linear color of the light from below |
| `intensity` | float | `1.0` | at least 0.0 | Scales both colors |

## Animator

Poses a skeleton: which one, the animation that is playing, and where in it the pose is taken from.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `skeleton` | asset (Skeleton) | `null` |  | The skeleton that is posed; without one there is no pose |
| `clip` | asset (Animation) | `null` |  | The animation that is playing; without one the skeleton is at rest. A clip moves the joints that have the names its tracks name |
| `time` | float | `0.0` | 0.0 to 1000000.0; s | Where in the clip the pose is taken from; it moves on while the animator is playing |
| `speed` | float | `1.0` | -100.0 to 100.0 | How fast the time moves: 1 as the clip was made, 2 twice as fast, negative backwards |
| `loop` | bool | `true` |  | At its end the clip starts again; without, it stays in its last pose |
| `playing` | bool | `true` |  | Whether the time moves on; without, the pose is that of `time`, and stays |
| `blendClip` | asset (Animation) | `null` |  | A second animation, blended over the first by `blendWeight` |
| `blendTime` | float | `0.0` | 0.0 to 1000000.0; s | Where in the second clip its pose is taken from; it moves on with the same speed |
| `blendWeight` | float | `0.0` | 0.0 to 1.0 | How much of the second clip is taken: 0 none of it, 1 nothing but it |

## AudioListener

The entity that hears: sound is heard from where its sources are in relation to it. The first enabled one counts.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `volume` | float | `1.0` | 0.0 to 4.0 | How loud everything is to this listener: 1 as it is, 0 nothing |

## AudioSource

A sound that is heard from the entity: which one, how loud, and how it carries.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `sound` | asset (Sound) | `null` |  | The sound that is played; without one there is silence |
| `bus` | enum | `"effects"` | `effects`, `music`, `ambience`, `voice`, `interface` | What it is mixed with, and whose volume it shares |
| `playing` | bool | `true` |  | Whether it is playing: it starts when this becomes true, and a sound that does not loop sets it to false at its end. In a scene file, whether it plays when the scene starts |
| `loop` | bool | `false` |  | At its end the sound starts again; without, it is over |
| `volume` | float | `1.0` | 0.0 to 4.0 | How loud it is: 1 as the sound was made, 0 not at all |
| `pitch` | float | `1.0` | 0.25 to 4.0 | How fast it is played: 1 as it was made, 2 twice as fast and an octave higher |
| `spatial` | bool | `true` |  | Heard from where the entity is, quieter with distance; without, the same in both ears wherever it is |
| `rolloff` | enum | `"inverse"` | `inverse`, `linear` | How it gets quieter with distance: inverse as in the open, never silent; linear evenly, to nothing at maxDistance |
| `minDistance` | float | `1.0` | 0.009999999776482582 to 10000.0; m | Up to this distance from the listener it is as loud as it gets |
| `maxDistance` | float | `50.0` | 0.009999999776482582 to 10000.0; m | Beyond this distance it gets no quieter, or with a linear rolloff is not heard |

## Camera

A point of view the scene can be rendered from, looking along -Z.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `projection` | enum | `"perspective"` | `perspective`, `orthographic` | How the view volume is shaped |
| `verticalFovDegrees` | float | `60.0` | 1.0 to 179.0; deg | Vertical field of view of a perspective camera |
| `orthographicHeight` | float | `10.0` | at least 0.001; m | Height of the volume an orthographic camera sees |
| `nearPlane` | float | `0.1` | at least 0.001; m | Distance to the near clipping plane |
| `farPlane` | float | `1000.0` | at least 0.01; m | Distance to the far clipping plane |
| `primary` | bool | `true` |  | The game renders from the first enabled primary camera |

## CameraShake

Shakes the view through the entity's Camera by its trauma, which a game raises when something happens and which falls again with time. The entity itself stays where it is.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `trauma` | float | `0.0` | 0.0 to 1.0 | *Not saved.* How much has happened to the camera, from nothing to the most there can be: raise it when something does, and it falls by `decay` every second. The camera is moved by its square |
| `decay` | float | `1.0` | 0.0 to 100.0; 1/s | How much trauma goes away every second |
| `maxOffset` | vec3 | `[0.05,0.05,0.05]` | 0.0 to 1.0; m | The furthest the camera is moved along its own axes - right, up and back - at a trauma of 1 |
| `maxRotation` | vec3 | `[3.0,3.0,2.0]` | 0.0 to 30.0; deg | The furthest the camera is turned about its own axes at a trauma of 1: about X it looks up and down, about Y from side to side, about Z it rolls |
| `frequency` | float | `12.0` | 0.0 to 100.0; Hz | How fast it shakes: about how many times a second it changes its way |
| `phase` | float | `0.0` | 0.0 to 256.0 | *Not saved.* Where in its shaking the camera is, in turns of its noise: it moves on by `frequency` every second, and goes round |

## CharacterController

Makes the entity a character: an upright capsule with its soles where the entity is, which walks as `movement` says and jumps when `jump` is given a speed. No body: it has no RigidBody and no Collider but a trigger.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `radius` | float | `0.3` | 0.05000000074505806 to 100.0; m | How far around the capsule is, before the entity's scale |
| `height` | float | `1.8` | 0.01 to 1000.0; m | From its soles, which are where the entity is, to the top of its head, before the entity's scale |
| `mass` | float | `70.0` | 0.001 to 1000000.0; kg | How hard it presses on what it stands on |
| `maxSlope` | float | `50.0` | 0.0 to 89.0; deg | The steepest ground it walks on, from level; on steeper ground it does not get up, and slides down |
| `stepHeight` | float | `0.35` | 0.0 to 100.0; m | The highest step it walks up as if it were a ramp, before the entity's scale; 0 for none |
| `pushForce` | float | `100.0` | 0.0 to 1000000000.0; N | The most force it pushes a dynamic body in its way with |
| `gravityScale` | float | `1.0` | -100.0 to 100.0 | How much gravity pulls at it: 1 as at everything, 0 not at all |
| `airControl` | float | `1.0` | 0.0 to 1.0 | How much say `movement` has while it is in the air: its speed sideways is that much of `movement`, and the rest of what it left the ground with |
| `movement` | vec3 | `[0.0,0.0,0.0]` | -100000.0 to 100000.0; m/s | *Not saved.* The speed it is to walk with along the ground, in the world, until it is told otherwise; what of it points up or down is left out |
| `jump` | float | `0.0` | 0.0 to 100000.0; m/s | *Not saved.* The speed it is to leave the ground with. Taken by the next update, which sets this back to 0; it jumps in the step after if it stands on ground then |
| `velocity` | vec3 | `[0.0,0.0,0.0]` | m/s | *Read only.* *Not saved.* How fast it moved in the last step: as fast as it got on, not as fast as it was told to. Kept up to date while the simulation runs |
| `ground` | enum | `"inAir"` | `onGround`, `onSteepGround`, `inAir` | *Read only.* *Not saved.* What it has under its feet: ground it can walk on, ground too steep for it, or nothing. Kept up to date while the simulation runs |
| `groundNormal` | vec3 | `[0.0,1.0,0.0]` |  | *Read only.* *Not saved.* Which way the ground under it faces; up, in the air |
| `groundEntity` | entity | `null` |  | *Read only.* *Not saved.* The entity it stands on; none in the air |

## Collider

The shape of the entity for collisions. Alone, it stands still; with a RigidBody on its entity or above it, it is part of that body.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `shape` | enum | `"box"` | `box`, `sphere`, `capsule`, `convexHull`, `mesh` | Box: of `size`. Sphere: of `radius`. Capsule: a cylinder along Y with round ends, `radius` thick and `height` from end to end. ConvexHull: the smallest shape without dents around `mesh`. Mesh: the triangles of `mesh`, a surface without an inside - not for a dynamic body |
| `size` | vec3 | `[1.0,1.0,1.0]` | 0.001 to 10000.0; m | Box: its edge lengths, before the entity's scale |
| `radius` | float | `0.5` | 0.0005 to 10000.0; m | Sphere and capsule: half their thickness |
| `height` | float | `2.0` | 0.001 to 10000.0; m | Capsule: from end to end; no taller than it is wide, it is a sphere |
| `mesh` | asset (Mesh) | `null` |  | Convex hull and mesh: the mesh the shape is made of |
| `trigger` | bool | `false` |  | It stops nothing: it only notices the bodies that move into it and out of it. A body of its own, which goes where its entity goes |
| `center` | vec3 | `[0.0,0.0,0.0]` | -10000.0 to 10000.0; m | Where the middle of the shape is, in the space of the entity |
| `friction` | float | `0.5` | 0.0 to 4.0 | How much the surface resists sliding: 0 ice, 1 rubber. Of two that touch, the geometric mean counts |
| `restitution` | float | `0.0` | 0.0 to 1.0 | How much speed is kept in a bounce: 0 none, 1 all of it. Of two that touch, the larger counts |

## Decal

A material laid onto the solid surfaces inside the entity's box, projected along its -Z axis: grime, cracks, rust, markings.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `material` | asset (Material) | `null` |  | The material laid on: its color, normal map, roughness, metallic, specular and light. How much of it there is goes by its alpha mode, as for a mesh: a blended material's alpha says it |
| `opacity` | float | `1.0` | 0.0 to 1.0 | How much of it is laid on: 0 nothing, 1 as its material says |
| `fadeAngle` | float | `60.0` | 10.0 to 90.0; deg | Surfaces that turn away from the projection by more than this get none of it; it fades over the ten degrees before |
| `order` | int | `0` | -1000.0 to 1000.0 | Of decals that overlap, one of a higher order is laid on over one of a lower |

## Environment

Everything far away, as an image that lights the scene and is its sky. Of several, the one whose entity has the lowest id is used.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `texture` | asset (Texture) | `null` |  | The image of everything around: twice as wide as high, the top row straight up, the middle what is seen looking along -Z. An .hdr image of a sky, usually |
| `intensity` | float | `1.0` | 0.0 to 1000.0 | Scales the light of the image: what it lights, and how bright it is seen |
| `rotationDegrees` | float | `0.0` | -360.0 to 360.0 | Turns the image about the vertical axis, counter-clockwise seen from above |
| `visible` | bool | `true` |  | Draws the image where nothing else is, as the sky; without, it only lights the scene and shows in reflections |

## EnvironmentProbe

A picture of everything around the entity, taken from the scene, which lights what is in the entity's box from all around in the place of the environment: a room lit by its own walls. Taken again when the entity moves.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `blendDistance` | float | `1.0` | 0.0 to 100.0; m | How far inside the faces of the entity's box the probe fades into what is around it; 0 ends it sharply |
| `intensity` | float | `1.0` | 0.0 to 100.0 | Scales the light of the picture |
| `priority` | int | `0` | -1000.0 to 1000.0 | Of probes whose boxes overlap, the higher is laid over the lower |
| `boxProjection` | bool | `true` |  | Whether reflections are looked up where they meet the box, rather than as if everything were far away: for a box that is the room it is in |

## Fog

Fog: the further away something is, the more of the fog's color is seen in its place. Densest at the height of the entity and below, thinning out above. Of several, the one whose entity has the lowest id is used.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `color` | color | `[0.5,0.6,0.7]` | at least 0.0 | Linear color of the fog itself: what is seen where nothing shows through it. Nothing lights this color (lights light the fog with `volumetric`); give a fog in a dark scene a dark color |
| `density` | float | `0.02` | 0.0 to 10.0; 1/m | The part of the light that is lost with every metre, at the height of the entity and below. 0.02 leaves 82% after ten metres and 14% after a hundred; 0 is no fog |
| `heightFalloff` | float | `0.2` | 0.0 to 10.0; 1/m | How fast the fog thins out above the entity: to about a third every one divided by this many metres. 0 is the same fog at every height, which hides the sky |
| `startDistance` | float | `0.0` | 0.0 to 100000.0; m | The fog begins this far from the viewer |
| `maxOpacity` | float | `1.0` | 0.0 to 1.0 | The most of the fog's color that takes the place of what is behind it: 1 lets distant things vanish, less keeps them and the sky faintly there |
| `sunColor` | color | `[0.0,0.0,0.0]` | at least 0.0 | Linear color added to the fog's where the view looks straight at the sun - the brightest directional light: the glow of a sun in haze. Black for none |
| `sunExponent` | float | `8.0` | 1.0 to 64.0 | How closely around the sun its glow stays: 1 lights half the sky, 64 a small patch |
| `volumetric` | bool | `false` |  | Whether lights light the fog: each throws its light into it, as much as its volumetricIntensity says, with its shadows and its cookie - beams and glows besides the fog's own color. It costs two passes over a grid of the view |
| `scattering` | color | `[1.0,1.0,1.0]` | 0.0 to 1.0 | Of the light the fog takes away, the share it throws on, in each color: white for mist, darker for smoke |
| `anisotropy` | float | `0.2` | -0.9 to 0.9 | Which way the fog throws the light of lights on: 0 alike in every direction, towards 0.9 mostly onwards - a beam is brightest seen from in front of its light - and below 0 back |
| `volumetricDistance` | float | `50.0` | 1.0 to 1000.0; m | How far in front of the viewer lights light the fog; beyond it only directional lights do, as if nothing stood in their way |

## Joint

Holds the body the entity is part of to the body of another entity, or to the world, as the two are when the simulation first meets the joint.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `type` | enum | `"fixed"` | `fixed`, `ball`, `hinge`, `slider`, `distance` | Fixed: the two move as one. Ball: held at `anchor`, turning freely. Hinge: turning about `axis` through `anchor`. Slider: sliding along `axis` without turning. Distance: `anchor` and `connectedAnchor` kept a distance apart - a rod, a rope, a spring |
| `connectedTo` | entity | `null` |  | The entity whose body this entity's body is held to; none holds it to the world |
| `anchor` | vec3 | `[0.0,0.0,0.0]` | -10000.0 to 10000.0; m | Where the joint is, in the space of this entity |
| `axis` | vec3 | `[0.0,1.0,0.0]` | -1000.0 to 1000.0 | Hinge: the axis it turns about. Slider: the axis it slides along. In the space of this entity, of any length but none |
| `connectedAnchor` | vec3 | `[0.0,0.0,0.0]` | -10000.0 to 10000.0; m | Distance: the other end, in the space of the entity it is held to - or in the world |
| `limited` | bool | `false` |  | Whether a hinge, a slider or a distance is kept between `minimum` and `maximum`. A distance that is not keeps its ends as far apart as they were when it first held them |
| `minimum` | float | `0.0` | -10000.0 to 10000.0 | Hinge: degrees, from -180 to 0. Slider: metres, at most 0. Distance: metres between the ends, at least 0 |
| `maximum` | float | `0.0` | -10000.0 to 10000.0 | Hinge: degrees, from 0 to 180. Slider: metres, at least 0. Distance: metres between the ends, no less than `minimum` |
| `frequency` | float | `0.0` | 0.0 to 1000.0; Hz | How a distance, or a limit, gives: the frequency at which it swings; 0 for not at all |
| `damping` | float | `0.0` | 0.0 to 100.0 | How fast that swinging dies away: 0 never, 1 at once |
| `collideConnected` | bool | `false` |  | Whether the two bodies collide with each other. Mostly they must not: what is joined is close together |

## Light

A light at the entity's place: directional, point or spot.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `type` | enum | `"point"` | `directional`, `point`, `spot` | Directional: from one direction, everywhere. Point: from a point, all around. Spot: from a point, within a cone. Directional and spot lights shine along -Z |
| `color` | color | `[1.0,1.0,1.0]` | at least 0.0 | Linear color of the light |
| `intensity` | float | `1.0` | at least 0.0 | Brightness; 1 lights a white matte surface facing the light to white (for a point or spot light: one metre away) |
| `range` | float | `10.0` | at least 0.01; m | Point and spot: the distance at which the light has faded to nothing |
| `innerConeDegrees` | float | `25.0` | 0.0 to 89.0; deg | Spot: the angle from the axis up to which the light is at full brightness |
| `outerConeDegrees` | float | `35.0` | 0.0 to 89.0; deg | Spot: the angle from the axis at which the light ends |
| `channels` | bits | `1` | 0.0 to 255.0 | The lighting channels the light shines on, one bit each (1 is the first, 255 all eight) |
| `castsShadows` | bool | `false` |  | Whether what stands in the light throws shadows. Of several directional lights only the brightest casts them |
| `shadowBias` | float | `1.0` | 0.0 to 16.0 | How many shadow-map texels nearer the light a surface is looked up; more removes stripes on lit surfaces, less keeps shadows attached to what casts them |
| `shadowNormalOffset` | float | `1.0` | 0.0 to 16.0 | How many shadow-map texels along its normal a surface is looked up where the light grazes it |
| `shadowResolution` | enum | `"medium"` | `low`, `medium`, `high` | The size of the light's shadow maps: half, the same or twice what the shadow quality gives a light, as far as there is room for them |
| `shadowDistance` | float | `60.0` | 1.0 to 2000.0; m | Directional: how far from the camera the shadows reach; they fade out towards it |
| `shadowCascades` | int | `4` | 1.0 to 4.0 | Directional: into how many shadow maps the shadow distance is divided |
| `shadowCascadeDistribution` | float | `0.75` | 0.0 to 1.0 | Directional: 0 makes the cascades equally deep, 1 makes each a constant factor deeper than the one before, which keeps the shadows near the camera sharpest |
| `cookie` | asset (Texture) | `null` |  | Spot and directional: a picture the light shines through, which filters it by its colors - thrown across a spot light's cone, laid out over the world and repeated by a directional light. A point light has none |
| `cookieSize` | float | `10.0` | 0.01 to 10000.0; m | Directional: how far the cookie reaches across and up before it repeats |
| `volumetricIntensity` | float | `1.0` | 0.0 to 100.0 | How brightly the light lights a fog lit by lights, against how brightly it lights a surface: 1 as its intensity says, 0 not at all |

## MeshRenderer

Draws a mesh at the entity's place in the world.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `mesh` | asset (Mesh) | `null` |  | The mesh to draw |
| `materials` | asset (Material) | `[]` |  | One material per submesh of the mesh; missing or empty entries use the default material |
| `lightChannels` | bits | `1` | 0.0 to 255.0 | The lighting channels the mesh belongs to, one bit each (1 is the first, 255 all eight); a light shines on it when they share a channel |
| `castsShadows` | bool | `true` |  | Whether the mesh throws shadows; translucent surfaces never do |

## PostProcess

How the picture is finished - exposure, bloom, tone mapping, grading - everywhere, in a box, or for the camera it is on.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `global` | bool | `true` |  | Applies wherever the camera is; without it, only in and around the box the entity's transform makes of a cube of one metre. On a camera: applies to that camera's views alone, whatever this says |
| `priority` | int | `0` | -1000.0 to 1000.0 | Several are applied from the lowest priority to the highest, each over what the ones before left |
| `blendDistance` | float | `1.0` | 0.0 to 1000.0; m | How far outside its box the settings fade in |
| `weight` | float | `1.0` | 0.0 to 1.0 | How much of the settings is applied: 0 none, 1 all |
| `exposureMode` | enum | `"manual"` | `manual`, `automatic` | Manual: the exposure is what `exposure` says. Automatic: the view sets itself so that the picture is mid-grey on average, and `exposure` is added |
| `exposure` | float | `0.0` | -16.0 to 16.0 | Exposure in stops: each one doubles the light, each negative one halves it |
| `autoExposureMinimum` | float | `-3.0` | -16.0 to 16.0 | Automatic exposure: the fewest stops the view may choose |
| `autoExposureMaximum` | float | `3.0` | -16.0 to 16.0 | Automatic exposure: the most stops the view may choose |
| `adaptationSpeedUp` | float | `3.0` | 0.0 to 100.0 | Automatic exposure: how fast the view gets used to a brighter picture; what is left of the way shrinks to a third in one second divided by this |
| `adaptationSpeedDown` | float | `1.0` | 0.0 to 100.0 | Automatic exposure: how fast the view gets used to a darker picture |
| `toneMapper` | enum | `"filmic"` | `filmic`, `aces`, `linear` | How light without an upper limit becomes the colors of a display. Filmic: soft contrast, slow highlights. Aces: more contrast and color. Linear: no curve; what is brighter than white is cut off |
| `bloomIntensity` | float | `0.15` | 0.0 to 4.0 | How much of the bloom is added to the picture; 0 for none |
| `bloomThreshold` | float | `1.0` | 0.0 to 64.0 | Light brighter than this blooms; 1 is white, 0 lets everything bloom a little |
| `bloomScatter` | float | `0.65` | 0.0 to 1.0 | How far the bloom spreads: 0 tight around what is bright, 1 over much of the picture |
| `temperature` | float | `0.0` | -1.0 to 1.0 | White balance: positive is warmer (more red, less blue), negative cooler |
| `tint` | float | `0.0` | -1.0 to 1.0 | White balance: positive towards magenta, negative towards green |
| `contrast` | float | `1.0` | 0.5 to 2.0 | 1 leaves the picture as it is; less is flatter, more is harder; mid-grey stays |
| `saturation` | float | `1.0` | 0.0 to 2.0 | 1 leaves the picture as it is; 0 is black and white |
| `lift` | vec3 | `[0.0,0.0,0.0]` | -1.0 to 1.0 | Raises the shadows, per color; 0 leaves them |
| `gamma` | vec3 | `[1.0,1.0,1.0]` | 0.2 to 5.0 | Bends the mid-tones, per color; 1 leaves them, more is brighter |
| `gain` | vec3 | `[1.0,1.0,1.0]` | 0.0 to 4.0 | Scales the highlights, per color; 1 leaves them |
| `colorLut` | asset (Texture) | `null` |  | A lookup table for the colors of the finished picture: an image of N * N by N pixels, such as 256 by 16 |
| `colorLutIntensity` | float | `1.0` | 0.0 to 1.0 | How much of the lookup table's colors is taken: 0 none, 1 all |
| `vignette` | float | `0.0` | 0.0 to 1.0 | How dark the corners of the picture get: 0 not at all, 1 black |
| `chromaticAberration` | float | `0.0` | 0.0 to 1.0 | How far red and blue part in the corners, in hundredths of the picture's size |
| `filmGrain` | float | `0.0` | 0.0 to 1.0 | How much film grain is laid over the picture: 0 none, 1 a lot |
| `ambientOcclusionIntensity` | float | `1.0` | 0.0 to 4.0 | Ambient occlusion: how much darker the light from all around gets in creases and under things. 0 not at all, 1 as the geometry says, more exaggerated |
| `ambientOcclusionRadius` | float | `0.5` | 0.01 to 10.0; m | Ambient occlusion: how far from a point things are looked for that stand in its light |
| `ambientOcclusionBias` | float | `2.0` | 0.0 to 30.0; deg | Ambient occlusion: how far something has to rise above a surface before it counts; more hides streaks on surfaces seen at a low angle |
| `focusDistance` | float | `10.0` | 0.0 to 10000.0; m | Depth of field: how far from the camera what is sharp is, along the way it looks |
| `focusRange` | float | `2.0` | 0.0 to 10000.0; m | Depth of field: how far in front of the focus distance and behind it everything stays sharp |
| `focusTransition` | float | `5.0` | 0.0 to 10000.0; m | Depth of field: over how many metres beyond that the blur grows to its most; with nothing, what is not sharp is blurred at its most |
| `nearBlur` | float | `0.0` | 0.0 to 1.0 | Depth of field: the most blur in front of what is sharp, as a share of the blur size: 0 none, 1 all of it |
| `farBlur` | float | `0.0` | 0.0 to 1.0 | Depth of field: the most blur behind what is sharp, and where nothing was drawn, as a share of the blur size: 0 none, 1 all of it |
| `blurSize` | float | `0.01` | 0.0 to 0.03 | Depth of field: the radius of the circle a point becomes at the most blur, as a share of the height of the picture |
| `lightShaftIntensity` | float | `0.0` | 0.0 to 4.0 | Light shafts: how much light the shafts of the sun add, along rays from where it is in the picture past what stands in its way; 0 none |
| `lightShaftThreshold` | float | `1.0` | 0.0 to 64.0 | Light shafts: the light of the sky brighter than this - a sun in an image of the sky - feeds them besides the sun's own |
| `lightShaftLength` | float | `0.5` | 0.0 to 1.0 | Light shafts: how far they reach, as a share of the way from a pixel to the sun in the picture |
| `motionBlur` | float | `0.0` | 0.0 to 1.0 | Motion blur: how much of the way each pixel went since the picture before is smeared along - the share of the time between two pictures the shutter is open; 0 none, 0.5 a film camera's |

## PrefabInstance

Says which prefab the entity was placed from. It is an ordinary entity all the same: changing the prefab later does not change it.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `prefab` | asset (Prefab) | `null` |  | The prefab this entity, and what hangs under it, was placed from |

## RigidBody

Makes the entity a body of the simulation, shaped by its Collider and by the Colliders below it that have no RigidBody of their own.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `motion` | enum | `"dynamic"` | `static`, `kinematic`, `dynamic` | Static: it does not move. Kinematic: it goes where its entity is put, and pushes what is in its way. Dynamic: gravity and what it collides with move it, and its entity follows |
| `mass` | float | `1.0` | 0.001 to 1000000.0; kg | Of a dynamic body |
| `linearDamping` | float | `0.05` | 0.0 to 100.0 | The share of its speed it loses in a second by itself |
| `angularDamping` | float | `0.05` | 0.0 to 100.0 | The share of its spin it loses in a second by itself |
| `gravityScale` | float | `1.0` | -100.0 to 100.0 | How much gravity pulls at it: 1 as at everything, 0 not at all, negative upwards |
| `continuous` | bool | `false` |  | Checked against what it might pass through between two steps, at some cost: for what is small and fast |
| `allowSleep` | bool | `true` |  | It may fall asleep when it has come to rest, and then costs nothing until something touches it |
| `linearVelocity` | vec3 | `[0.0,0.0,0.0]` | -100000.0 to 100000.0; m/s | *Not saved.* How fast it moves; kept up to date while the simulation runs. Setting it gives a dynamic body that speed |
| `angularVelocity` | vec3 | `[0.0,0.0,0.0]` | -100000.0 to 100000.0; rad/s | *Not saved.* How fast it turns about each axis; kept up to date while the simulation runs. Setting it gives a dynamic body that spin |

## SceneCapture

Draws what the Camera of the entity sees into a render target, which materials show: a security monitor, a mirror. The camera is not the game's: leave it not primary.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `target` | asset (RenderTarget) | `null` |  | The render target it draws into: a picture that materials show as a color or as light |
| `interval` | float | `0.0` | 0.0 to 60.0; s | How long a picture is kept before the next is drawn: 0 draws one every frame |

## Script

Runs a script for the entity while the game runs: its `start` once, its `update` every frame, its `destroy` when the script is taken off or the entity is destroyed by a script.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `script` | asset (Script) | `null` |  | The script that runs for the entity: a .lua file of the project. |
| `properties` | json | `{}` |  | What the script is told for this entity: a value for each of the properties it declares that is to be other than the script says. |

## SequencePlayer

Plays a sequence while the game's time passes: moves what its tracks name, cuts to its cameras and tells the entity's scripts of its events.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `sequence` | asset (Sequence) | `null` |  | The sequence that plays; the paths of its tracks set out from this entity |
| `time` | float | `0.0` | 0.0 to 1000000.0; s | Where in the sequence it is; it moves on while the player plays, and set it is where the sequence goes on from |
| `speed` | float | `1.0` | 0.0 to 100.0 | How fast the time moves: 1 as the sequence was made, 2 twice as fast, 0 not at all |
| `loop` | bool | `false` |  | At its end the sequence begins again; without, it stops there |
| `playing` | bool | `true` |  | Whether it plays: its time moves on and what it says is put into the world. One that does not loop stops itself at its end |
| `camera` | entity | `null` |  | *Read only.* *Not saved.* The camera the game is seen through while the sequence plays, as its camera track says; none for the game's own |

## Transform

Where the entity is, relative to its parent. Every entity has one.

| Property | Kind | Default | Values | Meaning |
|---|---|---|---|---|
| `position` | vec3 | `[0.0,0.0,0.0]` | m | Offset from the parent |
| `rotation` | quat | `[0.0,0.0,0.0,1.0]` |  | Orientation relative to the parent, as a quaternion [x, y, z, w] |
| `scale` | vec3 | `[1.0,1.0,1.0]` |  | Scale relative to the parent |
