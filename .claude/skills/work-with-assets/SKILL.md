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
| `AssetManager` | Loads by id and shares the result: `loadMesh`, `loadMaterial`, `loadTexture`, `loadModel` |
| `Mesh`, `MeshProcessing`, `Primitives` | Mesh data, tangents and optimization, built-in shapes |
| `Material` | The material model and the `.material.json` format |
| `GltfImporter`, `GltfExporter`, `GltfDocument` | glTF 2.0 in and out |
| `Image`, `Mipmaps` | Pixels |

On the GPU side, `renderer/RenderResources` turns loaded assets into buffers and textures.

## A project

```
my-game/
    cordite.project.json          name, start scene
    assets/
        models/crate.gltf         + crate.bin, + crate.gltf.meta
        textures/planks.png       + planks.png.meta
        materials/paint.material.json   (+ .meta)
        scenes/main.scene.json    (+ .meta)
    .cordite/                     derived data; not committed
```

File types are told apart by extension: `.gltf`/`.glb` model, `.png .jpg .jpeg .tga
.bmp .hdr` texture, `.material.json` material, `.scene.json` scene. Anything else, and
any directory whose name starts with a dot, is ignored.

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

## Import a model

glTF 2.0, `.gltf` or `.glb`. What comes across and what does not is listed at the top of
`GltfImporter.h`. One model file yields:

- a **model**: the node hierarchy, placed in a scene with `asset.instantiate`;
- a **mesh** per glTF mesh (`#mesh/N`), with one submesh per material it uses;
- a **material** per glTF material (`#material/N`);
- a **texture** per *embedded* image (`#image/N`). Images in files of their own are the
  assets of those files, and must lie inside the `assets` directory.

Ids of what is inside a model follow from its position in the file. Re-exporting a model
keeps them as long as the order of its meshes and materials stays the same.

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
- Ids are pinned by tests (`AssetDatabaseTests`). A change that moves one breaks every
  scene that refers to it: do not change `kBuiltinNamespace`, `kPathNamespace`, the
  sub-path scheme or `Uuid::fromName`.

## Regenerate the showcase's textures and models

They are made by `tools/sample-assets/Main.cpp`, from formulas:

```powershell
./scripts/dev.ps1 run -Target cordite-sample-assets -- samples/showcase/assets
```

Then look at what changed (the PNG files, and the showcase in the editor), run the tests
- the reference images of the showcase will differ and have to be reviewed (skill:
`render-tests`) - and commit the generator together with its output. The `.meta` files
stay as they are, so ids do not change.

## Troubleshooting

- **An object is not drawn:** the log names the mesh that failed to load, once. Check
  `scene.validate`, then `asset.get` on the mesh.
- **An object is plain white:** it has no material, or its material failed to load
  and the default was used; the log says which.
- **Bumps look like dents when lit from above or below:** the normal map has -Y up
  (the Direct3D convention). Invert its green channel; the engine expects +Y up.
- **A texture looks washed out or too dark:** it is in a slot of the wrong kind. Color
  goes in `baseColorTexture` or `emissiveTexture`; everything else is data.
- **Not sure what a material ends up as:** look at it channel by channel with the
  debug views (`viewport.debugView`, or View > Render view in the editor): base color,
  normals, roughness, metallic, occlusion, emissive.
- **Ids changed after moving files:** the `.meta` files were left behind. Move them
  back next to their assets.
- **"it has the same id as ..."** in `problems`: an asset was copied together with its
  `.meta`. Open the project in the editor once; it gives the copy an id of its own.
