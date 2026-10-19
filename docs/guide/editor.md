# The editor

The editor is laid out like Unity and Unreal Engine:

```
+------------------------------------------------------------------+
| File  Edit  View  Entity  Play  Assets  Help                      |
| [save] [undo] [redo]          [play] [pause] [step]               |
+-------------+-------------------------------------+--------------+
|  Hierarchy  |              Viewport               |  Inspector   |
|             |                                     |              |
|  entities   |      the scene, through the         |  components  |
|  of the     |      editor's camera or the         |  of what is  |
|  scene      |      game's                         |  selected    |
|             +-------------------------------------+              |
|             |  Assets | Console                   |              |
+-------------+-------------------------------------+--------------+
| status: the newest message                  the state of the scene|
+------------------------------------------------------------------+
```

- **Hierarchy** - the entities of the scene, as a tree. Drag one onto another to put it under
  it; right-click for Create child, Duplicate and Delete.
- **Inspector** - the components of the selected entity, with every property. Add components
  with the button at the bottom. Numbers are dragged or typed, colors picked, assets chosen
  from a list or dropped from the Assets panel.
- **Viewport** - the scene. Click to select; the gizmo moves, turns and scales.
- **Assets** - the files of the project as a tree, with a picture of the chosen asset beside
  it. See [Projects and assets](assets.md).
- **Console** - what the engine has to say, and a command line: type a command of the
  [automation API](automation.md), such as `entity.create {"name": "Lamp"}`.

Panels can be dragged apart and docked anywhere; the editor remembers the layout of each
project. View > Theme switches between a dark and a light theme.

## Moving around the viewport

| Input | Does |
|-------|------|
| Right mouse button, held | Look around; with **W A S D** fly, **Q E** down and up, **Shift** faster |
| Middle mouse button, held | Pan |
| Mouse wheel | Move forward and back |
| **Alt** + left mouse button | Orbit around what you look at |
| **F** | Frame the selection |

## More than one viewport

View > Open viewport opens another viewport beside the first - up to four, each with a camera
of its own, which starts where the camera of the viewport you used last is. One can show the
scene through the game's camera while you work in another, or a render view beside the
picture. The tab of each viewport after the first has a button that closes it; the first is
always there. A viewport that is opened again comes back to where it was.

The View menu and **F** act on the viewport you used last - the one you clicked in, with any
button, or brought to the front. Quality is that of every viewport at once, and so is what
the gizmo does. Playing shows the game in the first viewport; the others keep their cameras.

`viewport.open`, `viewport.close` and `viewport.list` do the same from the console. The other
commands of a viewport take its number, `{"viewport": 2}`, and without one are about the
viewport you used last.

## Selecting and changing things

- Click an entity in the viewport or the Hierarchy to select it; **Ctrl**+click adds to the
  selection or takes away from it.
- **W**, **E**, **R** pick the gizmo for moving, turning and scaling; **X** switches its axes
  between those of the world and those of the entity. Each drag of the gizmo is one step to
  undo.
- **Ctrl+C**, **Ctrl+X**, **Ctrl+V** copy, cut and paste entities - also from one scene into
  another. **Ctrl+D** duplicates, **Delete** deletes.
- **Ctrl+Z** undoes, **Ctrl+Y** (or **Ctrl+Shift+Z**) redoes. Every change to the scene can
  be undone, wherever it came from: the Inspector, the gizmo, a command.

The Entity menu creates entities that are ready to use: an empty one, a camera, lights, an
environment, fog, a post-process volume, and shapes - the built-in cube, sphere, plane,
cylinder, cone, capsule and torus. Give a shape a Collider to make it solid.

## Scenes

A scene is a file of its own (`.scene.json`) among the assets: File > New scene, Open scene
(**Ctrl+O**), Save (**Ctrl+S**) and Save as. The title of the window shows the scene, with a
`*` when it has changes that are not saved. The project names the scene a game starts with.

## Playing

Press **Play** (**F5**): the scene runs as it would in the game - scripts, physics, sound and
animation - seen through the game's camera, in the first viewport. **F6** pauses and resumes,
**F10** steps a single frame while paused, and **Stop** (**Shift+F5**) - the Play button of
the toolbar, which is a Stop button while the scene plays - puts the scene back exactly as it
was before you pressed Play. Changes made while playing are part of the game,
not of the scene: they are gone when it stops.

View > Game camera switches the viewport you used last between the editor's camera and the
game's at any time; View > Colliders draws the shapes of physics.

## The other menus

- **View** - Render view (the channels of the renderer: base color, normals, lighting only,
  shadow cascades and more), Game camera, Colliders, Open viewport, Quality, Theme,
  Statistics.
- **Entity** - create entities; Make prefab of the selection.
- **Assets** - Refresh, Place model, Place prefab.
- **File** - New project and Open project (the editor begins again on the project),
  scenes, and Export game.
