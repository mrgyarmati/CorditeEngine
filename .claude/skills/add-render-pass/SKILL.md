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
shadow maps -> depth pre-pass -> base pass (G-buffer) -> deferred lighting -> forward pass
            -> post-processing (or, for a picture that is not finished, display encoding)
```

Decide where a new pass belongs before writing it: does it draw meshes, read the
G-buffer, work on the light of the scene, or finish the picture?

### A pass that draws meshes

Do not write a class. A mesh pass is a `MeshPassDesc` given to `MeshPass`
(`engine/cordite/renderer/MeshPass.h`); five of those that exist are at the top of
`SceneRenderer.cpp`, and the one that draws shadow maps is in `ShadowAtlas.cpp`:

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
- **Per-instance data** is what `MeshDrawList` writes for every instance: the transform
  and, in a fourth row, the lighting channels (`kMeshInstanceStride` bytes). Something
  else that differs between two instances of the same mesh and material goes into
  that row, and from `meshVertex` into `MeshVaryings`.
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
- The set also holds the ambient occlusion of the view (`t5`). It means something
  only when `screenOcclusion` of the view's constants is 1: ask before reading it. A
  pass that draws into it, or into anything else of that set, needs a set of its own
  without it (`AmbientOcclusionPass`).
- A pass that needs an image of its own for every view - a smaller copy, a half-way
  result - asks the view's `SceneTargets` for it (`prepareTracedOcclusion` is the
  example) and keeps nothing itself. The image then goes when the view is resized,
  not sixty frames later.
- Draw into `targets.lightingFramebuffer()`. A framebuffer with the depth attached
  cannot be used while the depth is being read as a texture.
- Anything that lights a surface calls `shadeSurface` (`common/SceneLighting.slang`),
  which the forward pass calls too; extend that function, not one of its callers.
  Light that comes from all around - ambient light, the environment - is added in
  `shadeSurroundings`, which is also what the `reflections` debug view shows.
- Whatever a pass writes to scene color goes through `storableLight` first: the
  target is made of half floats, and what a GPU keeps of more light than they hold
  may be infinity. `shadeSurface` does it for what it returns.

### Something about the environment

The environment of a view is in its binding set too (`common/Environment.slang`,
included by `common/SceneLighting.slang`): nine spherical harmonics in the constants,
a cube map of blurred reflections at `t5`, the image itself at `t6`.

- `shadeEnvironment(surface, toViewer)` is what `shadeSurface` adds. A pass of its
  own that needs the light of a direction calls `environmentDiffuseLight(normal)`,
  `environmentReflectedLight(direction, roughness)` or `environmentSkyLight(direction,
  turn)`; all three take directions of the world and turn them themselves.
- A view without an environment has `environmentIntensity` zero and a black texel
  bound: the functions return nothing, and need no test for "is there one".
- What is made of the image - the levels of the cube map, the nine numbers - is made
  in `EnvironmentMaps` and `projectEnvironment`. Where a direction is in the image
  (`equirectUv`) exists in C++ and in `common/Math.slang`: change both, and
  `EnvironmentTests` says at once whether they still agree.
- The mirror level of the cube map is 256 texels a side (`kEnvironmentFaceSize`), a
  third of a degree a texel; something that must be sharper than that - the sky - reads
  the image.

### Something about lights

The lights of a view are in its binding set, so every pass that binds the view has
them (`common/Lights.slang`):

```slang
#include "common/Lights.slang"

// Every directional light, then the point and spot lights near the pixel.
for (uint index = 0; index < directionalLightCount; ++index) { ... lights[index] ... }
const ClusterRange range = clusterGrid[clusterIndexOf(uv, viewDepth(position))];
for (uint entry = 0; entry < range.count; ++entry) { ... lights[lightIndices[range.offset + entry]] ... }
```

- **A new property of a light** is a field of `LightComponent` (described with
  `TypeBuilder`, see the add-component skill), of `SceneLight`, and of `GpuLight` and
  its mirror `Light` in `Lights.slang` - 64 bytes, in rows of 16, with a
  `static_assert`. `makeSceneLight` (`SceneLights.cpp`) converts what a person sets,
  `toGpuLight` (`LightClusters.cpp`) what the shader needs.
- **A new kind of light** also needs a bound for the clusters (`boundsOf` in
  `LightClusters.cpp`): a sphere that encloses everything the light reaches. Too large
  costs time; too small cuts the light off along the borders of clusters, which the
  tests of `SceneLightingTests` catch.
- **How a light falls off** is in `shadeLight`. It must reach zero at the light's
  range, where the clusters stop listing the light.
- **The grid** (`kClusterColumns`, `kClusterRows`, `kClusterSlices`) and
  `clusterIndexOf` exist in C++ and in the shader. Change both; the light-count debug
  view shows at once whether they still agree.
- Measure: `./scripts/dev.ps1 perf` runs a scene with six hundred lights and reports
  what gathering and clustering them costs, and how many list entries there are.

### Something about shadows

`ShadowPlan.h` decides which light gets which shadow maps and what each map shows;
`ShadowAtlas` draws them; `common/Shadows.slang` looks surfaces up in them.

- **What a map shows** is a `ShadowView`: a matrix, a tile of the atlas, the size of a
  texel in the world and the directions its axes run in. `planShadows` is arithmetic
  and has unit tests (`ShadowPlanTests.cpp`); change it there first.
- **Depth in a shadow map is not reversed** (0 at the light), because the comparison
  sampler only knows "smaller". Use `orthographic()` and `perspective()`, not their
  `ReverseZ` relatives, and `MeshPassDesc::conventionalDepth`.
- **The shadow pass binds its own, smaller view set** - the constants alone - because
  the atlas cannot be read while it is drawn into. A pass that draws into a texture
  other passes read needs the same.
- **Nothing is biased when a map is drawn.** A surface is kept out of its own shadow
  by where it is looked up (`shadowLookupPosition`), and the filter reads beside the
  point on the point's own plane (`shadowMapVisibility`). A change there has two
  tests to pass that pull in opposite directions: no stripes under a sun three
  degrees up, and a shadow that begins at the foot of what casts it.
- **Look things up with `Surface::geometricNormal`**, the triangle's own normal, never
  with `normal`, which a normal map has bent: the map holds the depth of the triangle.
  A pass that builds a `Surface` by another way than `evaluateSurface` has to fill it
  in; deferred lighting does, from the depth buffer (`geometricNormalAt`).
- **A new kind of shadow-casting light** needs its maps planned (`planLocalLight`), a
  way to pick the map for a point in the shader, and a test like the one that checks
  each of the six sides of a point light.
- Measure: the `shadows` scenario of `./scripts/dev.ps1 perf` draws more than fifty
  shadow maps a frame.

### Something about the finished picture

`PostProcessPass` turns the light of the scene into what a screen shows; the order of
what it does, and every number, is in `PostProcessSettings.h`.

- **Something that happens to a pixel by itself** - a curve, a tint, a change of
  contrast - is a few lines in `passes/PostProcess.slang`, at the place in the order
  where a photographer would look for it: before the tone mapper it is arithmetic on
  light, after it on the colors of a display.
- **Write the arithmetic twice:** in the shader (`common/ToneMapping.slang` for what
  is shared) and as a function in `PostProcessSettings.cpp`. `PostProcessTests` draws
  fields of known light and holds every pixel against the function; a test of the
  function alone pins numbers worked out by hand.
- **A new setting** goes into `PostProcessSettings` with its range in `sanitized()`,
  into `blend()`, into `PostProcessConstants` on both sides (`PostProcessPass.cpp`,
  `common/PostProcess.slang`; mind the packing rules), into the description of the
  `PostProcess` component (`RenderComponents.cpp`) and into the showcase's scene.
- **Something that needs the picture around a pixel** is a pass of its own before the
  last one, as the bloom is: small targets in `SceneTargets`, a `FullscreenPass`, and
  binding sets kept per view (`PostProcessPass::ViewCache`), because a view is
  drawn every frame with the same textures.
- **Something that lasts from frame to frame** - the exposure a view has got used to -
  is kept in the `SceneTargets` of the view, and is told how much time has passed
  (`PostProcessFrame::deltaSeconds`). The renderer never looks at a clock.
- **Quality** (`postProcessSettingsFor`) changes how finely something is worked out,
  never whether the look is there.
- Count it: `PostProcessPass::draw` returns its passes, which are budgeted as
  `screenPasses` in `tests/perf/budgets.json`.

### A debug view

1. Add the value to `RenderDebugView` (`RenderDebugView.h`): the enumerator, its place
   in `kRenderDebugViews`, `nameOf` and `displayNameOf`. The editor's menu and
   `viewport.debugView` pick it up from the list.
2. If it is drawn from the G-buffer and the depth, keep it between `BaseColor` and
   `ShadowCascades` (`isDrawnFromGBuffer`), and add a `case` with the same number to
   `passes/DebugView.slang`. Write colors as they are and numbers through `asShown`,
   so that 0.5 comes out as mid-grey.
3. Otherwise handle it in `SceneRenderer::render`, as wireframe and overdraw are.
4. Test it in `SceneRendererTests`: what a known surface must look like in the view.

### Testing a scene pass

`tests/render/SceneRendererTests.cpp` and `SceneLightingTests.cpp`, with the
`SceneFixture` of `tests/render/SceneTestSupport.h`. State what a pixel must be
wherever that is possible:

- flat emissive materials (`writeFlatMaterial`), an orthographic camera and
  `flatSettings()` (no light) make colors exact: `checkPixel(image, x, y, linearColor)`;
- a white, matte material (`writeMatteMaterial`) shows exactly the light that reaches
  it: lit head-on by a light of intensity 1, it is white. `scene.addLight`, `setSun`
  and `setAmbient` light the world; `lightOn` in `SceneLightingTests.cpp` is the
  lighting model written out on the CPU, to compare whole pictures with;
- a debug view makes a number visible: `checkShown(image, x, y, Vec3(0.25f))`;
- `lightOn` with a list of boxes knows shadows: a point no light can see past a box
  gets none of it. `checkReceiver` in `SceneShadowTests.cpp` compares a whole floor or
  wall with that, leaving out the soft edges of shadows;
- `scene.renderer->statistics()` says which pass drew what, how many lights there are
  and how many shadow maps.

Look at surfaces head-on when the test predicts single pixels. Seen at a low angle, a
surface runs through a pixel for a long way, and the value depends on where in the
pixel the GPU samples; see the test with hundreds of lights for how to allow for that.
