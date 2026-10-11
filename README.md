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
| 3 | Scene and ECS | Planned |
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
./scripts/dev.ps1 run -Target cordite-editor
./scripts/dev.ps1 run -Target cordite-editor -- --headless --frames 10 --screenshot frame.png
```

The second form renders without a window and saves the last frame, which is how the
automated tests and AI agents drive the engine. `--help` lists every option.

## Documentation

- [AGENTS.md](AGENTS.md) - how to work in this repository: commands, coding standards,
  testing and review rules. Written for both people and AI agents.
- [docs/architecture.md](docs/architecture.md) - module map, conventions and the
  reasoning behind the main design decisions.
- Public APIs are documented in their headers under `engine/cordite/`, each with a
  usage example.
