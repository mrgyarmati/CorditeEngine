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
    "inputMap": "input/controls.input.json",
    "textureBudgetMegabytes": 256
}
```

The start scene and the input map are named by where they are in the assets, with forward
slashes. `textureBudgetMegabytes` is the most the game's textures may take together on the
GPU (0, or leaving it out, for no budget): past it, every texture is kept the same number of
mip levels smaller - the fewest at which they fit - which View > Statistics shows. How large
a texture is kept is also the quality's: up to 1024, 2048 or 4096 texels a side at a low, a
medium and a high quality. Double-click `project.cordite` to open the project in the editor (on Windows the
editor registers itself for these files the first time it runs). A start scene that cannot be
read does not keep the project from opening: the editor opens it with an empty scene and says
in the console what is wrong, so that it can be mended there. Controls that cannot be read do:
Open project says so and stays where it was.

## Kinds of assets

What a file is, its ending says:

| Kind | Files |
|------|-------|
| Texture | `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`, and `.hdr` for images of light (skies) |
| Model | `.gltf`, `.glb` - with the meshes, materials, skeletons and animations inside; `.fbx`, `.obj` - with the meshes and materials, and the MTL files an OBJ file names |
| Material | `.material.json` - see [Materials](materials.md) |
| Scene | `.scene.json` |
| Prefab | `.prefab.json` - see [Prefabs](prefabs.md) |
| Script | `.lua` - see [Scripting](scripting.md) |
| Sound | `.wav`, `.flac`, `.mp3` |
| Render target | `.target.json` - a picture a camera of the scene draws into, which materials show; see [Materials](materials.md#screens-and-mirrors) |
| Sequence | `.sequence.json` - what happens over a few seconds: a camera that flies, a cut, a light that flares; see [Cameras](cameras.md#cinematic-sequences) |

Every project also has built-in assets without files: the meshes `Cube`, `Sphere`, `Plane`,
`Cylinder`, `Cone`, `Capsule` and `Torus`, and the default material.

A mesh with many triangles gets **levels of detail** when it is loaded: coarser versions of
itself, each with about half the triangles of the one before. Where a mesh is so far away
that a coarser version looks the same to within a pixel, that version is drawn - and its
shadow with it. Nothing has to be set up for it; View > Render view > Levels of detail shows
which version each mesh is drawn at, and the quality of the view says how closely they must
match (see [Quality](lighting.md#quality)).

## Ids

Everything refers to an asset by its **id**, never by its path: a scene names the mesh of a
MeshRenderer by id, a material its textures. The id of a file is in a `.meta` file next to it
(`wall.png.meta`), which the editor writes the first time it sees the file. Keep `.meta` files
with their assets - in version control too - and move them along. What is inside a file - a
mesh of a model - has an id derived from the file's and its place in the file.

So renaming and moving files breaks nothing, as long as it happens in the editor: in the
Assets panel, right-click a file and choose **Rename or move...**, or drag it onto a folder -
of the tree, or among the contents; onto **Assets** to move it to the top. Its `.meta` file
goes with it and its id stays. Directories are renamed and moved
the same way, with everything in them. What still names an old path - a line of a script,
say - is listed, to be changed by hand.

## Bringing files in

Drop files from Explorer, Finder or a file manager onto the editor window: they are copied
into the folder the Assets panel shows. A glTF file comes with the buffers and images it
names. Agents use `asset.import`.

You can also copy files into the assets directory yourself, or change them there with other
programs: the editor notices, and reads the assets again within a second. Assets → Refresh
does the same by hand.

## The Assets panel

The Assets panel is laid out like the project browser of Unity and the content browser of
Unreal Engine:

```
 Assets > models                       [All types v] [name or path ] [list][grid] [refresh]
+---------------+------------------------------------------------------+---------+
| v Assets      |  [barrel.glb >] [crate.gltf >] [hoist.gltf <] [Frame] | picture |
|     materials |  [Cable] [Hoist paint] ...                            | of the  |
|    [models]   |                                                       | chosen  |
|     prefabs   |                                                       | asset   |
+---------------+------------------------------------------------------+---------+
 Cable  Mesh  models/hoist.gltf#mesh/1                Place  Copy id  Uses  Used by
```

- At the left, the **folders** of the project as a tree, with **builtin** - the meshes and
  the material the engine has built in - last. Click a folder to show what is in it; its
  arrow opens and closes it. Drag the edge of the tree to make it wider; the editor remembers
  how wide.
- In the middle, what the folder has in it: its folders, then its files. Show them as a
  **grid** of tiles or as a **list**, with the two buttons at the top; the editor remembers
  which you chose. Double-click a folder to go into it, and click a step of the path above to
  go back up. A file with something inside it - the meshes, materials and animations of a
  model - has an arrow: in the grid at the side of its tile, in the list before its name.
  Click it to show what is inside after the file.
- Narrow it down by kind, or by typing part of a name: then the folder shown, and every
  folder below it, is searched, and what is found is listed side by side - in the list with
  the folder each is in. Show **Assets** to search everything.
- At the right is a picture of the chosen asset: a texture as it is, a mesh, a material on a
  ball, a model or a prefab - and for a sound, its length with a **Play** button.

- Double-click a model, a prefab or a mesh to place it in the scene - under the selected
  entity, if there is one. Drag it onto the Viewport to place it where you let go: on the
  surface under the pointer, or on the grid. Drag it onto an entity of the Hierarchy to
  place it under that entity, or below the entities to place it at the top of the scene.
  A mesh of a model is placed with the materials the model gives it.
- Drag a material onto a mesh in the Viewport to give it to the part of the mesh under the
  pointer.
- Drag any asset onto a property of the Inspector that takes its kind - a texture onto an
  environment, a sound onto an AudioSource.
- **Uses** and **Used by** show what an asset needs and what needs it: the textures of a
  material, the scenes that place a prefab, and the entities of the open scene that use it,
  which a click selects. Check **Used by** before you delete something.
- Right-click for Copy id, Copy path and Rename or move; for a scene, Open scene; for a
  model, a prefab or a mesh, Place in the scene.

## When something is wrong

A broken asset never stops a scene: a texture that cannot be read, a model with errors, a
material that names a missing texture is reported once in the Console - with the file - and
the rest is drawn. `scene.validate` lists what the open scene refers to that is not there.
