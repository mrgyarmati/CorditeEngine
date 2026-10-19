#include "cordite/rhi/GraphicsDevice.h"

#include "cordite/core/Base.h"
#include "cordite/core/StringUtils.h"
#include "cordite/rhi/TextureReadback.h"

#include "render/RenderTestSupport.h"

#if CORDITE_PLATFORM_WINDOWS
#include <directx/d3d12.h>
#include <wrl/client.h>
#endif

#include <cstring>
#include <string>
#include <string_view>
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
        // Of Vulkan: what Direct3D 12 says of itself is tested below.
        const std::unique_ptr<GraphicsDevice> graphics = test::unwrap(GraphicsDevice::create({.validation = true}));

        REQUIRE(graphics->device() != nullptr);
        CHECK_FALSE(graphics->canPresent());

        const AdapterInfo& adapter = graphics->adapter();
        CHECK_FALSE(adapter.name.empty());
        CHECK(adapter.vendorId != 0);
        CHECK(adapter.apiVersion.rfind("1.", 0) == 0);
        CHECK_FALSE(nameOf(adapter.type).empty());
        MESSAGE("adapter: " << adapter.name << " (" << nameOf(adapter.type) << ", Vulkan " << adapter.apiVersion
                            << "), Vulkan validation layer: "
                            << std::string(graphics->hasApiValidation() ? "on" : "off"));

        checkNoValidationIssues(*graphics);
    }

    TEST_CASE("a device draws with Vulkan unless it is told to draw with another graphics API") {
        CHECK(test::unwrap(GraphicsDevice::create({.validation = true}))->backend() == GraphicsBackend::Vulkan);
        const Result<std::unique_ptr<GraphicsDevice>> vulkan =
            GraphicsDevice::create({.backend = GraphicsBackend::Vulkan});
        REQUIRE_MESSAGE(vulkan.has_value(), test::errorOf(vulkan));
        CHECK((*vulkan)->backend() == GraphicsBackend::Vulkan);
        CHECK(nameOf(GraphicsBackend::Auto) == "Auto");
        CHECK(nameOf(GraphicsBackend::Vulkan) == "Vulkan");
        CHECK(nameOf(GraphicsBackend::Direct3D12) == "Direct3D 12");
    }

#if CORDITE_PLATFORM_WINDOWS
    TEST_CASE("a device of Direct3D 12 draws offscreen, on the GPU and on WARP") {
        // The most capable adapter, and the rasterizer of Direct3D 12 that runs on the CPU.
        for (const char* preferred : {"", "Basic Render"}) {
            CAPTURE(std::string(preferred));
            const Result<std::unique_ptr<GraphicsDevice>> created = GraphicsDevice::create(
                {.backend = GraphicsBackend::Direct3D12, .validation = true, .preferredAdapter = preferred});
            REQUIRE_MESSAGE(created.has_value(), test::errorOf(created));
            GraphicsDevice& graphics = **created;
            CHECK(graphics.backend() == GraphicsBackend::Direct3D12);
            CHECK(graphics.device()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12);
            // A window is drawn into with Vulkan alone, so far.
            CHECK_FALSE(graphics.canPresent());

            const AdapterInfo& adapter = graphics.adapter();
            CHECK_FALSE(adapter.name.empty());
            CHECK(adapter.vendorId != 0);
            CHECK(adapter.apiVersion.find(", shader model 6.") != std::string::npos);
            CHECK(adapter.supportsTimerQueries);
            CHECK(adapter.supportsWireframe);
            if (std::string_view(preferred) == "Basic Render") {
                CHECK(adapter.type == AdapterType::Software);
                CHECK(adapter.name == "Microsoft Basic Render Driver");
            }
            MESSAGE("adapter: " << adapter.name << " (" << nameOf(adapter.type) << ", Direct3D 12 at "
                                << adapter.apiVersion
                                << "), debug layer: " << std::string(graphics.hasApiValidation() ? "on" : "off"));

            const nvrhi::TextureHandle texture = test::createColorTarget(graphics.device(), 8, 8);
            clearTexture(graphics, texture, nvrhi::Color(1.0f, 0.0f, 1.0f, 1.0f));
            CHECK(test::readToImage(graphics, texture).pixel(3, 5) == Vec4(1.0f, 0.0f, 1.0f, 1.0f));
            graphics.collectGarbage();
            graphics.waitIdle();
            checkNoValidationIssues(graphics);
        }
        // And Vulkan again after it.
        CHECK(test::unwrap(GraphicsDevice::create({.validation = true}))->backend() == GraphicsBackend::Vulkan);
    }

    TEST_CASE("the debug layer of Direct3D 12 reports misuse, where Windows has it") {
        const Result<std::unique_ptr<GraphicsDevice>> created =
            GraphicsDevice::create({.backend = GraphicsBackend::Direct3D12, .validation = true});
        REQUIRE_MESSAGE(created.has_value(), test::errorOf(created));
        GraphicsDevice& graphics = **created;
        if (!graphics.hasApiValidation()) {
            MESSAGE("the debug layer of Direct3D 12 is not installed here: nothing to hear");
            return;
        }
        // NVRHI checks what it is given before Direct3D 12 sees it, so the mistake is made
        // behind its back, on the device itself: a buffer of no bytes.
        auto* device = static_cast<ID3D12Device*>(graphics.device()->getNativeObject(nvrhi::ObjectTypes::D3D12_Device));
        REQUIRE(device != nullptr);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 0;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        const LogCapture capture(LogLevel::Error);
        CHECK(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&resource))));

        const std::vector<std::string> issues = graphics.validationIssues();
        REQUIRE_FALSE(issues.empty());
        CHECK(issues.front().rfind("Direct3D 12 error: ", 0) == 0);
        CHECK(capture.count(LogLevel::Error) >= 1);
        // Heard once: what is collected is not collected again.
        CHECK(graphics.validationIssueCount() == issues.size());
        graphics.clearValidationIssues();
        CHECK(graphics.validationIssueCount() == 0);
    }
#else
    TEST_CASE("Direct3D 12 is refused where Windows is not, and a device can be made after") {
        const Result<std::unique_ptr<GraphicsDevice>> refused =
            GraphicsDevice::create({.backend = GraphicsBackend::Direct3D12});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ErrorCode::Unsupported);
        CHECK(refused.error().message.find("Windows") != std::string::npos);
        CHECK(test::unwrap(GraphicsDevice::create({.validation = true}))->backend() == GraphicsBackend::Vulkan);
    }
#endif

    TEST_CASE("an adapter preference that matches nothing still yields a device") {
        GraphicsDeviceDesc desc;
        desc.preferredAdapter = "no adapter has this name";
        const Result<std::unique_ptr<GraphicsDevice>> graphics = GraphicsDevice::create(desc);
        REQUIRE_MESSAGE(graphics.has_value(), test::errorOf(graphics));
        CHECK_FALSE((*graphics)->adapter().name.empty());
    }

    TEST_CASE("an adapter can be selected by a case-insensitive part of its name") {
        const std::string name = createTestDevice()->adapter().name;
        REQUIRE(name.size() >= 3);

        // Among the adapters of the same graphics API.
        GraphicsDeviceDesc desc;
        desc.backend = test::testGraphics().backend;
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
            REQUIRE_MESSAGE(graphics.has_value(), test::errorOf(graphics));

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
        REQUIRE_MESSAGE(pixels.has_value(), test::errorOf(pixels));
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
        REQUIRE_MESSAGE(pixels.has_value(), test::errorOf(pixels));
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
        REQUIRE_MESSAGE(mip2.has_value(), test::errorOf(mip2));
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
