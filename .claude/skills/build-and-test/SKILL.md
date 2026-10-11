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

## Performance

```powershell
./scripts/dev.ps1 perf                    # RelWithDebInfo
./scripts/dev.ps1 perf -Config Release    # what a shipped game would have
```

runs the `performance` suite on its own and prints, for each of its scenarios, what it
measured next to the run before:

```
showcase on AMD Radeon RX 9070 XT, commit dd3c5d2+; the run before: commit dd3c5d2
                                    now       before    change
  CPU frame, median (ms)          0.177        0.175     +1.3%
  GPU frame, median (ms)          0.119        0.118     +0.8%
  first frame (ms)               79.813       80.755     -1.2%
  draw calls                     37.000       37.000      0.0%
  ...
lights on AMD Radeon RX 9070 XT, commit dd3c5d2+; the run before: commit dd3c5d2
  CPU frame, median (ms)          0.888        0.897     -1.0%
  collect lights (ms)             0.140        0.139     +0.5%
  cluster lights (ms)             0.331        0.331      0.0%
  light list entries         20,601.000   20,601.000      0.0%
  ...
```

The scenarios are the showcase as a game shows it (`ShowcasePerformanceTests.cpp`), a
field under six hundred lights, and the same field with more than fifty shadow maps
(`lights` and `shadows`, both in `LightingPerformanceTests.cpp`); what they share is in
`tests/perf/PerformanceTestSupport.h`.

Run it before a change and after it. Times move by a few percent from run to run; a
change is real when it is larger than that and repeats. The full report of the last run
of a scenario is `tests/perf/output/<scenario>.json`, and every run adds a line to
`history.jsonl` next to it (neither is committed).

The same suite runs with all the other tests and checks the numbers against
`tests/perf/budgets.json`:

- **A count or size is over budget** (draw calls, triangles, lights, entries of the
  light lists, shadow maps, bytes on the GPU): these are the same on every machine, so
  the change did it. If it is meant - the showcase gained something - raise the budget in the
  same commit, leaving little room. If not, find what stopped being instanced, culled
  or shared, or which lights got into lists of clusters they do not reach.
- **A time is over budget**: the budgets are many times what is normal, so something
  became very slow. Compare the scenario's report with an earlier run to see which
  phase.

To measure something new, add it to `FrameStatistics` (`engine/Engine.h`) if it is a
cost of every frame, show it in `StatisticsPanel`, and add it to the report
(`describeScene` in `PerformanceTestSupport.h`), to the budgets in
`tests/perf/budgets.json` and to the table `dev.ps1 perf` prints
(`Get-PerformanceMetrics`). A cost that the showcase is too small to show gets a
scenario of its own, built in code like the one for lights.
