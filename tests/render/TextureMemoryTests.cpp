#include "cordite/assets/Image.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/TextureBudget.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/TestSupport.h"

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::LogCapture;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// An image of `width` by `height` grey pixels: what it shows does not matter, its size does.
Image plain(uint32_t width, uint32_t height) {
    Image image = unwrap(Image::create(width, height, ImageFormat::Rgba8));
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            image.setPixel(x, y, Vec4(0.5f, 0.5f, 0.5f, 1.0f));
        }
    }
    return image;
}

/// The size of the texture the resources keep of the image `name` as colors.
UVec2 sizeOf(SceneFixture& scene, std::string_view name) {
    nvrhi::ITexture* texture = scene.renderer->resources().colorTexture(scene.asset(fmt::format("{}.png", name)));
    REQUIRE(texture != nullptr);
    return {texture->getDesc().width, texture->getDesc().height};
}

} // namespace

TEST_SUITE("TextureMemory") {
    TEST_CASE("another limit is kept from the next frame on: the textures are made again, as large as it lets them") {
        SceneFixture scene;
        scene.writeImage("large", plain(64, 64));
        scene.open({.maxTextureDimension = 64});
        RenderResources& resources = scene.renderer->resources();
        CHECK(sizeOf(scene, "large") == UVec2(64));
        CHECK(resources.statistics().textureLimit == 64);

        const uint64_t revision = resources.revision();
        resources.setTextureLimit(16);
        CHECK(resources.textureLimit() == 16);
        // Not in the middle of a frame: what was drawn with the textures of now may still be
        // shown.
        CHECK(sizeOf(scene, "large") == UVec2(64));
        resources.beginFrame();
        CHECK(resources.revision() > revision);
        CHECK(sizeOf(scene, "large") == UVec2(16));
        CHECK(resources.statistics().textureLimit == 16);

        // The same limit again changes nothing.
        const uint64_t after = resources.revision();
        resources.setTextureLimit(16);
        resources.beginFrame();
        CHECK(resources.revision() == after);
        // No limit: as large as the image.
        resources.setTextureLimit(0);
        resources.beginFrame();
        CHECK(sizeOf(scene, "large") == UVec2(64));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("over the budget every texture is kept the same number of levels smaller, whatever order they were "
              "asked for in") {
        const std::array<std::string_view, 4> names = {"square", "first", "second", "wide"};
        const std::array<TextureFootprint, 4> footprints = {{{.width = 32, .height = 32},
                                                             {.width = 16, .height = 16},
                                                             {.width = 16, .height = 16},
                                                             {.width = 16, .height = 8}}};
        // As much as the largest takes: each of them fits alone, all of them only a level
        // smaller - so it is all of them that decide, not one.
        const uint64_t budget = textureBytesFrom(footprints[0], 0);
        for (const TextureFootprint& footprint : footprints) {
            REQUIRE(textureBiasFor(std::span<const TextureFootprint>(&footprint, 1), 0, budget) == 0);
        }
        REQUIRE(textureBiasFor(footprints, 0, budget) == 1);

        for (const std::array<size_t, 4> order :
             {std::array<size_t, 4>{0, 1, 2, 3}, std::array<size_t, 4>{3, 2, 1, 0}}) {
            CAPTURE(names[order[0]]);
            SceneFixture scene;
            for (size_t index = 0; index < names.size(); ++index) {
                scene.writeImage(names[index], plain(footprints[index].width, footprints[index].height));
            }
            scene.open({.maxTextureDimension = 0, .textureBudgetBytes = budget});
            RenderResources& resources = scene.renderer->resources();
            const LogCapture log;
            for (const size_t index : order) {
                (void)sizeOf(scene, names[index]);
            }
            // Which of them came first decides how large each was made in this frame; from
            // the next, it is the bias of all of them.
            resources.beginFrame();
            CHECK(resources.statistics().textureLevelBias == 1);
            CHECK(sizeOf(scene, "square") == UVec2(16));
            CHECK(sizeOf(scene, "first") == UVec2(8));
            CHECK(sizeOf(scene, "second") == UVec2(8));
            CHECK(sizeOf(scene, "wide") == UVec2(8, 4));
            CHECK(log.count(LogLevel::Warn) == 1);
            // And it stays so.
            const uint64_t revision = resources.revision();
            resources.beginFrame();
            CHECK(resources.revision() == revision);
            CHECK(sizeOf(scene, "square") == UVec2(16));
            checkNoValidationIssues(*scene.context.graphics);
        }
    }

    TEST_CASE("a texture that can no longer be loaded no longer counts against the budget") {
        SceneFixture scene;
        scene.writeImage("large", plain(64, 64));
        scene.writeImage("other", plain(64, 64));
        // Room for one of them as it is: the two are kept a level smaller.
        scene.open({.maxTextureDimension = 0, .textureBudgetBytes = textureBytesFrom({.width = 64, .height = 64}, 0)});
        RenderResources& resources = scene.renderer->resources();
        (void)sizeOf(scene, "large");
        (void)sizeOf(scene, "other");
        resources.beginFrame();
        REQUIRE(resources.statistics().textureLevelBias == 1);
        CHECK(sizeOf(scene, "large") == UVec2(32));

        // The other is no image any more. Once the assets are read again it is no texture,
        // and takes nothing: the one that is left has the budget to itself.
        REQUIRE(writeTextFile(scene.file("other.png"), "no pixels in here").has_value());
        REQUIRE(scene.assets->refresh().has_value());
        resources.beginFrame();
        const LogCapture log;
        CHECK(resources.colorTexture(scene.asset("other.png")) == nullptr);
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(sizeOf(scene, "large") == UVec2(64));
        resources.beginFrame();
        CHECK(resources.statistics().textureLevelBias == 0);
        CHECK(sizeOf(scene, "large") == UVec2(64));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the log says how much smaller the textures are kept whenever that grows, and only then") {
        SceneFixture scene;
        const std::array<std::string_view, 5> names = {"a", "b", "c", "d", "e"};
        for (const std::string_view name : names) {
            scene.writeImage(name, plain(32, 32));
        }
        // One of them as it is; two or as many as four a level smaller; five two levels.
        scene.open({.maxTextureDimension = 0, .textureBudgetBytes = textureBytesFrom({.width = 32, .height = 32}, 0)});
        RenderResources& resources = scene.renderer->resources();
        const LogCapture log;
        const std::array<size_t, 5> warnings = {0, 1, 1, 1, 2};
        for (size_t index = 0; index < names.size(); ++index) {
            CAPTURE(index);
            (void)sizeOf(scene, names[index]);
            CHECK(log.count(LogLevel::Warn) == warnings[index]);
        }
        CHECK(log.contains("kept 2 levels smaller than the limit"));
        resources.beginFrame();
        CHECK(resources.statistics().textureLevelBias == 2);
        CHECK(sizeOf(scene, "a") == UVec2(8));
        CHECK(log.count(LogLevel::Warn) == 2);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a lookup table keeps its size whatever the limit and the budget, and does not count against the "
              "budget") {
        SceneFixture scene;
        // A table of four colors each way: four slices of four by four, side by side.
        scene.writeImage("table", plain(16, 4));
        scene.writeImage("large", plain(64, 64));
        scene.open({.maxTextureDimension = 2, .textureBudgetBytes = 1});
        RenderResources& resources = scene.renderer->resources();
        const auto tableSize = [&] {
            nvrhi::ITexture* table = resources.dataTexture(scene.asset("table.png"));
            REQUIRE(table != nullptr);
            return UVec2(table->getDesc().width, table->getDesc().height);
        };
        CHECK(tableSize() == UVec2(16, 4));
        resources.beginFrame();
        CHECK(resources.statistics().textureLevelBias == 0);
        CHECK(tableSize() == UVec2(16, 4));

        // A texture of a material keeps to both: nothing fits a byte, so it is as small as it gets.
        CHECK(sizeOf(scene, "large") == UVec2(1));
        resources.beginFrame();
        CHECK(sizeOf(scene, "large") == UVec2(1));
        CHECK(tableSize() == UVec2(16, 4));
        checkNoValidationIssues(*scene.context.graphics);
    }
}
