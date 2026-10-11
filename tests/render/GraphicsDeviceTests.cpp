#include "cordite/rhi/GraphicsDevice.h"

#include "cordite/core/StringUtils.h"
#include "cordite/rhi/TextureReadback.h"

#include "render/RenderTestSupport.h"

#include <cstring>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::createTestDevice;
using cordite::test::LogCapture;

namespace {

/// Clears a texture to a color and submits the work.
void clearTexture(GraphicsDevice& graphics, nvrhi::ITexture* texture, const nvrhi::Color& color) {
    const nvrhi::CommandListHandle commandList = graphics.device()->createCommandList();
    commandList->open();
    commandList->clearTextureFloat(texture, nvrhi::AllSubresources, color);
    commandList->close();
    graphics.device()->executeCommandList(commandList);
}

} // namespace

TEST_SUITE("GraphicsDevice") {
    TEST_CASE("a headless device can be created and describes its adapter") {
        const std::unique_ptr<GraphicsDevice> graphics = createTestDevice();

        REQUIRE(graphics->device() != nullptr);
        CHECK_FALSE(graphics->canPresent());

        const AdapterInfo& adapter = graphics->adapter();
        CHECK_FALSE(adapter.name.empty());
        CHECK(adapter.vendorId != 0);
        CHECK(adapter.apiVersion.rfind("1.", 0) == 0);
        CHECK_FALSE(nameOf(adapter.type).empty());
        MESSAGE("adapter: " << adapter.name << " (" << nameOf(adapter.type) << ", Vulkan " << adapter.apiVersion
                            << "), Vulkan validation layer: " << (graphics->hasVulkanValidation() ? "on" : "off"));

        checkNoValidationIssues(*graphics);
    }

    TEST_CASE("an adapter preference that matches nothing still yields a device") {
        GraphicsDeviceDesc desc;
        desc.preferredAdapter = "no adapter has this name";
        const Result<std::unique_ptr<GraphicsDevice>> graphics = GraphicsDevice::create(desc);
        REQUIRE_MESSAGE(graphics.has_value(), graphics.error().message);
        CHECK_FALSE((*graphics)->adapter().name.empty());
    }

    TEST_CASE("an adapter can be selected by a case-insensitive part of its name") {
        const std::string name = createTestDevice()->adapter().name;
        REQUIRE(name.size() >= 3);

        GraphicsDeviceDesc desc;
        desc.preferredAdapter = toLowerAscii(name.substr(0, 3));
        const Result<std::unique_ptr<GraphicsDevice>> graphics = GraphicsDevice::create(desc);
        REQUIRE(graphics.has_value());
        CHECK(toLowerAscii((*graphics)->adapter().name).find(desc.preferredAdapter) != std::string::npos);
    }

    TEST_CASE("asking for a presentation extension the runtime lacks fails cleanly") {
        GraphicsDeviceDesc desc;
        desc.presentationExtensions = {"VK_CORDITE_extension_that_does_not_exist"};
        const Result<std::unique_ptr<GraphicsDevice>> graphics = GraphicsDevice::create(desc);
        REQUIRE_FALSE(graphics.has_value());
        CHECK(graphics.error().code == ErrorCode::Unsupported);
        CHECK(graphics.error().message.find("VK_CORDITE_extension_that_does_not_exist") != std::string::npos);
    }

    TEST_CASE("only one device can exist at a time") {
        const std::unique_ptr<GraphicsDevice> first = createTestDevice();

        const Result<std::unique_ptr<GraphicsDevice>> second = GraphicsDevice::create({});
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == ErrorCode::InvalidState);

        // The refused attempt left the existing device fully usable.
        const nvrhi::TextureHandle texture = test::createColorTarget(first->device(), 8, 8);
        clearTexture(*first, texture, nvrhi::Color(1.0f, 0.0f, 0.0f, 1.0f));
        CHECK(test::readToImage(*first, texture).pixel(0, 0) == Vec4(1.0f, 0.0f, 0.0f, 1.0f));
        checkNoValidationIssues(*first);
    }

    TEST_CASE("devices can be created one after another, with different settings") {
        for (const bool validation : {true, false, true}) {
            GraphicsDeviceDesc desc;
            desc.validation = validation;
            const Result<std::unique_ptr<GraphicsDevice>> graphics = GraphicsDevice::create(desc);
            REQUIRE_MESSAGE(graphics.has_value(), graphics.error().message);

            const nvrhi::TextureHandle texture = test::createColorTarget((*graphics)->device(), 8, 8);
            clearTexture(**graphics, texture, nvrhi::Color(0.0f, 1.0f, 0.0f, 1.0f));
            CHECK(test::readToImage(**graphics, texture).pixel(7, 7) == Vec4(0.0f, 1.0f, 0.0f, 1.0f));
            CHECK((*graphics)->validationIssueCount() == 0);
        }
    }

    TEST_CASE("a failed creation does not block the next one") {
        GraphicsDeviceDesc broken;
        broken.presentationExtensions = {"VK_CORDITE_extension_that_does_not_exist"};
        REQUIRE_FALSE(GraphicsDevice::create(broken).has_value());

        const std::unique_ptr<GraphicsDevice> graphics = createTestDevice();
        CHECK(graphics->device() != nullptr);
    }

    TEST_CASE("validation reports misuse of the device") {
        // The other tests assert that validation stays silent. That only means something
        // if a real mistake is actually caught, which this test establishes.
        const std::unique_ptr<GraphicsDevice> graphics = createTestDevice();
        CHECK(graphics->validationIssueCount() == 0);

        LogCapture capture(LogLevel::Error);
        nvrhi::TextureDesc invalid;
        invalid.width = 0;
        invalid.height = 16;
        invalid.format = nvrhi::Format::RGBA8_UNORM;
        invalid.debugName = "Deliberately invalid";
        const nvrhi::TextureHandle texture = graphics->device()->createTexture(invalid);

        CHECK_FALSE(texture);
        REQUIRE(graphics->validationIssueCount() == 1);
        CHECK(graphics->validationIssues()[0].find("must not be zero") != std::string::npos);
        CHECK(capture.count(LogLevel::Error) == 1);

        graphics->clearValidationIssues();
        CHECK(graphics->validationIssueCount() == 0);
    }

    TEST_CASE("collectGarbage and waitIdle are safe on an idle device") {
        const std::unique_ptr<GraphicsDevice> graphics = createTestDevice();
        CHECK_NOTHROW(graphics->collectGarbage());
        CHECK_NOTHROW(graphics->waitIdle());
        CHECK_NOTHROW(graphics->collectGarbage());
        checkNoValidationIssues(*graphics);
    }

    TEST_CASE("after waitIdle, everything that was submitted counts as finished") {
        const std::unique_ptr<GraphicsDevice> graphics = createTestDevice();
        nvrhi::IDevice* device = graphics->device();
        {
            const nvrhi::TextureHandle target = test::createColorTarget(device, 1024, 1024);
            const nvrhi::EventQueryHandle finished = device->createEventQuery();
            // A driver may take note a moment late that its last piece of work is done.
            // Many rounds, so that a wait which leaves that gap open is caught here and
            // not once in a while by whichever test polls for a result first.
            for (int round = 0; round < 200; ++round) {
                const nvrhi::CommandListHandle commandList = device->createCommandList();
                commandList->open();
                commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.5f));
                commandList->close();
                device->executeCommandList(commandList);
                device->resetEventQuery(finished);
                device->setEventQuery(finished, nvrhi::CommandQueue::Graphics);

                graphics->waitIdle();
                CAPTURE(round);
                REQUIRE(device->pollEventQuery(finished));
            }
        }
        checkNoValidationIssues(*graphics);
    }
}

TEST_SUITE("TextureReadback") {
    TEST_CASE("a cleared texture reads back the clear color in every pixel") {
        const std::unique_ptr<GraphicsDevice> graphics = createTestDevice();
        const nvrhi::TextureHandle texture = test::createColorTarget(graphics->device(), 37, 19);
        clearTexture(*graphics, texture, nvrhi::Color(0.2f, 0.4f, 0.6f, 1.0f));

        const Result<TexturePixels> pixels = readTexture(*graphics, texture);
        REQUIRE_MESSAGE(pixels.has_value(), pixels.error().message);
        CHECK(pixels->width == 37);
        CHECK(pixels->height == 19);
        CHECK(pixels->bytesPerPixel == 4);
        CHECK(pixels->format == nvrhi::Format::RGBA8_UNORM);
        // An odd width exercises the removal of row padding.
        REQUIRE(pixels->data.size() == 37u * 19u * 4u);

        const std::byte expected[4] = {std::byte{51}, std::byte{102}, std::byte{153}, std::byte{255}};
        bool allMatch = true;
        for (size_t offset = 0; offset < pixels->data.size(); offset += 4) {
            allMatch = allMatch && std::memcmp(pixels->data.data() + offset, expected, 4) == 0;
        }
        CHECK(allMatch);
        checkNoValidationIssues(*graphics);
    }

    TEST_CASE("float textures read back with full precision") {
        const std::unique_ptr<GraphicsDevice> graphics = createTestDevice();
        const nvrhi::TextureHandle texture =
            test::createColorTarget(graphics->device(), 4, 4, nvrhi::Format::RGBA32_FLOAT);
        clearTexture(*graphics, texture, nvrhi::Color(12.5f, -3.0f, 0.001f, 2.0f));

        const Result<TexturePixels> pixels = readTexture(*graphics, texture);
        REQUIRE_MESSAGE(pixels.has_value(), pixels.error().message);
        REQUIRE(pixels->bytesPerPixel == 16);

        float values[4] = {};
        std::memcpy(values, pixels->data.data() + 16 * 5, sizeof(values));
        CHECK(values[0] == 12.5f);
        CHECK(values[1] == -3.0f);
        CHECK(values[2] == 0.001f);
        CHECK(values[3] == 2.0f);
        checkNoValidationIssues(*graphics);
    }

    TEST_CASE("a mip level reads back at its own size") {
        const std::unique_ptr<GraphicsDevice> graphics = createTestDevice();

        nvrhi::TextureDesc desc;
        desc.width = 16;
        desc.height = 8;
        desc.mipLevels = 3;
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.debugName = "Mipped target";
        desc.isRenderTarget = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        const nvrhi::TextureHandle texture = graphics->device()->createTexture(desc);
        REQUIRE(texture);
        clearTexture(*graphics, texture, nvrhi::Color(1.0f, 1.0f, 1.0f, 1.0f));

        const Result<TexturePixels> mip2 = readTexture(*graphics, texture, 2);
        REQUIRE_MESSAGE(mip2.has_value(), mip2.error().message);
        CHECK(mip2->width == 4);
        CHECK(mip2->height == 2);
        CHECK(mip2->data.size() == 4u * 2u * 4u);
        CHECK(mip2->data[0] == std::byte{255});
        checkNoValidationIssues(*graphics);
    }

    TEST_CASE("invalid readback requests are errors") {
        const std::unique_ptr<GraphicsDevice> graphics = createTestDevice();
        const nvrhi::TextureHandle texture = test::createColorTarget(graphics->device(), 8, 8);

        const Result<TexturePixels> null = readTexture(*graphics, nullptr);
        REQUIRE_FALSE(null.has_value());
        CHECK(null.error().code == ErrorCode::InvalidArgument);

        const Result<TexturePixels> badMip = readTexture(*graphics, texture, 5);
        REQUIRE_FALSE(badMip.has_value());
        CHECK(badMip.error().code == ErrorCode::OutOfRange);

        const Result<TexturePixels> badSlice = readTexture(*graphics, texture, 0, 3);
        REQUIRE_FALSE(badSlice.has_value());
        CHECK(badSlice.error().code == ErrorCode::OutOfRange);

        nvrhi::TextureDesc depthDesc;
        depthDesc.width = 8;
        depthDesc.height = 8;
        depthDesc.format = nvrhi::Format::D32;
        depthDesc.debugName = "Depth";
        depthDesc.isRenderTarget = true;
        depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
        depthDesc.keepInitialState = true;
        const nvrhi::TextureHandle depth = graphics->device()->createTexture(depthDesc);
        REQUIRE(depth);
        const Result<TexturePixels> depthRead = readTexture(*graphics, depth);
        REQUIRE_FALSE(depthRead.has_value());
        CHECK(depthRead.error().code == ErrorCode::Unsupported);

        checkNoValidationIssues(*graphics);
    }
}
