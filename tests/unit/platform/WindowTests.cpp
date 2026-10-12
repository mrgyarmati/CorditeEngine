#include "cordite/platform/Window.h"

#include "support/TestSupport.h"

#include <memory>

using namespace cordite;

namespace {

/// These tests use the headless window system, which needs no display, so they run on
/// CI machines and do not flash windows on a developer's desktop.
std::unique_ptr<WindowSystem> createHeadlessSystem() {
    Result<std::unique_ptr<WindowSystem>> system = WindowSystem::create({.headless = true});
    REQUIRE_MESSAGE(system.has_value(), test::errorOf(system));
    return std::move(*system);
}

std::unique_ptr<Window> createWindow(WindowSystem& system, const WindowDesc& desc = {.visible = false}) {
    Result<std::unique_ptr<Window>> window = system.createWindow(desc);
    REQUIRE_MESSAGE(window.has_value(), test::errorOf(window));
    return std::move(*window);
}

} // namespace

TEST_SUITE("Window") {
    TEST_CASE("only one window system may exist at a time") {
        const std::unique_ptr<WindowSystem> first = createHeadlessSystem();
        CHECK(first->isHeadless());

        const Result<std::unique_ptr<WindowSystem>> second = WindowSystem::create({.headless = true});
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == ErrorCode::InvalidState);
    }

    TEST_CASE("a window system can be created again after the previous one is destroyed") {
        { const std::unique_ptr<WindowSystem> first = createHeadlessSystem(); }
        const std::unique_ptr<WindowSystem> second = createHeadlessSystem();
        CHECK(second != nullptr);
    }

    TEST_CASE("a window reports the size it was created with") {
        const std::unique_ptr<WindowSystem> system = createHeadlessSystem();
        Result<std::unique_ptr<Window>> window = system->createWindow({.title = "Test", .width = 640, .height = 360});
        REQUIRE_MESSAGE(window.has_value(), test::errorOf(window));

        CHECK((*window)->logicalSize() == UVec2(640, 360));
        CHECK((*window)->framebufferSize() == UVec2(640, 360));
        CHECK((*window)->contentScale() > 0.0f);
        CHECK_FALSE((*window)->shouldClose());
        CHECK_FALSE((*window)->isMinimized());
        CHECK((*window)->nativeHandle() != nullptr);
    }

    TEST_CASE("a window can be resized, retitled, hidden and closed") {
        const std::unique_ptr<WindowSystem> system = createHeadlessSystem();
        const std::unique_ptr<Window> window = createWindow(*system);

        window->setLogicalSize(UVec2(800, 450));
        system->pollEvents();
        CHECK(window->logicalSize() == UVec2(800, 450));
        CHECK(window->framebufferSize() == UVec2(800, 450));

        CHECK_NOTHROW(window->setTitle("Renamed \xe2\x9c\x93"));
        CHECK_NOTHROW(window->setVisible(true));
        CHECK_NOTHROW(window->setVisible(false));

        window->requestClose();
        CHECK(window->shouldClose());
    }

    TEST_CASE("a zero-sized window is rejected") {
        const std::unique_ptr<WindowSystem> system = createHeadlessSystem();
        const Result<std::unique_ptr<Window>> window = system->createWindow({.width = 0, .height = 720});
        REQUIRE_FALSE(window.has_value());
        CHECK(window.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("pollEvents starts a new input frame on every window") {
        const std::unique_ptr<WindowSystem> system = createHeadlessSystem();
        const std::unique_ptr<Window> first = createWindow(*system);
        const std::unique_ptr<Window> second = createWindow(*system);

        first->input().injectKey(Key::Enter, true);
        second->input().injectMouseButton(MouseButton::Left, true);
        CHECK(first->input().wasKeyPressed(Key::Enter));
        CHECK(second->input().wasMouseButtonPressed(MouseButton::Left));
        // Each window has its own input state.
        CHECK_FALSE(second->input().isKeyDown(Key::Enter));

        system->pollEvents();
        CHECK(first->input().isKeyDown(Key::Enter));
        CHECK_FALSE(first->input().wasKeyPressed(Key::Enter));
        CHECK_FALSE(second->input().wasMouseButtonPressed(MouseButton::Left));
        CHECK(first->input().events().empty());
    }

    TEST_CASE("changing the cursor mode does not produce a mouse jump") {
        const std::unique_ptr<WindowSystem> system = createHeadlessSystem();
        const std::unique_ptr<Window> window = createWindow(*system);
        CHECK(window->cursorMode() == CursorMode::Normal);

        window->input().injectMouseMove(Vec2(10.0f, 10.0f));
        window->setCursorMode(CursorMode::Captured);
        CHECK(window->cursorMode() == CursorMode::Captured);

        window->input().injectMouseMove(Vec2(400.0f, 300.0f));
        CHECK(window->input().mouseDelta() == Vec2(0.0f));

        window->setCursorMode(CursorMode::Hidden);
        window->setCursorMode(CursorMode::Normal);
        CHECK(window->cursorMode() == CursorMode::Normal);
    }

    TEST_CASE("a headless window system offers no Vulkan presentation") {
        const std::unique_ptr<WindowSystem> system = createHeadlessSystem();
        CHECK(system->vulkanInstanceExtensions().empty());

        const std::unique_ptr<Window> window = createWindow(*system);
        const Result<uint64_t> surface = window->createVulkanSurface(nullptr);
        REQUIRE_FALSE(surface.has_value());
        CHECK(surface.error().code == ErrorCode::Unsupported);
    }

    TEST_CASE("windows may be destroyed in any order") {
        const std::unique_ptr<WindowSystem> system = createHeadlessSystem();
        std::unique_ptr<Window> first = createWindow(*system);
        std::unique_ptr<Window> second = createWindow(*system);
        std::unique_ptr<Window> third = createWindow(*system);

        second.reset();
        CHECK_NOTHROW(system->pollEvents());
        first.reset();
        third.reset();
        CHECK_NOTHROW(system->pollEvents());
    }
}
