#include "cordite/core/FileSystem.h"
#include "cordite/renderer/RenderTarget.h"
#include "cordite/renderer/TestPatternPass.h"
#include "cordite/ui/UiSystem.h"

#include "render/RenderTestSupport.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::checkAgainstReference;
using cordite::test::checkNoValidationIssues;
using cordite::test::readToImage;
using cordite::test::recordAndExecute;
using cordite::test::RenderContext;
using cordite::test::unwrap;

namespace {

constexpr UVec2 kSize(400, 300);
constexpr float kFrameSeconds = 1.0f / 60.0f;

RenderTarget makeTarget(RenderContext& context) {
    return unwrap(RenderTarget::create(context.device(), {.size = kSize, .clearColor = {0.05f, 0.06f, 0.08f, 1.0f}}));
}

/// Runs one interface frame described by `build` and draws it onto the cleared target.
void renderFrame(RenderContext& context, UiSystem& ui, const RenderTarget& target, const std::function<void()>& build) {
    ui.beginFrame(target.size(), kFrameSeconds);
    build();
    recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
        target.clear(commandList);
        ui.render(commandList, target.framebuffer());
    });
}

/// A window at a fixed place with one of each common widget.
void buildWidgets(bool& checked, float& amount) {
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 260.0f), ImGuiCond_Always);
    if (ImGui::Begin("Widgets", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("The quick brown fox jumps over the lazy dog");
        ImGui::Separator();
        ImGui::Button("Button");
        ImGui::SameLine();
        ImGui::Checkbox("Checkbox", &checked);
        ImGui::SliderFloat("Slider", &amount, 0.0f, 1.0f);
        ImGui::ProgressBar(0.6f, ImVec2(-1.0f, 0.0f), "60%");
        ImGui::ColorButton("Accent", ImVec4(0.9f, 0.48f, 0.16f, 1.0f), ImGuiColorEditFlags_NoTooltip,
                           ImVec2(40.0f, 20.0f));
        ImGui::SameLine();
        ImGui::ColorButton("Blue", ImVec4(0.3f, 0.5f, 0.95f, 1.0f), ImGuiColorEditFlags_NoTooltip,
                           ImVec2(40.0f, 20.0f));
        if (ImGui::CollapsingHeader("Section", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BulletText("0123456789 +-*/=()[]{}");
        }
    }
    ImGui::End();
}

} // namespace

TEST_SUITE("UiSystem") {
    TEST_CASE("the interface scale is what the system asks for, less what the framebuffer provides") {
        // Windows and X11 at 150%: as many pixels as the size says, so all of it is applied.
        CHECK(interfaceScale(1.5f, UVec2(1280, 720), UVec2(1280, 720)) == doctest::Approx(1.5f));
        // macOS and Wayland at 200%: the framebuffer is already twice as large.
        CHECK(interfaceScale(2.0f, UVec2(1280, 720), UVec2(2560, 1440)) == doctest::Approx(1.0f));
        // Fractional scaling where the framebuffer provides part of it.
        CHECK(interfaceScale(3.0f, UVec2(1000, 500), UVec2(2000, 1000)) == doctest::Approx(1.5f));
        CHECK(interfaceScale(1.0f, UVec2(800, 600), UVec2(800, 600)) == doctest::Approx(1.0f));

        // A minimized window reports no pixels; unusable values fall back to 100%.
        CHECK(interfaceScale(1.25f, UVec2(1280, 720), UVec2(0, 0)) == doctest::Approx(1.25f));
        CHECK(interfaceScale(1.25f, UVec2(0, 0), UVec2(1280, 720)) == doctest::Approx(1.25f));
        CHECK(interfaceScale(0.0f, UVec2(800, 600), UVec2(800, 600)) == doctest::Approx(1.0f));
        CHECK(interfaceScale(-2.0f, UVec2(800, 600), UVec2(800, 600)) == doctest::Approx(1.0f));
        CHECK(interfaceScale(std::numeric_limits<float>::quiet_NaN(), UVec2(800, 600), UVec2(800, 600)) ==
              doctest::Approx(1.0f));

        // Whatever is reported, the interface stays usable.
        CHECK(interfaceScale(100.0f, UVec2(800, 600), UVec2(800, 600)) == doctest::Approx(4.0f));
        CHECK(interfaceScale(0.1f, UVec2(800, 600), UVec2(800, 600)) == doctest::Approx(0.5f));
    }

    TEST_CASE("only one interface system can exist at a time") {
        RenderContext context;
        const std::unique_ptr<UiSystem> first = unwrap(UiSystem::create(context.device(), *context.shaders));

        const Result<std::unique_ptr<UiSystem>> second = UiSystem::create(context.device(), *context.shaders);
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == ErrorCode::InvalidState);
    }

    TEST_CASE("a new system can be created once the previous one is gone") {
        RenderContext context;
        for (int round = 0; round < 3; ++round) {
            const std::unique_ptr<UiSystem> ui = unwrap(UiSystem::create(context.device(), *context.shaders));
            const RenderTarget target = makeTarget(context);
            bool checked = false;
            float amount = 0.5f;
            renderFrame(context, *ui, target, [&] { buildWidgets(checked, amount); });
        }
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("unusable font sizes are rejected") {
        RenderContext context;
        for (const float fontSize : {0.0f, -4.0f, 1000.0f}) {
            const Result<std::unique_ptr<UiSystem>> ui =
                UiSystem::create(context.device(), *context.shaders, {.fontSize = fontSize});
            REQUIRE_FALSE(ui.has_value());
            CHECK(ui.error().code == ErrorCode::InvalidArgument);
        }
        // The failed attempts left nothing behind.
        CHECK(UiSystem::create(context.device(), *context.shaders).has_value());
    }

    TEST_CASE("widgets match the reference image") {
        RenderContext context;
        const std::unique_ptr<UiSystem> ui = unwrap(UiSystem::create(context.device(), *context.shaders));
        const RenderTarget target = makeTarget(context);

        bool checked = true;
        float amount = 0.25f;
        // A window settles its layout over its first frames; compare a later one.
        for (int frame = 0; frame < 3; ++frame) {
            renderFrame(context, *ui, target, [&] { buildWidgets(checked, amount); });
        }

        checkAgainstReference(readToImage(*context.graphics, target.color()), "ui-widgets-400x300");
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the interface is drawn the same way every time") {
        RenderContext context;
        const RenderTarget target = makeTarget(context);
        bool checked = true;
        float amount = 0.25f;

        std::vector<Image> images;
        for (int run = 0; run < 2; ++run) {
            const std::unique_ptr<UiSystem> ui = unwrap(UiSystem::create(context.device(), *context.shaders));
            for (int frame = 0; frame < 3; ++frame) {
                renderFrame(context, *ui, target, [&] { buildWidgets(checked, amount); });
            }
            images.push_back(readToImage(*context.graphics, target.color()));
        }

        const ImageCompareOptions exact{.channelTolerance = 0, .maxDifferingRatio = 0.0};
        CHECK(unwrap(compareImages(images[0], images[1], exact)).matches);
    }

    TEST_CASE("an engine texture can be shown inside the interface") {
        RenderContext context;
        TestPatternPass pattern = unwrap(TestPatternPass::create(context.device(), *context.shaders));
        const RenderTarget patternTarget = unwrap(RenderTarget::create(context.device(), {.size = {160, 90}}));
        recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
            pattern.draw(commandList, patternTarget.framebuffer(), {.timeSeconds = 1.0f});
        });

        const std::unique_ptr<UiSystem> ui = unwrap(UiSystem::create(context.device(), *context.shaders));
        const RenderTarget target = makeTarget(context);
        const auto build = [&] {
            // Drawn straight onto the background, at a known place and at its own size.
            ImGui::GetBackgroundDrawList()->AddImage(UiSystem::textureId(patternTarget.color()), ImVec2(100.0f, 50.0f),
                                                     ImVec2(260.0f, 140.0f));
        };
        renderFrame(context, *ui, target, build);
        renderFrame(context, *ui, target, build);

        const Image shown = readToImage(*context.graphics, target.color());
        const Image source = readToImage(*context.graphics, patternTarget.color());
        for (const UVec2 texel : {UVec2(0, 0), UVec2(80, 45), UVec2(159, 89), UVec2(20, 70), UVec2(130, 10)}) {
            CAPTURE(texel.x);
            CAPTURE(texel.y);
            const Vec4 expected = source.pixel(texel.x, texel.y);
            const Vec4 actual = shown.pixel(100 + texel.x, 50 + texel.y);
            CHECK(actual.r == doctest::Approx(expected.r).epsilon(0.02));
            CHECK(actual.g == doctest::Approx(expected.g).epsilon(0.02));
            CHECK(actual.b == doctest::Approx(expected.b).epsilon(0.02));
        }
        // Outside the image the background shows.
        CHECK(shown.pixel(90, 50).r == doctest::Approx(0.05f).epsilon(0.02));
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("a texture that is no longer shown is let go") {
        RenderContext context;
        const std::unique_ptr<UiSystem> ui = unwrap(UiSystem::create(context.device(), *context.shaders));
        const RenderTarget target = makeTarget(context);

        // A new texture every frame, as a viewport does while it is being resized.
        nvrhi::TextureHandle shown;
        for (uint32_t frame = 0; frame < 6; ++frame) {
            shown = test::createColorTarget(context.device(), 32 + frame, 32);
            renderFrame(context, *ui, target, [&] {
                ImGui::GetBackgroundDrawList()->AddImage(UiSystem::textureId(shown), ImVec2(0.0f, 0.0f),
                                                         ImVec2(64.0f, 64.0f));
            });
        }

        // After a frame that does not use it, the interface holds no reference any more:
        // ours is the only one left.
        renderFrame(context, *ui, target, [] {});
        context.graphics->waitIdle();
        context.graphics->collectGarbage();
        shown->AddRef();
        CHECK(shown->Release() == 1);
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the pointer operates widgets without a window") {
        RenderContext context;
        const std::unique_ptr<UiSystem> ui = unwrap(UiSystem::create(context.device(), *context.shaders));
        const RenderTarget target = makeTarget(context);

        int clicks = 0;
        bool hovered = false;
        const auto build = [&] {
            ImGui::SetNextWindowPos(ImVec2(50.0f, 50.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(200.0f, 120.0f), ImGuiCond_Always);
            ImGui::Begin("Clickable", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
            ImGui::SetCursorPos(ImVec2(20.0f, 20.0f));
            if (ImGui::Button("Press me", ImVec2(120.0f, 40.0f))) {
                ++clicks;
            }
            hovered = ImGui::IsItemHovered();
            ImGui::End();
        };

        // The button covers (70, 70) to (190, 110) on screen.
        const Vec2 onButton(130.0f, 90.0f);
        const Vec2 elsewhere(350.0f, 250.0f);

        ui->setPointerState({.position = elsewhere});
        renderFrame(context, *ui, target, build);
        renderFrame(context, *ui, target, build);
        CHECK_FALSE(hovered);
        CHECK_FALSE(ui->wantsPointer());

        ui->setPointerState({.position = onButton});
        renderFrame(context, *ui, target, build);
        renderFrame(context, *ui, target, build);
        CHECK(hovered);
        CHECK(ui->wantsPointer());
        CHECK(clicks == 0);

        ui->setPointerState({.position = onButton, .leftDown = true});
        renderFrame(context, *ui, target, build);
        CHECK(clicks == 0);
        ui->setPointerState({.position = onButton, .leftDown = false});
        renderFrame(context, *ui, target, build);
        CHECK(clicks == 1);

        // Pressing somewhere else does nothing.
        ui->setPointerState({.position = elsewhere, .leftDown = true});
        renderFrame(context, *ui, target, build);
        ui->setPointerState({.position = elsewhere, .leftDown = false});
        renderFrame(context, *ui, target, build);
        CHECK(clicks == 1);
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("text in new sizes extends the font texture without errors") {
        RenderContext context;
        const std::unique_ptr<UiSystem> ui = unwrap(UiSystem::create(context.device(), *context.shaders));
        const RenderTarget target = makeTarget(context);

        // Each size needs glyphs that are not in the texture yet.
        for (const float size : {12.0f, 24.0f, 48.0f, 72.0f, 18.0f}) {
            renderFrame(context, *ui, target, [size] {
                ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f), ImGuiCond_Always);
                ImGui::Begin("Text", nullptr, ImGuiWindowFlags_NoSavedSettings);
                ImGui::PushFont(nullptr, size);
                ImGui::TextUnformatted("AaBbCcDdEeFfGgHh 0123456789");
                ImGui::PopFont();
                ImGui::End();
            });
        }

        // Large text was drawn: some pixel in the text area is bright.
        const Image image = readToImage(*context.graphics, target.color());
        float brightest = 0.0f;
        for (uint32_t y = 30; y < 60; ++y) {
            for (uint32_t x = 10; x < 200; ++x) {
                brightest = std::max(brightest, image.pixel(x, y).r);
            }
        }
        CHECK(brightest > 0.8f);
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("a frame can be discarded instead of drawn") {
        RenderContext context;
        const std::unique_ptr<UiSystem> ui = unwrap(UiSystem::create(context.device(), *context.shaders));
        const RenderTarget target = makeTarget(context);
        CHECK_FALSE(ui->isFrameOpen());

        ui->beginFrame(kSize, kFrameSeconds);
        CHECK(ui->isFrameOpen());
        ImGui::Begin("Never shown");
        ImGui::End();
        ui->discardFrame();
        CHECK_FALSE(ui->isFrameOpen());
        CHECK_NOTHROW(ui->discardFrame());

        // The next frame works as usual.
        bool checked = false;
        float amount = 0.0f;
        renderFrame(context, *ui, target, [&] { buildWidgets(checked, amount); });
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("a system destroyed in the middle of a frame cleans up") {
        RenderContext context;
        {
            const std::unique_ptr<UiSystem> ui = unwrap(UiSystem::create(context.device(), *context.shaders));
            ui->beginFrame(kSize, kFrameSeconds);
            ImGui::Begin("Unfinished");
            ImGui::End();
        }
        CHECK(UiSystem::create(context.device(), *context.shaders).has_value());
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the layout is remembered in the settings file") {
        RenderContext context;
        const test::TempDirectory directory;
        const std::filesystem::path settings = directory.path() / "nested" / "layout.ini";
        const RenderTarget target = makeTarget(context);

        const auto build = [](const ImVec2& position) {
            ImGui::SetNextWindowPos(position, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(150.0f, 100.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Remembered");
            const ImVec2 actual = ImGui::GetWindowPos();
            ImGui::End();
            return actual;
        };

        {
            const std::unique_ptr<UiSystem> ui =
                unwrap(UiSystem::create(context.device(), *context.shaders, {.settingsFile = settings}));
            renderFrame(context, *ui, target, [&] { build(ImVec2(30.0f, 40.0f)); });
            renderFrame(context, *ui, target, [&] { build(ImVec2(30.0f, 40.0f)); });
        }
        REQUIRE(fileExists(settings));
        CHECK(unwrap(readTextFile(settings)).find("[Window][Remembered]") != std::string::npos);

        // A second session finds the window where the first one left it, not where the
        // code would put it by default.
        ImVec2 restored(0.0f, 0.0f);
        {
            const std::unique_ptr<UiSystem> ui =
                unwrap(UiSystem::create(context.device(), *context.shaders, {.settingsFile = settings}));
            renderFrame(context, *ui, target, [&] { restored = build(ImVec2(200.0f, 200.0f)); });
        }
        CHECK(restored.x == 30.0f);
        CHECK(restored.y == 40.0f);
        checkNoValidationIssues(*context.graphics);
    }
}
