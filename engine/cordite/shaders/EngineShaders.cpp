#include "cordite/shaders/EngineShaders.h"

#include "cordite/core/Assert.h"

#include <algorithm>

namespace cordite {

std::vector<ShaderRequest> engineShaderRequests() {
    std::vector<ShaderRequest> requests = {
        {.path = "common/Fullscreen.slang", .entryPoint = "fullscreenVertex", .stage = ShaderStage::Vertex},
        {.path = "passes/Blit.slang", .entryPoint = "blitPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DebugLines.slang", .entryPoint = "debugLineVertex", .stage = ShaderStage::Vertex},
        {.path = "passes/DebugLines.slang", .entryPoint = "debugLinePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DisplayEncode.slang", .entryPoint = "displayEncodePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/Bloom.slang", .entryPoint = "bloomPrefilterPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/AmbientOcclusion.slang", .entryPoint = "ambientOcclusionPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/AmbientOcclusionResolve.slang",
         .entryPoint = "ambientOcclusionResolvePixel",
         .stage = ShaderStage::Pixel},
        {.path = "passes/Bloom.slang", .entryPoint = "bloomDownsamplePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/Bloom.slang", .entryPoint = "bloomUpsamplePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/Exposure.slang", .entryPoint = "exposureMeterPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/PostProcess.slang", .entryPoint = "postProcessPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/AntiAliasing.slang", .entryPoint = "antiAliasingPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DebugView.slang", .entryPoint = "debugViewPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/Decal.slang", .entryPoint = "decalVertex", .stage = ShaderStage::Vertex},
        {.path = "passes/Decal.slang", .entryPoint = "decalPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DepthOfField.slang", .entryPoint = "depthOfFieldPrefilterPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DepthOfField.slang", .entryPoint = "depthOfFieldGatherPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DepthOfField.slang", .entryPoint = "depthOfFieldCompositePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DeferredLighting.slang", .entryPoint = "deferredLightingPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DeferredLighting.slang",
         .entryPoint = "deferredLightingPixel",
         .stage = ShaderStage::Pixel,
         .defines = {{"LIGHTING_ONLY", "1"}}},
        {.path = "passes/DeferredLighting.slang",
         .entryPoint = "deferredLightingPixel",
         .stage = ShaderStage::Pixel,
         .defines = {{"REFLECTIONS_ONLY", "1"}}},
        {.path = "passes/DeferredLighting.slang",
         .entryPoint = "deferredLightingPixel",
         .stage = ShaderStage::Pixel,
         .defines = {{"FOG_LIGHT_ONLY", "1"}}},
        {.path = "passes/DeferredLighting.slang",
         .entryPoint = "deferredLightingPixel",
         .stage = ShaderStage::Pixel,
         .defines = {{"PROBES_ONLY", "1"}}},
        {.path = "passes/FogScattering.slang", .entryPoint = "fogScatteringCompute", .stage = ShaderStage::Compute},
        {.path = "passes/FogIntegration.slang", .entryPoint = "fogIntegrationCompute", .stage = ShaderStage::Compute},
        {.path = "passes/LightShafts.slang", .entryPoint = "lightShaftSourcePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/LightShafts.slang", .entryPoint = "lightShaftBlurPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/LightShafts.slang", .entryPoint = "lightShaftCompositePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/ScreenReflectionTrace.slang",
         .entryPoint = "screenReflectionTracePixel",
         .stage = ShaderStage::Pixel},
        {.path = "passes/ScreenReflectionComposite.slang",
         .entryPoint = "screenReflectionCompositePixel",
         .stage = ShaderStage::Pixel},
        {.path = "passes/ScreenReflectionComposite.slang",
         .entryPoint = "screenReflectionCompositePixel",
         .stage = ShaderStage::Pixel,
         .defines = {{"SCREEN_REFLECTIONS_ONLY", "1"}}},
        {.path = "passes/EnvironmentPrefilter.slang",
         .entryPoint = "environmentPrefilterPixel",
         .stage = ShaderStage::Pixel},
        {.path = "passes/EnvironmentPrefilter.slang",
         .entryPoint = "environmentPrefilterPixel",
         .stage = ShaderStage::Pixel,
         .defines = {{"CUBE_SOURCE", "1"}}},
        {.path = "passes/ProbeFace.slang", .entryPoint = "probeFacePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/ProbeDownsample.slang", .entryPoint = "probeDownsamplePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/ProbeHarmonics.slang", .entryPoint = "probeHarmonicsCompute", .stage = ShaderStage::Compute},
        {.path = "passes/MeshDebug.slang", .entryPoint = "meshWireframePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MeshDebug.slang", .entryPoint = "meshOverdrawPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MeshDebug.slang", .entryPoint = "meshLodPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MeshDepth.slang", .entryPoint = "meshDepthMaskedPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MeshDepth.slang",
         .entryPoint = "meshDepthMaskedPixel",
         .stage = ShaderStage::Pixel,
         .defines = {{"MOTION", "1"}}},
        {.path = "passes/MeshForward.slang", .entryPoint = "meshForwardPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MeshGBuffer.slang",
         .entryPoint = "meshGBufferPixel",
         .stage = ShaderStage::Pixel,
         .defines = {{"MOTION", "1"}}},
        {.path = "passes/MeshVertex.slang", .entryPoint = "meshVertex", .stage = ShaderStage::Vertex},
        {.path = "passes/MeshVertex.slang",
         .entryPoint = "meshVertex",
         .stage = ShaderStage::Vertex,
         .defines = {{"SKINNED", "1"}}},
        {.path = "passes/MeshVertex.slang",
         .entryPoint = "meshVertex",
         .stage = ShaderStage::Vertex,
         .defines = {{"MOTION", "1"}}},
        {.path = "passes/MeshVertex.slang",
         .entryPoint = "meshVertex",
         .stage = ShaderStage::Vertex,
         .defines = {{"MOTION", "1"}, {"SKINNED", "1"}}},
        {.path = "passes/MotionBlur.slang", .entryPoint = "motionBlurTilesPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MotionBlur.slang", .entryPoint = "motionBlurNeighboursPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MotionBlur.slang", .entryPoint = "motionBlurPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/TestPattern.slang", .entryPoint = "testPatternPixel", .stage = ShaderStage::Pixel},
        {.path = "ui/ImGui.slang", .entryPoint = "uiVertex", .stage = ShaderStage::Vertex},
        {.path = "ui/ImGui.slang", .entryPoint = "uiPixel", .stage = ShaderStage::Pixel},
    };
    for (ShaderRequest& request : requests) {
        const Status normalized = request.normalize();
        // Written here, and held valid by the tests in every configuration: only asserted.
        CORDITE_ASSERT(normalized.has_value(), "every permutation of the list is a valid request");
        static_cast<void>(normalized);
    }
    std::ranges::sort(requests, [](const ShaderRequest& first, const ShaderRequest& second) {
        return first.describe() < second.describe();
    });
    return requests;
}

} // namespace cordite
