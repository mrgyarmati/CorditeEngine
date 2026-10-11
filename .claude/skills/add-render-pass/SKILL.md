---
name: add-render-pass
description: Add a shader or a render pass to Cordite's renderer. Use when writing a new .slang shader, a new screen-space or geometry pass, a new shader permutation, or when a shader fails to compile or binds the wrong resources.
---

# Add a shader and a render pass

`TestPatternPass` (`engine/cordite/renderer/TestPatternPass.*` with
`shaders/passes/TestPattern.slang`) is the smallest complete pass. Read it first and use
it as the template.

## 1. Write the shader

Create `shaders/passes/<Name>.slang`. Shared code goes in `shaders/common/` and is pulled
in with `#include "common/<File>.slang"` (use `#pragma once` in shared files).

```slang
#include "common/Fullscreen.slang"

cbuffer VignetteConstants : register(b0)
{
    float4 colorAndStrength;     // rgb: tint, a: strength
    float2 resolution;
    float2 padding;
};

Texture2D<float4> sceneColor : register(t0);
SamplerState linearSampler : register(s0);

[shader("pixel")]
float4 vignettePixel(FullscreenVaryings input) : SV_Target0
{
    ...
}
```

Conventions:

- Entry points are named `<pass><Stage>` (`vignettePixel`, `gbufferVertex`), carry a
  `[shader("...")]` attribute and are requested by that name.
- Matrices are column-major: `mul(viewProjection, float4(position, 1.0))`.
- Registers map to Vulkan bindings with NVRHI's defaults, so in C++ you use the same
  slot numbers as in the shader: `tN` textures, `sN` samplers, `bN` constant buffers,
  `uN` unordered access views. `spaceN` is binding set N.
- Fill constant buffers in multiples of 16 bytes and add explicit padding members.
- Optional features are `#if defined(FEATURE)` blocks, selected with shader defines.
- Never `normalize()` what can be zero (an interpolated normal, the difference of two
  points): use `normalizeOr` from `common/Math.slang`. Guard divisions and `pow` of
  negative numbers the same way. A single "not a number" pixel ruins every blur.
- Comment what the shader computes and why, not the syntax.

## 2. Register it for compile testing

Add every entry point (and every define combination the engine uses) to
`engineShaders()` in `tests/unit/shader_compiler/EngineShadersTests.cpp`. That test
compiles them all on every platform without a GPU and fails on any compiler warning.

## 3. Write the pass

Create `engine/cordite/renderer/<Name>Pass.h/.cpp` and add them to
`engine/cordite/renderer/CMakeLists.txt`.

- A pass has a static `create(device, shaders, ...)` returning `Result<Pass>` and a
  `draw(commandList, ...)` that assumes an open command list. It does not own a
  command list and does not submit.
- Mirror each `cbuffer` with a C++ struct of the same name and layout, in an anonymous
  namespace, followed by `static_assert(sizeof(...) == N, ...)`.
- Screen-space passes are built on `FullscreenPass`, which creates the pipeline for each
  framebuffer format on first use. Create the binding layout once in `create`.
- Per-frame constants use a volatile constant buffer
  (`nvrhi::utils::CreateVolatileConstantBufferDesc`) written with
  `commandList->writeBuffer` just before drawing.
- Create textures with `initialState` + `keepInitialState = true` so NVRHI tracks their
  state and inserts the barriers.
- Get shaders through `ShaderManager::get({.path, .entryPoint, .stage, .defines})`.
  Never load SPIR-V or call the compiler directly.
- Document the pass in its header: what it does, its inputs and outputs, and a `@code`
  usage example.

## 4. Test it

Add tests to `tests/render/RenderPassTests.cpp` (or a new file in `tests/render/`,
registered in `tests/CMakeLists.txt`). See the `render-tests` skill. At minimum:

- the pass renders without validation issues;
- a property of the output that shows it does its job;
- a reference image if the exact look matters;
- its failure paths (for example a missing input texture).

## 5. Verify

```powershell
./scripts/dev.ps1 build
./scripts/dev.ps1 test -Filter "shader_compiler|render"
./scripts/dev.ps1 run -Target cordite-editor -- --headless --validation --frames 3 --screenshot out.png
```

Look at the screenshot. Then run `./scripts/dev.ps1 verify` before committing.

## Troubleshooting

- **The shader compiles but draws nothing:** check the framebuffer formats, that depth
  testing is off for fullscreen passes, and that the binding set matches the layout
  (NVRHI validation reports mismatches).
- **Garbage in a constant buffer:** the C++ struct and the `cbuffer` disagree. Compare
  member by member; remember `float3` followed by `float` packs into 16 bytes, but
  `float3` followed by `float3` does not.
- **Upside-down or mirrored output:** UV (0, 0) is the top-left of a render target and
  clip-space +Y is up. `common/Fullscreen.slang` already accounts for this.
- **A changed shader is not picked up:** development builds read `shaders/` from the
  repository; a Release build reads the copy beside the executable, refreshed on build.
  The shader cache is keyed by content, so it never serves a stale shader.

## The passes of a scene

`SceneRenderer::render` runs these for every view, in this order (the table in
`SceneRenderer.h` is the reference):

```
depth pre-pass -> base pass (G-buffer) -> deferred lighting -> forward pass -> display encoding
```

Decide where a new pass belongs before writing it: does it draw meshes, read the
G-buffer, or work on the lit picture?

### A pass that draws meshes

Do not write a class. A mesh pass is a `MeshPassDesc` given to `MeshPass`
(`engine/cordite/renderer/MeshPass.h`); the five that exist are at the top of
`SceneRenderer.cpp`:

```cpp
MeshPassDesc desc;
desc.debugName = "Shadow pass";
desc.maskedPixelShader = {.path = "passes/MeshDepth.slang", .entryPoint = "meshDepthMaskedPixel",
                          .stage = ShaderStage::Pixel};        // no pixel shader for solid surfaces
desc.depth = MeshPassDepth::TestAndWrite;
```

- **Never write a vertex shader for it.** Every mesh pass uses `meshVertex` in
  `passes/MeshVertex.slang`, and the pixel shader takes a `MeshVaryings`
  (`common/MeshVaryings.slang`). The base pass depends on getting exactly the depth the
  pre-pass wrote, which only the same code gives.
- **Input** is the batches of the view's `MeshDrawList`: `solidBatches()` (opaque and
  masked, grouped for instancing), `blendedBatches()` (far to near) or `batches()`.
  The list is built once per view; a pass never walks the world or sorts.
- **The material** comes through `#include "materials/Material.slang"`:
  `evaluateSurface(input, isFrontFace)` gives the `Surface` at the pixel, and
  `evaluateOpacity` is enough for a pass that only needs to cut holes. Never sample
  material textures directly.
- **Call it** from `SceneRenderer::render`, add it to `SceneRenderer::Passes`, and
  count what it draws in `SceneRenderStatistics`.

### A pass that reads the G-buffer

`DeferredLightingPass` is the template: a `FullscreenPass` with two binding sets, the
view's and the G-buffer's.

```slang
#include "common/Fullscreen.slang"
#include "common/GBuffer.slang"
#include "common/View.slang"

Texture2D<float4> gbufferBaseColor : register(t0, space1);
Texture2D<float4> gbufferNormal : register(t1, space1);
Texture2D<float4> gbufferParameters : register(t2, space1);
Texture2D<float4> gbufferEmissive : register(t3, space1);
Texture2D<float4> sceneDepth : register(t4, space1);
```

- Get the layout and the set from `createGBufferLayout` / `createGBufferBindings`
  (`GBufferBindings.h`); pass `withConstants` for a constant buffer of the pass's own
  at `b0, space1`.
- Read with `Load`, not `Sample`: one pixel of the G-buffer belongs to one surface.
  `decodeGBuffer` turns the four values into a `Surface`, and
  `worldPositionFromDepth` gives its position.
- Skip pixels whose depth is 0 - nothing was drawn there.
- Draw into `targets.lightingFramebuffer()`. A framebuffer with the depth attached
  cannot be used while the depth is being read as a texture.
- Anything that lights a surface calls `shadeSurface` (`common/SceneLighting.slang`),
  which the forward pass calls too; extend that function, not one of its callers.

### A debug view

1. Add the value to `RenderDebugView` (`RenderDebugView.h`): the enumerator, its place
   in `kRenderDebugViews`, `nameOf` and `displayNameOf`. The editor's menu and
   `viewport.debugView` pick it up from the list.
2. If it shows something the G-buffer holds, keep it between `BaseColor` and `Depth`,
   and add a `case` with the same number to `passes/DebugView.slang`. Write colors as
   they are and numbers through `asShown`, so that 0.5 comes out as mid-grey.
3. Otherwise handle it in `SceneRenderer::render`, as wireframe and overdraw are.
4. Test it in `SceneRendererTests`: what a known surface must look like in the view.

### Testing a scene pass

`tests/render/SceneRendererTests.cpp` with `SceneFixture`. State what a pixel must be
wherever that is possible:

- flat emissive materials (`writeFlatMaterial`), an orthographic camera and
  `flatSettings()` (no light) make colors exact: `checkPixel(image, x, y, linearColor)`;
- a debug view makes a number visible: `checkShown(image, x, y, Vec3(0.25f))`;
- `scene.renderer->statistics()` says which pass drew what.
