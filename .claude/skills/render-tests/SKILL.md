---
name: render-tests
description: Run, debug and update Cordite's GPU rendering tests and their reference images. Use when a test in tests/render fails, when rendered output changed on purpose and a reference image must be re-approved, when adding an image-comparison test, or when the Vulkan validation layers report an issue.
---

# GPU rendering tests

The `render` suite (`tests/render/`, CTest label `gpu`) renders on a real Vulkan device.
Every test creates its device with validation enabled and ends with
`checkNoValidationIssues`, and image tests compare a rendered frame with a reference PNG
in `tests/render/references/`.

## Running

```powershell
./scripts/dev.ps1 test -Label gpu                    # all GPU tests
./scripts/dev.ps1 test -Filter render                # just the render suite
./scripts/dev.ps1 test -ExcludeLabel gpu             # everything else, no GPU needed

# One suite or test case, with full output (run from the repository root):
$env:VK_ADD_LAYER_PATH = "$PWD/.tools/vulkan-sdk/Bin"   # only when running the exe directly
build/windows/bin/RelWithDebInfo/cordite_render_tests.exe -ts="Swapchain"
build/windows/bin/RelWithDebInfo/cordite_render_tests.exe -tc="*reference image*"
```

CTest and `dev.ps1` set `VK_ADD_LAYER_PATH` themselves. If the Khronos validation layer
is not installed, tests still run with NVRHI's own validation and log a warning; install
it with `./scripts/bootstrap.ps1 -WithVulkanSdk`. The first test of the suite prints the
adapter and whether the Vulkan validation layer is active.

## When an image test fails

The failure message names three files:

- `tests/render/references/<name>.png` - what was expected.
- `tests/render/output/<name>.actual.png` - what was rendered.
- `tests/render/output/<name>.diff.png` - differing pixels in red over a dimmed copy.

Look at all three (the Read tool displays PNGs). Then decide:

1. **The change is a bug.** Fix the code. Do not touch the reference.
2. **The change is intended** (you changed the shader or the pass on purpose). Check
   that the new image is exactly what you meant, then re-approve it (next section) and
   say in the commit message why the image changed.
3. **Only a few edge pixels differ on a different GPU or driver.** That is what the
   tolerance is for. Raise `maxDifferingRatio` slightly for that test rather than
   loosening `channelTolerance` globally, and explain it in a comment.

## Approving a new or changed reference image

```powershell
$env:CORDITE_UPDATE_REFERENCES = '1'
build/windows/bin/RelWithDebInfo/cordite_render_tests.exe -tc="<test name>"
Remove-Item Env:CORDITE_UPDATE_REFERENCES
```

This overwrites the reference with the rendered image. Then:

1. Open the new reference image and confirm it shows what the test intends. Never
   commit a reference you have not looked at.
2. Run the suite again without the variable and confirm it passes.
3. Commit the PNG together with the code change that produced it.

Keep reference images small (a few hundred pixels per side). They live in git.

## Writing an image test

```cpp
TEST_CASE("the vignette darkens the corners") {
    RenderContext context;                                   // device + shaders, validation on
    VignettePass pass = *VignettePass::create(context.device(), *context.shaders);

    const nvrhi::TextureHandle target = createColorTarget(context.device(), 320, 180);
    ... render into it ...

    const Image image = readToImage(*context.graphics, target);
    checkAgainstReference(image, "vignette-320x180");        // tolerance: 4 steps, 0.2% of pixels
    checkNoValidationIssues(*context.graphics);
}
```

- Prefer asserting on properties of the image (a corner is darker than the centre, the
  tint removed green) in addition to, or instead of, a reference image. Property checks
  survive intentional restyling and explain what matters.
- Best of all is a test that says what a pixel must be. For anything drawn by the
  scene renderer, `SceneFixture` (`tests/render/SceneTestSupport.h`) makes that
  possible: materials that emit a color or reflect all light evenly, cameras without
  perspective, lights of known brightness. `SceneRendererTests.cpp`,
  `SceneLightingTests.cpp` and `SceneShadowTests.cpp` are full of examples. Such a
  test draws a picture that is not finished (`SceneFixture::flatSettings()`, which
  sets `PostProcessQuality::Off`): the light as it was worked out, and nothing done
  to it. What finishing does - exposure, bloom, the curves, grading - is tested in
  `PostProcessTests.cpp`, against the functions of `PostProcessSettings.h`.
- Images a test needs are made in the test and written with `writeImage` or, for
  light, `writeHdrImage`; `EnvironmentTests.cpp` builds its skies from a function of
  the direction (`makeEnvironment`). A world in which every direction has a color of
  its own leaves nothing about orientation to chance.
- A value that is not 8 bits - scene color, a level of a cube map - is read back with
  `readTexture` (`rhi/TextureReadback.h`), which takes a mip level and an array slice.
- Where no formula says what a pixel must be but the arithmetic is the engine's own
  on the CPU as well, build the answer from that arithmetic and a scene given as a
  formula - `Reference` in `AmbientOcclusionTests.cpp` walks a wall that is a plane -
  and first hold that reference against a case that does have a formula.
- Make sure the test can fail: break the code on purpose once (leave a light out,
  shrink a bound) and see the test notice.
- Everything that influences the image must be fixed: sizes, times, seeds. Use
  `EngineDesc::fixedTimeStep` rather than the clock.
- Give the reference a name that includes its size and anything else that distinguishes
  variants.
- End every test with `checkNoValidationIssues`.

## When validation reports an issue

The message is listed in the test failure and logged under the `Vulkan` or `RHI`
category. It is a real bug even if the image looks right: other drivers may not be as
forgiving.

- `NVRHI error: Unknown prior state of texture ...`: the texture was created without
  `keepInitialState`. Set `initialState` and `keepInitialState = true` in its desc.
- `... layout transition on presentable VkImage ... has not been acquired`: something
  touched a swapchain image outside `beginFrame()`/`present()`.
- `... missing VK_IMAGE_USAGE_...`: the texture desc lacks a usage flag
  (`isRenderTarget`, `isUAV`, ...).
- A message that only appears on one GPU still counts. Fix it rather than excluding it.

To see every message as it happens, run the editor headless:

```powershell
./scripts/dev.ps1 run -Target cordite-editor -- --headless --validation --frames 3 --log-level debug
```

It exits with status 2 when validation reported anything.
