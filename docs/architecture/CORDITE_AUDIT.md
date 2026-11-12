# Cordite Phase 0 audit

**Date:** 12 November 2026
**Commit:** `4e03feb` on `main` ("Import FBX and OBJ models with ufbx"), one commit ahead
of `origin/main`
**Machine:** Windows 11 Pro (10.0.26200), MSVC through `scripts/dev.ps1`, AMD Radeon RX
9070 XT
**Scope:** Phase 0 of the [plan](../GAME_ENGINE_PLAN.md): a source- and test-based audit,
the baseline, and a proposed backlog for Phases 1 and 2. No engine code was changed.

**Where things are:** the project lives in `E:\GameEngine`. That is a clone of the
Cordite repository (`Bitrose-Studio/GameEngineExperiment`, full history, `main` at
`4e03feb`), with the plan at `docs/GAME_ENGINE_PLAN.md` and these documents in
`docs/architecture/`. Every source path in these documents, such as
`engine/cordite/ui/UiRenderer.cpp`, is relative to the repository root. The audit and
the baseline were taken in the original working copy at `E:\GameEngineExperiment`, at
the same commit. That copy is left as it was.

## Deliverables

| Document | Contents |
|----------|----------|
| [IMPLEMENTATION_MATRIX.md](IMPLEMENTATION_MATRIX.md) | Every subsystem labelled Verified / Partial / Planned / Broken / Unknown, with source and test evidence |
| [DEPENDENCY_AND_PLATFORM_MATRIX.md](DEPENDENCY_AND_PLATFORM_MATRIX.md) | Module graph, what can be left out, third-party libraries, platforms, backends, CI coverage |
| [TECHNICAL_RISKS.md](TECHNICAL_RISKS.md) | Sixteen risks with evidence, severity and mitigation |
| [BASELINE.md](BASELINE.md) | Exact commands, platform and results of the build and test gates |
| [PHASE_1_2_BACKLOG.md](PHASE_1_2_BACKLOG.md) | Prioritised tickets for Phase 1 (editor workflow) and Phase 2 (runtime UI foundation) |
| [GAME_ENGINE_PLAN.md](../GAME_ENGINE_PLAN.md) | The plan this audit answers |

## Method

1. Read `CLAUDE.md`, `AGENTS.md`, `README.md`, `AIGameEngine.md`, `docs/architecture.md`,
   `docs/third-party.md`, the `build-and-test` skill, the CMake build and the CI
   workflow.
2. Read the source and tests of every module, in five groups: core, math, platform,
   world, assets and animation; rhi, shaders and renderer; ImGui UI and engine; editor
   and automation; physics, audio and scripting. Each feature claimed by the README was
   checked in source and matched to a test case that exercises it. The key findings of
   each group were spot-checked a second time against the source.
3. Ran the documented gates on this machine. See [BASELINE.md](BASELINE.md).
4. Checked the two editor defects recorded in `AIGameEngine.md` against the code, the
   commits that fixed them, and their regression tests.

What this audit did **not** do:

- Run anything on Linux or macOS hardware.
- Click through the editor by hand.
- Listen to audio.
- Plug in a gamepad.

Those rows are labelled **Unknown** or carry a note.

## Findings in brief

**Baseline:**

- A clean configure, build and full test run in an empty tree passes: 27/27 CTest
  entries, GPU suites included.
- `verify` passes for format, MSVC (RelWithDebInfo, Release and Debug), clang-cl and
  tidy. The only failure was a link check, caught on this audit's own half-written
  documents; it passes now.
- On Direct3D 12, 468 of 482 GPU cases pass. The 14 that fail are exactly the ones
  AGENTS.md lists.

See [BASELINE.md](BASELINE.md).

**The foundation is real.** In every module, the features the README lists as done have
real implementations and real tests. The full test suite passes, GPU tests included.
Nothing is stubbed, and there are no `TODO` or `FIXME` markers. Test discipline is
unusually strong: renderer tests assert pixel values from the lighting model, reference
pages are generated and checked, module layering and document links are tested, and
performance has budgets. The plan's instruction to preserve rather than rebuild is
right.

**The two October editor defects are fixed and covered by tests that pass:**

- **File > Open project froze the editor.** This was a COM apartment deadlock with
  miniaudio. Commit `6f550ce` fixed it by showing the dialog from an STA thread
  (`runForSystemDialog`).
- **Assets could not be dropped on the viewport or the hierarchy.** The viewport took
  nothing and meshes were refused. Commit `b0d88f7` fixed it with `AssetDrop` and mesh
  placement.
- **Remaining gaps:** the pointer drag of a model or prefab, and a real dialog over a
  real window, are not tested. The Open project menu item is still offered with unsaved
  changes and is then refused in the console (backlog P1-01, P1-02).

**What the plan's priorities need does not exist yet:**

- **No game-facing UI of any kind.** The renderer has no 2D or text path apart from
  ImGui's.
- **Input cannot be consumed.** Gameplay actions update before anything else sees the
  frame, and there are no contexts or modifiers. A menu cannot block gameplay input
  today.
- **Editor operations block.** Project open, import, refresh, move and export run
  synchronously on the main thread, with no progress or cancellation. The job system is
  tested but has no users.
- **Lua, physics, audio and ImGui are mandatory** in every runtime. Every exported game
  creates an ImGui context that nothing draws into.
- **Direct3D 12 is offscreen only.** `Auto` always means Vulkan, so a machine without
  Vulkan 1.3 cannot run the editor or a game. There is no OpenGL, by a recorded design
  decision.
- **GPU tests run only on this machine** (Windows) and on Linux lavapipe in CI. No CTest
  runs the D3D12 suite. macOS has never been run.

## Reuse, refactor or replace

| Subsystem | Recommendation | Reason |
|-----------|---------------|--------|
| Core, math, world, assets (data), animation, physics, audio, renderer | **Reuse** | Verified and well tested. Extend in place |
| Shader pipeline and exported-game shader cache | **Reuse** | New UI shaders only need registering in `engineShaderRequests()` |
| `rhi` with NVRHI as the contract | **Reuse** | Do not add a second abstraction over NVRHI (plan section 7.2). Record that OpenGL is not reachable through NVRHI (R7) |
| `InputMap` / `InputActions` | **Refactor (extend)** | Add contexts, priorities, consumption and modifiers in front of the existing actions. Keep the file format (P2-03) |
| `ui/UiRenderer` (ImGui backend) | **Refactor** | Extract a neutral batched 2D renderer and keep ImGui as one client. The existing reference images guard the change (P2-01) |
| ImGui fonts and themes | **Reuse partially** | The embedded font bytes and the palette values are ImGui-free. The atlas and the style application stay editor-only |
| ImGui as game UI | **Replace** | A new retained runtime UI module (P2-00, P2-05). ImGui remains for the editor and optional debug overlays |
| Editor panels, actions, undo, commands | **Reuse** | Add confirmations, progress, recent projects and asset operations (Phase 1) |
| Editor command registry | **Reuse, extend later** | It is a good base for the automation API and MCP. Missing: read-only flags, caller identity, an audit log, path confinement for every command, and a route through the registry for every UI action (Phase 6, R10) |
| Prefabs (copy on placement) | **Refactor (by ADR)** | Linked instances with overrides replace a recorded design decision, so they need an ADR and a scene migration (P1-12) |
| Asset manager (synchronous) | **Refactor in Phase 5** | Async handles and cancellation. Do not add locks piecemeal (R9) |
| Lua scripting | **Reuse, unchanged** | Optional at the project level, mandatory at link level. A neutral binding layer is Phase 7 work (R11) |
| Reflection | **Extend** | Lists of structs, categories, defaults and a type registry, as the first consumers need them (R12) |

## Conflicts between the plan and the repository

| # | Plan says | Repository says | Proposal |
|---|-----------|-----------------|----------|
| C1 | OpenGL is not an early deliverable (open decision 3). D3D12 becomes a shipping backend only if Windows support warrants it (decision 4) | `AIGameEngine.md` notes: "We need OpenGL support as fall back if no Vulkan available". `docs/architecture.md` "Direct3D 12 beside Vulkan" answers that note with D3D12, because NVRHI has no OpenGL backend. D3D12 cannot help on Linux | **Decided 12 November: OpenGL is required, as the fallback on Linux when there is no Vulkan.** That overrides both the plan's default and the repository's D3D12-only answer. The approach is in [OpenGL fallback](#opengl-fallback-decided) and tickets GL-01 to GL-08 |
| C2 | No required Lua runtime for C++-only projects (4.2) | `engine` links `scripting` unconditionally | Accept for now. Revisit with Phase 5 packages or Phase 7 C#. Record it in the ADR log |
| C3 | Platform support must be demonstrated before it is claimed (4.3) | `README.md` and `AGENTS.md` open with "for Windows, macOS and Linux". Milestone 12, "standalone runtime builds for Windows, macOS, and Linux" in `AIGameEngine.md`, is marked Done although the README itself says Linux and macOS are "not tried" | Reword the README status so Windows is verified, Linux is built and tested in CI on software Vulkan, and macOS is built only |
| C4 | `AIGameEngine.md`'s old milestones are partly superseded | `README.md` says "The full product brief is AIGameEngine.md". `AGENTS.md` points to it as the product vision | Point both at the plan for direction, and keep `AIGameEngine.md` for the visual and rendering brief |
| C5 | The plan describes the Open Project freeze and drag/drop as possibly current | Both are fixed (commits `6f550ce`, `b0d88f7`) and tested, but `AIGameEngine.md` still lists them as open notes | Mark them resolved in `AIGameEngine.md` with the commit ids |
| C6 | Proposed module name `ui_runtime` (5.1) | The ImGui module is already called `ui`. Two modules called `ui` and `ui_runtime` invite confusion | Decide in ADR P2-00. The backlog proposes `gui` |
| C7 | MCP must be optional; one shared automation API (10.1) | `CommandRegistry.h` says menus, scripts and agents "all go through the same registry". In fact the Edit menu, hierarchy, inspector and gizmo call `EditorActions` directly | Correct the comment now. Route the UI through the registry in Phase 6 if that reduces duplication |
| C8 | `third-party.md` says ImGui is used for "in-game debug overlays" | Games create an ImGui context but draw nothing into it | Reword, or make the overlay real when P2-09 makes ImGui optional in games |
| C9 | The plan lived outside the repository | Cordite's AGENTS.md hard rule 1: "Stay inside the repository" | **Resolved 12 November (D1, D5):** the repository was cloned into `E:\GameEngine`, where the plan already was, so the plan and the code are now in one repository |

### Smallest doc updates

Proposed. None of them has been applied: they change project-facing documents and should
be the owner's call (backlog P1-11).

1. `README.md`, Status:
   - Add a line linking the plan and this audit as the current roadmap.
   - Reword milestone 12 and the opening sentence as in C3.
2. `AGENTS.md`, first paragraph: "The product vision and feature list are in
   AIGameEngine.md" becomes "The visual and rendering brief is in AIGameEngine.md, and the
   current roadmap is docs/GAME_ENGINE_PLAN.md, and its audit is in docs/architecture/."
3. `AIGameEngine.md`, Note section:
   - Append "(fixed in 6f550ce)" to the Open Project note, and "(fixed in b0d88f7)" to
     the drag-and-drop note.
   - Append "(decided 12 November: OpenGL backend for Linux without Vulkan; Direct3D 12
     stays the Windows path)" to the OpenGL note.
   - Update the design decision "Direct3D 12 beside Vulkan" in `docs/architecture.md`
     in the same change as the GL-01 ADR.
4. `docs/third-party.md`, Dear ImGui row: "the editor and in-game debug overlays" becomes
   "the editor's interface".
5. Header comments that the audit found stale:
   - `engine/cordite/editor/AssetBrowserPanel.h:12-15`: meshes can be dropped onto the
     hierarchy too.
   - `AssetCommands.h:6-14`: its table omits `asset.import`.
   - `EditorApp.h:38-44`: the list of application commands omits `project.*`,
     `asset.listen`, `asset.preview`, `asset.move`, `asset.moveDirectory`,
     `probes.capture` and `editor.theme`.
   - `CommandRegistry.h:7`: C7.

## Owner decisions

### Decided on 12 November 2026

| # | Question | Decision | Consequence |
|---|----------|----------|-------------|
| D1 | Where does the project live? | **`E:\GameEngine`**, not the Cordite repository's folder | These documents moved here, and the original working copy is back to its commit |
| D5 | Where does the code live? | **Cordite is copied into `E:\GameEngine`** with its history, and all future work happens here | `E:\GameEngine` is a git clone on `main` (`4e03feb`), tracking the same GitHub remote. `E:\GameEngineExperiment` stays untouched as the original |
| D2 | Fallback when there is no Vulkan | **OpenGL, because of Linux.** Overrides the plan's default for open decision 3 | A Cordite OpenGL backend behind NVRHI's interfaces; see [OpenGL fallback](#opengl-fallback-decided) and tickets GL-01 to GL-08. Direct3D 12 remains the Windows path |
| D3 | Text in the first runtime UI | **Bitmap fonts for now** | P2-02 uses stb_truetype atlases, which are already pinned, behind a `FontAtlas` interface. No new dependency. Shaping and MSDF come later, if ever |
| D4 | Prefabs | **As the plan says** | Phase 1 (P1-12): linked instances, overrides, apply and revert, nested prefabs, override display. Phase 4: variants |

### Still open

1. **Runtime UI module name (C6, P2-00).** The proposal is `gui`.
2. **Git remote.** `E:\GameEngine` tracks the same GitHub repository as the original. If
   the project is to have a repository of its own, set it before the first push.
3. **Branding.** Keep "Cordite" (plan default). Nothing in the audit argues otherwise.

## OpenGL fallback (decided)

**Goal:** on Linux, when no Vulkan 1.3 device can be made, the editor and games start on
OpenGL instead of failing. Windows keeps Vulkan with Direct3D 12 behind it.

**Constraint found in the audit:** NVRHI, the engine's RHI in practice, has backends for
Direct3D 11, Direct3D 12 and Vulkan only. That was checked in the pinned source:
`nvrhi-src/src` holds `d3d11`, `d3d12`, `vulkan`, `validation` and `common`. Every
renderer pass and the ImGui renderer are written against `nvrhi::` (R7).

| Option | Verdict |
|--------|---------|
| **A. A Cordite OpenGL 4.5 backend that implements NVRHI's interfaces** (`nvrhi::IDevice`, `ICommandList` and the resources) inside the `rhi` module, as `rhi/opengl/` | **Recommended.** Every pass, the ImGui renderer and the future runtime UI work unchanged. NVRHI's validation layer wraps any `IDevice`, so the GL backend is checked by the same validation as the others. NVRHI's own D3D11 backend (immediate context, implicit hazards) is the closest model to follow |
| B. A new Cordite RHI over Vulkan, D3D12 and GL, with the renderer ported to it | Rejected. It rewrites every pass and 31+ public headers, and the plan warns against an abstraction that mirrors one API |
| C. Software Vulkan (Mesa lavapipe) when no GPU Vulkan exists | Not OpenGL and CPU-only, but cheap: `Auto` could choose it already. A stopgap while A is built, not a substitute |
| D. ANGLE or another translation layer | Rejected. A large dependency, GLES feature limits, and it still needs an NVRHI backend |

**Shape of option A:**

- **Version: OpenGL 4.5 core.** It has direct state access, compute shaders, cube-map
  arrays, and `glClipControl`. The last one matters: Cordite renders with D3D-style
  clip space and reversed depth, and without `glClipControl` every projection would
  need a second convention.
- **Shaders: Slang to GLSL 4.50** (the pinned Slang has a GLSL target). Use GLSL rather
  than SPIR-V through `ARB_gl_spirv`, which needs GL 4.6 and has weaker driver support
  on Linux. `ShaderTarget` gains a GLSL entry. The shader cache and the exported-game
  cache store GLSL text, which the driver compiles when the game loads.
- **Biggest technical risk: bindings.** The engine binds HLSL registers with NVRHI's
  offsets, and keeps textures and samplers separate. GL GLSL has combined samplers and
  its own binding points for uniform buffers, textures, images and storage buffers.
  The GL-02 spike must prove a deterministic mapping, and how Slang combines textures
  and samplers for this target, on the engine's real shaders, before anything else is
  built.
- **Window: GLFW makes a GL context** when GL is chosen (today it always asks for
  `GLFW_NO_API`). Presenting is `glfwSwapBuffers`, and vsync is the swap interval.
- **Selection:** `GraphicsBackend::Auto` tries Vulkan and then, on Linux, OpenGL. Every
  backend can be forced with `CORDITE_GRAPHICS` and a command-line option, so it can be
  tested anywhere.
- **Testing:**
  - The GL backend also runs on Windows drivers, so it can be developed and run under
    the GPU suite on the development machine.
  - CI's Linux job already has Mesa, whose llvmpipe implements OpenGL 4.5, so the suite
    can run on GL there as it does on lavapipe.
  - Known failures go into the same checked list as Direct3D 12's (P1-08).

**Place in the plan:** the plan puts OpenGL in Phase 8. This decision brings it forward,
as a track beside Phases 1 and 2. It touches only `rhi`, `platform` (the GL context),
`shaders` and `shader_compiler`, so it does not block the editor or UI work, and the
runtime UI gets GL for free because it renders through NVRHI. The tickets are in
[PHASE_1_2_BACKLOG.md](PHASE_1_2_BACKLOG.md#opengl-track-decided-12-november). Expect a
large piece of work. In the pinned NVRHI, the D3D11 backend (the nearest equivalent) is
about 4,700 lines, against about 11,400 for Vulkan and 12,500 for D3D12.
