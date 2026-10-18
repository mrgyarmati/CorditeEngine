# Editor commands

Every command of the editor: what the menus, the toolbar and the Inspector do, by name, with JSON parameters and a JSON result - typed in the Console, run from a script of commands (`--exec`), or by an agent. Written by the engine (`commandReference`, editor/ReferenceDocs.h) and held to it by a test: do not change it by hand. How to run them is in the [guide](../guide/automation.md).

## asset.get

Describes one asset: the assets it depends on (`dependencies`) and those that depend on it (`usedBy`: a material that has a texture, a scene or a prefab whose entities refer to it - as their files were last read or saved), where the open scene refers to it (`usedInScene`: entity, component and property), what its importer left out, and what is in it - the size of a mesh or a texture, the properties of a material, the nodes of a model, the text of a script, the length of a sound, the entities of a prefab with the assets they use and what is wrong with them ("problems"). Fails with the reason when the asset cannot be loaded.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `asset` | string | yes | An asset: its id, or its path within the asset directory, followed by '#' and its place in the file for an asset inside another ("models/crate.gltf#mesh/0"). asset.list shows both for every asset. |

## asset.import

Brings files from elsewhere into the assets: copies them into a directory of the asset directory under their own names - a glTF file with the buffers and images it names by where they are, to the same places beside it - and reads the assets again. They get ids of their own; a .meta beside them stays behind. Refused, copying nothing, for a file that is not there, is a directory, is among the assets already or is no kind of asset, for two files that would go to one place, for a place that holds a file, and for a glTF file whose files would land outside the assets. Returns where each file went, and the assets they are. Files are no part of the document: editor.undo does not take them away.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `files` | array | yes | The paths of the files: absolute, or from where the editor runs. |
| `to` | string | no | The directory of the assets they go into, with forward slashes: "props/crates". The top when left out. |

## asset.instantiate

Places a model or a prefab in the scene. A model becomes one entity per node, with their meshes and materials, under a new root entity. A prefab becomes copies of its entities, with ids of their own, the root of them marked as an instance of it by a PrefabInstance component. The root is returned and selected; move it to place what was placed.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `asset` | string | yes | An asset: its id, or its path within the asset directory, followed by '#' and its place in the file for an asset inside another ("models/crate.gltf#mesh/0"). asset.list shows both for every asset. |
| `parent` | string | no | The entity to put the new entities under: its id, path or unique name. Without one their root becomes a root of the scene. |
| `name` | string | no | The name of the new root entity. Defaults to the name of the model, or to the name the prefab gives its root. |

## asset.list

Lists the assets the scene can use, each with its id, type, name and path, and what is wrong with the asset directory, if anything. Built-in assets have the path "builtin".

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `type` | string | no | Only assets of this type: texture, mesh, material, model, scene, skeleton, animation, script, sound, prefab, renderTarget or sequence. |

## asset.listen

Plays a sound of the assets for whoever is at the editor: once, from its beginning, without a place and as loud as it was made - also while the scene is not played, which is silent otherwise. A sound that is heard already is replaced. Without `asset`, stops what is heard. Returns the sound's length in seconds, its channels and its rate, and whether it plays.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `asset` | string | no | The sound: its id, or where it is. Left out, what plays stops. |

## asset.move

Moves or renames the file of an asset, with its .meta: it keeps its id, so whatever refers to it by id follows it, and so do the file of the open scene and the start scene of the project. Refused for what it would break or cannot do: an asset that is built in or inside a file (move the file), a path outside the assets, with a part that begins with a dot, or with another ending, a place that holds a file, a glTF file that names its buffers and images by where they are and would leave them behind in another directory, and a texture that a glTF file names by where it is. Returns `mentions`: lines of scripts, scenes and prefabs, and values in the open scene, that still name it by where it was - those follow by hand. Files are no part of the document: editor.undo does not move it back.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `asset` | string | yes | The asset whose file moves: its id, or where it is. |
| `to` | string | yes | Where the file goes, inside the asset directory, with forward slashes: "textures/brick.png". |

## asset.moveDirectory

Moves or renames a directory of the assets, with all that is in it. Every asset in it keeps its id - a file without a .meta gets one first - so whatever refers to them by id follows, and so do the file of the open scene and the start scene and the input map of the project. Refused, moving nothing, for a directory that is not there, a place that holds something, a place inside the directory, a path outside the assets or with a part that begins with a dot, and for a move after which a glTF file would not find a file it names by where it is. Returns every asset that moved, each with `mentions`: lines of scripts, scenes and prefabs, and values in the open scene, that still name it by where it was - those follow by hand. Files are no part of the document: editor.undo does not move them back.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `from` | string | yes | The directory, inside the asset directory, with forward slashes: "textures/old". |
| `to` | string | yes | Where it goes, likewise: "art/textures". |

## asset.preview

Draws a picture of an asset as the Assets panel shows it - a mesh as it is, a material on a ball, a model or a prefab as it is placed, whole, from in front and above - and saves it as a PNG file. A texture is its own picture: its file. Returns the path and the size.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `asset` | string | yes | The asset: its id, or where it is. |
| `path` | string | yes | Where to save the picture. Missing directories are made. |
| `size` | integer | no | The sides of the square picture, in pixels, from 16 to 2048. Defaults to 256. |

## asset.refresh

Reads the asset directory again and reloads everything, after files were added, changed or removed. Returns how many assets there are and what is wrong, if anything.

It takes no parameters.

## assets.browse

Says what the Assets panel lists, and sets what it lists: the assets of the project as a tree of directories, files and what is inside files, narrowed down to one type, to a text, or to both. It can choose an asset in the panel, and open or close a directory or a file. Returns the rows from top to bottom, each with how far in it is ("depth"); what is below a row that is closed is not among them, and while something is left out every row is open. Without parameters it changes nothing.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `type` | string | no | Only assets of this type: texture, mesh, material, model, scene, skeleton, animation, script, sound, prefab, renderTarget or sequence - or "all" for those of every type. |
| `search` | string | no | Only assets with this text in their name or in their path, whatever the case. "" for all of them. |
| `select` | string | no | The asset to choose: its id, or its path as a row has it. "" for none. What it is in - its directories, the file it is inside - is opened, so that it is among the rows; an asset the filter leaves out is chosen all the same, and shows once it is listed. |
| `open` | string | no | The path of a directory or of a file whose rows are to be shown: "models", "models/crate.gltf". |
| `close` | string | no | The path of a directory or of a file whose rows are to be hidden. |

## component.add

Gives an entity a component it does not have yet.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entity` | string | yes | An entity: its id, its path of names from a root ("Room/Table/Lamp"), or its name if that is unique. |
| `component` | string | yes | The name of the component type, e.g. "Camera". |
| `properties` | object | no | Properties to set; the others get their defaults. |

## component.get

Returns the properties of one component of an entity.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entity` | string | yes | An entity: its id, its path of names from a root ("Room/Table/Lamp"), or its name if that is unique. |
| `component` | string | yes | The name of the component type. |

## component.remove

Takes a component away from an entity.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entity` | string | yes | An entity: its id, its path of names from a root ("Room/Table/Lamp"), or its name if that is unique. |
| `component` | string | yes | The name of the component type. |

## component.set

Changes properties of a component. Properties that are not mentioned keep their values. Returns the component's properties after the change.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entity` | string | yes | An entity: its id, its path of names from a root ("Room/Table/Lamp"), or its name if that is unique. |
| `component` | string | yes | The name of the component type. |
| `properties` | object | yes | The properties to change and their new values. |

## component.types

Describes every component type: what it is for and the name, type, range and meaning of each of its properties.

It takes no parameters.

## editor.commands

Lists every command with its description and parameters.

It takes no parameters.

## editor.exit

Closes the editor after the current frame. Unsaved changes are discarded.

It takes no parameters.

## editor.focusPanel

Brings one of the editor's panels to the front of those it shares its place with, as a click on its tab does, so that it is what a screenshot shows. It is in front once a frame has been drawn. A viewport brought to the front is the one used last from then on.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `panel` | string | yes | The panel: "hierarchy", "inspector", "viewport", "console" or "assets" - or "viewport2" and on, the viewports opened besides the first, while they are open. |

## editor.redo

Makes the most recently undone edit again.

It takes no parameters.

## editor.runFrames

Lets the editor run for a number of frames, so that what was changed is drawn.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `count` | integer | no | How many frames to run. Defaults to 1. |

## editor.screenshot

Saves a PNG image of the editor window, or of the scene alone as a viewport shows it. Returns the path and the size of the image.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `path` | string | yes | Where to save the image. Missing directories are created. |
| `viewportOnly` | boolean | no | Save the scene without the interface around it. Defaults to false. |
| `viewport` | integer | no | With viewportOnly: which viewport, by its number. Defaults to the one used last. |
| `width` | integer | no | With viewportOnly: the image width in pixels. Defaults to the width of the editor window. |
| `height` | integer | no | With viewportOnly: the image height in pixels. Defaults to the height of the editor window. |

## editor.showPanel

Opens or closes one of the editor's optional windows, so that it is part of what a screenshot shows. Returns which of them are open.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `panel` | string | yes | The window: "statistics" or "interfaceDemo". |
| `visible` | boolean | no | Whether it is to be open. Defaults to true. |

## editor.state

Returns what the editor is working on: the scene and its file, whether there are unsaved changes, what can be undone and redone, and the selection.

It takes no parameters.

## editor.statistics

Reports what the last frame cost: milliseconds on the CPU and on the GPU (null where the GPU cannot time its work, and before its first measurement has arrived), what the scene renderer drew, the lights it drew it with (their number, the entries of the cluster lists they are in, and how many were left out because a view holds only so many), their shadow maps, the cube maps of environments (how many the frame had to make - none, but for the first frame an environment is seen in - and the memory of those held), and what it holds on the GPU. Run a few frames first: a frame that loads assets is not typical.

It takes no parameters.

## editor.theme

Says which theme the interface is drawn in - its colors - and, given one, draws it in that one from the next frame on. A person's choice is remembered for every project they open; a batch run remembers nothing. Returns the theme and the names of all of them.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `theme` | string | no | The theme to change to: "dark" or "light". |

## editor.undo

Takes back the most recent edit. Returns what can be undone and redone afterwards.

It takes no parameters.

## entity.copy

Keeps a copy of entities, with everything below them, for entity.paste - in this scene or another. Changes nothing. Returns how many were copied.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entities` | array | yes | The entities to copy, each given as an id, a path or a name. |

## entity.create

Creates an entity and returns its id and path.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `name` | string | no | Its name. Defaults to "Entity". |
| `parent` | string | no | The entity to create it under. Without one it becomes a root. |
| `components` | object | no | Components to give it, by name, each with the properties to set, e.g. {"Transform": {"position": [0, 1, 0]}, "Camera": {}}. |

## entity.cut

Copies entities as entity.copy does, and deletes them: one step to undo.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entities` | array | yes | The entities to cut, each given as an id, a path or a name. |

## entity.delete

Deletes entities and everything below them.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entities` | array | yes | The entities to delete, each given as an id, a path or a name. |

## entity.duplicate

Copies entities, with everything below them, next to the originals. Returns the copies.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entities` | array | yes | The entities to copy, each given as an id, a path or a name. |

## entity.get

Returns everything about an entity: its state, parent, children and components with all their properties.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entity` | string | yes | An entity: its id, its path of names from a root ("Room/Table/Lamp"), or its name if that is unique. |

## entity.paste

Creates what entity.copy or entity.cut kept, with ids of its own, and selects it: under the given parent, or where it was copied from. One step to undo. Returns the new entities.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `parent` | string | no | The entity to paste under; empty for the roots of the scene. Without it, under the parent of what was copied, while that is still there. |

## entity.rename

Changes the name of an entity.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entity` | string | yes | An entity: its id, its path of names from a root ("Room/Table/Lamp"), or its name if that is unique. |
| `name` | string | yes | The new name. Must not be empty or contain '/'. |

## entity.setEnabled

Enables or disables an entity. A disabled entity, and everything below it, is ignored.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entity` | string | yes | An entity: its id, its path of names from a root ("Room/Table/Lamp"), or its name if that is unique. |
| `enabled` | boolean | yes | The new state. |

## entity.setParent

Moves an entity under another one, or makes it a root.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entity` | string | yes | An entity: its id, its path of names from a root ("Room/Table/Lamp"), or its name if that is unique. |
| `parent` | any | yes | The new parent, or null to make the entity a root. |
| `index` | integer | no | Its position among the new siblings. Defaults to last. |
| `keepWorldTransform` | boolean | no | Keep the entity where it is in the world (the default) rather than keeping its local transform. |

## input.inject

Does what a hand does, one thing at a time: presses or lets go of a key or a mouse button, moves the pointer, turns the wheel, sets what a gamepad is like, or drops files onto the window - which the editor brings into the assets. The next frame sees it - the interface of the editor as well as the actions of the game. A key or a button stays down until it is let go of.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `key` | string | no | The name of a key, such as "Space" or "W"; with `down`. |
| `mouseButton` | string | no | The name of a mouse button, such as "Left"; with `down`. |
| `down` | boolean | no | Whether the key or the button goes down (true) or comes up (false). |
| `mouseMove` | array | no | Where the pointer goes: [x, y] in pixels from the top left. |
| `scroll` | array | no | How far the wheel is turned: [x, y] in notches. |
| `gamepad` | integer | no | Which gamepad is set, from 0 to 3: to the `buttons` and `axes` given, and connected. |
| `buttons` | array | no | The names of the gamepad buttons that are held, such as ["A"]; none when left out. |
| `axes` | object | no | How far sticks and triggers are pushed, by name: {"LeftX": 0.5}; at rest when left out. |
| `connected` | boolean | no | False unplugs the gamepad. Defaults to true. |
| `drop` | array | no | The paths of files dropped onto the window from outside. |

## input.map

Returns the controls of the game: its actions and axes by name, and the keys, buttons and sticks that move them, as the input map of the project has them (`map`, in the format of an input file), and the file they were read from (`file`, or null).

It takes no parameters.

## input.state

Says what the controls were doing in the last frame: of every action whether it is `down`, and whether it was `pressed` or `released` in that frame; of every axis its `value`.

It takes no parameters.

## physics.overlap

Lists the entities whose bodies reach into a ball (`radius`) or a box (`size`, and `rotation`) around `center`, each once. Of a collider that is the triangles of a mesh, the triangles count, not what they enclose. The bodies are those of the scene as it is; no time passes.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `center` | array | yes | The middle of the ball or the box: [x, y, z] in metres. |
| `radius` | number | no | The radius of a ball, in metres. |
| `size` | array | no | The edge lengths of a box: [x, y, z] in metres. |
| `rotation` | array | no | How the box is turned about its middle: a quaternion [x, y, z, w]. Defaults to not at all. |

## physics.raycast

Says what a ray hits first: the entity whose body it is, how far along the ray, the point and the direction the surface faces there. With a `radius`, what a ball of that size touches first when its middle is moved along the ray. Returns {"hit": false} for nothing. The bodies are those of the scene as it is; no time passes.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `origin` | array | yes | Where the ray starts: [x, y, z] in metres. |
| `direction` | array | yes | The way it goes: [x, y, z], of any length but none. |
| `maxDistance` | number | no | How far it is followed, in metres: at most 1000000. Defaults to 10000. |
| `radius` | number | no | The radius of a ball to move along the ray instead, in metres. Defaults to 0: a ray. |

## play.pause

Holds the game that is being played where it is: nothing in it moves on and nothing is heard, and the editor shows it as it stands. play.step lets single frames pass.

It takes no parameters.

## play.resume

Lets a game that is paused go on.

It takes no parameters.

## play.start

Plays the scene in the editor: from the next frame on its scripts run, its bodies move and its sounds are heard, and the first viewport shows it through the camera of the game (viewport.show changes that); the others keep their cameras. Nothing of it stays: play.stop puts the scene back as it was, and what is edited while it plays goes with the game. Returns what play.status returns.

It takes no parameters.

## play.status

Says whether the scene is being played - `state` is "stopped", "playing" or "paused" - for how many `frames` its time has passed since play.start, and what keeps scripts of it from running (`scriptProblems`).

It takes no parameters.

## play.step

Lets a game that is paused go on for a number of frames, runs those frames, and leaves it paused: for looking at what a game does one frame at a time.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `frames` | integer | no | How many frames to let pass. Defaults to 1. |

## play.stop

Stops playing: the scripts end, and the scene is what it was when play.start was called - its entities, its history, what is selected of what is still there. The first viewport looks through the camera it looked through before.

It takes no parameters.

## prefab.create

Makes a prefab of an entity and everything below it: writes them into a file among the assets, reads the assets again, and marks the entity as an instance of the new prefab with a PrefabInstance component. Returns the prefab. asset.instantiate places it, and so does a script, with cordite.world.instantiate. What was placed is a copy: changing the prefab later does not change it. Undoing takes the mark off the entity; the file stays.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entity` | string | yes | The entity to make the prefab of: its id, its path of names from a root, or its name if that is unique. |
| `path` | string | no | Where the file goes, within the asset directory. Its name ends in ".prefab.json": "prefabs/cart.prefab.json". Without one it is named after the entity, in "prefabs". |
| `overwrite` | boolean | no | Whether a file that is there already is replaced. Without it, such a file is an error. A prefab that is replaced keeps its id. |

## probes.capture

Takes the pictures of all environment probes again, in the next frames: what is around them has changed - a wall was moved, a light turned on - and they still show it as it was. A probe whose entity moves is taken again by itself. Returns how many probes have their pictures waiting to be taken.

It takes no parameters.

## project.export

Makes a game of the project: a directory with its program - cordite-player, under the name of the game - the project file, the assets with their ids, the engine's shaders compiled, and the notices of the software the program is made of; what a person plays without the editor. It runs on the kind of system that exported it. Refused while the scene is played, and when it has changes that are not saved: they would not be in the game. Returns the program, and how many files and bytes were written.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `path` | string | yes | The directory the game goes into, outside the project: not there yet, empty, or one that a game was exported into - that game is replaced, and nothing else there. |

## project.new

Makes a new project - a directory with a project.cordite and its assets - with a scene to begin with: a camera that looks at a box on a floor, under a sun and a sky. Then the editor closes, and the program that runs it opens it again on the new project. Refused while the scene is played or has changes that are not saved, and for a directory that holds a project already. Returns the project and its start scene.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `path` | string | yes | The directory of the project: not there yet, or one without a project in it. |
| `name` | string | no | What the game is called. Defaults to the name of the directory. |

## project.open

Opens another project: the editor closes, and the program that runs it opens it again on that project. Refused while the scene is played or has changes that are not saved, and for a path that holds no project. Returns the project.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `path` | string | yes | The project: its directory, or its project.cordite. |

## scene.describe

Returns the hierarchy: every entity with its id, path, state, component names and children.

It takes no parameters.

## scene.get

Returns the whole scene as the JSON document that scene.save would write.

It takes no parameters.

## scene.new

Replaces the scene with an empty one. Unsaved changes are discarded. Not while the scene is being played.

It takes no parameters.

## scene.open

Loads a scene file, replacing the current scene. Nothing changes if the file is invalid.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `path` | string | yes | The scene to load: a scene asset as asset.list names it ("scenes/main.scene.json"), or the path of a file. |

## scene.save

Saves the scene to a file, or to the file it already has when no path is given.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `path` | string | no | Where to save. Becomes the scene's file. |

## scene.validate

Checks every reference from the scene to an asset - the asset must exist and be of the kind the property expects - and every collider for what no shape or body can be made of: a shape of a mesh that names none, the triangles of a mesh in a dynamic body. Returns the problems found, and the assets the scene uses.

It takes no parameters.

## script.api

Returns what gameplay scripts are written against: `functions`, everything a script can call, each with its name, how it is called and what it does; and `callbacks`, the functions the engine calls in a script, in the order a frame calls them.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `prefix` | string | no | Only the functions whose names begin with this, such as "cordite.physics." or "cordite.Entity."; all of them when left out. The callbacks are always returned. |

## selection.get

Returns the selected entities in the order they were selected.

It takes no parameters.

## selection.set

Selects exactly the given entities; an empty list clears the selection. The entity given last is the one the inspector shows.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entities` | array | yes | The entities to select, each given as an id, a path or a name. |

## viewport.camera

Returns where the camera of a viewport is and which way it looks.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `viewport` | integer | no | Which viewport, by its number: 1 is the first. Defaults to the one used last. |

## viewport.close

Closes a viewport other than the first, which is always there. A drag of the gizmo in it ends where it is, as a step of the history. After the viewport used last is closed, the first is the one used last. Returns what viewport.list returns.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `viewport` | integer | yes | Which viewport, by its number: 2 and on. |

## viewport.debugView

Chooses what a viewport shows: the lit picture ("none") or a debug view of the renderer - one thing the G-buffer holds, with what decals laid on (baseColor, normals, roughness, metallic, specular, occlusion, emissive, depth), how many point and spot lights each pixel has to ask (lightCount: dark for none, blue for one, through green to red for eight and more), which cascade of the sun's shadow each pixel is looked up in (shadowCascades: red, green, blue, yellow from the nearest on; grey where the sun casts none), how much of the light from all around gets to each surface past what stands around it (ambientOcclusion: white is all of it), how much of each pixel is fog (fog: white where nothing shows through), how far and which way each pixel moved since the picture before (motion: black where nothing moved, red to the right, yellow-green up, cyan to the left, blue-purple down, brighter the further), the light without the materials (lightingOnly), what the surroundings give every surface to reflect, each of them made a perfect mirror (reflections), the edges of the triangles (wireframe), how often each pixel is drawn (overdraw), how blurred each pixel is by the depth of field (focus: black where it is sharp, red in front of that and blue behind), the light the shafts of the sun add, alone (lightShafts), what the lights throw towards the viewer from the fog in front of each pixel, alone (fogLight: black where they throw nothing), which environment probes light each surface (probes: a color for each probe, grey for the environment), what the reflections from the screen put in the place of the surroundings' reflections, alone (screenReflections: black where they meet nothing), or the level of detail each mesh is drawn at (lod: the mesh itself grey, then green, yellow, orange and red as the levels get coarser). Returns the view now shown and all there are; without a parameter it changes nothing.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `view` | string | no | The view to show, by one of the names above. |
| `viewport` | integer | no | Which viewport, by its number: 1 is the first. Defaults to the one used last. |

## viewport.frame

Moves the camera of a viewport so that an entity is in view, keeping the viewing direction - or, without one, what is selected, as F does. What is framed is what the camera orbits about with Alt and the left button.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `entity` | string | no | The entity, by id, path or name. Without it, the selection. |
| `radius` | number | no | The size of the area to show around the entity, in metres. Defaults to 1. Only with `entity`: the selection is framed by its own size. |
| `viewport` | integer | no | Which viewport, by its number: 1 is the first. Defaults to the one used last. |

## viewport.gizmo

Says what the gizmo of the selection does - translate, rotate or scale it - and along whose axes, the world's or the entity's own (scale is always along the entity's own); given either, changes it - in every viewport. The gizmo is drawn on the entity selected last and moves every selected entity that does not hang under another selected one. Dragging it is an edit of their Transform, one step to undo, which component.set makes as well.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `mode` | string | no | "translate", "rotate" or "scale". |
| `space` | string | no | "world" or "local". |

## viewport.list

Lists the viewports that are open: for each its number (`viewport`), through whose camera it shows the scene (`camera`, and `showsGame`: whether its last picture was the game's), its render view (`debugView`) and whether it draws the colliders of all entities; and `active`, the number of the one used last - clicked into or brought to the front - which the View menu, F and the commands that name no viewport act on.

It takes no parameters.

## viewport.lookAt

Places the camera of a viewport at a position, looking at a target point.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `position` | array | yes | Where the camera goes: [x, y, z] in metres. |
| `target` | array | yes | The point it looks at: [x, y, z] in metres. |
| `viewport` | integer | no | Which viewport, by its number: 1 is the first. Defaults to the one used last. |

## viewport.open

Opens another viewport beside those that are open - up to 4 - with a camera of its own, which starts where the camera of the viewport used last is. A viewport has its own camera, render view and colliders; what its picture may cost and what the gizmo does are those of every viewport. The one that is opened is the one used last from then on: a command of a viewport that names none is about it. Returns its number, the lowest that is not open.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `camera` | string | no | Through whose eyes it shows the scene: "editor" (the default) or "game". |

## viewport.quality

Chooses how much the pictures of the viewports may cost - of all of them: they share one atlas of shadow maps and the textures. "preset" sets everything by one word ("low", "medium", "high") - also the largest side the textures keep, 1024, 2048 and 4096 texels, which every view of the editor shares; "shadows" sets the shadows alone ("off" draws none; the others give the shadow maps of a view an atlas of 2048, 4096 and 8192 texels a side to share), "effects" what is worked out from the screen - ambient occlusion ("off" draws none; "low" and "medium" work it out for one pixel in four, "high" for every pixel) - and "postProcess" the finishing of the picture alone ("off" shows the light as the renderer worked it out, without exposure, bloom, tone mapping or grading), and "lodErrorPixels" how large the error of a level of detail may look, in pixels (a preset sets 2, 1 and a half; 0 draws every mesh at its finest). "antiAliasing" says how the steps of slanted edges are smoothed in a finished picture: "fxaa" from the picture itself, "off" not at all; no preset changes it. Returns what is now in use and all there is to choose from; without parameters it changes nothing. A game sets its own quality; this is that of the editor's views of the scene.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `preset` | string | no | Sets shadows, screen effects and finishing together, by one of the names above. |
| `shadows` | string | no | The quality of the shadows; applied after the preset. |
| `effects` | string | no | The quality of what is worked out from the screen; applied after the preset. |
| `postProcess` | string | no | The quality of the finishing of the picture; applied after the preset. |
| `lodErrorPixels` | number | no | How large the error of a level of detail may look, in pixels, from 0 to 64; applied after the preset. |
| `antiAliasing` | string | no | How edges are smoothed: "fxaa" or "off". |

## viewport.show

Chooses through whose eyes a viewport shows the scene, and what it draws over it. `camera` is "editor" - the editor's own camera, with the grid, the markers and the frames around what is selected - or "game": the camera the game renders from, the first enabled primary one, and nothing but what a player sees; a scene without such a camera is shown through the editor's. `colliders` draws the collider and the joint of every entity that has one, and the capsule of every character, as lines (those of selected entities and of what hangs below them are always drawn). Returns what is asked for now, and `showsGame`: whether the last picture was the game's. Without a parameter it changes nothing.

| Parameter | Kind | Needed | Meaning |
|---|---|---|---|
| `camera` | string | no | Whose camera to look through: "editor" or "game". |
| `colliders` | boolean | no | Draw the colliders, the joints and the characters of all entities. |
| `viewport` | integer | no | Which viewport, by its number: 1 is the first. Defaults to the one used last. |
