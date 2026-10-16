---
name: work-with-assets
description: Work with assets in Cordite - models, textures, materials, scenes and their ids. Use when adding files to a project, writing a material, referring to an asset from a scene or from code, importing a glTF model, changing the importer or the asset database, regenerating the showcase's textures and models, or when an asset shows up missing, pink or wrong.
---

# Work with assets

An asset is identified by an id that never changes. Its file may be renamed and moved;
everything that refers to it keeps working. The code is in `engine/cordite/assets/`:

| File | What it is |
|------|------------|
| `AssetDatabase` | What exists: ids, types, names, dependencies, `.meta` files, problems |
| `AssetManager` | Loads by id and shares the result: `loadMesh`, `loadMaterial`, `loadTexture`, `loadModel`, `loadScript`, `loadSound`, `loadPrefab` |
| `Prefab` | `PrefabData`: what a `.prefab.json` holds, a name and entities as a scene writes them. Placing one is the business of the `world` module (`world/Prefab.h`) |
| `Mesh`, `MeshProcessing`, `Primitives` | Mesh data, tangents and optimization, built-in shapes |
| `Material` | The material model and the `.material.json` format |
| `GltfImporter`, `GltfExporter`, `GltfDocument` | glTF 2.0 in and out |
| `Image`, `Mipmaps` | Pixels |
| `ScriptSource` | The text of a `.lua` file; running it is the business of the `scripting` module |
| `Sound` | `SoundData`, the samples of a sound file, decoded whole; `decodeSound` reads WAV, FLAC and MP3, `encodeWav` writes WAV. An `AudioSource` component plays one (`audio` module) |

On the GPU side, `renderer/RenderResources` turns loaded assets into buffers and textures.

## A project

```
my-game/
    project.cordite               name, start scene
    assets/
        models/crate.gltf         + crate.bin, + crate.gltf.meta
        textures/planks.png       + planks.png.meta
        materials/paint.material.json   (+ .meta)
        scenes/main.scene.json    (+ .meta)
        scripts/door.lua          (+ .meta)
        prefabs/cart.prefab.json  (+ .meta)
        sounds/creak.wav          (+ .meta)
    .cordite/                     derived data; not committed
```

File types are told apart by extension: `.gltf`/`.glb` model, `.png .jpg .jpeg .tga
.bmp .hdr` texture, `.material.json` material, `.scene.json` scene, `.prefab.json`
prefab, `.lua` script,
`.wav .flac .mp3` sound. Anything else, and any directory whose name starts with a dot,
is ignored.

`.meta` files hold the id. The editor writes them; **commit them with the asset and
move them with it.** Never copy a `.meta` to make a new asset, and never edit an id.

## Add an asset to a project

1. Put the file under `assets/`.
2. In a running editor: `asset.refresh`. Otherwise it is found at the next start.
3. `asset.list` shows it with its id; `asset.get {"asset": "<path>"}` shows what is in
   it and what its importer left out (`warnings`).
4. Commit the file and its `.meta`.

Read `problems` in the result of `asset.list` or `asset.refresh`: a texture a model
refers to but cannot find, a material with a typo, a `.meta` without its file. A
problem leaves that one asset out or incomplete; it never stops the others.

## Rename or move an asset

Use `asset.move {"asset": "textures/wall.png", "to": "textures/brick/wall.png"}` (or Rename
or move in the menu of the Assets panel, or drop the asset onto a directory there), never
the file system: it moves the `.meta` along, so the asset keeps its id and every scene,
prefab and material that refers to it by id follows. The open scene's file and the
project's `startScene` follow too.

- What names a file by where it is does not follow. A `.gltf` names its `.bin` and its
  images so: it is renamed where it is, and refused another directory while it names
  any. A texture that a glTF file names is refused altogether.
- The result's `mentions` are the lines of scripts, scenes and prefabs, and the values in
  the open scene (`Script` properties), that still name the old path. Change those by
  hand.
- The ending stays: a `.png` stays a `.png`. Directories whose names begin with a dot
  are never read for assets, and are refused.
- It is not undoable; move it back with another `asset.move`.

## Write a material

A material is a JSON file. Only what differs from the defaults has to be stated:

```json
// assets/materials/rusty.material.json
{
    "format": "cordite.material",
    "version": 1,
    "baseColorTexture": "1f804f6a-bce0-87c1-845b-878a41c831ab",
    "normalTexture": "3c53f298-3827-8c13-bc40-53e50eee789b",
    "metallicRoughnessTexture": "78cc51ce-4b8a-8d5c-9941-161e5f841c84",
    "metallic": 1,
    "roughness": 1,
    "uvScale": [2, 2]
}
```

- Textures are named by id (`asset.list {"type": "texture"}`).
- Numbers multiply their texture: with a metallic-roughness texture, set `metallic` and
  `roughness` to 1 and let the texture decide.
- `metallicRoughnessTexture` reads roughness from green and metallic from blue;
  `occlusionTexture` reads red. One packed "ORM" texture serves both slots.
- `uvScale` and `uvOffset` lay out all the textures of the material together,
  occlusion included. Only an occlusion texture that was baked for the whole mesh,
  into its second set of coordinates (`"occlusionUvChannel": 1` while the others use
  0), is used as it is.
- Normal maps have +Y up (the OpenGL convention, as in glTF).
- `"parent": "<material id>"` starts from another material and changes only what is
  stated. See `samples/showcase/assets/materials/painted_metal_red.material.json`.
- `alphaMode`: `opaque`, `masked` (with `alphaCutoff`) or `blended`.
- Every property, with its range and meaning, is in `Material.cpp` (`describeMaterial`).
  `asset.get` shows the values a material ends up with, its parent's included. Unknown
  properties are errors, not ignored.

The materials of the showcase are written by hand, with comments: they are the examples.

## Refer to an asset

- **In a scene file or a component:** by id, in a property declared with
  `PropertySemantic::AssetReference` and a `referenceType` (skill: `add-component`).
- **In an editor command:** `asset.get` and `asset.instantiate` take an id or a path
  as `asset.list` prints it (`models/crate.gltf#mesh/0`).
- **In code:** `AssetDatabase::findByReference("models/crate.gltf#mesh/0")` gives the
  `AssetInfo`; keep its `id`. Built-in ones: `builtinMeshId(BuiltinMesh::Cube)`,
  `defaultMaterialId()`.

Check a scene with `scene.validate` (or `findSceneProblems` in code): it reports
references to assets that do not exist or are of the wrong kind.

Before deleting, moving or changing an asset, ask what needs it: `asset.get` lists
`dependencies` (what it needs), `usedBy` (the assets that need it - materials, models,
scenes and prefabs, as their files are) and `usedInScene` (where the open scene refers
to it, to the property). In code: `AssetInfo::dependencies` and
`AssetDatabase::dependents`. In the editor: Uses and Used by under the Assets panel.

## Import a model

glTF 2.0, `.gltf` or `.glb`. What comes across and what does not is listed at the top of
`GltfImporter.h`. One model file yields:

- a **model**: the node hierarchy, placed in a scene with `asset.instantiate`;
- a **mesh** per glTF mesh (`#mesh/N`), with one submesh per material it uses;
- a **material** per glTF material (`#material/N`);
- a **texture** per *embedded* image (`#image/N`). Images in files of their own are the
  assets of those files, and must lie inside the `assets` directory;
- a **skeleton** per glTF skin (`#skeleton/N`): the joints a skinned mesh hangs on, by
  name, with what each hangs on and where it rests;
- an **animation** per glTF animation (`#animation/N`): a clip that moves joints by
  name - translation, rotation and scale, stepped, linear or as splines.

`asset.get` says what is in each: for a mesh whether it is `skinned` and how many
`joints` it needs, for a skeleton its joints and what they hang on, for an animation
its `duration` and the joints it moves, for a model which of its nodes are joints. A
skin or a channel that makes no sense - a joint listed twice, times that run
backwards - is left out and named in the model's `warnings`; the rest of the file is
imported.

## Animate a model

Placing a model that has a skeleton (`asset.instantiate`, `instantiateModel`) gives:

```
Hoist              the instance: move it to move the model
  Frame            a node with a mesh that no joint moves
    Cable anchor   a node with nothing in it
  Skeleton         one for each skeleton of the model: the Animator
    Cable          the meshes skinned to that skeleton, unmoved
```

Joints get no entities. The `Animator` names the skeleton and, at first, no clip: the
figure stands as it was modelled. Give it one of the model's animations - or any
other whose tracks name joints this skeleton has:

```json
"Animator": {"skeleton": "90b33595-4c64-8e1a-b4e8-aebbe2f0e623",
             "clip": "1d58e9ef-d762-815a-ac62-67492aafb6aa",
             "time": 0.75, "speed": 1.0, "loop": true, "playing": true,
             "blendClip": null, "blendTime": 0.0, "blendWeight": 0.0}
```

- In a game (`Engine::runFrame`, or `Engine::updateWorld()` in a loop of one's own)
  `time` moves on while `playing` is set, by `speed`, and goes round at the end of the
  clip while `loop` is set. Code changes the clip, the speed or the weight by writing
  the component: `world.tryGet<AnimatorComponent>(entity)->clip = runId;`.
- In the editor nothing plays: the pose is that of `time`, so setting `time` looks
  through a clip, and what is set is what is saved.
- `blendClip` is a second clip over the first, by `blendWeight` (0 none of it, 1
  nothing else), at `blendTime`. Fading from one clip to another is moving the weight.
- A skinned mesh is drawn in the pose of its own entity or of the nearest entity above
  it that has an `Animator` with a skeleton. Meshes of one figure therefore go under
  one animator; a mesh that needs another skeleton needs an animator of its own.
- A mesh that follows more joints than the skeleton has is drawn as it was modelled,
  and the log names it. `asset.get` on the mesh says how many joints it needs, on the
  skeleton how many it has.
- `editor.statistics` counts the animators that were posed (`animation`) and the
  skinned meshes that were drawn in a pose (`scene.skinned`).
- In the viewport a skinned mesh is clicked where it is drawn, and selecting the entity
  with the `Animator` draws its skeleton: a line from every joint to the joint it
  hangs on.

Ids of what is inside a model follow from its position in the file. Re-exporting a model
keeps them as long as the order of its meshes and materials stays the same.

## Make and place a prefab

A prefab is an entity with everything below it, kept in a `.prefab.json` to be placed
again. In the editor:

```json
[
    {"command": "prefab.create", "params": {"entity": "Yard/Cart", "path": "prefabs/cart.prefab.json"}},
    {"command": "asset.instantiate", "params": {"asset": "prefabs/cart.prefab.json", "parent": "Yard", "name": "Cart 2"}}
]
```

`prefab.create` writes the file - it refuses to write over one unless it is given
`"overwrite": true` -, reads the assets again and marks the entity with a
`PrefabInstance` component. Without a `path` the file is named after the entity:
`prefabs/cart.prefab.json` for "Cart". In the menus these are Entity > Make prefab and
Assets > Place prefab.

A script places one and gets its root back; scripts inside the prefab start with the
next frame:

```lua
local cart = cordite.world.instantiate("prefabs/cart.prefab.json",
                                       { parent = yard, position = cordite.vec3(2, 0, -1) })
```

What is placed is a copy: entities with ids of their own, saved in full with their
scene. Changing the prefab changes what is placed afterwards, not what was. Undoing
`prefab.create` takes the mark off the entity; the file stays.

The file is a small scene - the same format for its entities, one root, which comes
first. `samples/showcase/assets/prefabs/bollard.prefab.json` is the example, with
comments. After writing one by hand, ask `asset.get {"asset": "prefabs/x.prefab.json"}`:
it names the entities, the `assets` they use, and the `problems` placing it would
meet - a component that does not exist, a mesh that is not there - without placing it.

In code: `AssetManager::loadPrefab(id)` gives the `PrefabData`,
`instantiatePrefab(world, components, prefab->entities, id, options)` in
`world/Prefab.h` places it, and `serializePrefabEntities` with `prefabDocument` makes
what is written into a file.

## Change the importer or the database

- Entries of a `GltfDocument` keep their glTF index; never drop or reorder them. Leave
  what cannot be imported empty and add a sentence to `warnings`.
- Decide what is in a mesh (its submeshes, its materials) from the *structure* of the
  file only, so that `loadData = false` and `true` agree: the database lists assets
  from the structure, the manager loads them with data.
- Treat every number in a file as hostile. Bound it and check it against the data that
  is there before using it (see `checkAccessorLayouts`); add a test with a file that
  lies (`GltfImporterTests`, "files whose numbers do not match their data").
- Build test files in the test with `GltfFile` (`tests/unit/assets/GltfTestSupport.h`)
  or write them with `exportGltf`; do not check in binary test models.
- A mesh that is skinned has `MeshData::skin` beside its vertices. Anything that
  changes which vertex is where has to take it along; `SkinnedMeshTests` checks every
  function of `MeshProcessing.h` for that with `skinnedCorners`.
- Ids are pinned by tests (`AssetDatabaseTests`). A change that moves one breaks every
  scene that refers to it: do not change `kBuiltinNamespace`, `kPathNamespace`, the
  sub-path scheme or `Uuid::fromName`.

## Make a color lookup table

`PostProcess.colorLut` takes an image of N * N by N pixels - 256 by 16 is usual - that
says what becomes of each color of the finished picture: N slices side by side, red
running across each, green down it, blue from slice to slice. The table that changes
nothing has at every place the color the place stands for.

To make one by eye: paste the neutral table into a screenshot, change the colors of
the whole image in an image editor until the picture is right, cut the table out
again, save it as PNG in the project. To make one from a formula, see `gradeColor` in
`tools/sample-assets/Main.cpp`, which makes the showcase's. A table of another shape
is left out, and the renderer says so once.

## Light a scene with an image of the sky

An `Environment` component takes an image of everything around the scene as `texture`:
twice as wide as high, the top row straight up, the middle what is seen looking along
-Z (`cordite/math/Equirectangular.h`). That is the form HDR images of skies come in;
put the `.hdr` file into the project like any other texture.

- An `.hdr` file holds light: numbers without an upper limit, in which a sun is
  thousands of times brighter than the sky around it. That is what makes it light a
  scene like a sky does. An image of 8-bit colors (PNG, JPEG) works too, as sRGB, but
  holds no more than white: use it for a backdrop, not for light.
- If the sky is far too bright or too dark for the scene, set `intensity` on the
  component, or the exposure of the view (`PostProcess`), rather than changing the
  file.
- A sun in the image lights the scene softly, from all of its disc and the glow
  around it, and casts no shadow. For shadows, add a directional light and turn the
  image (`rotationDegrees`) until its sun is where that light shines from; an image
  with a very bright sun and a directional light together light the scene twice.
- The first frame an environment is seen in makes what the renderer needs of it
  (`EnvironmentMaps`): a few megabytes on the GPU, nothing on disk. Large images
  take their time to load; 2048 by 1024 is plenty for light and reflections, and a
  sky that is seen sharply wants 4096 by 2048 (`RenderResourcesDesc::maxTextureDimension`
  is where larger ones are cut down).
- To make one from a formula, see `skyLight` in `tools/sample-assets/Main.cpp`, which
  makes the showcase's, and `Image::saveHdr`.

## Regenerate the showcase's textures, models and sounds

They are made by `tools/sample-assets/Main.cpp`, from formulas:

```powershell
./scripts/dev.ps1 run -Target cordite-sample-assets -- samples/showcase/assets
```

Then look at what changed (the image files, and the showcase in the editor), run the tests
- the reference images of the showcase will differ and have to be reviewed (skill:
`render-tests`) - and commit the generator together with its output. The `.meta` files
stay as they are, so ids do not change.

A sound cannot be looked at, and nobody listens during a test run. Check what was
written in numbers instead: that it begins and ends at nothing (or it clicks), that its
peak is well below 1, and for one that loops that the step from its last sample to its
first is no larger than the steps inside it. `asset.get` says how long it is; a sound
that is to go round without a seam is made of parts with a whole number of cycles in it,
as `makeHum` is.

## Troubleshooting

- **An object is not drawn:** the log names the mesh that failed to load, once. Check
  `scene.validate`, then `asset.get` on the mesh.
- **An object is plain white:** it has no material, or its material failed to load
  and the default was used; the log says which.
- **Bumps look like dents when lit from above or below:** the normal map has -Y up
  (the Direct3D convention). Invert its green channel; the engine expects +Y up.
- **A texture looks washed out or too dark:** it is in a slot of the wrong kind. Color
  goes in `baseColorTexture` or `emissiveTexture`; everything else is data.
- **The sky is the wrong way round, or its light comes from the wrong side:** look at
  it with the `reflections` debug view, and turn it with `rotationDegrees`. An image
  that is mirrored - text in it reads backwards - was made for looking at a sphere
  from outside; flip it left to right.
- **Metal looks black:** it has nothing to reflect. Give the scene an `Environment`.
- **Not sure what a material ends up as:** look at it channel by channel with the
  debug views (`viewport.debugView`, or View > Render view in the editor): base color,
  normals, roughness, metallic, occlusion, emissive.
- **Ids changed after moving files:** the `.meta` files were left behind. Move them
  back next to their assets.
- **"it has the same id as ..."** in `problems`: an asset was copied together with its
  `.meta`. Open the project in the editor once; it gives the copy an id of its own.
