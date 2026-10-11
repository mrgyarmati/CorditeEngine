# Cordite

Cordite is a 3D game engine for Windows, macOS and Linux, written in C++20 and rendered
with Vulkan through [NVRHI](https://github.com/NVIDIA-RTX/NVRHI). Its default look is
the gritty, high-contrast, normal-mapped style of late-2000s console games: deferred
shading, deep shadows, strong speculars, bloom and filmic post-processing.

The full product brief is [AIGameEngine.md](AIGameEngine.md).

## Status

Cordite is under active development. Milestones follow the brief:

| # | Milestone | State |
|---|-----------|-------|
| 1 | Engine foundation: build system, core, math, tests, tooling | Done |
| 2 | Window and renderer skeleton: GLFW, Vulkan via NVRHI, swapchain, Slang shaders | Done |
| 3 | Scene and ECS: entities, components, hierarchy, scene files, editor with viewport and command automation | Done |
| 4 | Static mesh rendering | Planned |
| 5 | Deferred G-buffer | Planned |
| 6 | Lighting | Planned |
| 7 | Shadows | Planned |
| 8 | HDR and post-processing | Planned |
| 9 | Screen-space effects | Planned |
| 10 | Physics and gameplay | Planned |
| 11 | Editor maturity | Planned |
| 12 | Packaging and export | Planned |

## Building

### Windows

Requires Visual Studio 2022 with the "Desktop development with C++" workload.

```powershell
./scripts/bootstrap.ps1     # once: downloads pinned CMake and Ninja into .tools/
./scripts/dev.ps1 build
./scripts/dev.ps1 test
```

### Linux (Ubuntu 24.04+) and macOS

```bash
# Ubuntu: sudo apt install build-essential cmake ninja-build xorg-dev libwayland-dev libxkbcommon-dev
# macOS:  brew install cmake ninja
cmake --preset linux        # or: macos
cmake --build --preset linux-dev
ctest --preset linux-dev
```

All third-party code is downloaded at configure time from archives pinned by SHA-256;
see [docs/third-party.md](docs/third-party.md).

## Running

A Vulkan 1.3 capable GPU and driver are required.

```powershell
./scripts/dev.ps1 run -Target cordite-editor -- --scene samples/showcase/showcase.scene.json
```

![The editor showing the showcase scene](tests/render/references/editor-showcase-1280x720.png)

The picture is one of the reference images the rendering tests compare against. Until
the engine draws meshes (milestone 4), the viewport shows the grid, a marker for every
entity and the view volumes of cameras.

In the viewport, hold the right mouse button to look around and fly with W A S D, Q and
E; hold the middle button to slide; turn the wheel to move forwards and backwards.

## Automation

Everything the editor can do is a named command that takes and returns JSON, and the
editor runs without a window. A script is a list of commands:

```json
[
    {"command": "entity.create", "params": {"name": "Eye", "components": {"Camera": {}}}},
    {"command": "component.set", "params": {"entity": "Eye", "component": "Transform",
                                            "properties": {"position": [0, 1.7, 4]}}},
    {"command": "editor.screenshot", "params": {"path": "out/eye.png"}},
    {"command": "scene.save", "params": {"path": "out/eye.scene.json"}}
]
```

```powershell
./scripts/dev.ps1 run -Target cordite-editor -- --headless --exec script.json
```

The outcome of every step is written to standard output as JSON, and the exit status
says whether the script ran to the end. `editor.commands` lists every command with its
parameters, and `component.types` describes every component and property; that is what
an AI agent reads to find out what it can do. The same commands can be typed into the
console panel of the interactive editor. `--help` lists every command-line option.

## Documentation

- [AGENTS.md](AGENTS.md) - how to work in this repository: commands, coding standards,
  testing and review rules. Written for both people and AI agents.
- [docs/architecture.md](docs/architecture.md) - module map, conventions and the
  reasoning behind the main design decisions.
- [docs/third-party.md](docs/third-party.md) - every library the engine uses, with
  versions and licenses.
- [.claude/skills](.claude/skills) - step-by-step guides for recurring tasks: building
  and testing, adding a component, a render pass, a dependency or an editor command.
- Public APIs are documented in their headers under `engine/cordite/`, each with a
  usage example.
