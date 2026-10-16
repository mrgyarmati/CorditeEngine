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

Every light has a `color` and an `intensity`. An intensity of 1 lights a white, matte surface
that faces the light to white - at any distance for a directional light, one metre away for
the others, which fall off with distance and reach nothing at their `range`. A light also
ends at its range, so keep ranges as small as the light needs: it is cheaper, and the scene
is easier to control.

**AmbientLight** lights what the direct lights do not reach: a `skyColor` from above, a
`groundColor` from below, times `intensity`. Several ambient lights add up.

**Lighting channels**: every MeshRenderer and every Light has eight channel bits; a light
shines only on surfaces that share one of its bits. Use them to light a character apart from
its surroundings, or to keep an interior light from leaking through a wall.

A view takes up to 4 directional lights and 1024 point and spot lights at once - hundreds of
local lights are fine, because they are sorted into clusters of the view and each pixel only
looks at the lights near it.

## Shadows

Set `castsShadows` on a light, and every MeshRenderer whose `castsShadows` is on throws
shadows in it:

- The **sun** - the brightest directional light that casts shadows - gets up to 4
  **cascades**: sharper shadows near the camera, coarser ones further away, up to
  `shadowDistance` metres. `shadowCascadeDistribution` moves detail towards the camera.
- A **spot** light gets one shadow map, a **point** light six (one for every direction).
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

## Fog

A **Fog** component puts haze between the viewer and everything seen: the farther away, the
more of the fog's `color` is seen in the place of what is behind it. It is densest at the
height of its entity and below, and thins out above (`heightFalloff`) - raise or lower the
entity to raise or lower the fog. `density` 0.02 leaves 82% of the light of something ten
metres away. `sunColor` adds the glow of a sun seen through haze. A world uses one fog.

## Ambient occlusion

Corners, creases and the ground under things get less light from around. Cordite works this
out from the screen (screen-space ambient occlusion) and applies it to ambient and
environment light only - direct light and shadows are not touched. Its `intensity`, `radius`
and `bias` are post-processing settings (below); what it costs is a quality setting.

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
| Ambient occlusion | `ambientOcclusionIntensity`, `ambientOcclusionRadius`, `ambientOcclusionBias` |
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

A color lookup table is an image of N times N by N pixels - N slices side by side - that
holds the colors you want in place of a neutral table's. Grade a screenshot in an image
editor together with a neutral table, and use the graded table.

## Quality

What a picture costs - the size of the shadow atlas, how many samples ambient occlusion
takes, how far anti-aliasing searches - is a quality of the view, not of the scene: `low`,
`medium` or `high`, chosen in the editor's View > Quality menu (`viewport.quality`). The look
stays the same at every quality; only detail and cost change.

## Seeing what the renderer sees

View > Render view shows the channels of the renderer instead of the finished picture: base
color, normals, roughness, metallic, specular, occlusion, emissive, depth, how many lights
each pixel has, the shadow cascades, ambient occlusion, fog, lighting only, reflections,
wireframe, overdraw and depth of field (`viewport.debugView` for scripts and agents). Use them to find out
why something looks the way it does - a wrong normal map, a light with too small a range, a
surface outside every cascade. View > Statistics shows what a frame costs: the time on the
processor and on the GPU, draw calls, triangles, lights and shadow maps, and what meshes and
textures take of the GPU's memory (`editor.statistics` returns the same).
