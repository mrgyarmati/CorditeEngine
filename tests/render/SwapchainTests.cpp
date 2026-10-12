#include "cordite/rhi/Swapchain.h"

#include "cordite/platform/Window.h"

#include "render/RenderTestSupport.h"

#include <memory>

using namespace cordite;
using cordite::test::checkNoValidationIssues;

namespace {

/// A hidden window with a device that can present to it. The window is never shown, so
/// running the tests does not disturb the desktop.
struct WindowedContext {
    WindowedContext() {
        Result<std::unique_ptr<WindowSystem>> system = WindowSystem::create();
        REQUIRE_MESSAGE(system.has_value(), test::errorOf(system));
        windowSystem = std::move(*system);

        Result<std::unique_ptr<Window>> created = windowSystem->createWindow(
            {.title = "Cordite swapchain test", .width = 320, .height = 200, .visible = false});
        REQUIRE_MESSAGE(created.has_value(), test::errorOf(created));
        window = std::move(*created);

        GraphicsDeviceDesc desc;
        desc.validation = true;
        desc.presentationExtensions = windowSystem->vulkanInstanceExtensions();
        REQUIRE_FALSE(desc.presentationExtensions.empty());
        Result<std::unique_ptr<GraphicsDevice>> device = GraphicsDevice::create(desc);
        REQUIRE_MESSAGE(device.has_value(), test::errorOf(device));
        graphics = std::move(*device);
        commandList = graphics->device()->createCommandList();
    }

    /// Clears the back buffer and presents it.
    void renderFrame(Swapchain& swapchain, const nvrhi::Color& color) {
        windowSystem->pollEvents();
        const Result<SwapchainFrame> frame = swapchain.beginFrame();
        REQUIRE_MESSAGE(frame.has_value(), test::errorOf(frame));
        REQUIRE(frame->texture != nullptr);
        REQUIRE(frame->framebuffer != nullptr);

        commandList->open();
        commandList->clearTextureFloat(frame->texture, nvrhi::AllSubresources, color);
        commandList->close();
        graphics->device()->executeCommandList(commandList);

        const Status presented = swapchain.present();
        REQUIRE_MESSAGE(presented.has_value(), test::errorOf(presented));
        graphics->collectGarbage();
    }

    /// Creates a swapchain for the window, failing the test if that is not possible.
    [[nodiscard]] std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc = {}) const {
        Result<std::unique_ptr<Swapchain>> swapchain = Swapchain::create(*graphics, *window, desc);
        REQUIRE_MESSAGE(swapchain.has_value(), test::errorOf(swapchain));
        return std::move(*swapchain);
    }

    std::unique_ptr<WindowSystem> windowSystem;
    std::unique_ptr<Window> window;
    std::unique_ptr<GraphicsDevice> graphics;
    nvrhi::CommandListHandle commandList;
};

} // namespace

TEST_SUITE("Swapchain") {
    TEST_CASE("frames can be acquired, rendered and presented") {
        WindowedContext context;
        CHECK(context.graphics->canPresent());

        Result<std::unique_ptr<Swapchain>> swapchain = Swapchain::create(*context.graphics, *context.window);
        REQUIRE_MESSAGE(swapchain.has_value(), test::errorOf(swapchain));

        CHECK((*swapchain)->size() == context.window->framebufferSize());
        const nvrhi::Format format = (*swapchain)->format();
        CHECK((format == nvrhi::Format::BGRA8_UNORM || format == nvrhi::Format::RGBA8_UNORM));
        CHECK((*swapchain)->vsync());

        for (int frame = 0; frame < 8; ++frame) {
            context.renderFrame(**swapchain, nvrhi::Color(0.1f * static_cast<float>(frame), 0.2f, 0.3f, 1.0f));
        }
        CHECK((*swapchain)->recreationCount() == 0);

        swapchain->reset();
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the frame describes the back buffer") {
        WindowedContext context;
        std::unique_ptr<Swapchain> swapchain = context.createSwapchain();

        context.windowSystem->pollEvents();
        const Result<SwapchainFrame> frame = swapchain->beginFrame();
        REQUIRE(frame.has_value());
        REQUIRE(frame->texture != nullptr);
        CHECK(frame->size == swapchain->size());
        CHECK(frame->texture->getDesc().width == swapchain->size().x);
        CHECK(frame->texture->getDesc().height == swapchain->size().y);
        CHECK(frame->texture->getDesc().format == swapchain->format());
        CHECK(frame->framebuffer->getFramebufferInfo().width == swapchain->size().x);

        // Presenting a frame nothing was drawn into is allowed.
        REQUIRE(swapchain->present().has_value());
        context.graphics->collectGarbage();

        swapchain.reset();
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("resizing the window recreates the swapchain at the new size") {
        WindowedContext context;
        std::unique_ptr<Swapchain> swapchain = context.createSwapchain();
        context.renderFrame(*swapchain, nvrhi::Color(1.0f, 0.0f, 0.0f, 1.0f));
        const UVec2 originalSize = swapchain->size();

        for (const UVec2 size : {UVec2(480, 270), UVec2(200, 400), UVec2(320, 200)}) {
            context.window->setLogicalSize(size);
            context.windowSystem->pollEvents();
            const UVec2 expected = context.window->framebufferSize();

            const uint32_t before = swapchain->recreationCount();
            context.renderFrame(*swapchain, nvrhi::Color(0.0f, 1.0f, 0.0f, 1.0f));
            CHECK(swapchain->size() == expected);
            CHECK(swapchain->recreationCount() > before);

            // Further frames at the same size do not recreate it again.
            const uint32_t settled = swapchain->recreationCount();
            context.renderFrame(*swapchain, nvrhi::Color(0.0f, 0.0f, 1.0f, 1.0f));
            context.renderFrame(*swapchain, nvrhi::Color(0.0f, 0.0f, 1.0f, 1.0f));
            CHECK(swapchain->recreationCount() == settled);
        }
        CHECK(swapchain->size() == originalSize);

        swapchain.reset();
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("changing vsync takes effect on the next frame") {
        WindowedContext context;
        std::unique_ptr<Swapchain> swapchain = context.createSwapchain({.vsync = false});
        CHECK_FALSE(swapchain->vsync());
        context.renderFrame(*swapchain, nvrhi::Color(0.5f));

        swapchain->setVsync(true);
        CHECK(swapchain->vsync());
        const uint32_t before = swapchain->recreationCount();
        context.renderFrame(*swapchain, nvrhi::Color(0.5f));
        CHECK(swapchain->recreationCount() == before + 1);

        // Setting the same value again changes nothing.
        swapchain->setVsync(true);
        context.renderFrame(*swapchain, nvrhi::Color(0.5f));
        CHECK(swapchain->recreationCount() == before + 1);

        swapchain.reset();
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("a swapchain can be destroyed and created again for the same window") {
        WindowedContext context;
        for (int round = 0; round < 3; ++round) {
            std::unique_ptr<Swapchain> swapchain = context.createSwapchain();
            context.renderFrame(*swapchain, nvrhi::Color(0.25f));
            context.renderFrame(*swapchain, nvrhi::Color(0.75f));
        }
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("a headless device cannot create a swapchain") {
        const std::unique_ptr<WindowSystem> windowSystem = test::unwrap(WindowSystem::create());
        const std::unique_ptr<Window> window = test::unwrap(windowSystem->createWindow({.visible = false}));
        const std::unique_ptr<GraphicsDevice> headless = test::createTestDevice();

        const Result<std::unique_ptr<Swapchain>> swapchain = Swapchain::create(*headless, *window);
        REQUIRE_FALSE(swapchain.has_value());
        CHECK(swapchain.error().code == ErrorCode::InvalidState);
    }
}
