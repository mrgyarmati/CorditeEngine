# Cordite technical risks

Phase 0 audit, 12 November 2026, at commit `4e03feb`. Part of the
[Phase 0 audit](CORDITE_AUDIT.md). Each risk names its evidence and the phase of the
[plan](../GAME_ENGINE_PLAN.md) it threatens. Source paths are relative to the
repository root, at commit `4e03feb`. Severity is the damage if it is ignored when
that phase starts. Likelihood is how sure we are that the phase will hit it.

| Id | Risk | Severity | Likelihood | Phase |
|----|------|----------|------------|-------|
| R1 | No way for a UI to consume input before gameplay sees it | High | Certain | 2, 3 |
| R2 | No 2D or text rendering outside ImGui | High | Certain | 2 |
| R3 | Editor operations block the main thread, with no progress or cancellation | Medium | Certain | 1, 5 |
| R4 | DPI and framebuffer changes are not observed | Medium | Likely | 2 |
| R5 | GPU coverage on Windows, D3D12 and macOS exists only on one developer machine, or nowhere | High | Likely | all |
| R6 | Lua, physics, audio and ImGui are mandatory parts of every runtime | Medium | Certain | 2, 7 |
| R7 | NVRHI is the engine's de facto public RHI, and has no OpenGL backend | High | Certain | GL track |
| R8 | No device-loss recovery | Medium | Possible | 1, 8 |
| R9 | Synchronous, single-threaded asset loading; the job system is unused | Medium | Certain | 5 |
| R10 | Commands are not uniformly routed, flagged or confined | Medium | Certain | 6 |
| R11 | The script API is hand-bound to Lua | Medium | Certain | 7 |
| R12 | Reflection is too thin for widget properties, data assets and C# | Medium | Likely | 2, 4, 7 |
| R13 | Document formats have versions but no migrations | Medium | Certain | 1 (prefab links), 2, 4 |
| R14 | Editor reference images let small changes through | Low | Certain | 1, 3 |
| R15 | Audio has never been heard; real gamepads never tested | Low | Possible | 3, 4 |
| R16 | Plan and repository documents contradict each other | Low | Certain | 0 |
| R17 | The engine's shaders may not map cleanly onto GLSL bindings | High | Possible | GL track |

## R1 - Input cannot be consumed

`InputActions::update` runs at `Engine.cpp:270`, before anything else sees the frame's
input. The only routing in the codebase is the editor's own (`IsItemHovered`,
`WantTextInput`). `UiSystem::wantsPointer` and `wantsKeyboard` exist but nothing outside
`UiSystem` calls them. `InputMap` has no contexts, priorities or modifiers, and GLFW's
`mods` argument is dropped (`platform/Window.cpp:176`).

The CommonUI requirement "gameplay input blocked while a modal is active" cannot be met
without a change here. **Mitigation:** an input-context stack with consumption, as the
first ticket of Phase 2 (P2-03), designed together with the runtime UI router rather
than bolted on afterwards.

## R2 - No 2D or text path outside ImGui

The renderer has no sprite, quad-batch or text path. `ui/UiRenderer.cpp` is a sound
batched, scissor-clipped, textured-triangle renderer, but its input is `ImDrawData`. Its
limits are fine for the editor and would hurt a HUD: 16-bit indices, whole-atlas
re-upload, a full `setGraphicsState` per draw, and straight alpha in sRGB. Fonts are
ImGui's atlas with no shaping.

**Mitigation:** extract an ImGui-free batch renderer from `UiRenderer.cpp`, and keep
ImGui as one client of it, so the existing UI reference images guard the extraction
(P2-01). Text uses bitmap atlases from stb_truetype for now (decided 12 November;
P2-02). No shaping means Latin text only, which is accepted for now. Any new shader permutation must be
added to `engineShaderRequests()` (`shaders/EngineShaders.cpp:104-105`), or exported
games fail with NotFound.

## R3 - Blocking editor operations

Project open and new destroy and recreate the `EditorApp` synchronously
(`editor/Main.cpp:228-283`). `asset.import`, `asset.refresh`, `asset.moveDirectory`,
`project.export` and `editor.screenshot` all run inline between frames. No progress UI
or cancellation exists anywhere. The October freeze was a COM deadlock, now fixed, but a
large project will look frozen for a different reason.

**Mitigation:** Phase 1 tickets P1-03 and P1-04. Measure first with the existing perf
harness (`coldStart`, `assetIndex`, `assetLoad` in `tests/perf`).

## R4 - DPI and framebuffer changes

`Window.cpp` registers no content-scale or framebuffer-size callback. The ImGui scale is
read once, at creation (`UiSystem.cpp:68`). A window moved to a monitor with another
scale keeps the old UI scale. A resolution-independent runtime UI needs these events.

**Mitigation:** add both callbacks and an event that the UI and the editor react to,
early in Phase 2 (P2-04).

## R5 - Untested platforms and backends

- CI renders only on Linux lavapipe, and did not build on Linux at all from the first
  commit until 12 November: the GPU suites had never run there. 17 cases fail on lavapipe
  and are listed as expected failures (P1-13). Windows CI excludes the `gpu` label. macOS
  CI runs only on demand and without GPU tests.
- No CTest runs the GPU suite on D3D12. AGENTS.md lists 14 failures there in prose, with
  no skip list. The [baseline](BASELINE.md) records what this audit measured.
- MoltenVK and the macOS bundle have never run.
- clang-tidy and clang-cl run only locally.

**Mitigation:** keep Windows-first, as the plan's default says. Add a D3D12/WARP CTest
entry with an explicit expected-failure list, so a regression becomes visible (P1-08).
Move clang-tidy into CI when a runner budget allows. Do not claim macOS support until
someone runs it.

## R6 - Mandatory runtime parts

`engine` depends on `scripting`, `physics`, `audio` and `ui`. `Engine.h` includes
`ScriptSystem.h` and `UiSystem.h`. Every exported game creates a Lua runtime and an ImGui
context, and pays for ImGui's GLFW backend, which changes the cursor and enables keyboard
navigation even though nothing draws.

This conflicts with plan section 4.2 ("No required ... Lua runtime for C++-only
projects"). **Mitigation:** do not refactor now. Make the ImGui context in games optional
when the runtime UI lands (P2-09). Split scripting out of `engine` only when C# work
(Phase 7) or the package system (Phase 5) needs it, and record that in an ADR.

## R7 - NVRHI as public API

31 of 55 renderer headers, the `ui` headers and `Engine.h` expose `nvrhi::` types.
`GraphicsDevice.h` declares `vulkan()`. This is not a problem for Vulkan and D3D12,
which NVRHI covers. NVRHI has no OpenGL backend: the pinned source has D3D11, D3D12 and
Vulkan only. OpenGL is now required, for Linux without Vulkan (decided 12 November).

**Mitigation:** keep NVRHI as the RHI contract (plan section 7.2: "avoid an abstraction
that merely mirrors one API"), and implement OpenGL 4.5 as a Cordite backend of NVRHI's
interfaces (tickets GL-01 to GL-08). Then no pass changes. Write new runtime UI code
against the same NVRHI surface the renderer uses, so it runs on GL too. The size of
the work is the residual risk. NVRHI's D3D11 backend, the closest model, is about 4,700
lines. Ask the owner before taking on an upgrade of the pinned NVRHI that changes its
interfaces, because every change there now lands in two places.

## R17 - Shader bindings on OpenGL

The engine binds HLSL registers with NVRHI's offsets, and keeps textures and samplers
apart. OpenGL's GLSL combines samplers with textures and has separate binding
namespaces. If Slang's GLSL output cannot be given a deterministic binding map for the
engine's real shaders, option A fails or the shaders need changes.

**Mitigation:** spike GL-02 comes before any backend code, and ends with a go or no-go
report.

## R8 - Device loss

Nothing handles `VK_ERROR_DEVICE_LOST` or `DXGI_ERROR_DEVICE_REMOVED`. Errors become
`DeviceError` and end the frame loop. An editor that loses its device loses unsaved
work. **Mitigation:** in Phase 1, at minimum make sure the editor reports the error and
offers to save the scene (the world is CPU-side) before exiting (P1-07). Full device
recreation can wait.

## R9 - Synchronous loading, unused job system

`AssetManager` is synchronous and not thread-safe (`AssetManager.h:24`). There are no
mutexes or atomics in assets, world or animation. The enkiTS job system is tested but
unused. Physics runs single-threaded by design, for determinism.

**Mitigation:** Phase 5. Design async loading around explicit handles and main-thread
GPU upload. Do not retrofit locks into `AssetManager` piecemeal.

## R10 - Command routing, flags and confinement

71 commands are registered with a machine-readable schema. But:

- The Edit menu, hierarchy, inspector and gizmo call `EditorActions` directly, not
  through the registry.
- No command carries a read-only flag or a caller identity, and nothing writes an audit
  log.
- `scene.save`, `scene.open`, `editor.screenshot`, `asset.preview`, the `asset.import`
  sources and `project.*` accept any path.
- Commands must run on the main thread between frames. The console queue logs results
  but does not return them.

**Mitigation:** Phase 6. Add `readOnly` and caller fields to `CommandDesc` first; they
are cheap and testable. Confine paths at the engine side, not in an adapter.

## R11 - Lua-bound script API

About 98 functions in seven `Script*Api.cpp` files are `int(*)(lua_State*)`.
`ScriptSystem.h` exposes `lua_State`. Only component access goes through neutral
reflection. **Mitigation:** Phase 7. A neutral API layer is a prerequisite for C#. Do
not start it before the C++ API is stable.

## R12 - Thin reflection

Reflection covers data members of fixed `PropertyType`s. It has no methods, maps or
lists of structs, no categories, display names or visibility conditions, and no global
type registry. Widget assets (Phase 2/3) need nested lists of slot structs. A details
panel needs categories. Data assets (Phase 4) need a registry. **Mitigation:** extend
reflection in the slice that first needs each feature, with tests. Do not add a second
reflection system (plan section 5.2).

## R13 - No migrations

`core/Document.h` refuses newer versions and has no upgrade path. Every format so far is
version 1. The first breaking change to scenes, prefabs or widget assets needs a
migration mechanism. **Mitigation:** introduce `Document` migrations in the first slice
that changes a format. The widget asset format (P2-06) starts at version 1.

## R14 - Reference images tolerate small changes

AGENTS.md documents it: a reference image may differ in 0.2% of its pixels, so a new tab
or a line of text can pass unnoticed. **Mitigation:** UI tests must assert focus,
hierarchy and layout numerically, as `SceneRendererTests` do for pixels. Images should
supplement those checks, never replace them.

## R15 - Unheard audio, untested gamepads

No test opens a real audio device, and no person has listened to the showcase. Gamepads
are tested through injected state only. Both matter for the Phase 3 acceptance sample
(gamepad navigation, interface sounds). **Mitigation:** add a manual check-list item to
the Phase 3 exit, and a gamepad smoke test that does not need hardware in CI.

## R16 - Contradicting documents

See [CORDITE_AUDIT.md, Conflicts](CORDITE_AUDIT.md#conflicts-between-the-plan-and-the-repository).
