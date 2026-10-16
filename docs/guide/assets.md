# Projects and assets

## A project

A project is a directory with a `project.cordite` file in it, and an `assets` directory
beside that. Make one with **File > New project** in the editor, or
`cordite-editor --new-project my-game`; open another with **File > Open project**.

```
my-game/
    project.cordite          the name of the game, its start scene, its input map
    assets/                  everything the game is made of
        scenes/main.scene.json
        scripts/player.lua
        textures/wall.png
        ...
    .cordite/                what the editor keeps for itself: caches, the layout of the
                             panels. Not needed by anyone else; leave it out of version control.
```

`project.cordite` is JSON:

```json
{
    "format": "cordite.project",
    "version": 1,
    "name": "My Game",
    "startScene": "scenes/main.scene.json",
    "inputMap": "input/controls.input.json"
}
```

The start scene and the input map are named by where they are in the assets, with forward
slashes. Double-click `project.cordite` to open the project in the editor (on Windows the
editor registers itself for these files the first time it runs).

## Kinds of assets

What a file is, its ending says:

| Kind | Files |
|------|-------|
| Texture | `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`, and `.hdr` for images of light (skies) |
| Model | `.gltf`, `.glb` - with the meshes, materials, skeletons and animations inside |
| Material | `.material.json` - see [Materials](materials.md) |
| Scene | `.scene.json` |
| Prefab | `.prefab.json` - see [Prefabs](prefabs.md) |
| Script | `.lua` - see [Scripting](scripting.md) |
| Sound | `.wav`, `.flac`, `.mp3` |

Every project also has built-in assets without files: the meshes `Cube`, `Sphere`, `Plane`,
`Cylinder`, `Cone`, `Capsule` and `Torus`, and the default material.

## Ids

Everything refers to an asset by its **id**, never by its path: a scene names the mesh of a
MeshRenderer by id, a material its textures. The id of a file is in a `.meta` file next to it
(`wall.png.meta`), which the editor writes the first time it sees the file. Keep `.meta` files
with their assets - in version control too - and move them along. What is inside a file - a
mesh of a model - has an id derived from the file's and its place in the file.

So renaming and moving files breaks nothing, as long as it happens in the editor: in the
Assets panel, right-click a file and choose **Rename or move...**, or drag it onto a
directory. Its `.meta` file goes with it and its id stays. Directories are renamed and moved
the same way, with everything in them. What still names an old path - a line of a script,
say - is listed, to be changed by hand.

## Bringing files in

Drop files from Explorer, Finder or a file manager onto the editor window: they are copied
into the assets, beside the chosen asset. A glTF file comes with the buffers and images it
names. Agents use `asset.import`.

You can also copy files into the assets directory yourself, or change them there with other
programs: the editor notices, and reads the assets again within a second. Assets → Refresh
does the same by hand.

## The Assets panel

The Assets panel lists the assets as a tree of directories and files, with what is inside
files - the meshes, materials and animations of a model. Narrow it down by kind, or by typing
part of a name. Beside the list is a picture of the chosen asset: a texture as it is, a mesh,
a material on a ball, a model or a prefab.

- Double-click a model or a prefab to place it in the scene, or drag it onto an entity of
  the Hierarchy to place it under that entity.
- Drag any asset onto a property of the Inspector that takes its kind - a texture onto an
  environment, a sound onto an AudioSource.
- **Uses** and **Used by** show what an asset needs and what needs it: the textures of a
  material, the scenes that place a prefab, and the entities of the open scene that use it,
  which a click selects. Check **Used by** before you delete something.
- Right-click for Copy id, Copy path and Rename or move; for a scene, Open scene; for a
  model or a prefab, Place in the scene.

## When something is wrong

A broken asset never stops a scene: a texture that cannot be read, a model with errors, a
material that names a missing texture is reported once in the Console - with the file - and
the rest is drawn. `scene.validate` lists what the open scene refers to that is not there.
