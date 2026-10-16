# Exporting a game

A game is exported as a directory that people can play without the editor: its program, its
assets and everything the program needs.

## Exporting

Save the scene, then choose **File > Export game...** and say where the game goes. Or, from a
terminal:

```
cordite-editor --project my-game --export builds/MyGame
```

The directory gets:

```
builds/MyGame/
    MyGame.exe            the game's program (named after the game; no .exe on Linux and macOS)
    project.cordite       the project file
    assets/               the assets, with their .meta files
    .cordite/             the engine's shaders, compiled for the game
    licenses/             the licenses of the libraries the program is made of
```

Hand the directory out as it is - zip it, or put it into an installer or a store's uploader.
The program needs nothing installed but a graphics driver with Vulkan 1.3. Double-clicking it
starts the game's start scene in a window; if the game cannot start, a message says why.

Exporting into the same directory again replaces the game that was exported there, and
nothing else: files you put beside it - a readme, a manual - stay. A directory that holds
anything else is refused, and so is one inside the project.

A game runs on the kind of system it was exported on: export on Windows for Windows, on Linux
for Linux. For a game to be shipped, export with the `Release` build of the editor: the
program is the one built beside the editor, and only `Release` leaves out the engine's
internal checks.

## Running a game from a terminal

The program takes options, for testing and for players who want them:

| Option | Does |
|--------|------|
| `--fullscreen` | Fill the screen |
| `--width`, `--height` | The size of the window, in pixels |
| `--no-vsync` | Do not wait for the display |
| `--adapter NAME` | Prefer the GPU whose name contains this |
| `--game DIR` | Play the game in another directory |
| `--headless --frames N --screenshot out.png` | Play N frames without a window and save the last one |
| `--log-level debug` | Say more (`trace`, `debug`, `info`, `warn`, `error`, `critical`, `off`) |

## Licenses

The `licenses` directory holds the notices of the open-source libraries the program is built
from (`THIRD-PARTY.md` lists them). Their licenses ask for these notices to go with every
copy: leave the directory in what you hand out, and mention it in your game's credits.
