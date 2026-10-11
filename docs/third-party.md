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
