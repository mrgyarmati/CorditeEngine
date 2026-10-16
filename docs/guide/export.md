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

## The settings of the person who plays

A game keeps what the person who plays it chose in a file of their own: `settings.json`, in
a directory named after the game in their settings directory - `%APPDATA%\My Game` on
Windows, `~/Library/Application Support/My Game` on macOS, `~/.config/My Game` on Linux.
The first time the game starts it writes the defaults there; whatever is changed in the file
is what the next start takes:

```json
{
    "format": "cordite.settings",
    "version": 1,
    "fullscreen": false,
    "width": 1280,
    "height": 720,
    "vsync": true,
    "quality": "medium",
    "masterVolume": 1.0,
    "effectsVolume": 1.0,
    "musicVolume": 1.0,
    "ambienceVolume": 1.0,
    "voiceVolume": 1.0,
    "interfaceVolume": 1.0
}
```

`quality` is `low`, `medium` or `high`: what shadows, ambient occlusion and the smoothing of
edges may cost. Volumes go from 0, nothing, to 1, as the sounds were made; each one is a bus
of the audio (see [Audio](audio.md)), and `masterVolume` is over all of them. A file that
cannot be read - a value out of its range, a name spelled wrong - is left as it is, and the
game plays with the defaults; its log says what is wrong.

The settings are the player's, not the game's: they are not in the game's directory, an
export does not touch them, and two copies of a game share them.

## Running a game from a terminal

The program takes options, for testing and for players who want them. Those that the
settings have too win over the settings for that start, and are not written into them:

| Option | Does |
|--------|------|
| `--fullscreen`, `--windowed` | Fill the screen, or play in a window |
| `--width`, `--height` | The size of the window, in pixels |
| `--no-vsync` | Do not wait for the display |
| `--quality low` | What a picture may cost: `low`, `medium` or `high` |
| `--settings FILE` | Keep the settings in another file |
| `--adapter NAME` | Prefer the GPU whose name contains this |
| `--game DIR` | Play the game in another directory |
| `--headless --frames N --screenshot out.png` | Play N frames without a window and save the last one; no settings are read or written unless `--settings` names a file |
| `--log-level debug` | Say more (`trace`, `debug`, `info`, `warn`, `error`, `critical`, `off`) |

## Licenses

The `licenses` directory holds the notices of the open-source libraries the program is built
from (`THIRD-PARTY.md` lists them). Their licenses ask for these notices to go with every
copy: leave the directory in what you hand out, and mention it in your game's credits.
