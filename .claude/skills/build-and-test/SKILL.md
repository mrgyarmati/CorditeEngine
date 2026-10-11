---
name: build-and-test
description: Build Cordite and run its tests on Windows, Linux or macOS. Use when you need to compile the engine, run or debug a failing test, run a single test case, check a second compiler, or work out why a build or test run fails.
---

# Build and test Cordite

## Windows

Everything goes through `scripts/dev.ps1`, which loads the MSVC environment and puts the
bootstrapped CMake and Ninja on PATH. Run it with the PowerShell tool.

```powershell
./scripts/bootstrap.ps1                 # once per machine, if .tools/ is missing
./scripts/dev.ps1 build                 # RelWithDebInfo, configures on first use
./scripts/dev.ps1 test
```

Useful variations:

```powershell
./scripts/dev.ps1 build -Config Release
./scripts/dev.ps1 build -Target cordite_math_tests      # one target
./scripts/dev.ps1 test -Config Release
./scripts/dev.ps1 test -Filter core                     # CTest name regex
./scripts/dev.ps1 test -ExcludeLabel gpu                # no Vulkan device available
./scripts/dev.ps1 build -Preset windows-clang           # clang-cl, separate build tree
./scripts/dev.ps1 test -Preset windows-clang
./scripts/dev.ps1 configure                             # after editing any CMake file
```

Do not append `2>&1` to these commands. Windows PowerShell 5.1 converts stderr lines of
native programs into error records, which hides the real output and makes a successful
command look failed.

### Reading a failed build

Pipe the build through a filter so only the diagnostics remain:

```powershell
./scripts/dev.ps1 build | Select-String -Pattern '\): (fatal )?error |: warning |error: |LNK\d+|ninja: build stopped' | ForEach-Object { $_.Line.Trim() } | Select-Object -First 40
```

Fix the first error first; later ones are often consequences of it. Warnings are errors
in the `windows` and `windows-clang` presets.

### Running one test

CTest runs each suite as one process. To iterate on a single case, call the suite
executable directly (from the repository root, as CTest does):

```powershell
build/windows/bin/RelWithDebInfo/cordite_core_tests.exe -tc="*FixedTimestep*"   # test cases by name
build/windows/bin/RelWithDebInfo/cordite_core_tests.exe -ts="Config"            # a whole TEST_SUITE
build/windows/bin/RelWithDebInfo/cordite_core_tests.exe -ltc                    # list test cases
build/windows/bin/RelWithDebInfo/cordite_core_tests.exe -s                      # also print passing checks
```

## Linux and macOS

```bash
cmake --preset linux                 # or: macos
cmake --build --preset linux-dev     # linux-debug, linux-release, macos-dev, ...
ctest --preset linux-dev
ctest --preset linux-dev -R math     # one suite
ctest --preset linux-dev -LE gpu     # skip GPU tests
cmake --preset linux-sanitize && cmake --build --preset linux-sanitize && ctest --preset linux-sanitize
```

## What must pass before a commit

`./scripts/dev.ps1 verify` runs every row of this table and reports one line per step:

| Check | Command |
|-------|---------|
| Formatting | `./scripts/dev.ps1 format-check` |
| MSVC, development | `./scripts/dev.ps1 build` then `./scripts/dev.ps1 test` |
| MSVC, shipping | `./scripts/dev.ps1 build -Config Release` then `./scripts/dev.ps1 test -Config Release` |
| clang-cl | `./scripts/dev.ps1 build -Preset windows-clang` then `./scripts/dev.ps1 test -Preset windows-clang` |
| Linter | `./scripts/dev.ps1 tidy` (uses the `windows-clang` build tree; configure it first) |

`tidy` prints each finding once and keeps the full clang-tidy output, with source
excerpts and fix suggestions, in `build/windows-clang/tidy/`. Narrow a run with
`-Filter <regex>` while iterating.

`Release` is not optional: assertions compile away there, so code that only works
because an assertion fired first, or a variable used only inside an assertion, breaks
in that configuration alone.

## When something fails

- **"cmake was not found"**: run `./scripts/bootstrap.ps1`.
- **A dependency fails its SHA-256 check**: the archive changed upstream or the download
  was corrupted. Delete `build/<preset>/_deps/<name>-subbuild` and reconfigure. If it
  fails again, do not update the hash blindly; find out why the archive changed.
- **Compiles with MSVC, fails with clang-cl**: usually a missing `typename`/`template`
  keyword, a missing include, or a warning MSVC does not have. Fix the code; do not
  disable the warning unless it is wrong for the whole project, and then do it in
  `cmake/CorditeCompilerOptions.cmake` with a comment.
- **Passes in RelWithDebInfo, fails in Release**: something depends on an assertion or
  on debug-only state. Look for side effects inside `CORDITE_ASSERT`.
- **`module_layering` fails**: a module includes a header from a module it does not
  depend on. Either declare the dependency in that module's `CMakeLists.txt` (only if
  the layering in `AGENTS.md` allows it) or move the code.
- **A test that used to pass is flaky**: it depends on timing, thread scheduling or
  iteration order. Make it deterministic rather than retrying it.
