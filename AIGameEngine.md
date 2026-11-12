# AI Game Engine

We're building a production-grade, simple and straight-forward 3D Game Engine for Windows, macOS and Linux (Ubuntu 24+). This is professional-grade software that needs to be stable, fully-tested, and real-world deployable. No hacks, no shortcuts - solid as a rock.

## Project Vision

The engine should evoke the rendering language of **Unreal Engine 3 circa 2007**, especially the look established by *Gears of War*: dense normal-mapped environments, strong specular highlights, high-contrast lighting, deep shadows, bloom, and cinematic post-processing.

The goal is **not** to recreate UE3's codebase, APIs, assets, or exact visual output. It is to build a modern, original engine whose default rendering aesthetic and workflow feel reminiscent of that era.

### Visual direction

- Dark, grounded, industrial and military-inspired environments.
- High-contrast direct lighting: bright practical lights, sun shafts, and point lights against deep shadow.
- Detailed-looking surfaces through normal maps, specular maps, and material layering rather than extremely high-polygon geometry.
- Chunky, readable silhouettes and strong material response.
- Muted, desaturated base palette with controlled warm and cool light accents.
- Cinematic post-processing: bloom, tone mapping, vignette, color grading, depth of field, and optional film grain.
- Soft shadow edges and believable ambient occlusion.

### Rendering principles

- Prioritize a coherent, stylized-realistic look over photorealism.
- Use modern GPU APIs and render techniques, while preserving the visual constraints and readability of late-2000s console-era rendering.
- Favor deterministic, artist-controllable lighting over fully automatic realism.
- Keep the renderer modular: deferred rendering for opaque geometry, forward rendering for translucency and special materials.
- Make quality scalable without changing the core look: performance settings should reduce resolution, shadow quality, effect intensity, and draw distance—not remove the engine's identity.

## Dev Workflow

- Ask questions only when absolutely necessary - work autonomously
- IMPORTANT! DO NOT look outside of working directory or use other code on my computer as reference
- Start by asking question to determine engine name. Suggest a few
- Testing is of the utmost importance - use lots of unit tests (any framework, simple is good), run automated testing
- Setup a scene used for testing, which tests every single feature in the engine - all components, and entire scripting API
- Do things properly, this is production-grade and not a hack project
- Create AGENTS.md and relevant skills to support development, and contain concrete development guidelines
- Follow the coding standards and SOLID principles
- Do not reinvent the wheel; if something already exists, do not try implement own one.
- Create git repository, commit and push to [GitHub repo](https://github.com/Bitrose-Studio/GameEngineExperiment.git). IMPORTANT: do a code review before committing, and make sure all changes comply with code style, production-grade quality standards, and have been properly tested/have unit tests that have passed where necessary

## Tech stack

- C++ and CMake for core engine
- [GLFW](https://github.com/glfw/glfw) and [nvrhi](https://github.com/NVIDIA-RTX/NVRHI), using Vulkan primarily on all platforms
- [glm](https://github.com/g-truc/glm) for math
- [miniaudio](https://github.com/mackron/miniaudio) for audio
- Native (C++) and Lua for scripting, though this can be up for discussion

## Basic Architecture

- Static library for core engine, executable for editor and runtime (depending on chosen design)
- Need to have editor to build games - this can be embedded into runtime executable (and stripped from distribution builds), or can be a standalone executable
- Simple ECS perhaps, to author scene with entities and components
- Ability to "export" game - executable that runs game without editing ability that we can distribute
- Editor needs to be fully controllable by AI agents - I should be able to ask you to build me a game that's represent the game engine, and you should have all the tools available to do so without my intervention

## Engine Modules

- **Core:** platform layer, logging, assertions, memory allocators, job system, configuration, serialization.
- **Math:** vectors, matrices, quaternions, transforms, AABBs, frustums, curves, and color utilities.
- **Renderer:** scene representation, material system, shader system, render passes, lighting, shadows, post-processing.
- **Assets:** mesh, texture, material, shader, audio, prefab, scene, and animation import/export.
- **ECS / World:** entities, components, systems, transforms, hierarchy, scene loading, and prefab instantiation.
- **Physics:** rigid bodies, colliders, raycasts, character movement, triggers, and constraints.
- **Audio:** 2D/3D playback, listeners, buses, attenuation, and scripted audio events.
- **Scripting:** native components plus Lua scripting API.
- **Editor:** viewport, hierarchy, inspector, asset browser, gizmos, scene authoring, play-in-editor, and AI automation interface.
- **Runtime / Export:** game packaging, startup flow, settings, input mapping, and distribution builds.

## 3D Renderer

### Rendering architecture

- Implement a **deferred rendering path** for opaque and masked geometry.
- Use a G-buffer containing at minimum:
  - Albedo / base color
  - World-space or view-space normals
  - Roughness / specular power
  - Metallic or specular intensity
  - Emissive color
  - Motion vectors, where required
- Use a separate **forward path** for transparent materials, particles, sky, and effects that do not fit deferred shading.
- Include a depth pre-pass to reduce overdraw and support screen-space effects.
- Use clustered or tiled light culling for scenes with many dynamic lights.
- Support render targets, render passes, compute shaders, and GPU resource barriers through NVRHI.

UE3 used deferred shading to accelerate dynamic lighting and shadowing, with per-pixel lighting and a gamma-correct HDR renderer. [3][12]

### Materials

- PBR material workflow as the default, but tuned toward the readable, high-contrast look of early UE3.
- Material inputs:
  - Base color
  - Normal map
  - Roughness
  - Metallic
  - Specular
  - Ambient occlusion
  - Emissive color and intensity
  - Opacity / alpha
  - Tiling, UV offset, and texture coordinate channels
- Support material instances so artists can reuse a parent material while changing textures and scalar parameters.
- Support material layers or blend modes for decals, dirt, rust, wet surfaces, and damaged metal.
- Provide an artist-friendly material graph in the editor, or initially a declarative material format that can later be represented visually.
- Shader permutations must be deterministic, cached, and testable.

Early UE3 titles relied heavily on normal maps, aggressive specular response, and flexible material definitions to make lower-polygon assets appear highly detailed. [2]

### Lighting

- Light types:
  - Directional sun/moon light
  - Point lights
  - Spot lights
  - Rect or area lights, if practical
  - Sky / ambient light
- Support per-light:
  - Color
  - Intensity
  - Range / attenuation
  - Inner and outer cone angle for spotlights
  - Shadow casting
  - Volumetric contribution
  - Light cookies or projected textures
- Implement physically inspired units where useful, but expose simplified artist controls first.
- Support light channels or lighting layers for gameplay and cinematic control.
- Ensure lights can be enabled, disabled, moved, recolored, and animated through scripts.

### Shadows

- Soft shadow maps for directional, point, and spot lights.
- Cascaded shadow maps for the primary directional light.
- PCF or a comparable filtered shadow technique.
- Shadow bias, normal offset, fade distance, resolution, and cascade settings exposed in the editor.
- Contact-hardening or screen-space shadow refinement can be added later, but must not become a prerequisite for the core look.
- Provide stable shadow quality across platforms and scalable presets.

### Global illumination and ambient lighting

- Start with practical, controllable ambient lighting:
  - Sky ambient light
  - Hemisphere ambient light
  - Baked or captured ambient probes
  - IBL using HDR environment maps
- Support HDRIs from [Poly Haven](https://polyhaven.com/hdris) for sky lighting, reflections, and test scenes.
- Add baked static lighting or light probes once the core renderer is stable.
- Avoid making real-time global illumination an initial requirement; prioritize stable direct lighting, shadows, IBL, and post-processing first.
- Later options may include light probes, screen-space GI, voxel GI, or a similar approximation.

### Screen-space effects

- SSAO with configurable radius, intensity, bias, and quality.
- Screen-space reflections, initially optional and quality-scaled.
- Screen-space decals or deferred decals for surface variation.
- Screen-space fog or volumetric fog for atmosphere and light shafts.
- All screen-space effects must have clear quality settings and graceful fallbacks.

### HDR and post-processing

- Render internally in HDR.
- Use ACES-style or filmic tone mapping.
- Support exposure control, auto-exposure, and manual exposure override.
- Post-processing stack:
  - Bloom
  - Tonemapping
  - Color grading / LUT support
  - Contrast, saturation, lift-gamma-gain, and white balance
  - Vignette
  - Chromatic aberration
  - Film grain
  - Depth of field
  - Motion blur
  - Lens flares or light shafts
  - FXAA, TAA, or another practical anti-aliasing solution
- Post-processing must be authored through a reusable post-process volume or camera settings component.
- Preserve the early-UE3 feel: controlled bloom, soft contrast, muted color grading, and strong light/shadow separation. [2][5]

### Meshes and assets

- Import glTF meshes with materials and textures.
- Support static meshes, skinned meshes, LODs, and mesh instancing.
- Import or generate:
  - Base-color textures
  - Normal maps
  - Roughness / specular maps
  - Metallic maps
  - Ambient-occlusion maps
  - Emissive maps
- Support texture streaming or at minimum predictable texture memory budgets.
- Provide mesh validation, material validation, and clear import diagnostics.
- Editor has gizmo to position them in the world.

### Camera and rendering features

- Perspective and orthographic cameras.
- Configurable field of view, near/far planes, exposure, and post-process overrides.
- Camera shake, cinematic sequences, and simple camera animation.
- Scene capture / render-to-texture for mirrors, security cameras, and UI previews.
- Multiple viewport support in the editor.

## 3D Physics

- Use a simple, well-supported 3D physics library of your choosing.
- Controllable via scripting, authored via components in editor.
- Required features:
  - Static and dynamic rigid bodies
  - Box, sphere, capsule, convex mesh, and trimesh colliders
  - Raycasts, sphere casts, and overlap queries
  - Triggers and collision events
  - Constraints and joints
  - Character controller with ground detection, slopes, and step handling
  - Fixed-timestep simulation with deterministic behavior where possible
- Physics and rendering transforms must remain synchronized without coupling their internal implementations.

## Behavior and Scripting

- Scripting with Lua (or other chosen language)
- Control entities/components and run in update loop, and entity destruction/creation
- Spawn new entities/prefabs
- Native C++ components for engine-critical systems and performance-sensitive gameplay.
- Lua for gameplay prototyping, mission logic, UI behavior, and editor automation.
- Scripting API must expose:
  - Entity lookup, creation, destruction, and parenting
  - Transform manipulation
  - Component access and property changes
  - Input events
  - Physics queries and collision callbacks
  - Audio playback
  - Scene and prefab loading
  - Timers, coroutines, and update callbacks
  - Logging and debugging
- Scripts must hot-reload safely during editor development where feasible.
- Script errors must never crash the engine; they should produce actionable diagnostics.

## Editor

- Viewport with orbit, fly, pan, zoom, selection, and focus controls.
- Entity hierarchy panel.
- Inspector with component properties, asset references, colors, curves, and enums.
- Asset browser with import, preview, rename, move, and dependency tracking.
- Transform gizmos for translate, rotate, and scale.
- Multi-select, undo/redo, copy/paste, and prefab workflows.
- Play-in-editor with clean state reset.
- Rendering debug views:
  - Wireframe
  - Normals
  - Base color
  - Roughness / metallic
  - Emissive
  - Depth
  - Lighting-only
  - Shadow cascades
  - SSAO
  - G-buffer channels
  - Overdraw and draw-call statistics
- Editor automation API so AI agents can create scenes, import assets, configure components, run tests, and export builds.

## Asset Pipeline

- Asset IDs must be stable and independent of file paths where possible.
- Support source assets and imported runtime assets.
- Track dependencies between scenes, prefabs, materials, textures, meshes, shaders, and scripts.
- Provide deterministic asset imports and cache invalidation.
- Support asset references in scenes and prefabs.
- Include validation for missing textures, invalid materials, malformed meshes, and incompatible asset versions.

## Testing and Quality

- Unit tests for math, ECS, serialization, asset loading, scripting bindings, physics queries, and renderer resource management.
- Rendering tests should compare captured frames against approved reference images with a controlled tolerance.
- Automated tests must run headlessly where possible.
- Every public engine API should have documentation and at least one usage example.
- The all-features test scene must include:
  - Static and animated meshes
  - PBR materials
  - Transparent materials
  - Directional, point, and spot lights
  - Shadows
  - SSAO
  - Bloom and post-processing
  - Physics bodies and triggers
  - Lua and native scripts
  - Audio
  - Prefabs
  - UI or debug overlay
- Performance tests should track frame time, draw calls, GPU memory, asset load time, and script execution time.

## Initial Milestones

1. **Engine foundation:** CMake project, platform layer, logging, math, core tests, Git workflow, and AGENTS.md.
2. **Window and renderer skeleton:** GLFW window, NVRHI device, swapchain, clear screen, resize handling, and shader loading.
3. **Scene and ECS:** entities, transforms, hierarchy, serialization, and basic editor viewport.
4. **Static mesh rendering:** glTF import, textures, materials, camera, and depth testing.
5. **Deferred G-buffer:** base pass, G-buffer layout, debug views, and forward translucency path.
6. **Lighting:** directional, point, and spot lights with clustered/tiled lighting.
7. **Shadows:** shadow maps, cascades, filtering, and shadow debug visualization.
8. **HDR and post-processing:** HDR render targets, tonemapping, bloom, color grading, and quality presets.
9. **Screen-space effects:** SSAO, optional SSR, fog, and anti-aliasing.
10. **Physics and gameplay:** rigid bodies, colliders, character controller, input, and scripting.
11. **Editor maturity:** asset browser, prefabs, undo/redo, play-in-editor, and automation API.
12. **Packaging and export:** standalone runtime builds for Windows, macOS, and Linux.

## Non-Goals

- Do not attempt to replicate Unreal Engine 3 source code, file formats, editor UI, or proprietary tooling.
- Do not target consoles initially.
- Do not require ray tracing for the core visual identity.
- Do not implement every modern rendering feature before establishing a stable, testable deferred renderer.
- Do not sacrifice engine stability, diagnostics, or test coverage for visual effects.

## Post ToDo

- Once the engine is done, write a documentation for end users who wants to make a game in the engine
- Write a detaield documentation (that I can use to learn from this project) for me or pont to me if already done.
- @AEGEON_Vertical_Slice_Prototype_Brief.md for a later demo when I ask it

## Note

- Skeletal mesh and animation missing froom the Initial Milestones
- The editor should be modern, flat designed UI and layout inspired by Unity and Unreal Engine. A project could be opened by double-click on the project file, and editor theme.

- All these bellow can be done after the originally planned version done:
- Asset browser should be like in Unity and Unreal Engine. List and Grid view with folder tree on the side.
- File open should use file open dialog, not hand typing. 
- The play button should change to stop button, not just highlight when play started.
- We need OpenGL support as fall back if no Vulkan available. (Decided 12 November 2026: an OpenGL backend for Linux without Vulkan; Direct3D 12 stays the Windows path. See docs/architecture/CORDITE_AUDIT.md.)

- When I tried to open a project, so I clicked on Open Project in menu, the whole editor went to frozen (not responding) (fixed in 6f550ce)
- From Asset Browser I cannot drag and drop anything to viewport niether to hierarchy (fixed in b0d88f7)