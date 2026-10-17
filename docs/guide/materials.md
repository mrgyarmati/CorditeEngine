# Materials

A material says how a surface responds to light: its color, how rough or metallic it is,
its bumps, whether it glows, whether it lets light through. Cordite uses the
metallic-roughness model of glTF, so a model you import looks the way its author saw it.

A **MeshRenderer** component draws a mesh with one material for each of its submeshes (its
`materials` list, in order). A submesh without one is drawn with the default material: plain
white, neither metal nor mirror.

## Material files

A material is an asset of its own: a file whose name ends in `.material.json`, in JSON
with comments allowed. Every value has a default, so a file says only what it changes:

```json
// Poured concrete: three textures, repeated ten times across the surface.
{
    "format": "cordite.material",
    "version": 1,
    "baseColorTexture": "2dda2b13-09fa-8d68-8783-86d9ed8d9606",
    "normalTexture": "214caee0-158f-8752-8cca-3e27f5babf4b",
    "metallicRoughnessTexture": "ae918817-5388-8d00-9021-1f94f118bcbc",
    "occlusionTexture": "ae918817-5388-8d00-9021-1f94f118bcbc",
    "metallic": 1,
    "roughness": 1,
    "uvScale": [10, 10]
}
```

Textures are referred to by their **asset id**, so that a texture can be renamed or moved
without breaking the materials that use it. The id of a texture is in the `.meta` file
beside it, and the Assets panel of the editor shows it (`asset.get` returns it to scripts and
agents).

The materials of the showcase (`samples/showcase/assets/materials/`) each show one kind of
surface, with comments.

## What a material holds

| Property | Default | Meaning |
|----------|---------|---------|
| `baseColor` | `[1, 1, 1, 1]` | Color and opacity, multiplied with `baseColorTexture`. |
| `baseColorTexture` | none | An image of the color (sRGB), its alpha the opacity. |
| `metallic` | 0 | 0 for anything but bare metal, 1 for metal. Multiplies the blue of `metallicRoughnessTexture`. |
| `roughness` | 0.5 | 0 is a mirror, 1 completely matte. Multiplies the green of `metallicRoughnessTexture`. |
| `metallicRoughnessTexture` | none | Roughness in green, metallic in blue (glTF's packing). |
| `specular` | 0.5 | How strongly a surface that is not metal reflects - 0.5 is what most materials do. |
| `normalTexture` | none | The bumps of the surface, as a tangent-space normal map (+Y up, as glTF and OpenGL have them). |
| `normalScale` | 1 | How strongly the normal map tilts the surface. |
| `occlusionTexture` | none | Where light from around does not reach, in red. Often packed with roughness and metallic. |
| `occlusionStrength` | 1 | How much of it is applied. |
| `emissiveColor`, `emissiveIntensity` | black, 1 | Light the surface gives off itself, times `emissiveTexture`. Above 1 it blooms. |
| `alphaMode` | `opaque` | `opaque`, `masked` (cut out where alpha is below `alphaCutoff`) or `blended` (see-through). |
| `alphaCutoff` | 0.5 | Masked only. |
| `doubleSided` | false | Draw the back of the surface too - for leaves, gratings and cloth. |
| `uvScale`, `uvOffset` | `[1, 1]`, `[0, 0]` | How the textures are laid on: repeated `uvScale` times, moved by `uvOffset`. |
| `uvChannel` | 0 | Which texture coordinates of the mesh the textures use. |
| `occlusionUvChannel` | 0 | The coordinates of the occlusion texture, when it was baked into a layout of its own. |
| `wrapU`, `wrapV` | `repeat` | Beyond the edge of a texture: `repeat`, `mirroredRepeat` or `clamp`. |
| `filter` | `linear` | `linear`, or `nearest` for hard-edged texels. |

The slot decides how a texture is read, not the texture: base color and emissive are colors,
everything else is data. You cannot import a texture "in the wrong color space".

## Material instances

A material can start from another one - its **parent** - and change only some of its values.
Ten crates of different colors are ten small files that share one parent:

```json
{
    "format": "cordite.material",
    "version": 1,
    "parent": "5d0b6c1e-9c3f-4f0e-8a54-0d6f3c1b2a79",
    "baseColor": [0.55, 0.12, 0.08, 1]
}
```

A change to the parent reaches every instance that does not set that value itself.

## Translucent and cut-out surfaces

- **Masked** surfaces - foliage, fences, gratings - are solid where they are drawn: they cast
  shadows, take ambient occlusion and are lit like everything else.
- **Blended** surfaces - glass, water, smoke - are drawn after everything solid, sorted back
  to front. They do not cast shadows.

## Decals

A **Decal** component lays a material onto whatever solid surfaces are inside its entity's
box - grime on a floor, a crack in a wall, rust down a pipe, a painted marking. The box is a
cube of one metre, scaled, turned and placed by the entity's Transform; the material is
projected along the entity's -Z axis, the way a light shines, with its image running along
the entity's X axis and up its Y axis. In the editor, the box is drawn with a line through it
along the way it is projected.

| Property | Default | Meaning |
|----------|---------|---------|
| `material` | none | The material laid on. |
| `opacity` | `1` | How much of it: 0 nothing, 1 as the material says. |
| `fadeAngle` | `60` | Surfaces that turn away from the projection by more than this many degrees get none of it; it fades over the ten degrees before. |
| `order` | `0` | Where decals overlap, the one of the higher order is laid on over the other. |

A decal's material is an ordinary material, and its alpha mode says how much of it is laid
on, as it does for a mesh: all of an **opaque** one - a square of it - all or nothing of a
**masked** one, and of a **blended** one as much as the alpha of its base color says, which is
what most decals want: an image that fades out before its edges, with `wrapU` and `wrapV` set
to `clamp`. The decal lays on the material's color, roughness, metal and specular, bends the
normals of what it lands on with the material's normal map, and adds the light it gives off;
its occlusion texture is not used. `uvScale` and `uvOffset` place the textures in the box as
they place them on a mesh: a strip of warning paint can repeat its stripes along its length. A decal lands on everything solid in its box, moving or not
- turn the box and make it shallow where it should not reach the things standing on a floor.
Glass and other blended surfaces are drawn after decals, and get none.

The `baseColor`, `normals` and `roughness` render views show what decals laid onto the
surfaces. A view lays on 256 decals at the most; the statistics (View > Statistics, or
`editor.statistics`) count them.

## Screens and mirrors

A material can show what a camera of the scene sees: the screen of a security monitor, a
mirror, the preview of a character. Three things make one:

1. A **render target**: a file whose name ends in `.target.json` and that says how large the
   picture is. It is written by hand, like a material:

   ```json
   {"format": "cordite.target", "version": 1, "width": 256, "height": 144}
   ```

2. A camera that draws into it: an entity with a **Camera** - not primary, or the game would
   be seen through it - and a **SceneCapture** component that names the render target.
   `interval` is how long a picture is kept before the next is drawn; 0 draws one every
   frame. Every picture costs what a view of the game costs, so keep render targets small; a
   frame draws four at the most.
3. A material that shows it: name the render target as its `emissiveTexture` - a screen gives
   off its picture - or as its `baseColorTexture`, for a picture that is lit. A render target
   is a picture, not numbers: as a normal map or a roughness texture it is refused.

The camera sees the scene finished, as the game would; where it sees its own screen, it sees
the picture of the frame before. `scene.validate` says so when a scene capture has no camera,
or a camera that is primary. The showcase has a security camera in a corner and its monitor on
the wall.

## The look of the engine

Cordite's lighting favors strong, readable contrast: high specular response, deep shadows,
bloom on bright highlights. Rough, dark-ish base colors with detailed normal maps read best -
a base color of pure white or pure black is rare in the real world and looks wrong under
strong light. Keep `specular` at 0.5 unless a surface really reflects more (wet surfaces,
polished stone) or less (chalk, cloth).
