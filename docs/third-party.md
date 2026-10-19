# Third-party software

Everything Cordite links against or needs to build. Versions and download hashes are
pinned in [cmake/CorditeDependencies.cmake](../cmake/CorditeDependencies.cmake) and
[scripts/bootstrap.ps1](../scripts/bootstrap.ps1); keep this list in sync with them.

## Linked into the engine

| Library | Version | License | Used for |
|---------|---------|---------|----------|
| [fmt](https://github.com/fmtlib/fmt) | 12.2.0 | MIT | Text formatting |
| [spdlog](https://github.com/gabime/spdlog) | 1.17.0 | MIT | Log sinks (console, file) |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | JSON parsing and writing |
| [tl::expected](https://github.com/TartanLlama/expected) | 1.3.1 | CC0-1.0 | `Result<T>` error handling |
| [enkiTS](https://github.com/dougbinks/enkiTS) | 1.12 | zlib | Task scheduler behind `JobSystem` |
| [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.4 | BSD-2-Clause | Content hashing |
| [glm](https://github.com/g-truc/glm) | 1.0.3 | MIT | Vector, matrix and quaternion math |
| [GLFW](https://github.com/glfw/glfw) | 3.5.1 | zlib | Windows and input |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | commit `6b96fb0` (2026-10-05) | MIT | Rendering hardware interface over Vulkan |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | 1.4.352 | Apache-2.0 OR MIT | Vulkan API declarations (headers only) |
| [stb](https://github.com/nothings/stb) | commit `2c980bb` | MIT OR public domain | Image decoding (`stb_image`), PNG and Radiance HDR encoding (`stb_image_write`) and mipmap generation (`stb_image_resize2`) |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT | Parsing glTF 2.0 files |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | 1.3 | MIT | Welding vertices and ordering imported meshes for the GPU |
| [MikkTSpace](https://github.com/mmikk/MikkTSpace) | commit `3e895b4` | zlib | Tangents for meshes that come without them |
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.7.2 | BSD-3-Clause | Command-line parsing in the executables |
| [EnTT](https://github.com/skypjack/entt) | 3.16.0 | MIT | Entity and component storage behind `World` |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | 5.6.0 | MIT | Rigid bodies, collision queries and character movement behind `PhysicsWorld` |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9b, docking branch | MIT | Immediate-mode user interface: the editor and in-game debug overlays |
| [Lua](https://www.lua.org) | 5.4.8 | MIT | The language of gameplay scripts, behind `ScriptRuntime` |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | Unlicense OR MIT-0 | Sound: decoding files (WAV, FLAC, MP3), writing WAV files, and behind `AudioSystem` mixing, placing sound in space and playing it on the machine's device |

EnTT stays on the 3.x line: 4.0 needs C++20 syntax that the default MSVC toolset of
many Visual Studio 2022 installations (19.38) does not accept. Dear ImGui uses its
bundled copies of stb_rect_pack, stb_textedit and stb_truetype (public domain / MIT);
the fonts it would embed are left out of the build (`IMGUI_DISABLE_DEFAULT_FONT`).

Lua is built from its sources as C++, so that an error in a script unwinds the engine's
functions as an exception does, and without the libraries a script has no business
with: files, the operating system, loading of code and the debug library are not
compiled (see `cmake/CorditeDependencies.cmake` and `engine/cordite/scripting/LuaConfig.h`).

miniaudio is built from its one source file by a target of Cordite's own, without its
resource manager: the engine reads files itself. Its decoders are the copies it carries
of dr_wav, dr_flac and dr_mp3 (public domain or MIT-0), the last of which is built on
minimp3 (CC0-1.0).

glTF files are written by the engine's own exporter, on top of the JSON library, not
by cgltf's writer: a glTF file is a JSON document plus a buffer, and building cgltf's
pointer-linked structures only to have them turned back into JSON would be more code
than writing the JSON.

## Compiled into the engine: typefaces and icons

| Font | Version | License | Used for |
|------|---------|---------|----------|
| [Inter](https://github.com/rsms/inter) | 4.1 | SIL Open Font License 1.1 | The typeface of the interface, regular and semibold |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | 2.304 | SIL Open Font License 1.1 | What is typed and logged in the interface (the version without ligatures) |
| [Lucide](https://lucide.dev) | 0.469.0 (`lucide-static`) | ISC; parts from Feather, MIT | The icons of the interface, as a font |

The font files are compiled into the `ui` module byte for byte, with the names and the
licenses they carry inside them. The Open Font License allows fonts to be bundled with
software of any license, as long as they are not sold by themselves and the license
goes with them: a package of the editor or of a game has to carry the license texts
of these three (`LICENSE.txt` of Inter, `OFL.txt` of JetBrains Mono, `LICENSE` of
Lucide), which the build copies into `licenses/` beside the programs and an export ships
with every game.

## Compiled into the engine: data

| Data | Version | License | Used for |
|------|---------|---------|----------|
| [ltc_code](https://github.com/selfshadow/ltc_code) | commit `31e5e96` (2019-04-15) | BSD-2-Clause, with a request to cite the paper | The fit of GGX by linearly transformed cosines that what a rectangle of light gives a shiny surface is worked out with: the two tables of `fit/results/ltc.js` |

The numbers of the two tables are compiled into the `renderer` module
(`cmake/CorditeEmbedNumbers.cmake`, `engine/cordite/renderer/RectLights.cpp`), which gives
them to the shaders of every view as two textures (`ViewBindings`); nothing else of the
repository is built. Its authors ask that the paper be cited where its code or what
it made is used:

> Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt. Real-Time Polygonal-Light
> Shading with Linearly Transformed Cosines. ACM Transactions on Graphics (Proceedings of
> ACM SIGGRAPH 2016) 35(4), 2016. Project page: https://eheitzresearch.wordpress.com/415-2/

The fit is of GGX with Smith's height-correlated visibility, which is the GGX of
`shaders/common/Brdf.slang`; `RectLightsTests` holds it to that GGX, added up over the
rectangles and the sky it reflects.

## Shipped with games on macOS

| Library | Version | License | Used for |
|---------|---------|---------|----------|
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) | 1.4.2 (official prebuilt binaries, `MoltenVK-macos.tar`) | Apache-2.0 | Vulkan on Metal, which macOS does not have: the library a game exported there brings in `Contents/Frameworks` of its bundle, and loads directly, without the Vulkan loader |

Only a build on macOS downloads it. Building it from source takes Xcode and scripts of its
own, so the dynamic library of its publisher is pinned by hash instead, as Slang's are; the
build copies it beside the player, with its notice into `licenses/`, and an export puts it
into the bundle (`editor/GameExport.h`). MoltenVK's own guide describes embedding it so. The
editor and the tests on a Mac use the Vulkan the system has - the Vulkan SDK's - with its
validation layers.

## Linked into the editor and tools, not into exported games

| Library | Version | License | Used for |
|---------|---------|---------|----------|
| [Slang](https://github.com/shader-slang/slang) | 2026.19 (official prebuilt binaries) | Apache-2.0 WITH LLVM-exception | Shader compilation to SPIR-V, and through DXC to DXIL |
| [DXC](https://github.com/microsoft/DirectXShaderCompiler), the DirectX shader compiler | 1.9.2609 (official prebuilt binaries; Windows only) | MIT and the LLVM Release License (University of Illinois/NCSA) | `dxcompiler.dll`, which Slang loads to write DXIL for Direct3D 12 |
| [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended) | 1.4.1 | Zlib | The system's own dialogs in which the editor's File menu asks for paths (`editor/FileDialogs.h`) |

Slang is a shared library. On Windows its DLLs are copied next to the executables at
build time; on Linux and macOS the executables find it through their build RPATH.

DXC comes as its publisher's release, of which only `dxcompiler.dll` is copied beside the
executables, where Slang finds it; nothing is linked against it. The release also holds
`dxil.dll`, the validator, under Microsoft's own license terms: it is not used - DXC signs
the DXIL it writes itself since 1.8.2502 - nor copied. No exported game carries DXC: a game
ships its shaders compiled.

nativefiledialog-extended is built from its sources by its own CMake project. On Linux it
asks xdg-desktop-portal for its dialogs over D-Bus, and the build needs the headers of
D-Bus (`libdbus-1-dev`), not GTK; its support for Wayland is left out (AGENTS.md, Known
pitfalls). On Windows the editor links the COM libraries it needs (`ole32`, `uuid`).

## Tests only

| Library | Version | License | Used for |
|---------|---------|---------|----------|
| [doctest](https://github.com/doctest/doctest) | 2.5.3 | MIT | Unit test framework |

## Build tools

Not distributed with the engine or with exported games.

| Tool | Version | License | Notes |
|------|---------|---------|-------|
| [CMake](https://cmake.org) | 3.28 or newer (4.4.4 bootstrapped on Windows) | BSD-3-Clause | Build system generator |
| [Ninja](https://ninja-build.org) | 1.13.2 bootstrapped on Windows | Apache-2.0 | Build executor |
| [clang-format](https://clang.llvm.org/docs/ClangFormat.html) | 19 | Apache-2.0 WITH LLVM-exception | Code formatting |
| [clang-tidy](https://clang.llvm.org/extra/clang-tidy/) | 19 | Apache-2.0 WITH LLVM-exception | Static analysis and naming checks |
| [Vulkan SDK](https://vulkan.lunarg.com) | 1.4.363.0 (optional) | Various, see SDK | Validation layers for development |
