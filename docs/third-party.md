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
| [stb](https://github.com/nothings/stb) | commit `2c980bb` | MIT OR public domain | Image decoding (`stb_image`) and PNG encoding (`stb_image_write`) |
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.7.2 | BSD-3-Clause | Command-line parsing in the executables |
| [EnTT](https://github.com/skypjack/entt) | 3.16.0 | MIT | Entity and component storage behind `World` |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9b, docking branch | MIT | Immediate-mode user interface: the editor and in-game debug overlays |

EnTT stays on the 3.x line: 4.0 needs C++20 syntax that the default MSVC toolset of
many Visual Studio 2022 installations (19.38) does not accept. Dear ImGui comes with the
fonts it embeds (ProggyClean and ProggyForever, MIT) and uses its bundled
copies of stb_rect_pack, stb_textedit and stb_truetype (public domain / MIT).

## Linked into the editor and tools, not into exported games

| Library | Version | License | Used for |
|---------|---------|---------|----------|
| [Slang](https://github.com/shader-slang/slang) | 2026.19 (official prebuilt binaries) | Apache-2.0 WITH LLVM-exception | Shader compilation to SPIR-V |

Slang is a shared library. On Windows its DLLs are copied next to the executables at
build time; on Linux and macOS the executables find it through their build RPATH.

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
