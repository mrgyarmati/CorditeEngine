#include "render/RenderTestSupport.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Platform.h"
#include "cordite/shader_compiler/EngineShaderCache.h"

#include <fmt/format.h>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cordite::test {
namespace {

std::filesystem::path referenceDirectory() {
    return sourceDirectory() / "tests" / "render" / "references";
}

std::filesystem::path outputDirectory() {
    return sourceDirectory() / "tests" / "render" / "output";
}

bool shouldUpdateReferences() {
    const std::optional<std::string> value = environmentVariable("CORDITE_UPDATE_REFERENCES");
    return value.has_value() && *value != "0" && !value->empty();
}

} // namespace

std::filesystem::path shaderDirectory() {
    return sourceDirectory() / "shaders";
}

const std::filesystem::path& compiledShaderCache() {
    // Compiling every permutation takes seconds; once a run is enough.
    static const TempDirectory directory;
    static const bool compiled = [] {
        const Status status = compileEngineShaders(shaderDirectory(), directory.path() / "shaders");
        REQUIRE_MESSAGE(status.has_value(), errorOf(status));
        return true;
    }();
    static_cast<void>(compiled);
    return directory.path();
}

std::unique_ptr<GraphicsDevice> createTestDevice() {
    GraphicsDeviceDesc desc;
    desc.applicationName = "Cordite tests";
    desc.validation = true;

    Result<std::unique_ptr<GraphicsDevice>> device = GraphicsDevice::create(desc);
    REQUIRE_MESSAGE(device.has_value(), test::errorOf(device));
    return std::move(*device);
}

void checkNoValidationIssues(const GraphicsDevice& device) {
    for (const std::string& issue : device.validationIssues()) {
        FAIL_CHECK("GPU validation issue: " << issue);
    }
}

RenderContext::RenderContext() : graphics(createTestDevice()) {
    Result<std::unique_ptr<SlangShaderCompiler>> created =
        SlangShaderCompiler::create({.sourceRoots = {shaderDirectory()}});
    REQUIRE_MESSAGE(created.has_value(), test::errorOf(created));
    compiler = std::move(*created);

    ShaderLibraryDesc libraryDesc;
    libraryDesc.sourceRoots = {shaderDirectory()};
    libraryDesc.compiler = compiler.get();
    library = std::make_unique<ShaderLibrary>(std::move(libraryDesc));
    shaders = std::make_unique<ShaderManager>(graphics->device(), *library);
}

RenderContext::~RenderContext() {
    // What was made with the device is gone by now, the fixtures of the test with it;
    // whatever the validation layers said while it went - and since the test last
    // asked - fails the test like anything they said before.
    if (graphics) {
        graphics->waitIdle();
        checkNoValidationIssues(*graphics);
    }
    // GPU shaders must be released before the device that created them.
    shaders.reset();
    library.reset();
    compiler.reset();
    graphics.reset();
}

void recordAndExecute(RenderContext& context, const std::function<void(nvrhi::ICommandList*)>& record) {
    const nvrhi::CommandListHandle commandList = context.device()->createCommandList();
    REQUIRE(commandList);
    commandList->open();
    record(commandList);
    commandList->close();
    context.device()->executeCommandList(commandList);
}

nvrhi::TextureHandle createColorTarget(nvrhi::IDevice* device, uint32_t width, uint32_t height, nvrhi::Format format) {
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.debugName = "Test color target";
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;

    nvrhi::TextureHandle texture = device->createTexture(desc);
    REQUIRE(texture);
    return texture;
}

Vec3 HalfPicture::at(uint32_t x, uint32_t y) const {
    REQUIRE(pixels.format == nvrhi::Format::RGBA16_FLOAT);
    REQUIRE(x < pixels.width);
    REQUIRE(y < pixels.height);
    std::array<uint16_t, 3> halves{};
    std::memcpy(halves.data(), pixels.data.data() + (static_cast<size_t>(y) * pixels.width + x) * 8, sizeof(halves));
    return {glm::unpackHalf1x16(halves[0]), glm::unpackHalf1x16(halves[1]), glm::unpackHalf1x16(halves[2])};
}

Image readToImage(GraphicsDevice& device, nvrhi::ITexture* texture) {
    Result<TexturePixels> pixels = readTexture(device, texture);
    REQUIRE_MESSAGE(pixels.has_value(), test::errorOf(pixels));
    REQUIRE(pixels->bytesPerPixel == 4);

    if (pixels->format == nvrhi::Format::BGRA8_UNORM || pixels->format == nvrhi::Format::SBGRA8_UNORM) {
        for (size_t offset = 0; offset + 3 < pixels->data.size(); offset += 4) {
            std::swap(pixels->data[offset], pixels->data[offset + 2]);
        }
    }

    Result<Image> image = Image::fromPixels(pixels->width, pixels->height, ImageFormat::Rgba8, std::move(pixels->data));
    REQUIRE_MESSAGE(image.has_value(), test::errorOf(image));
    return std::move(*image);
}

std::optional<std::string> firstDifference(std::string_view expected, std::string_view actual) {
    const auto linesOf = [](std::string_view text) {
        std::vector<std::string> lines;
        std::string line;
        for (const char character : text) {
            if (character == '\n') {
                lines.push_back(std::move(line));
                line.clear();
            } else if (character != '\r') {
                line += character;
            }
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        return lines;
    };
    const std::vector<std::string> wanted = linesOf(expected);
    const std::vector<std::string> written = linesOf(actual);
    for (size_t index = 0; index < std::min(wanted.size(), written.size()); ++index) {
        if (wanted[index] != written[index]) {
            return fmt::format("line {}: the file has \"{}\", the engine \"{}\"", index + 1, wanted[index],
                               written[index]);
        }
    }
    if (wanted.size() != written.size()) {
        return fmt::format("the file has {} lines, the engine {}", wanted.size(), written.size());
    }
    return std::nullopt;
}

void checkTextAgainstReference(std::string_view text, const std::filesystem::path& file) {
    const std::filesystem::path path = sourceDirectory() / file;
    if (shouldUpdateReferences()) {
        REQUIRE_MESSAGE(writeTextFile(path, text).has_value(), "cannot write " << pathToUtf8(path));
        MESSAGE("wrote " << pathToUtf8(file) << ": read it before it is committed");
        return;
    }
    const Result<std::string> existing = readTextFile(path);
    REQUIRE_MESSAGE(existing.has_value(),
                    pathToUtf8(file) << " is not there: run the test with CORDITE_UPDATE_REFERENCES=1 to write it");
    const std::optional<std::string> difference = firstDifference(existing.value_or(std::string()), text);
    CHECK_MESSAGE(!difference.has_value(),
                  pathToUtf8(file) << " is not what the engine writes (" << difference.value_or(std::string())
                                   << "): run the test with CORDITE_UPDATE_REFERENCES=1, read the change, "
                                      "and commit it");
}

void checkAgainstReference(const Image& actual, std::string_view name, const ImageCompareOptions& options) {
    const std::string fileName = std::string(name) + ".png";
    const std::filesystem::path referencePath = referenceDirectory() / pathFromUtf8(fileName);

    if (shouldUpdateReferences()) {
        const Status saved = actual.savePng(referencePath);
        REQUIRE_MESSAGE(saved.has_value(), test::errorOf(saved));
        MESSAGE("updated reference image " << pathToUtf8(referencePath));
        return;
    }

    const Result<Image> expected = Image::load(referencePath);
    if (!expected) {
        const std::filesystem::path actualPath = outputDirectory() / pathFromUtf8(std::string(name) + ".actual.png");
        const Status saved = actual.savePng(actualPath);
        const std::string location = saved ? pathToUtf8(actualPath) : "(not saved: " + saved.error().message + ")";
        FAIL("no reference image for '" << name << "' (" << expected.error().message << "). The rendered image is at "
                                        << location
                                        << ". Review it, then rerun with CORDITE_UPDATE_REFERENCES=1 to approve it.");
        return;
    }

    const Result<ImageComparison> comparison = compareImages(actual, *expected, options);
    REQUIRE_MESSAGE(comparison.has_value(), test::errorOf(comparison));
    if (comparison->matches) {
        return;
    }

    const std::filesystem::path actualPath = outputDirectory() / pathFromUtf8(std::string(name) + ".actual.png");
    const std::filesystem::path diffPath = outputDirectory() / pathFromUtf8(std::string(name) + ".diff.png");
    const Status savedActual = actual.savePng(actualPath);
    const Result<Image> difference = visualizeDifference(actual, *expected, options.channelTolerance);
    const Status savedDifference =
        difference ? difference->savePng(diffPath) : Status(tl::unexpected(difference.error()));
    const std::string outputNote = savedActual && savedDifference ? "" : " (writing the output images failed)";

    FAIL_CHECK("'" << name << "' does not match its reference: " << comparison->differingPixels << " of "
                   << comparison->totalPixels << " pixels differ (" << comparison->differingRatio() * 100.0
                   << "%, allowed " << options.maxDifferingRatio * 100.0 << "%), largest channel difference "
                   << comparison->maxChannelDifference << ". Compare " << pathToUtf8(referencePath) << " with "
                   << pathToUtf8(actualPath) << "; differences are marked in " << pathToUtf8(diffPath) << outputNote);
}

} // namespace cordite::test
