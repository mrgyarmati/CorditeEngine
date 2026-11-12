# Cordite baseline: build and test results

Phase 0 audit, 12 November 2026. Part of the [Phase 0 audit](CORDITE_AUDIT.md).

## Platform

| Item | Value |
|------|-------|
| Commit | `4e03feb` (`main`, one commit ahead of `origin/main`), with no source changes |
| Working copy | Rows 1 to 6: the original at `E:\GameEngineExperiment`. Row 7: the project's clone at `E:\GameEngine` |
| OS | Windows 11 Pro 10.0.26200, x64 |
| GPU | AMD Radeon RX 9070 XT (Vulkan 1.4.359) |
| MSVC | 19.38.33145 (selected by `vcvars64.bat`; `windows` preset) |
| clang-cl | 19.1.5 (`windows-clang` preset) |
| CMake / Ninja | 4.4.4 / 1.13.2, from `.tools/` (`scripts/bootstrap.ps1`) |
| Vulkan validation | Khronos layer from `.tools/vulkan-sdk` (`bootstrap.ps1 -WithVulkanSdk`) |
| D3D12 debug layer | **Not installed** (the optional Windows feature "Graphics Tools") |

## Results

| # | Gate | Command | Result | Time |
|---|------|---------|--------|------|
| 1 | Build, MSVC RelWithDebInfo | `./scripts/dev.ps1 build` | PASS | incremental |
| 2 | Tests, MSVC RelWithDebInfo, all 27 CTest entries including `gpu` and `perf` | `./scripts/dev.ps1 test` | **PASS** (27/27) | 208 s |
| 3 | Full pre-commit gate | `./scripts/dev.ps1 verify` | See rows 3a to 3i | 17 min 41 s |
| 3a | Format | (verify) `format-check` | PASS | 4 s |
| 3b | Build and test, MSVC RelWithDebInfo | (verify) | PASS | 205 s |
| 3c | Build and test, MSVC Release | (verify) | 26/27; `document_links` failed, see note 1 | 205 s |
| 3d | Build and test, MSVC Debug | (verify) | 26/27; `document_links` failed, see note 1 | 403 s |
| 3e | Build and test, clang-cl RelWithDebInfo | (verify) | 26/27; `document_links` failed, see note 1 | 235 s |
| 3f | clang-tidy on changed files | (verify) | PASS (no C++ changed) | 1 s |
| 4 | `document_links` after the audit documents were complete | `cmake -DROOT=<repository> -P cmake/CheckDocumentLinks.cmake` (what the CTest entry runs) | PASS: "127 links of 46 documents lead somewhere" | 1 s |
| 5 | GPU suite on Direct3D 12 | `$env:CORDITE_GRAPHICS='d3d12'; build/windows/bin/RelWithDebInfo/cordite_render_tests.exe` | 468 of 482 cases pass; **14 fail**, see note 2 | 237 s |
| 6 | Clean configure (dependencies downloaded afresh), build and all tests in an empty build tree, MSVC RelWithDebInfo | MSVC environment as `dev.ps1` loads it, then `cmake --preset windows -B build/audit-clean`; `cmake --build build/audit-clean --config RelWithDebInfo`; `ctest --test-dir build/audit-clean -C RelWithDebInfo -j 8` | **PASS**: 717 build steps, no warnings in Cordite's targets (the preset makes them errors), 27/27 tests including `gpu`, `perf` and `document_links` | configure 65 s, build 143 s, tests 252 s |

| 7 | The project's clone at `E:\GameEngine`, built from scratch with this audit's documents in it (toolchain copied from the original `.tools/`) | `./scripts/dev.ps1 build`, then `./scripts/dev.ps1 test` | **PASS**: 27/27, including `gpu`, `perf` and `document_links` over these documents | build 3 min 47 s, tests 249 s |

The `export_smoke`, `new_project_smoke`, `editor_smoke` and `player_starts` entries are
part of rows 2 and 3. They export the showcase, run it as a game with nothing of the
build beside it, make a new project, and run the editor headless with a script of about
95 commands.

### Note 1 - `document_links` during `verify`

`verify` ran while this audit's documents were being written, inside the original
working copy at `E:\GameEngineExperiment`. They have since moved to `E:\GameEngine`
(decisions D1 and D5), and the original working tree is clean again.
`IMPLEMENTATION_MATRIX.md` already linked to `CORDITE_AUDIT.md` and `BASELINE.md`,
which did not exist yet. The test
reported exactly those links:

```
links that lead nowhere:
  docs/architecture/IMPLEMENTATION_MATRIX.md: CORDITE_AUDIT.md
  docs/architecture/IMPLEMENTATION_MATRIX.md: BASELINE.md
```

Every other suite passed in all three configurations and with both compilers. Row 4
re-runs the test with the documents complete.

### Note 2 - the suite on Direct3D 12

No CTest entry runs the GPU suite on D3D12, so it was run by hand. All 14 failures are
the ones AGENTS.md already lists in prose:

| Kind | Test cases |
|------|------------|
| Reference pictures (8) | the showcase as a game; the viewport alone; the viewport through the game's camera; the editor showing the showcase scene; showing its assets; showing a selected light; playing the showcase; in the light theme |
| Fog (3) | "however dense, deep or far away: a fog leaves numbers in every pixel" (1,620 failed checks); "what a view shows through fog is what the fog on each line of sight says" (2,205); "the sky is behind all the fog there is..." (441) |
| Light | "an open floor gets all the light from around it, from wherever it is seen" (3 checks) |
| Editor | "the assets are in front of the console until something fails, which the status bar says too" |
| Readback | "a mip level reads back at its own size" (`GraphicsDeviceTests.cpp:344`, 0 instead of 255) |

The D3D12 debug layer was absent, so the run was not validated by it. The suite says so
itself: "the debug layer of Direct3D 12 is not installed here: nothing to hear". The
suite was run directly rather than through CTest, so the Vulkan validation layer was not
on its path either; that only affects the few Vulkan-specific tests in it. Backlog
P1-08 turns this list into a checked expectation.

## Reproducing

From the repository root, in any shell (`dev.ps1` loads the MSVC environment):

```powershell
./scripts/dev.ps1 build
./scripts/dev.ps1 test
./scripts/dev.ps1 verify
$env:CORDITE_GRAPHICS = 'd3d12'; ./build/windows/bin/RelWithDebInfo/cordite_render_tests.exe; Remove-Item Env:CORDITE_GRAPHICS
```

## Other observations

- The performance reports (`tests/perf/output/*.json`) record `"commit": "unknown"`, so
  `dev.ps1 perf` cannot say which commit a run belongs to. The showcase on this machine
  ran at 1.50 ms median CPU frame at 960x540.
- Windows CI runs no GPU tests and no clang-cl or clang-tidy. These results are
  therefore the only record of the GPU suites on Windows. See
  [DEPENDENCY_AND_PLATFORM_MATRIX.md](DEPENDENCY_AND_PLATFORM_MATRIX.md).
