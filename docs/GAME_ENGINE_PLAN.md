# Cordite Evolution — Research & Development Plan for Claude

**Prepared:** 11 November 2026  
**Repository:** [Bitrose-Studio/GameEngineExperiment](https://github.com/Bitrose-Studio/GameEngineExperiment)  
**Current engine name:** Cordite (confirm branding before renaming)  
**Status:** Repository-informed product and technical roadmap  
**Primary language:** Native C++20  
**Existing renderer:** Vulkan through NVRHI  

> **Instructions for Claude:** This is an evolution of an existing engine, not a greenfield engine. Start by reading `CLAUDE.md`, `AGENTS.md`, `AIGameEngine.md`, `README.md`, `docs/architecture.md`, the relevant `.claude/skills`, and the source/tests for the subsystem you touch. Preserve working code. Do not rebuild a subsystem merely because this plan describes it. Verify current implementation in source and tests before marking anything complete or missing.

---

## 1. Executive direction

Evolve Cordite into a practical, modular, C++-first game engine that combines selected strengths of Unity and Unreal Engine. Preserve its working Vulkan renderer and the substantial engine/editor foundation already present. The differentiating product priorities are:

1. A high-quality game UI framework inspired by **UMG + CommonUI**, including a visual UI authoring workflow.
2. A clean, approachable editor and asset workflow inspired by the best parts of Unity and Unreal.
3. A modular architecture in which optional features do not become mandatory dependencies.
4. Native C++ as the authoritative language, with optional C# integration and Lua as a possible later scripting backend.
5. An engine automation interface that can be used by the editor, command line, tests and optional MCP adapters.
6. Preserve Vulkan and the current renderer; establish a renderer abstraction suitable for future Direct3D 12 and possibly OpenGL backends without prematurely implementing them.

**Do not turn this into a feature-count competition with Unity or Unreal.** Focus on a complete, coherent workflow for making and shipping a small game.

## 2. What the repository already contains

Repository checked: `main` branch of `Bitrose-Studio/GameEngineExperiment`. The following is based on the repository's README, project brief, `AGENTS.md`, CMake setup and architecture description. The source and tests must still be checked before any implementation decision; README status is not a substitute for code verification.

### 2.1 Existing foundation — treat as implemented until source audit disproves it

| Area | What the repository documents | Direction |
|---|---|---|
| Build and language | C++20, CMake 3.28+, presets, platform-specific developer scripts, clang-format/tidy, CI-oriented verification | Keep and improve incrementally |
| Core | Logging, assertions, `Result`, hashing, UUIDs, JSON, reflection, binary streams, file I/O, file watching, config, jobs, allocators, timing | Reuse; identify genuine gaps only |
| Platform and input | GLFW-based platform layer, keyboard/mouse/gamepad, named `InputMap` and `InputActions` | Reuse; evolve toward action/context/modifier/trigger input |
| World/ECS | EnTT-backed world, entity/component registry, hierarchy, paths, scenes and prefabs | Reuse; add gameplay abstractions without replacing ECS |
| Assets | Asset database, stable IDs and `.meta`, loading by ID, glTF import/export, images, meshes, skeletons, clips, materials, scripts, sounds, prefabs, sequences and render targets | Reuse; extend pipeline, variants and async workflows |
| Animation | Skinned glTF assets, skeletons, clips, pose sampling/blending, Animator and AnimationSystem; camera shake and sequences | Reuse; roadmap state machines, blend spaces, IK, root motion and layered animation after audit |
| Physics | Jolt Physics integration, rigid bodies, colliders, joints, character controller, queries and collision/trigger events | Reuse; improve authoring and validation |
| Audio | miniaudio integration, AudioSource/Listener, spatial audio, buses and in-memory/device paths | Reuse; test actual device playback and improve authoring |
| Rendering | Vulkan through NVRHI, Slang shader path, deferred G-buffer, forward translucency, clustered lights, multiple light types, shadows, IBL, probes, AO, SSR, fog, decals, LOD, scene capture, motion blur, depth of field, light shafts, camera effects, debug views and post-processing | Preserve. Do not restart renderer work |
| UI currently documented | Dear ImGui context and NVRHI renderer, fonts/icons/themes, used for editor tooling | Keep for editor/debug tooling; it is **not** the desired full game UI framework |
| Editor | Viewport, hierarchy, inspector, asset browser, command/action model, undo/redo, gizmos, play/pause/step/stop, themes, multiple viewports, asset operations and editor automation commands | Reuse; improve workflow and UX based on tests/feedback |
| Scripting | Sandboxed Lua 5.4 runtime, Script component/system, lifecycle callbacks, reflection-based component access, input, physics, audio, timers/tasks, scene loading, prefab instantiation and self-test coverage | Preserve as existing optional scripting path; do not make it mandatory |
| Runtime/export | Separate `cordite-player`, exported game without editor, precompiled shader cache, project settings and platform-specific layouts | Reuse; verify packaging on each target platform |
| Testing | doctest-based unit/integration/render/performance test layout, showcase project, headless editor commands, test scripts and verification scripts | Preserve and extend |
| Automation | Editor command system and commands for scenes, assets, play/export, script API, viewport and other editor actions | Treat as foundation for shared Automation API and MCP |

### 2.2 Important repository observations

- `README.md` documents Milestones 1–12 as complete, including the engine foundation, renderer, ECS, physics/gameplay foundations, editor and packaging. It also clearly records some limitations, especially animation feature depth, platform verification and audio-device validation.
- `AGENTS.md` defines a strong modular architecture under `engine/cordite/<module>/`, with static libraries and explicit dependencies. Respect this structure; do not impose a second architecture beside it.
- The module map already includes `core`, `math`, `platform`, `assets`, `world`, `animation`, `physics`, `audio`, `scripting`, `rhi`, `shaders`, `shader_compiler`, `renderer`, `ui`, `engine` and `editor`.
- The current `ui` module is explicitly an **immediate-mode Dear ImGui interface**. It powers editor panels and themes. The new runtime UI must be designed as a separate retained/hybrid widget framework; do not confuse the editor UI with game-facing UMG/CommonUI functionality.
- `AIGameEngine.md` still contains older greenfield milestones and says Lua scripting is being considered. Those parts are now partly superseded by the actual repository. Update docs deliberately so the plan and the codebase do not contradict each other.
- The root currently contains `AIGameEngine.md`, `AGENTS.md`, `CLAUDE.md`, `README.md`, `coding-standards.md`, CMake configuration, `engine/`, `editor/`, `player/`, `tools/`, `samples/`, `tests/`, `shaders/`, `scripts/`, and `docs/`.

### 2.3 Known limitations and issues to explicitly re-check

The README/brief describes or implies these as incomplete, limited or requiring verification:

- Animation: blend more than two clips, layers, blend trees, state machines, root motion, sockets/attachments, morph targets, IK and animation events.
- UI: no full game-facing retained widget framework and visual UMG-style UI designer is documented. Dear ImGui should remain a developer/editor tool, not be the runtime UI solution.
- Audio: automated mixing is documented, but real output-device playback needs real listening/validation and tests.
- Graphics APIs: Vulkan is the primary backend. The architecture describes Direct3D 12 as offscreen-only so far; OpenGL is not documented as implemented. Re-check current source before claiming either backend is production-ready.
- Platform verification: Linux/macOS build/run/export support needs actual CI and hardware validation, not merely compiling shared code. The repository notes macOS bundle execution has not been verified.
- Editor workflow: older notes mention an Open Project freeze and asset-browser drag/drop not working. Verify whether these are still current, reproduce them, and create regression tests before changing the relevant code.
- Project/asset workflow: re-check async loading, cancellation, import feedback, error handling, drag-and-drop, recovery, scalability and source-control behaviour.

Use status labels **Verified implemented**, **Partially implemented**, **Planned**, **Broken/regression**, **Unknown**. Never infer functionality solely from the presence of a type, header or command name.

---

## 3. Research synthesis: what to borrow from Unity and Unreal

### 3.1 Evidence and limitations

There is no single representative survey that ranks every feature by developer satisfaction. Recommendations combine developer surveys, official documentation and practical workflow comparisons. Survey populations and methodology differ; treat results as directional, not as universal preference.

- The 2026 GDC State of the Game Industry report says 42% of surveyed developers primarily use Unreal and 30% primarily use Unity. It surveyed more than 2,300 industry professionals. [R1]
- JetBrains' 2025 State of Game Development survey reports C# and C++ among the leading languages used by respondents (40% and 29%, respectively, in the reported language measure). This supports offering both, but does not imply equal runtime priority. [R2]
- Unity's 2026 Game Development Report (Cint survey of 300 respondents) reports frequent engine/editor connectivity among studios using MCP; the study cites faster iteration and fewer errors among reported benefits. This supports optional integrations, not mandatory AI infrastructure. [R3]

### 3.2 Unity-inspired workflows

| Pattern | Adopt / adapt |
|---|---|
| GameObject/component composition | Keep Cordite's existing ECS; provide friendly Actor/GameObject-like editor concepts where useful without a second runtime object model |
| Prefabs and variants | Upgrade existing prefabs with nested instances, inheritance/variants, override inspection and safe apply/revert workflows |
| Package Manager | Versioned plugins/modules with manifests, dependencies, platform support, licences, enable/disable and lockfiles |
| C# accessibility | Optional C# gameplay/editor extension layer after native reflection and ABI boundaries stabilise |
| Asset import/reimport | Improve import settings, source tracking, deterministic reimport, dependency invalidation and clear diagnostics |
| Project templates | New project templates, sample projects, feature packages and migration tooling |
| Profiler and iteration | Integrated CPU/GPU/memory/assets/scripts profiling and fast Play-in-Editor iteration |
| UI Toolkit concepts | Retained widget tree, reusable templates, stylesheets, visual builder and debugger—combined with CommonUI interaction behaviour |

### 3.3 Unreal-inspired workflows

| Pattern | Adopt / adapt |
|---|---|
| UMG | Main reference for visual, game-facing widget and layout authoring |
| CommonUI | Layered UI, activatable widget stacks, input routing, focus, controller navigation and action hints |
| Gameplay Framework | Compact optional framework for GameInstance, GameMode, GameState, PlayerState, Controller and Pawn concepts, built on existing world/ECS |
| Enhanced Input | Actions, contexts, modifiers, triggers, runtime context priority and remapping |
| GAS | Optional gameplay module for attributes, tags, effects, abilities, cues and asynchronous ability tasks; do not put in core |
| Data assets | Strong reflected, serializable data assets for reusable gameplay configuration |
| Asset Manager | Async loading, bundles, labels, auditing and dependency-aware packaging |
| World Partition | Future scalable world streaming, cells, data layers and editor streaming visualisation |
| Blueprint philosophy | Future optional visual scripting for designers; keep native C++ as the foundation and avoid forcing graphs on programmers |
| Sequencer | Evolve existing sequence support into a timeline editor for transforms, properties, events and cameras |

### 3.4 Product synthesis

Use Unity's modularity and friction-reducing iteration practices; use Unreal's integrated authoring and gameplay patterns where they demonstrably improve workflow. Prefer Cordit's current implementation over importing either engine's conceptual complexity wholesale.

---

## 4. Product principles and scope

### 4.1 Developer experience

- A new user can create a project, import assets, create a scene, make a UI, play, debug and export without reading engine source.
- Common operations are discoverable in the editor and also scriptable through the same command surface.
- Defaults are sensible; advanced users retain control.
- Errors identify the operation, asset/path/property and a practical next step.
- Iteration time is measured and improved, especially import, project open, Play-in-Editor, build and UI preview.

### 4.2 Optionality

- No required cloud account or online activation for local development.
- No required AI account, MCP server or MCP client.
- No required C# or Lua runtime for C++-only projects.
- Optional modules are opt-in and must not enlarge or compromise the minimal runtime unnecessarily.
- No mandatory asset marketplace or proprietary project format lock-in.

### 4.3 Platform and game scope

Default assumption: desktop-first 3D engine. Keep architecture portable across Windows, Linux and macOS; validate actual support continuously. Do not promise platform support solely because an API abstraction exists. Console, mobile and full 2D authoring can be considered after the initial desktop vertical slice.

---

## 5. Target architecture — evolve the existing structure

Do not replace Cordit's modular static-library architecture. The repository already has a sensible dependency graph and module-layering test. Extend it only where a concrete requirement needs it.

```text
External workflows
  Native C++ | Optional C# | Existing optional Lua | Optional MCP client
                  |
Editor / CLI / Test Runner / Optional MCP Adapter
                  |
Shared validated Editor Command + Automation API
                  |
Game & Content Framework
  World/ECS | Gameplay Framework | Input | Runtime UI | Assets
  Animation | Physics | Audio | Optional GAS-like module
                  |
Engine Runtime Modules
  Core | Reflection | Serialization | Jobs | Platform | Events
                  |
Renderer Hardware Interface
  Existing Vulkan/NVRHI | Future D3D12 | Possible OpenGL
                  |
Operating System + GPU
```

### 5.1 Existing modules

Keep current modules and boundaries unless audit proves a structural issue. Likely extensions include:

- `ui_runtime`: retained widget tree, layout, style, input/focus, activation, accessibility, UI rendering.
- `ui_editor` or editor panels in current `editor`: visual UI designer, hierarchy, widget inspector, preview/debugger.
- `automation`: shared command schemas and validation extracted from current editor command system only if a separate module is justified.
- `mcp_adapter`: optional standalone process or plugin; not linked into runtime.
- `scripting_csharp`: optional managed runtime module only after ABI design and native C++ workflows are stable.

These names are proposals, not instructions to create empty folders. Follow existing naming conventions and CMake module-layering rules.

### 5.2 Reflection and serialization

Build on existing reflection and JSON/binary serialization. Extend only for real needs: UI widget properties, C# bindings, editor customisation, gameplay data assets and stable migration. Preserve the project's existing `Result`, ownership, module dependency and external input safety rules. Do not introduce another object/reflection system without a compelling ADR.

### 5.3 Gameplay framework

Build an optional higher-level gameplay layer atop ECS. Provide concepts similar to Unreal's GameMode/GameState/PlayerState/Controller/Pawn where useful, but avoid duplicate state ownership. It should support:

- Project/world startup and shutdown lifecycle.
- Player controller and possessed pawn concepts.
- Local player state and game/world state.
- Input contexts and gameplay tags.
- Events/delegates and data-driven gameplay configuration.
- Optional ability/effect framework as a plugin.

### 5.4 Data assets and prefab evolution

Cordite already has assets and prefabs. Improve them rather than reinventing them:

- Stable GUID identity independent of path.
- Typed data assets with reflected properties and validation.
- Prefab variants, nested prefabs, instance overrides and override visualisation.
- Dependency graph and reference auditing.
- Async loading handles, cancellation and explicit lifetimes.
- Deterministic import/reimport and versioned migration.
- Asset bundles/groups for startup, menus, levels, characters, UI and downloadable content.

---

## 6. Runtime UI framework — highest product priority

**The game UI must feel like a combination of UMG and CommonUI.** This is a product-defining system. The existing Dear ImGui module is appropriate for editor/debug tooling, but it is not a substitute for the game-facing runtime UI framework.

### 6.1 Design goals

- Retained or hybrid widget tree with deterministic ownership and lifecycle.
- Visual widget designer in the editor; support native C++ custom widgets.
- Reusable widget assets, templates, styles and data binding.
- Resolution-independent layout, anchors, constraints, safe zones and DPI scaling.
- Text rendering, localisation-ready text, rich text and font assets.
- Images, borders, panels, overlays, grids, stacks, scroll boxes, progress bars, buttons, checkboxes, sliders, lists and custom draw widgets.
- Animation/tweens for properties and transitions; event-driven interaction.
- Screen-space UI and separate world-space widgets.
- Keyboard, mouse, touch-ready abstractions and gamepad navigation.
- Performance diagnostics: layout invalidation, rebuild cost, draw batches and text cost.

### 6.2 UMG-inspired authoring

- Widget Blueprint equivalent: a serializable widget asset with hierarchy, properties, bindings and event hooks.
- Designer canvas with outline/hierarchy, preview viewport, details/inspector and palette.
- Parent-child layout, anchors, alignment, padding, sizing rules and slot properties.
- Reusable widget classes and nested widget assets.
- Styles/themes as assets; reusable typography and colour tokens.
- Data binding should be explicit and debuggable; avoid uncontrolled per-frame binding polling.
- Native C++ widgets must be first-class and usable without visual scripting.

### 6.3 CommonUI-inspired interaction model

- Multiple named UI layers/stacks: HUD, menus, modal dialogs, notifications and overlays.
- Activatable widgets with explicit activate/deactivate lifecycle.
- Central input router selects the active widget tree/context.
- Focus path management, focus restoration and explicit default focus.
- Gamepad directional navigation, back/cancel, accept and contextual action hints.
- Input action ownership must prevent gameplay input leaking through an active menu/modal.
- Nested modals and priority rules must be deterministic.
- Transitions must define when a widget is interactive and when it is removed.

### 6.4 UI architecture sketch

```text
UI Application / Viewport
  ├─ Layer Manager / Widget Stacks
  ├─ Widget Tree / Widget Lifecycle
  ├─ Layout + Slot System
  ├─ Style / Theme / Typography
  ├─ Input Router / Focus Manager / Navigation
  ├─ Animation + Transitions
  ├─ Binding / Events / Data Sources
  ├─ Accessibility + Localisation hooks
  └─ RHI UI Renderer / Text / Images
```

Keep the UI renderer behind the existing RHI. It must not call Vulkan directly. Reuse current font/icon/render infrastructure where appropriate, but ensure game UI rendering does not depend on ImGui internals.

### 6.5 UI MVP acceptance sample

Create a sample project with:

- Main menu, settings screen, pause menu, HUD, modal confirmation and notification toast.
- Buttons, sliders, toggles, scroll list, text, images and progress bars.
- Full keyboard/mouse and gamepad navigation.
- Back/cancel behaviour and focus restoration.
- Gameplay input blocked while a modal/menu is active and restored when closed.
- Widget asset reuse, nested widget and a style/theme asset.
- Save/load of widget asset and runtime instantiation.
- Visual designer preview and UI debugger showing hierarchy, focus and active layer.

No custom game renderer code should be required to build this sample.

---

## 7. Rendering strategy

### 7.1 Preserve the current renderer

The README documents a substantial Vulkan renderer through NVRHI. Do not create a new renderer or repeat completed rendering milestones. First audit renderer/RHI source, tests and backend boundaries. Preserve shader cache and exported-game shader compilation workflow.

### 7.2 RHI roadmap

- **Vulkan:** primary backend; preserve and improve existing implementation.
- **Direct3D 12:** future production backend. Existing docs describe D3D12 as offscreen-only so far; inspect exact coverage and tests. Do not claim swapchain/runtime parity until it is demonstrated.
- **OpenGL 4.5 Core:** compatibility backend only if there is a concrete platform/hardware requirement. It should not constrain modern RHI contracts. Note that OpenGL is not documented as implemented in the current README/architecture.
- **Metal:** consider only if native Apple support becomes a goal; MoltenVK may remain the practical path for Vulkan-first support.

RHI rules:
- Gameplay/editor public APIs must not expose backend-specific types.
- Define resource, command, synchronisation, swapchain and capability contracts according to real renderer needs.
- Use shared conformance tests and reference scenes for every backend.
- Feature capability must be queryable; no fake support flags.
- Avoid an abstraction that merely mirrors one API and makes all others awkward.

### 7.3 Graphics priorities

1. Reliability, validation, device-loss/error paths and cross-platform reproducibility.
2. Renderer profiling and performance budgets.
3. Material and render pipeline authoring quality.
4. GPU-driven/advanced rendering only when sample projects require it.

---

## 8. Languages and scripting

### 8.1 Native C++ — primary and complete path

- Engine and gameplay systems can be built fully in C++.
- Keep C++20 currently in use unless a deliberate standards upgrade is approved.
- Native game modules must have an approachable build, hot-reload/debug experience where practical and stable ABI boundaries where needed.
- No C# or Lua runtime is required for a C++ project.

### 8.2 C# — optional second language

C# is a strong candidate due to developer familiarity and ecosystem, but managed runtime hosting adds deployment, debugging, GC, hot reload, platform and ABI complexity. Introduce only after the C++ reflection/API boundary is stable.

Proposed design:
- Optional runtime package; not linked into core.
- C ABI or carefully controlled native interop boundary, not arbitrary direct access to engine internals.
- Generated bindings from existing reflection metadata where safe.
- Explicit lifetime rules for entities, assets and native objects.
- Debugging, exception reporting, reload and assembly dependency policy.
- Projects opt in and package the runtime only when used.

### 8.3 Lua — future/optional

Cordite already documents Lua 5.4 scripting. Keep that implementation and its current use cases; do not remove it merely because the initial preference is C++. Do not expand Lua into a competing mandatory language. Later decide whether it remains useful for lightweight scripting/modding, and make the choice based on actual project needs.

### 8.4 One engine API, multiple language bindings

The canonical engine semantics should be language-neutral. Native C++, C# and Lua adapters must not invent separate entity, component, input, UI or asset semantics. Use shared API definitions/reflection metadata where possible and test parity for supported operations.

---

## 9. Editor and content workflow

The current editor already has a viewport, hierarchy, inspector, content browser, gizmos, undo/redo, Play-in-Editor, themes and automation. The goal is to refine it, not replace it.

Priorities:
1. Fix and regression-test any still-existing project-open freeze and asset drag/drop defects recorded in the old brief.
2. Improve project creation/opening, recent projects, templates, path pickers and diagnostics.
3. Asset browser: tree + grid/list, filtering, previews, drag/drop, dependency views, import/reimport and clear progress/error reporting.
4. Prefab workflow: variants, nested instances, overrides, apply/revert and diff visualisation.
5. Details/inspector: searchable categories, multi-edit, reset-to-default and validation messages.
6. Scene editing: stable selection, gizmos, snapping, multi-select, grouping and undo semantics.
7. Play-in-Editor: state isolation, pause/step, runtime inspection and reliable restore.
8. UI Designer and UI debugger as part of editor, not a separate external app.
9. Integrated profiler, trace viewer, logs, test runner and build/export status.
10. Package manager and project feature selection when the module/package model is ready.

Avoid adding editor features without tests for keyboard routing, docking/panels, lifecycle, file operations and undo.

---

## 10. MCP and automation — optional by design

MCP must be supported without making MCP mandatory. A developer must be able to install the engine, create/edit/test/package a game entirely without AI software or an MCP server.

### 10.1 Shared automation layer

The current editor command system is the logical starting point. Audit and evolve it into a documented, validated Automation API only if this reduces duplication. The same operations should be available through:

- Editor UI and shortcuts.
- CLI/headless execution.
- Automated tests.
- Optional MCP adapter.

Do not create a privileged AI-only API. All routes must call the same validation, permission, path-sandboxing and command implementation.

### 10.2 Optional MCP adapter

- Separate process or optional plugin; never a core runtime dependency.
- Disabled by default and enabled per user/project.
- Prefer local STDIO transport initially; add other transports only when needed.
- Read-only baseline; write operations require explicit configuration/consent and scoped permissions.
- Per-project path boundary, no arbitrary shell execution, no unrestricted file-system access and no implicit network access.
- Audit log for operations, source (UI/CLI/MCP), arguments and result.
- MCP disconnection/crash must not impact engine/editor operation.
- Validate all input on the engine side, not just in the adapter.

### 10.3 Initial MCP tool categories

Read-only first:
- Inspect project, modules, assets, scenes and entity/component data.
- Query asset dependencies and import/build/test status.
- Read logs and performance counters.
- Inspect UI widget tree, active layer and focus state.

Scoped write tools after safeguards:
- Create/update entities and components.
- Import assets and instantiate prefabs.
- Create/edit UI widget assets.
- Run scene validation, tests and builds.
- Export a project after explicit permission.

Follow the current MCP specification and maintained SDK guidance at implementation time. Protocol specifics must not be frozen from an old draft.

---

## 11. Plugins and packages

Cordite's explicit module layering is a strong base. Build a user-facing package system over it, not a replacement for it.

A package manifest should eventually describe:
- Unique package id, display name, version and engine compatibility.
- Runtime/editor module entry points.
- Dependencies and version constraints.
- Platform/architecture support.
- Optional feature flags and required capabilities.
- Licence and third-party notices.
- Load phase and safe enable/disable rules.

Initial package candidates: UI runtime/editor, gameplay framework, GAS-like abilities, C# integration, procedural tools and future networking. Avoid a marketplace until package installation, versioning and compatibility are reliable.

---

## 12. Development phases — revised for the actual Cordit state

Phases are dependency-ordered, not calendar promises. Work in small vertical slices and keep the existing codebase building throughout.

### Phase 0 — Source audit and baseline (first task)

1. Read repository guide/docs and current architecture.
2. Inspect source and tests for every module documented in `AGENTS.md` and `README.md`.
3. Verify actual Vulkan, D3D12 and platform support; inspect `rhi` and build/test coverage.
4. Audit current ImGui UI and identify what can be reused for a separate runtime UI renderer.
5. Inventory editor command/automation API and test harness.
6. Reproduce the old project-open freeze and asset drag/drop issues if still present.
7. Run the documented clean build, unit/integration tests, render tests where hardware allows, export smoke test and formatting/tidy gates.
8. Record real state and technical risks. Do not begin a broad rewrite.

Deliverables:
- `docs/architecture/CORDITE_AUDIT.md`
- `docs/architecture/IMPLEMENTATION_MATRIX.md`
- `docs/architecture/DEPENDENCY_AND_PLATFORM_MATRIX.md`
- `docs/architecture/TECHNICAL_RISKS.md`
- Baseline build/test results with exact commands and platform.

Exit: each subsystem is labelled Verified implemented / Partial / Planned / Broken / Unknown, with source/test evidence. Reuse/refactor/replace recommendations are documented.

### Phase 1 — Editor and content workflow polish

Focus on usability defects that block actual game creation. Address project open/create reliability, async operations and cancellation, drag/drop, asset import/reimport feedback, prefab workflows and editor regression tests. Maintain current editor architecture.

Exit: create/open project, import and place assets, edit scene, save, play and export complete reliably with automated tests for the previously reported defects.

### Phase 2 — Runtime UI foundation

Design and implement the retained/hybrid runtime UI model behind the RHI. Start with widget lifecycle/tree, layout, basic rendering/text, input/focus and serializable widget assets. Keep Dear ImGui for editor/debug panels.

Exit: a C++ project can create and display runtime widgets without ImGui as the UI runtime.

### Phase 3 — UMG-style UI designer and CommonUI interaction

Implement designer palette, hierarchy, details panel, preview, styles/themes, nested widgets, event hooks, layer stacks, activatable widgets, focus routing, gamepad navigation, action hints and UI debugger.

Exit: UI MVP acceptance sample passes with both keyboard/mouse and gamepad and has automated focus/input regression tests.

### Phase 4 — Gameplay and data workflow

Evolve existing input contexts, data assets, prefab variants and compact gameplay framework. Add gameplay tags and optional GAS-inspired module only where useful for the sample game. Improve animation state machines/blend trees/IK in separate slices based on audited gaps.

Exit: complete sample gameplay uses reusable data and components, with no duplicate entity model and no mandatory extra language.

### Phase 5 — Asset streaming, profiling and packages

Improve async asset loading/cancellation, bundles, memory reporting, deterministic import, dependency diagnostics and integrated CPU/GPU profiler. Design package manifests and enable/disable flow.

Exit: assets can be imported, referenced, asynchronously loaded/unloaded, audited and packaged with dependable diagnostics and budgets.

### Phase 6 — Automation API and optional MCP adapter

Extract/share current editor command API as appropriate; provide stable schemas and CLI/test integration. Add optional MCP adapter with read-only default, project sandbox and scoped writes.

Exit: MCP-disabled workflow is fully functional; optional MCP client can inspect and run permitted actions without bypassing validation.

### Phase 7 — Optional language/runtime expansion

Stabilise reflection and native interop. Prototype optional C# with lifecycle, error reporting, GC boundaries, packaging and debugging. Keep Lua optional and decide future role based on real use cases.

Exit: C++-only projects build with no managed dependency; projects opt into C# explicitly; supported language bindings share API semantics.

### Phase 8 — Additional backends and advanced systems

Based on platform demand: complete D3D12, consider OpenGL 4.5, enhance world streaming, PCG, networking, advanced animation and optional visual scripting. Do not develop all at once.

Exit: each backend/system has shared conformance tests, a working sample, performance baselines and documented limitations.

---

## 13. Minimum product demonstration

Create one small 3D sample project proving complete workflow using the existing engine and new runtime UI:

- Controllable character/pawn and camera.
- Static and skeletal mesh rendering where verified.
- A small interactable and data-driven gameplay object.
- Input contexts for gameplay and UI.
- Main menu, settings, pause menu, HUD, modal and notification built with the new UI system.
- Keyboard/mouse and gamepad navigation.
- Reusable prefab and variant/override.
- Import/reimport of a mesh, texture and audio asset.
- Scene save/load and Play-in-Editor.
- Automated validation/tests and diagnostics.
- Packaged standalone executable.
- Optional MCP tool call to inspect the project and run validation, with MCP disabled by default.

Do not require multiplayer, complete GAS, full Blueprint clone, all render backends, C# and Lua at once, or a marketplace for this demonstration.

---

## 14. Engineering standards and definition of done

Follow `AGENTS.md` and existing coding standards. Current repo rules include C++20, explicit module dependencies, no hidden cross-module dependencies, `Result`/`Status` error handling, no throwing across module boundaries, RAII, safe external input validation, tests for behaviour changes and code review before commit.

Feature done means:
1. Implemented and integrated, not just an interface/stub.
2. Covered by relevant unit/integration/render/UI tests.
3. Visible in a sample project where appropriate.
4. Handles invalid input and failures safely.
5. Has documented API, ownership/lifetime and known limitations.
6. Passes the relevant build, format, tidy, test and verification gates.
7. Does not break existing project loading or export compatibility without a migration.

For every substantial design decision, add an ADR under the repository's existing documentation structure (use actual existing docs layout; do not make a duplicate Docs/ tree solely to match this plan).

---

## 15. Key risks and mitigations

| Risk | Mitigation |
|---|---|
| Rebuilding existing systems | Audit source and tests; preserve proven systems; refactor only with evidence |
| UI effort balloons | Deliver a narrow but complete UI vertical slice first; expand based on sample needs |
| Confusing editor ImGui with game UI | Separate runtime widget model; ImGui remains editor/debug only |
| RHI over-abstraction | Derive contracts from existing NVRHI use and real D3D/OpenGL requirements; conformance tests |
| D3D/OpenGL become premature distractions | Vulkan remains the maintained primary backend until there is a real target requirement |
| C# delays native engine | Optional phase after native API stabilises; no dependency for C++ projects |
| Lua/C# duplicate semantics | One language-neutral engine API with adapters and parity tests |
| MCP adds attack surface | Optional, local, read-only default, sandboxed, scoped write permission and audit |
| Plugin compatibility problems | Versioned manifests, dependency graph, lockfiles and clear compatibility checks |
| Feature list outgrows resources | One playable vertical slice, explicit P0/P1/P2 and reject speculative modules |

---

## 16. Open decisions for the project owner

Claude should **not block Phase 0** while waiting for answers. Record these as open questions and use the defaults shown.

1. **Product identity:** Keep the name Cordite, rename it, or use a new product name while preserving Cordite as the codebase? Default: keep Cordite until a deliberate rename decision.
2. **Primary platform:** Windows first with Linux/macOS maintained, or simultaneous Windows/Linux from the next milestone? Default: Windows-first development, shared cross-platform CI where practical.
3. **OpenGL priority:** Is OpenGL a required fallback for a specific hardware/platform target, or merely a backend we want to leave room for? Default: not an early deliverable.
4. **Direct3D 12 priority:** Is the existing offscreen D3D12 path intended to become a shipping backend soon? Default: Vulkan first, then D3D12 if Windows support warrants it.
5. **UI authoring:** Visual designer only, declarative assets only, or both? Default: both, backed by one widget model; native C++ custom widgets supported.
6. **Visual scripting:** Should a Blueprint-like graph system be on the long-term roadmap? Default: optional and later; C++ remains first-class.
7. **C# scope:** Gameplay scripting only, or editor extensions/tools too? Default: gameplay first; editor extensions only after the host lifecycle is proven.
8. **Lua:** Keep existing Lua scripting as an optional capability, or eventually replace it with C#? Default: preserve existing Lua; do not expand/remove until use case is clear.
9. **Distribution/licensing:** Open source, source-available or private? Default: unchanged; keep third-party licence and attribution inventory accurate.
10. **2D:** Is a full 2D workflow required? Default: 3D-first, with basic 2D/UI support only where existing architecture makes it inexpensive.

---

## 17. Instructions for Claude

1. Read `CLAUDE.md` and `AGENTS.md` before touching code. Their project-specific rules override generic instructions here.
2. Work only in the repository and use its existing build/test scripts and pinned dependencies.
3. Audit actual code and tests; do not rely on this document or README alone as proof of implementation.
4. Do not restart completed milestones or rebuild the Vulkan renderer, ECS, physics, audio, asset database or editor foundation without clear evidence of a blocker.
5. Keep the current modular CMake/static-library architecture and obey module dependency tests.
6. Keep C++ authoritative. C#, Lua and MCP are optional.
7. Treat runtime UI as the first major new product subsystem. Do not ship ImGui-only game UI.
8. Work in small vertical slices, each buildable and tested.
9. Reproduce documented bugs before fixing; add regression tests.
10. Update documentation when implementation status changes. Distinguish verified, partial, experimental, planned and unsupported.
11. Review all changes, run the appropriate verification gates and do not commit untested code.
12. Do not create placeholder modules, empty APIs or speculative frameworks without a real implementation consumer.

### First task for Claude

**Do not immediately start implementing a new subsystem.** Produce the Phase 0 audit, based on source code and tests, then propose a prioritised ticket breakdown for Phase 1 (editor workflow defects) and Phase 2 (runtime UI foundation). Every ticket must include affected modules/files, dependencies, acceptance criteria and tests. Highlight any conflict between this plan and current `AIGameEngine.md`/`README.md`, and propose the smallest doc updates needed.

---

## 18. Research sources

Official documentation describes feature capabilities, not developer satisfaction. Survey figures are limited to their published samples and questions.

**[R1] GDC — 2026 State of the Game Industry**  
https://gdconf.com/article/gdc-2026-state-of-the-game-industry-reveals-impact-of-layoffs-generative-ai-and-more/  
Survey overview and primary engine usage.

**[R2] JetBrains — The State of Game Development 2025**  
https://lp.jetbrains.com/the-state-of-gamedev-2025/  
Engine usage by studio type and programming language usage.

**[R3] Unity — 2026 Unity Game Development Report**  
https://unity.com/resources/gaming-report  
Cint survey of 300 respondents; developer workflow and MCP adoption/connectivity.

**[R4] Unity — Packages and package management (Unity 6)**  
https://docs.unity.com/en-us/engine/6000.0/manual/packages-list

**[R5] Unity — UI Toolkit**  
https://docs.unity3d.com/6000.0/Documentation/Manual/UIElements.html

**[R6] Epic Games — Common UI Plugin for Advanced User Interfaces**  
https://dev.epicgames.com/documentation/unreal-engine/common-ui-plugin-for-advanced-user-interfaces-in-unreal-engine

**[R7] Epic Games — Design Guidelines for Using CommonUI**  
https://dev.epicgames.com/documentation/en-us/unreal-engine/design-guidelines-for-using-commonui-in-unreal-engine

**[R8] Epic Games — Gameplay Framework**  
https://dev.epicgames.com/documentation/unreal-engine/gameplay-framework-in-unreal-engine

**[R9] Epic Games — Enhanced Input**  
https://dev.epicgames.com/documentation/unreal-engine/enhanced-input-in-unreal-engine

**[R10] Epic Games — Gameplay Ability System**  
https://dev.epicgames.com/documentation/unreal-engine/gameplay-ability-system-for-unreal-engine

**[R11] Epic Games — Asset Management**  
https://dev.epicgames.com/documentation/unreal-engine/asset-management-in-unreal-engine

**[R12] Epic Games — World Partition**  
https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine

**[R13] Epic Games — Introduction to Blueprints**  
https://dev.epicgames.com/documentation/en-us/unreal-engine/introduction-to-blueprints-visual-scripting-in-unreal-engine

**[R14] Model Context Protocol — Core architecture**  
https://modelcontextprotocol.io/docs/learn/architecture

**[R15] Model Context Protocol — 2026-07-28 specification announcement**  
https://blog.modelcontextprotocol.io/posts/2026-07-28/

---

## Final recommendation

Cordite already has a substantial technical base. Preserve it. The highest-value next step is **not another renderer/ECS/physics foundation**; it is turning the existing engine into a pleasant game-development product. Start with a source-verified audit, fix workflow friction, then build the UMG/CommonUI-inspired runtime UI and designer. Keep C++ as the complete native path and let C#, Lua and MCP remain optional integrations.
