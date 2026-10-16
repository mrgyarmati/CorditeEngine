# Getting started

This chapter takes you from the source code to a game you can hand to someone: build the
engine, make a project, put something in it that moves, and export it.

## 1. Build Cordite

Cordite is built from source. On **Windows** you need Visual Studio 2022 with the "Desktop
development with C++" workload; everything else is fetched by the scripts:

```powershell
./scripts/bootstrap.ps1          # once: CMake and Ninja
./scripts/dev.ps1 build -Config Release
```

On **Linux** (Ubuntu 24.04 or newer) and **macOS**:

```bash
# Ubuntu: sudo apt install build-essential cmake ninja-build xorg-dev libwayland-dev libxkbcommon-dev mesa-vulkan-drivers
# macOS:  brew install cmake ninja   (and the Vulkan SDK, which brings MoltenVK)
cmake --preset linux                  # or: macos
cmake --build --preset linux-release  # or: macos-release
```

The editor is `build/<platform>/bin/Release/cordite-editor` (`.exe` on Windows), with the
player of games, `cordite-player`, beside it. Your graphics driver must support Vulkan 1.3.

`Release` is the build to make games with. `RelWithDebInfo`, the default of `dev.ps1`, keeps
the engine's internal checks on, which is what you want while working on the engine itself.

## 2. Make a project

Start the editor and choose **File > New project...**, give it a directory and a name - or,
from a terminal:

```
cordite-editor --new-project my-game --name "My Game"
```

The new project opens with a scene to start from: a camera looking at a box on a floor, under
a sun and a sky. On Windows, double-clicking `my-game/project.cordite` opens it again later.

## 3. Find your way around

Hold the right mouse button in the viewport and fly with **W A S D**; select the box with a
click and press **F** to frame it; press **W**, **E** or **R** for the gizmo that moves, turns or
scales it. [The editor](editor.md) explains the rest.

## 4. Make something move

1. **Entity > Create shape > Sphere**. Move it up, above the box (gizmo, or set its position
   in the Inspector to `0, 3, 0.2`).
2. In the Inspector, **Add component > Collider** and set its shape to `sphere`, then
   **Add component > RigidBody**.
3. Select the box and give it a **Collider** too (a `box`, the default). Give the floor one as
   well, with a size of `1, 0.01, 1` - a collider is scaled with its entity, and the floor is
   a plane scaled to twenty metres.
4. Press **Play** (F5). The sphere falls, hits the box and rolls off. Press **Stop**
   (Shift+F5): everything is back where it was.

## 5. Give it a script

Create a file `scripts/bounce.lua` in the project's `assets` directory:

```lua
-- Throws its entity up when "jump" is pressed... or every two seconds, without an input map.
local Bounce = { properties = { strength = 4.0 } }

function Bounce:start()
    cordite.time.every(2.0, function()
        self.entity:addImpulse({ 0, self.properties.strength, 0 })
    end)
end

return Bounce
```

Select the sphere, **Add component > Script**, and pick `scripts/bounce.lua`. Play: the sphere
hops every two seconds. Change `strength` in the Inspector's `properties`, or change the file
while the game runs - the editor reads it again when it is saved. More in
[Scripting](scripting.md).

## 6. Save and export

**File > Save** (Ctrl+S) saves the scene. **File > Export game...** makes a directory with your
game's program and everything it needs - see [Exporting a game](export.md). Double-click the
program, and the game runs without the editor.

## Where to go next

- [Projects and assets](assets.md) - importing models, textures and sounds.
- [Light, shadows and the finished picture](lighting.md) - the look of your game.
- [Input](input.md) - controls with names, for keyboards and gamepads alike.
- The showcase project, `samples/showcase`, which uses every feature of the engine.
