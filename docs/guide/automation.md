# Automating the editor

Everything the editor can do is a **command**: a name, JSON parameters and a JSON result.
The menus, the toolbar and the Inspector run the same commands, so whatever a person can do,
a script - or an AI agent - can do too, and every change is undoable. This is how you can ask
an AI assistant to build a scene, import assets, set up components, play the game, take
screenshots and export it, without touching the editor yourself.

## Typing a command

The Console has a command line. Type a command and its parameters:

```
entity.create {"name": "Lamp", "components": {"Light": {"type": "point", "intensity": 3}}}
viewport.frame {"entity": "Lamp"}
editor.undo
```

`editor.commands` lists every command, with what it does and what it takes - the
[command reference](../reference/editor-commands.md) is the same list.

## Running a script of commands

A script is a JSON array of steps:

```json
[
    {"command": "entity.create", "params": {"name": "Crate",
        "components": {"Transform": {"position": [0, 1, 0]}, "Collider": {}, "RigidBody": {}}}},
    {"command": "asset.instantiate", "params": {"asset": "models/barrel.glb", "parent": "Crate"}},
    {"command": "scene.save"},
    {"command": "play.start"},
    {"command": "editor.runFrames", "params": {"count": 120}},
    {"command": "editor.screenshot", "params": {"path": "shots/after-two-seconds.png"}},
    {"command": "play.stop"}
]
```

Run it without a window:

```
cordite-editor --headless --project my-game --exec build-scene.json --frames 1 --screenshot out.png
```

- The outcome of every step is written to standard output as JSON (or to a file with
  `--exec-output`); a step that fails ends the script.
- Exit status: `0` success, `1` the editor could not run, `2` the GPU validation layers
  (`--validation`) reported something, `3` a step of the script failed.
- `--fixed-time-step 0.0166667` makes every frame a sixtieth of a second: two runs give the
  same pictures, which is what tests want.

## What there is to ask

| Command | Returns |
|---------|---------|
| `editor.commands` | Every command, with its parameters |
| `component.types` | Every component, with every property: its type, range, unit and options |
| `scene.describe` | The entities of the scene as a tree |
| `entity.get` | Everything about one entity |
| `asset.list`, `asset.get` | The assets, and one asset with what it uses and what uses it |
| `asset.preview` | A picture of an asset, saved as a PNG |
| `asset.listen` | Hear a sound of the assets, also while the scene is not played |
| `script.api` | Every function scripts can call, and every function the engine calls in a script |
| `editor.screenshot` | The editor's window, or the viewport, as a PNG |
| `editor.statistics` | What a frame costs |
| `input.inject` | Press keys and buttons, move the pointer, drop files - between frames |
| `project.export` | Export the game |

Entities are named by path (`"Stage/Crate"`) or id; assets by path (`"models/barrel.glb"`)
or id.

## Working with an AI assistant

The repository's `AGENTS.md` and the skills in `.claude/skills/` tell an AI assistant how the
engine is built and how to drive the editor. Ask for what you want in plain words - "add a
row of lamp posts along the walkway, each with a flickering point light" - and the assistant
writes and runs the commands, looks at screenshots of the result, and saves the scene.
