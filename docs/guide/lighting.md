# Light, shadows and the finished picture

Cordite's default look is the high-contrast lighting of late-2000s console games: bright
practical lights against deep shadow, strong highlights, bloom and filmic tone mapping. All
of it is set with components, so it is saved with the scene and changed by scripts like
anything else.

A scene without any light component is black in a game - as it should be. The editor shows
such a scene with stand-in lights, so that you can see what you build.

## Lights

A **Light** component makes its entity a light. It shines from where the entity is, along
the entity's -Z axis (its forward direction):

| Type | What it is | Its own settings |
|------|------------|------------------|
| `directional` | The sun or the moon: parallel light from far away. Only its direction matters. | - |
| `point` | A bulb: light in every direction from one point. | `range` |
| `spot` | A torch or a stage light: a cone. | `range`, `innerConeDegrees`, `outerConeDegrees` |
| `rect` | A panel, a window, a screen: light from one face of a rectangle. | `range`, `width`, `height` |

Every light has a `color` and an `intensity`. An intensity of 1 lights a white, matte surface
that faces the light to white - at any distance for a directional light, one metre away for
the others, which fall off with distance and reach nothing at their `range`. A light also
ends at its range, so keep ranges as small as the light needs: it is cheaper, and the scene
is easier to control.

A **rect** light gives off its light from a rectangle `width` by `height` metres along its
entity's X and Y axes, from the face that looks along -Z; the entity's scale does not change
its size. Its intensity is a point light's: seen from far along the way it shines, it lights
what a point light of the same intensity in its middle would, less at a slant and nothing
behind its face. Nearer, its light comes from all over the face - a soft light that does not
grow without end as you come close - and shiny surfaces reflect the rectangle itself: a long
highlight for a long light. However large it is, it gives off the same light in all, so make
it the size of what glows. Its range is measured from its middle and reaches its corners at
the least. The showcase's monitor lights what is in front of it this way.

**Cookies**: a spot or a directional light can shine through a picture, as through a slide in
a projector - the bars of a guard, the panes of a window, leaves, moving clouds. Name a
texture as the light's `cookie`; its colors filter the light, white letting all of it
through and black none. A spot light throws the picture across its cone, as it is seen from
behind the light, looking the way it shines, and turns it as the light is turned about its
axis. A directional light lays it out over the world from where its entity is, repeated every
`cookieSize` metres along the light's right and up: move the entity - a script, a sequence -
and the clouds drift. A point or a rect light has no cookie. A cookie is brought to 256 by
256 pixels, and up to 16 can be in use at once; an `.hdr` image, which holds light, is no
cookie. The showcase's lamp shines through a wire guard (`textures/lamp_cookie.png`).

**AmbientLight** lights what the direct lights do not reach: a `skyColor` from above, a
`groundColor` from below, times `intensity`. Several ambient lights add up.

**Lighting channels**: every MeshRenderer and every Light has eight channel bits; a light
shines only on surfaces that share one of its bits. Use them to light a character apart from
its surroundings, or to keep an interior light from leaking through a wall.

A view takes up to 4 directional lights and 1024 point, spot and rect lights at once -
hundreds of local lights are fine, because they are sorted into clusters of the view and each
pixel only looks at the lights near it.

## Shadows

Set `castsShadows` on a light, and every MeshRenderer whose `castsShadows` is on throws
shadows in it:

- The **sun** - the brightest directional light that casts shadows - gets up to 4
  **cascades**: sharper shadows near the camera, coarser ones further away, up to
  `shadowDistance` metres. `shadowCascadeDistribution` moves detail towards the camera.
- A **spot** light gets one shadow map, a **point** light six (one for every direction). A
  **rect** light casts the shadows of a point light in its middle: six maps, and shadows as
  sharp as a point light's.
- `shadowResolution` (`low`, `medium`, `high`) is the size of a light's maps.
- `shadowBias` and `shadowNormalOffset` keep a surface from shadowing itself - the
  defaults seldom need changing. Larger values make shadows end short of what casts them.

All shadow maps of a view share one atlas. A light that finds no room gets smaller maps, or
none, and the editor's statistics (View > Statistics) say how many lights went without.

## The sky: Environment

An **Environment** component turns an HDR image of everything around - a sky, a studio, a
landscape, such as the free images of [Poly Haven](https://polyhaven.com/hdris) - into three
things at once: the sky drawn behind the scene, the soft light that comes from all around,
and what shiny surfaces reflect. `intensity` scales its light, `rotationDegrees` turns it
about the vertical axis, and `visible` off keeps the light and reflections but not the sky.

Import an `.hdr` file into the assets and pick it as the environment's `texture`. A world
uses one environment.

## Rooms: EnvironmentProbe

The environment lights everything, wherever it is: inside a room the walls are lit by the
sky, and a shiny barrel reflects a sky it could not see. An **EnvironmentProbe** component
takes a picture of everything around the middle of its entity's box - scale the entity to
the room - and inside the box that picture stands in for the environment: matte surfaces get
the light of the walls around them, shiny ones reflect the room. Towards the faces of the box
it fades into what is around it over `blendDistance`; `intensity` scales its light;
`boxProjection` looks up what a surface reflects where its reflection meets the box, which is
right for a box that is the room it is in. Where boxes overlap, the probe of the higher
`priority` wins - a small room inside a large yard. The viewport draws the box, and for a
selected probe also where it has faded in fully, `blendDistance` inside the faces.

The picture is taken when the scene is first drawn, and again when the probe moves or the
assets are read again. What is around it does not retake it: when a wall was moved or a light
turned on, use View > Capture probes (`probes.capture`). The picture is the scene lit by its
lights and its environment, not by probes: light bounces once. The render view "Probes" shows
which probe lights each surface.

## Fog

A **Fog** component puts haze between the viewer and everything seen: the farther away, the
more of the fog's `color` is seen in the place of what is behind it. It is densest at the
height of its entity and below, and thins out above (`heightFalloff`) - raise or lower the
entity to raise or lower the fog. `density` 0.02 leaves 82% of the light of something ten
metres away. `sunColor` adds the glow of a sun seen through haze. A world uses one fog.

**Lights in the fog**: tick the fog's `volumetric`, and the lights light it - a spot light
becomes a cone of light in the haze, cut by whatever stands in its way and patterned by its
cookie; a lamp gets a glow; the sun lights the air between the shadows of a roof. A light's
`volumetricIntensity` says how brightly it lights the fog against how it lights surfaces:
1 as its intensity says, 0 not at all - raise it for a beam that is to be seen, lower it for
a sun that would otherwise make a pale wall of the distance. The fog's `scattering` is the
share of the light it throws on, in each color (white for mist, darker for smoke), its
`anisotropy` how much more of it goes onwards (a beam is then brightest seen from in front
of its light), and `volumetricDistance` how far from the viewer lights light it; beyond that,
only the sun does. It costs a view two passes over a grid of boxes, as fine as its screen
effects quality, and none when that is off. A beam thinner than a box is not seen whole:
with a wide view and a narrow beam, raise the quality. The render view "fog light" shows
what the lights throw into the fog, alone.

## Ambient occlusion

Corners, creases and the ground under things get less light from around. Cordite works this
out from the screen (screen-space ambient occlusion) and applies it to ambient and
environment light only - direct light and shadows are not touched. Its `intensity`, `radius`
and `bias` are post-processing settings (below); what it costs is a quality setting.

## Reflections from the screen

A shiny surface reflects the environment, or the picture of a probe - neither of which has the
barrel that stands on the floor. Cordite also looks for what a reflection meets in the picture
itself (screen-space reflections), and where it finds something, that is what the surface
reflects: a wet floor shows the crate on it. What is not on the screen - behind the camera,
off the edge of the picture, hidden behind something - cannot be found, and there the
environment or the probe is reflected as before; towards the edges of the picture the two
fade into each other. What is found is as sharp as in a mirror, so only smooth surfaces take
it: up to `reflectionMaxRoughness`, fully up to half of that. `reflectionIntensity` says how
much it counts (0 switches it off), `reflectionMaxDistance` how far a reflection is followed,
and `reflectionThickness` how thick things are taken to be - a reflection that passes further
behind something goes on past it. These are post-processing settings; the quality says how
finely reflections are followed, and the `off` quality draws none; nor does a view in which
nothing is smooth enough to take them. Translucent surfaces get none. The render view "screen
reflections" shows what they put in place, over black.

## Post-processing

The renderer computes light; post-processing makes a picture of it. Its settings are those
of the **PostProcess** component:

| Group | Settings |
|-------|----------|
| Exposure | `exposureMode` (`manual` or `automatic`), `exposure` in stops, the limits and speeds of automatic exposure |
| Bloom | `bloomIntensity`, `bloomThreshold`, `bloomScatter` |
| Tone mapping | `toneMapper`: `filmic` (soft contrast, muted colors - the default), `aces` (more contrast and saturation), `linear` |
| Grading | `temperature`, `tint`, `contrast`, `saturation`, `lift`, `gamma`, `gain`, and a color lookup table (`colorLut`, `colorLutIntensity`) |
| Lens | `vignette`, `chromaticAberration`, `filmGrain` |
| Depth of field | `focusDistance`, `focusRange`, `focusTransition`, `nearBlur`, `farBlur`, `blurSize` |
| Light shafts | `lightShaftIntensity`, `lightShaftThreshold`, `lightShaftLength` |
| Motion blur | `motionBlur` |
| Ambient occlusion | `ambientOcclusionIntensity`, `ambientOcclusionRadius`, `ambientOcclusionBias` |
| Reflections from the screen | `reflectionIntensity`, `reflectionMaxRoughness`, `reflectionMaxDistance`, `reflectionThickness` |
| Anti-aliasing | how far the smoothing follows edges |

Where the settings apply:

- **Global** (`global` on): everywhere.
- **A volume** (`global` off): where the camera is inside the box of the entity - a cube of
  one metre, scaled by the entity's transform - fading in over `blendDistance` outside it.
  Use volumes to give a room, a tunnel or the outdoors a look of its own.
- **A camera's own**: a PostProcess component on the camera entity applies to that camera
  only, after all the others.

Several are laid over one another by `priority`, lower first; `weight` says how much of a
component applies.

**Depth of field** blurs what is not at the focus, as a camera's lens does. What is
`focusDistance` metres in front of the camera is sharp, and so is everything up to
`focusRange` metres in front of that and behind it; over the next `focusTransition` metres
the blur grows to its most - `nearBlur` in front, `farBlur` behind - and stays there. The
sky is behind everything. `blurSize` is how wide the most blur is, as a share of the height
of the picture (0.01 is a hundredth). With `nearBlur` and `farBlur` both 0 there is no depth
of field, and it costs nothing. A volume can bring the focus closer where the camera goes -
into a room, under a lamp - and a camera's own PostProcess component is the whole look of
that camera, its focus included. The Depth of field render view shows how blurred each
pixel is: red in front of the focus, blue behind, black where it is sharp. Glass and other
translucent surfaces are blurred as much as what is behind them.

**Light shafts** are the light of the sun streaming past what stands in its way, when the
camera looks towards it: streaks that fan out from the sun, and shadows of whatever is in
front of it. The sun is the brightest directional light of the scene. `lightShaftIntensity`
is how much light they add (0, the default, is none, and costs nothing);
`lightShaftThreshold` lets the bright parts of an image of the sky add to them besides the
sun's own light; `lightShaftLength` is how far they reach across the picture. With the sun
behind the camera there are none, and a camera without perspective has none. The Light
shafts render view shows them alone. Give a volume indoors an intensity of 0, and the
shafts stay outside.

**Motion blur** smears what moves while the shutter of the camera is open along the way
it goes: a figure that runs past, a turning camera's view of the world, the swing of a
crane. `motionBlur` is how much of the time between two pictures the shutter is open -
0, the default, is none and costs nothing; 0.5 is what a film camera does; 1 the most.
Everything moves in it as it moves on the screen: what the camera follows stays sharp,
and the background it passes blurs. A blur is never longer than a fortieth of the
picture either way. When the camera cuts - to another camera, or jumps more than ten
metres or turns more than 45 degrees between two pictures - the first picture of the new
shot is not blurred. What moves while no time passes - an edit in the editor - is no
motion, and is not blurred either. The editor's own camera never blurs; play the scene to
see it, and the game's camera blurs while the game's time passes. The Motion render view
shows how far, and which way, each pixel moved. Glass and other translucent surfaces
blur as what is behind them.

A color lookup table is an image of N times N by N pixels - N slices side by side - that
holds the colors you want in place of a neutral table's. Grade a screenshot in an image
editor together with a neutral table, and use the graded table.

## Quality

What a picture costs - the size of the shadow atlas, how many samples ambient occlusion
takes, in how many steps reflections are followed, how far anti-aliasing searches - is a quality of the view, not of the scene: `low`,
`medium` or `high`, chosen in the editor's View > Quality menu (`viewport.quality`), for all
its viewports at once. The look
stays the same at every quality; only detail and cost change. At a lower quality, meshes
also change to their coarser levels of detail sooner, and textures are kept smaller: up to
1024, 2048 or 4096 texels a side - for every view of the program at once, since they share
their textures.

## Seeing what the renderer sees

View > Render view shows the channels of the renderer instead of the finished picture: base
color, normals, roughness, metallic, specular, occlusion, emissive, depth, how many lights
each pixel has, the shadow cascades, ambient occlusion, fog, lighting only, reflections,
wireframe, overdraw, depth of field, light shafts, the fog's light, the probes, the reflections from the
screen, levels of detail and motion
(`viewport.debugView`
for scripts and agents). Use them to find out why something looks the way it does - a wrong normal map, a
light with too small a range, a surface outside every cascade. View > Statistics shows what
a frame costs: the time on the processor and on the GPU, draw calls, triangles, lights and
shadow maps, and what meshes and textures take of the GPU's memory (`editor.statistics`
returns the same).
