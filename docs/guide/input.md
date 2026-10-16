# Input

A game never asks whether the space bar is down. It asks whether **jump** was pressed, or
how far **moveX** is pushed; which keys, buttons and sticks do that is written in one file,
the project's **input map**. A player with a gamepad and one with a keyboard play the same
game, and changing a key is a change of one line.

## The input map

The project file names it (`"inputMap": "input/controls.input.json"` in `project.cordite`).
It holds **actions** - things that are pressed and released - and **axes** - things that are
pushed some way:

```json
{
    "format": "cordite.input",
    "version": 1,
    "actions": [
        {
            "name": "jump",
            "bindings": [
                { "key": "Space" },
                { "gamepadButton": "A" }
            ]
        },
        {
            "name": "fire",
            "bindings": [
                { "mouseButton": "Left" },
                { "gamepadAxis": "RightTrigger", "threshold": 0.3 }
            ]
        }
    ],
    "axes": [
        {
            "name": "moveX",
            "bindings": [
                { "positiveKey": "D", "negativeKey": "A", "scale": 1.0 },
                { "gamepadAxis": "LeftX", "deadZone": 0.2, "scale": 1.0 }
            ]
        },
        {
            "name": "lookX",
            "bindings": [
                { "mouse": "x", "scale": 0.1 }
            ]
        }
    ]
}
```

What each kind of binding means:

| Binding | Of an action | Of an axis |
|---------|--------------|------------|
| `key` | Down while the key is | - |
| `mouseButton` | Down while the button is (`Left`, `Right`, `Middle`) | - |
| `gamepadButton` | Down while the button is (`A`, `B`, `X`, `Y`, `DpadUp`, ...) | - |
| `gamepadAxis` | Down while pushed past `threshold` (0.5 by default; a negative threshold is the other way) | Its position, with nothing inside `deadZone` |
| `positiveKey` / `negativeKey` | - | 1, -1 or 0 |
| `positiveButton` / `negativeButton` | - | The same, with gamepad buttons |
| `mouse` | - | How far the pointer moved in the frame, in pixels (`x` or `y`) |
| `scroll` | - | How far the wheel turned, in notches (`x` or `y`) |

An axis has the value of the binding that is moved furthest - a stick and the keys beside it
do not add up to twice the speed - times its `scale`. A negative scale turns it round: the Y
of a stick points down, and forward is up.

The map is read strictly: an unknown key name, a member that is not known or a binding that
names two controls is an error that says which action and which binding. The names of the
keys, buttons and sticks are listed in the [input reference](../reference/input.md). The
showcase's map, `samples/showcase/assets/input/controls.input.json`, uses every kind of
binding and explains each.

## Asking in scripts

```lua
function Player:input(action, pressed)       -- once, when it happens
    if action == "jump" and pressed then
        self:jump()
    end
end

function Player:update(deltaSeconds)          -- every frame
    local x = cordite.input.axis("moveX")
    if cordite.input.isDown("fire") then self:shoot() end
    if cordite.input.wasPressed("pause") then self:pause() end
end
```

A key that goes down and comes up between two frames still counts as pressed and released.
All scripts asking during a frame get the same answer: controls are worked out once, at the
start of the frame.

## Trying it in the editor

The controls work while a scene is played in the editor (F5). Agents and tests press keys
with the `input.inject` command, and read what the controls say with `input.state`;
`input.map` returns the map.
