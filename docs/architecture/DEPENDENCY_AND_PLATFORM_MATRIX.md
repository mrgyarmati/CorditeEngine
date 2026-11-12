# Cordite dependency and platform matrix

Phase 0 audit, 12 November 2026, at commit `4e03feb`. Part of the
[Phase 0 audit](CORDITE_AUDIT.md).

## Engine modules

Each module is a static library declared with `cordite_add_module(... DEPENDS ...)`. The
`module_layering` test enforces that a module includes only headers of its transitive
dependencies. It passed in the [baseline](BASELINE.md).

| Module | Declared dependencies | Size (source files / lines) | Unit test cases |
|--------|----------------------|----------------------------:|----------------:|
| `core` | - | 43 / 4.8k | 204 |
| `math` | core | 24 / 2.0k | 118 |
| `platform` | core, math | 10 / 2.3k | 44 |
| `assets` | core, math | 41 / 8.7k | 284 |
| `world` | core, math | 15 / 1.7k | 73 |
| `animation` | core, math, assets | 11 / 1.4k | 57 |
| `physics` | core, math, assets, world | 9 / 5.3k | |
| `audio` | core, math, assets, world | 4 / 1.3k | |
| `scripting` | core, math, platform, assets, world, animation, physics, audio | 18 / 5.4k | |
| `rhi` | core, math, platform | 10 / 2.0k | GPU suite |
| `shaders` | core | 9 / 1.0k | |
| `shader_compiler` | core, shaders | 4 / 0.5k | |
| `renderer` | core, math, assets, world, animation, rhi, shaders | 109 / 18.5k | GPU suite (483 cases in `cordite_render_tests`, all areas) |
| `ui` (Dear ImGui) | core, math, platform, rhi, shaders, renderer | 12 / 1.6k | |
| `engine` | every module above except shader_compiler | 16 / 2.5k | 60 |
| `editor` | core, math, platform, assets, world, rhi, renderer, shaders, shader_compiler, ui, engine | 71 / 16.4k | 272 |

Blank test counts were not tallied in this audit; every one of those suites ran and
passed in the baseline.

### Optionality today

The plan asks that C#, Lua and MCP stay optional and that optional modules not enlarge
the minimal runtime. Today:

| Capability | Can a project or build leave it out? | Why |
|------------|--------------------------------------|-----|
| Lua scripting | **No** | `engine` depends on `scripting`; `Engine.h:53` includes `ScriptSystem.h`; the engine creates the script system unconditionally; there is no CMake option |
| Physics | No | `engine` and `scripting` depend on it |
| Audio | No | `engine` and `scripting` depend on it; `assets` links miniaudio to decode sounds |
| Dear ImGui | No | `engine` depends on `ui`; `Engine.cpp:198` creates an ImGui context in every game |
| Shader compiler | **Yes** | Exported games use a precompiled cache; `cordite-player` does not link `shader_compiler` |
| Editor | Yes | `cordite-player` links nothing of the editor |

A Lua-free C++ project is therefore possible only in the sense that it attaches no
scripts. The runtime still links Lua. The plan's principle "No required C# or Lua
runtime for C++-only projects" is not met at link level. See risk R6 in
[TECHNICAL_RISKS.md](TECHNICAL_RISKS.md).

## Third-party dependencies

All are pinned by URL and SHA-256 in `cmake/CorditeDependencies.cmake` and listed with
licences in `docs/third-party.md`. The two agree.

| Library | Version | Licence | Used by | Public in headers? |
|---------|---------|---------|---------|--------------------|
| fmt | 12.2.0 | MIT | core | yes (logging) |
| spdlog | 1.17.0 | MIT | core | no |
| nlohmann/json | 3.12.0 | MIT | core | yes (`Json` is the interface) |
| tl::expected | 1.3.1 | CC0-1.0 | core | yes (`Result`) |
| enkiTS | 1.12 | zlib | core | no (pimpl); **unused by engine code** |
| xxHash | 0.8.4 | BSD-2-Clause | core | no |
| glm | 1.0.3 | MIT | math | yes |
| GLFW | 3.5.1 | zlib | platform | no |
| Vulkan-Headers | 1.4.352 | Apache-2.0 OR MIT | rhi, platform (surface) | `GraphicsDevice::vulkan()` declared publicly |
| DirectX-Headers | 1.717.0-preview | MIT | rhi (Windows) | no |
| NVRHI | `6b96fb0` | MIT | rhi, renderer, ui, engine, editor | **yes, widely** (31 of 55 renderer headers; `Engine.h`) |
| stb | `2c980bb` | MIT / public domain | assets | no |
| cgltf | 1.15 | MIT | assets | no |
| meshoptimizer | 1.3 | MIT | assets | no |
| MikkTSpace | `3e895b4` | zlib | assets | no |
| ufbx | 0.23.1 | MIT / public domain | assets | no |
| CLI11 | 2.7.2 | BSD-3-Clause | editor, player executables | no |
| EnTT | 3.16.0 (kept on 3.x for MSVC 19.38) | MIT | world | no |
| Jolt Physics | 5.6.0 | MIT | physics | no |
| Dear ImGui | 1.92.9b docking | MIT | ui, editor | yes (`imgui.h` in editor headers) |
| Lua | 5.4.8, compiled as C++ | MIT | scripting | yes (`lua_State` in `ScriptSystem.h`) |
| miniaudio | 0.11.25 | Unlicense OR MIT-0 | audio, assets | no |
| nativefiledialog-extended | 1.4.1 | Zlib | editor | no |
| Slang (prebuilt) | 2026.19 | Apache-2.0 WITH LLVM-exception | shader_compiler | no |
| DXC (prebuilt, Windows) | 1.9.2609 | MIT and University of Illinois/NCSA | shader_compiler (loaded by Slang) | no |
| MoltenVK (macOS) | 1.4.2 | Apache-2.0 | shipped in macOS bundles | no |
| doctest | 2.5.3 | MIT | tests | no |
| Inter 4.1, JetBrains Mono 2.304, Lucide 0.469.0 | | OFL-1.1 / ISC | ui (embedded) | no |
| ltc_code `31e5e96` | | BSD-2-Clause | renderer (tables) | no |

### Dependencies the runtime UI may need (not yet pinned)

ImGui rasterises its fonts with its bundled stb_truetype and does no shaping.

**Decided 12 November:** the runtime UI uses bitmap fonts for now, rasterised with
stb_truetype from the stb tree already pinned. So Phase 2 adds no dependency. The others
below remain options for later. Each needs an ADR and a licence entry under the
`add-dependency` skill:

| Need | Candidates | Notes |
|------|-----------|-------|
| Glyph rasterisation | stb_truetype (already present inside ImGui and the pinned stb tree), FreeType | stb_truetype is enough for Latin text at fixed sizes |
| Shaping, bidirectional text | HarfBuzz | Only when non-Latin scripts are a target |
| Scalable text | msdfgen / msdf-atlas-gen | Sharp text at any scale and in world space |

## Platforms

"Built" means compiled and unit-tested in CI. "Rendered" means GPU tests ran on that
system. "Run by a person" means someone used the editor or a game there.

| Platform | Compilers | Built in CI | GPU tests | Editor / game run by a person | Export |
|----------|-----------|-------------|-----------|-------------------------------|--------|
| Windows 11 x64 | MSVC (`windows`), clang-cl (`windows-clang`) | MSVC only, RelWithDebInfo and Release, `-ExcludeLabel gpu` | **Locally only**: all GPU suites pass on an AMD Radeon RX 9070 XT ([baseline](BASELINE.md)) | Yes (the user's machine) | Verified (`export_smoke`) |
| Ubuntu 24.04 x64 | GCC (`linux`), GCC with ASan and UBSan (`linux-sanitize`) | Yes, on every push (it did not build from the first commit until 12 November) | **Yes, lavapipe (software Vulkan) under xvfb**, with validation layers, but for 17 cases listed as expected failures (P1-13) | Not recorded | Built and smoke-tested in CI; not run on hardware |
| macOS 15 | Apple Clang (`macos`) | Only on manual dispatch and `v*` tags, `-LE gpu` | No | **No** (README: "not yet run on a Mac") | Bundle layout tested as files only |

### Graphics backends

| Backend | State | Evidence |
|---------|-------|----------|
| Vulkan 1.3 (Windows, Linux) | Production | Every GPU suite; swapchain tests |
| Vulkan via MoltenVK (macOS) | Unknown | Code path present (`VulkanDevice.cpp:168-175`); never run |
| Direct3D 12 (Windows) | Offscreen only | No swapchain; `Auto` never selects it; `render_d3d12` runs the GPU suite on it (P1-08), its 14 known failures listed. See [BASELINE.md](BASELINE.md) for the suite run on D3D12 in this audit |
| WARP (D3D12 software) | Offscreen only | `CORDITE_GRAPHICS=warp` |
| OpenGL 4.5 | Absent; **decided 12 November** as the Linux fallback when there is no Vulkan | No code. NVRHI has no GL backend; see GL-01 to GL-08 in [PHASE_1_2_BACKLOG.md](PHASE_1_2_BACKLOG.md) |
| Metal (native) | Absent | Not planned |

### Platform-specific code

`CORDITE_PLATFORM_WINDOWS` appears 31 times, `CORDITE_PLATFORM_MACOS` 12 times and
`CORDITE_PLATFORM_LINUX` 3 times, in `core/Base.h`, `core/Platform.cpp`,
`platform/SystemWindow.cpp`, `rhi/GraphicsDevice.cpp`, `rhi/vulkan/VulkanDevice.cpp`,
`shader_compiler/SlangShaderCompiler.cpp`, `editor/FileDialogs.cpp` and
`editor/GameExport.cpp`. The renderer and `ui` have none. Window, input and gamepads go
entirely through GLFW.

### CI gates versus the local gate

| Check | Local `dev.ps1 verify` | CI (`.github/workflows/ci.yml`) |
|-------|------------------------|---------------------------------|
| clang-format | yes | yes (clang-format 19.1.5) |
| MSVC RelWithDebInfo / Release | yes, with GPU tests | yes, **without** GPU tests |
| MSVC Debug | yes | no |
| clang-cl | yes | no |
| clang-tidy | changed files | **no** |
| GCC, ASan/UBSan | no | yes |
| GPU tests | Windows hardware | Linux lavapipe only, 17 cases expected to fail |
| D3D12 suite | no | no |
| macOS | no | manual only |
