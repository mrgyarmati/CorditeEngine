#pragma once

/// @file
/// Fixtures for tests that render on a real GPU.
///
/// Every device these tests create has validation enabled, and every test ends by
/// checking that the validation layers stayed silent. Rendered frames are compared
/// against reference images in tests/render/references/. To approve new or changed
/// output, run the suite with CORDITE_UPDATE_REFERENCES=1 and review the images it
/// writes before committing them.

#include "cordite/assets/Image.h"
#include "cordite/assets/ImageCompare.h"
#include "cordite/renderer/ShaderManager.h"
#include "cordite/rhi/GraphicsDevice.h"
#include "cordite/rhi/TextureReadback.h"
#include "cordite/shader_compiler/SlangShaderCompiler.h"
#include "cordite/shaders/ShaderLibrary.h"

#include "support/TestSupport.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace cordite::test {

/// The engine's shader sources in the repository.
std::filesystem::path shaderDirectory();

/// Creates a headless device with validation on. Fails the test when the machine has no
/// usable GPU.
std::unique_ptr<GraphicsDevice> createTestDevice();

/// Fails the test if the validation layers reported anything, listing what they said.
void checkNoValidationIssues(const GraphicsDevice& device);

/// A device plus everything needed to load shaders on it. When it goes, it fails the
/// test for any validation issue there is: what happens while the things a test made
/// are taken down - a texture destroyed while the GPU still uses it - counts.
struct RenderContext {
    RenderContext();
    ~RenderContext();
    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;

    [[nodiscard]] nvrhi::IDevice* device() const { return graphics->device(); }

    std::unique_ptr<GraphicsDevice> graphics;
    std::unique_ptr<SlangShaderCompiler> compiler;
    std::unique_ptr<ShaderLibrary> library;
    std::unique_ptr<ShaderManager> shaders;
};

/// Opens a new command list, lets `record` fill it, and executes it.
void recordAndExecute(RenderContext& context, const std::function<void(nvrhi::ICommandList*)>& record);

/// A color texture that can be rendered to, sampled and read back.
nvrhi::TextureHandle createColorTarget(nvrhi::IDevice* device, uint32_t width, uint32_t height,
                                       nvrhi::Format format = nvrhi::Format::RGBA8_UNORM);

/// Reads a color texture into an Rgba8 image. BGRA textures are converted.
Image readToImage(GraphicsDevice& device, nvrhi::ITexture* texture);

/// Compares a rendered image with tests/render/references/<name>.png. On a mismatch the
/// actual image and a difference visualization are written to tests/render/output/.
void checkAgainstReference(const Image& actual, std::string_view name,
                           const ImageCompareOptions& options = {.channelTolerance = 4, .maxDifferingRatio = 0.002});

} // namespace cordite::test
