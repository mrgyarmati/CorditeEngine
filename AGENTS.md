# Cordite - guide for agents and contributors

Cordite is a 3D game engine for Windows, macOS and Linux (Ubuntu 24.04+), written in
C++20 and rendered with Vulkan through NVRHI. Its default look is the gritty,
high-contrast, normal-mapped style of late-2000s console games. The product vision and
feature list are in [AIGameEngine.md](AIGameEngine.md); the generic coding standard is
[coding-standards.md](coding-standards.md). This file is the concrete, project-specific
version of both. When they disagree, this file wins.

Read this file before changing anything. Task-specific walkthroughs live in
[.claude/skills](.claude/skills) and are referenced from the relevant sections below.

## Hard rules

1. **Stay inside the repository.** Do not read, copy from or reference other code on
   the machine. Third-party code comes only from the pinned archives in
   `cmake/CorditeDependencies.cmake`.
2. **No hacks.** If the proper fix is larger than the task, do the proper fix or stop
   and say so. Never leave a stub that silently does nothing.
3. **Every behavior change ships with tests** that cover the normal case, the edge
   cases and the failure paths.
4. **Review before every commit** using the checklist in
   [Committing](#committing). A commit that was not built, tested, formatted and
   reviewed is not allowed.
5. **Do not reinvent the wheel.** Prefer a maintained library over new code, and
   record why when you decide otherwise (see [Design decisions](docs/architecture.md#design-decisions)).

## Commands

### Windows

```powershell
./scripts/bootstrap.ps1                  # once: CMake + Ninja into .tools/ (git-ignored)
./scripts/bootstrap.ps1 -WithVulkanSdk   # also the Vulkan SDK, for validation layers

./scripts/dev.ps1 build                  # configure if needed, build RelWithDebInfo
./scripts/dev.ps1 build -Config Debug -Target cordite_core_tests
./scripts/dev.ps1 test                   # all tests
./scripts/dev.ps1 test -Filter math      # CTest name regex
./scripts/dev.ps1 test -ExcludeLabel gpu # skip tests that need a Vulkan device
./scripts/dev.ps1 format                 # clang-format in place
./scripts/dev.ps1 format-check           # what CI runs
./scripts/dev.ps1 build -Preset windows-clang   # second compiler (clang-cl)
./scripts/dev.ps1 tidy                   # clang-tidy over the engine (needs the windows-clang preset configured)
./scripts/dev.ps1 tidy -Changed          # only sources affected by uncommitted changes
./scripts/dev.ps1 tidy -Filter math      # only files whose path matches the regex
./scripts/dev.ps1 verify                 # the whole pre-commit gate, one PASS/FAIL line per step
./scripts/dev.ps1 verify -Filter quick   # format-check plus the default build and tests
```

`dev.ps1` loads the MSVC environment itself, so it works from any shell. It requires
Visual Studio 2022 with the "Desktop development with C++" workload.

Run one test case directly from the suite executable:

```powershell
build/windows/bin/RelWithDebInfo/cordite_core_tests.exe -tc="*FixedTimestep*"
build/windows/bin/RelWithDebInfo/cordite_core_tests.exe -ts="Config"      # a whole suite
```

### Linux and macOS

```bash
# Ubuntu 24.04: sudo apt install build-essential cmake ninja-build
# macOS:        brew install cmake ninja
cmake --preset linux            # or: macos
cmake --build --preset linux-dev
ctest --preset linux-dev
```

### Build configurations

| Configuration    | Use                         | Assertions | Optimized |
|------------------|-----------------------------|------------|-----------|
| `Debug`          | Debugging                   | on         | no        |
| `RelWithDebInfo` | Day-to-day development (default) | on    | yes       |
| `Release`        | Shipping / exported games   | off        | yes       |

Code must build warning-free and pass every test in all three. `Release` matters:
`CORDITE_ASSERT` compiles away there, so a bug hidden behind an assertion shows up only
in that configuration.

## Repository layout

```
cmake/                 Build helpers: dependencies, compiler options, module/test macros
engine/cordite/<mod>/  Engine modules (static libraries), one directory each
tests/support/         Test entry point and fixtures
tests/unit/<mod>/      Unit tests, mirroring engine/cordite/<mod>
scripts/               Developer tooling (bootstrap, dev, hash-url)
docs/                  Architecture notes and the third-party list
.claude/skills/        Step-by-step guides for recurring tasks
.tools/                Bootstrapped toolchain (git-ignored)
build/<preset>/        Build trees (git-ignored)
```

## Architecture

Full description: [docs/architecture.md](docs/architecture.md).

The engine is a set of static libraries, one per module, named `cordite::<module>`.
Each module declares the modules it may use in its `CMakeLists.txt`
(`cordite_add_module(... DEPENDS ...)`). The `module_layering` test fails if a module
includes a header from outside its transitive dependencies, so layering cannot erode
by accident.

| Module | Depends on | Contains |
|--------|-----------|----------|
| `core` | -         | Logging, assertions, `Result`, hashing, UUIDs, JSON, binary streams, file I/O, config, job system, allocators, timing |
| `math` | `core`    | glm aliases, `Transform`, bounds, rays, frustum, projections, color, curves, random |

Rules that keep the architecture honest:

- A lower module never knows about a higher one. If `core` needs something from the
  renderer, invert the dependency with an interface declared in `core`.
- Third-party types stay out of public headers unless they *are* the interface (glm
  types, `Json`, `Result`). Wrap everything else behind a pimpl, as `JobSystem` does
  with enkiTS, so a dependency can be replaced without touching its users.
- Engine code never calls `exit`, never throws across a module boundary and never
  blocks the main thread on I/O it could do in a job.

## C++ standards

Language: C++20, no compiler extensions. Formatting is whatever `.clang-format`
produces (clang-format 19); do not hand-format against it. `.clang-tidy` is the linter:
it enforces the naming table below and a curated set of bug, performance and
readability checks, with every finding treated as an error. Fix findings in the code.
Suppress one only when the check is wrong for that line, with
`// NOLINT(check-name)` and a comment saying why; disable a check project-wide only in
`.clang-tidy`, next to a reason.

### Naming

| Thing | Style | Example |
|-------|-------|---------|
| Types, enums, concepts | `PascalCase` | `LinearAllocator`, `ErrorCode` |
| Functions, methods, variables, parameters | `camelCase` | `readTextFile`, `frameSeconds` |
| Private and protected data members | `m_camelCase` | `m_accumulator` |
| Public struct fields | `camelCase` | `transform.position` |
| Constants and enumerators | `kPascalCase` / `PascalCase` | `kPi`, `LogLevel::Warn` |
| Macros | `CORDITE_UPPER_SNAKE` | `CORDITE_ASSERT` |
| Namespaces | lowercase | `cordite`, `cordite::test` |
| Files | `PascalCase`, named after the main type | `FileSystem.h`, `FileSystemTests.cpp` |

Names are descriptive and unabbreviated (`index`, not `idx`; `directory`, not `dir`).
The name of the string form of an enum is `nameOf(value)`, never `toString`: doctest
finds a `toString` overload by ADL and fails to compile.

### Error handling

- **Expected failures** (I/O, malformed data, missing assets, bad user input, device
  loss) return `Result<T>` or `Status`. Build errors with `makeError(code, "format {}",
  args)`; propagate with `CORDITE_TRY` / `CORDITE_TRY_ASSIGN`. Messages say what failed
  and why, and name the file or key involved.
- **Programmer errors** (broken invariants, misuse of an API) use `CORDITE_ASSERT`.
  After the assertion, still leave the program in a safe state where that is cheap,
  because the assertion is absent in `Release`.
- **Never ignore a failure.** Either handle it, propagate it, or log it with enough
  context to act on. `(void)result;` needs a comment explaining why it is safe.
- **External input is untrusted.** Files, JSON, script arguments and automation
  requests are validated at the boundary: types, ranges, lengths, and paths (use
  `relativePathWithin` to keep paths inside a project). Readers must not crash or
  allocate unbounded memory on corrupt data; see `BinaryReader`.
- Exceptions are used only where a dependency forces them, and are caught at that
  boundary and converted to a `Result`.

### Ownership and lifetime

- Prefer values and `std::unique_ptr`. Use `std::shared_ptr` only for genuinely shared
  ownership. Raw pointers and references never own.
- Types that manage a resource delete or define all special members; use
  `CORDITE_NON_COPYABLE` / `CORDITE_NON_MOVABLE`.
- Return `[[nodiscard]]` from functions whose result must be checked or used.

### Headers and includes

- `#pragma once`. Include what you use; do not rely on transitive includes (each
  standard library ships different ones, and that is the most common way a change
  compiles on MSVC and fails on GCC or Apple Clang).
- Include order is enforced by clang-format: own header, engine headers, third-party,
  standard library.
- Keep heavy third-party headers out of public headers; forward-declare
  (`cordite/core/JsonFwd.h`) or use a pimpl.

### Documentation

Every public type and function has a `///` comment that states what it does, its
units and conventions, and how it fails. Every public header starts with a `/// @file`
block containing at least one `@code` usage example. Comments explain *why*; do not
restate what the code already says.

### Portability

Windows is the only platform that can be built on every development machine, so write
defensively for the other two:

- Paths are UTF-8 in data and `std::filesystem::path` in code. Convert with
  `pathToUtf8` / `pathFromUtf8`; never construct a path from `std::string` directly.
- No `std::from_chars` for floating point (missing in Apple's libc++), no `std::pmr`,
  no `std::format` (use fmt).
- Platform-specific code lives behind `#if CORDITE_PLATFORM_*` in a `.cpp`, never in
  a public header.
- No dependence on `long` being 32 or 64 bits, on `char` signedness, or on
  `wchar_t` size. Use `<cstdint>` types.
- Also build with `-Preset windows-clang` before committing; clang-cl catches most of
  what GCC and Apple Clang would reject.

### Math conventions

Right-handed, +Y up, metres, -Z forward. Column-major matrices, column vectors.
Radians in code, degrees in user-facing data. Direct3D-style clip space with reversed
depth for scene rendering. Colors are linear unless a name says sRGB. Quaternions are
serialized as `[x, y, z, w]`. Details are in `engine/cordite/math/Math.h`.

## Testing

Framework: doctest. One executable per module (`cordite_<module>_tests`), registered
with CTest under the module name. See the skill
[build-and-test](.claude/skills/build-and-test/SKILL.md).

- Test files are named `<Thing>Tests.cpp` and live in `tests/unit/<module>/`.
- Test case names are sentences describing behavior: `"an exhausted arena returns
  nullptr and stays usable"`.
- Cover the normal path, the boundaries, and each documented failure. For parsers and
  readers, feed truncated and corrupt input.
- Pin exact expected values where an output is persisted or shared (hashes, UUIDs
  derived from names, file formats, random sequences). A test that compares a function
  against itself proves nothing.
- Tests are deterministic: seeded `Random`, no wall-clock dependence beyond generous
  lower bounds, no network, no files outside `TempDirectory`.
- Assertion-failure paths are tested with `CHECK_THROWS_AS(expr, AssertionFailure)`
  inside `#if defined(CORDITE_ENABLE_ASSERTS)`.
- Use `LogCapture` to assert that a warning or error was (or was not) logged.
- Suites that need a GPU carry the CTest label `gpu`.

## Dependencies

Dependencies are pinned by URL and SHA-256 in `cmake/CorditeDependencies.cmake` and
listed with their licenses in [docs/third-party.md](docs/third-party.md). The build
never tracks a branch or a mutable tag. To add or upgrade one, follow the skill
[add-dependency](.claude/skills/add-dependency/SKILL.md).

A new dependency needs a reason: what it replaces, its license, and that it builds on
all three platforms without tools the build does not already require (no Python, no
package manager).

## Committing

Follow the skill [pre-commit-review](.claude/skills/pre-commit-review/SKILL.md). In
short, before every commit:

1. `./scripts/dev.ps1 verify` prints `VERIFY PASSED`. It runs the format check, the
   MSVC build and tests in `RelWithDebInfo`, `Release` and `Debug`, the clang-cl build
   and tests, and clang-tidy on the changed files. Every step must pass with zero
   warnings.
2. You have re-read the whole diff as a reviewer: design, correctness, error paths,
   lifetime, thread safety, input validation, naming, documentation, tests.
3. Documentation that the change makes stale is updated in the same commit
   (`AGENTS.md`, `docs/`, header comments, skills).

`verify` lints only changed files. Before a commit that touches widely included
headers, also run the full `./scripts/dev.ps1 tidy`.

Commit messages: an imperative subject of at most 72 characters, a blank line, then a
body explaining what changed and why. One logical change per commit.

## Known pitfalls

- PowerShell 5.1: do not append `2>&1` to native commands; it turns stderr lines into
  errors and hides the real output.
- A test executable that cannot find its data is usually being run from the wrong
  directory; CTest runs suites from the repository root.
- The default MSVC toolset on a machine may be older than the newest one installed.
  `dev.ps1` uses whatever `vcvars64.bat` selects; do not rely on features newer than
  MSVC 19.38.
