#pragma once

/// @file
/// Fixtures for tests that render on a real GPU.
///
/// Every device these tests create has validation enabled, and every test ends by
/// checking that the validation layers stayed silent. Rendered frames are compared
/// against reference images in tests/render/references/. To approve new or changed
/// output, run the suite with CORDITE_UPDATE_REFERENCES=1 and review the images it
/// writes before committing them. The pages of docs/reference are held to what the engine
/// writes the same way (checkTextAgainstReference).

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
#include <optional>
#include <string>
#include <string_view>

namespace cordite::test {

/// The engine's shader sources in the repository.
std::filesystem::path shaderDirectory();

/// A cache directory with the engine's shaders compiled ahead into it (`<it>/shaders`,
/// compileEngineShaders()), as a game that is exported has them: compiled the first time a
/// test asks for it, and kept until the tests end.
const std::filesystem::path& compiledShaderCache();

/// The cache directory that what compiles shaders in the tests of a run shares: the
/// RenderContext of every test and the engines and editors of the fixtures
/// (headlessDesc()) keep in `<it>/shaders` what they compile, and read there what a test
/// before them compiled - compiled anew for every test, the shaders were most of the time
/// a test on the GPU took. Made the first time it is asked for, and taken away when the
/// tests end. A test that counts what is compiled, or wants no cache, gives its engine a
/// cache directory of its own, or none.
const std::filesystem::path& sharedCacheDirectory();

/// The graphics API the GPU tests draw with, and the adapter they ask for.
struct TestGraphics {
    GraphicsBackend backend = GraphicsBackend::Vulkan;
    std::string preferredAdapter;
};

/// What the environment variable CORDITE_GRAPHICS asks the GPU tests to draw with: unset
/// or "vulkan", Vulkan; "d3d12", Direct3D 12; "warp", Direct3D 12 on WARP, its rasterizer
/// that runs on the CPU. Fails the test for a name it does not know. Every device and
/// engine of the tests is made with it, but those of the tests that are about one API.
[[nodiscard]] TestGraphics testGraphics();

/// Creates a headless device with validation on, of testGraphics(). Fails the test when
/// the machine has no usable GPU.
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

/// A texture of four half floats a texel (RGBA16_FLOAT), read back: light as it was worked
/// out, before anything made a picture of it.
struct HalfPicture {
    TexturePixels pixels;

    /// The red, green and blue of the texel at `x`, `y`. Fails the test for a texture of
    /// another format.
    [[nodiscard]] Vec3 at(uint32_t x, uint32_t y) const;
};

/// Compares a rendered image with tests/render/references/<name>.png. On a mismatch the
/// actual image and a difference visualization are written to tests/render/output/.
void checkAgainstReference(const Image& actual, std::string_view name,
                           const ImageCompareOptions& options = {.channelTolerance = 4, .maxDifferingRatio = 0.002});

/// Where two texts first differ, for a person to find it: the line, with what the file has
/// there (`expected`) and what the engine wrote (`actual`), or how many lines each has.
/// Nullopt when they are the same, but for carriage returns, which Git may put into files.
[[nodiscard]] std::optional<std::string> firstDifference(std::string_view expected, std::string_view actual);

/// Compares a text with the file of the repository it is meant to be - `file`, from the
/// root of the repository: a page of docs/reference that the engine writes. With
/// CORDITE_UPDATE_REFERENCES=1 the file is written instead, to be read and committed.
void checkTextAgainstReference(std::string_view text, const std::filesystem::path& file);

} // namespace cordite::test
