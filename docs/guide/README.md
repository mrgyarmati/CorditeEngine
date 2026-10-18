# Making games with Cordite

This guide is for people who want to make a game with Cordite: how to build a world in the
editor, how to make it look the way you want, how to bring it to life with physics, sound and
scripts, and how to hand the finished game to the people who will play it.

Start with **[Getting started](getting-started.md)**: from the source code to a game you can
export, in six steps. The other chapters can be read in any order.

| Chapter | What it covers |
|---------|----------------|
| [Getting started](getting-started.md) | Building Cordite, a first project, something that moves, exporting it |
| [The editor](editor.md) | The panels, moving around, selecting, the gizmo, undo, playing a scene |
| [Projects and assets](assets.md) | The project file, kinds of assets, ids, importing, renaming, the Assets panel |
| [Scenes, entities and components](scenes.md) | How a scene is made, and which component does what |
| [Materials](materials.md) | How surfaces respond to light; material files and instances; decals; screens and mirrors |
| [Light, shadows and the finished picture](lighting.md) | Lights, shadows, sky, fog, ambient occlusion, post-processing, quality |
| [Physics](physics.md) | Colliders, rigid bodies, triggers, joints, characters |
| [Sound](audio.md) | Sound sources, listeners and buses |
| [Animation](animation.md) | Skinned models, skeletons and clips |
| [Cameras](cameras.md) | The camera a game is seen through, shaking it, cinematic sequences and camera moves |
| [Prefabs](prefabs.md) | Groups of entities to place again and again |
| [Input](input.md) | Controls with names, for keyboards, mice and gamepads |
| [Scripting](scripting.md) | Gameplay in Lua |
| [Exporting a game](export.md) | A game people can play without the editor |
| [Automating the editor](automation.md) | Commands, scripts of commands, and working with an AI assistant |

The reference pages list everything there is, and are generated from the engine itself, so
they are always up to date:

- [Components](../reference/components.md) - every component and every property;
- [Scripting API](../reference/script-api.md) - every function scripts can call;
- [Editor commands](../reference/editor-commands.md) - every command of the editor;
- [Input](../reference/input.md) - the names of keys, buttons and sticks.

The showcase project, `samples/showcase`, uses every feature of the engine, and its files are
written to be read: its scene, materials, input map and scripts explain themselves.
