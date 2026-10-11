#include "cordite/renderer/ShadowAtlas.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/core/Assert.h"
#include "cordite/math/Projection.h"
#include "cordite/renderer/MeshDrawList.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/ShadowPlan.h"

#include "render/RenderTestSupport.h"

#include <initializer_list>
#include <memory>
#include <vector>

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::RenderContext;
using cordite::test::unwrap;

namespace {

/// A device, and what an atlas needs to be made: the layout of a material's bindings.
struct AtlasFixture {
    AtlasFixture() {
        assets = unwrap(AssetManager::create());
        resources = unwrap(RenderResources::create(context.device(), *assets));
    }

    ~AtlasFixture() {
        // GPU objects go before the device that made them.
        resources.reset();
        context.graphics->waitIdle();
    }

    AtlasFixture(const AtlasFixture&) = delete;
    AtlasFixture& operator=(const AtlasFixture&) = delete;

    [[nodiscard]] ShadowAtlas createAtlas() {
        return unwrap(ShadowAtlas::create(context.device(), *context.shaders, resources->materialLayout()));
    }

    RenderContext context;
    std::unique_ptr<AssetManager> assets;
    std::unique_ptr<RenderResources> resources;
};

} // namespace

TEST_SUITE("ShadowAtlas") {
    TEST_CASE("a new atlas is as small as an atlas can be, so that there is always one to bind") {
        AtlasFixture fixture;
        {
            const ShadowAtlas atlas = fixture.createAtlas();
            REQUIRE(atlas.texture() != nullptr);
            CHECK(atlas.size() == kMinShadowTileSize);
            const nvrhi::TextureDesc& desc = atlas.texture()->getDesc();
            CHECK(desc.width == kMinShadowTileSize);
            CHECK(desc.height == kMinShadowTileSize);
            CHECK(desc.format == nvrhi::Format::D32);
            CHECK(desc.isRenderTarget);
            CHECK(desc.debugName == "Shadow atlas");
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }

    TEST_CASE("resizing replaces the texture only when the size changes") {
        AtlasFixture fixture;
        {
            ShadowAtlas atlas = fixture.createAtlas();
            REQUIRE(atlas.resize(1024).has_value());
            CHECK(atlas.size() == 1024);
            // Kept alive past the next resize, to tell the textures apart by address.
            const nvrhi::TextureHandle first = atlas.texture();
            CHECK(first->getDesc().width == 1024);

            REQUIRE(atlas.resize(1024).has_value());
            CHECK(atlas.texture() == first.Get());

            REQUIRE(atlas.resize(2048).has_value());
            CHECK(atlas.texture() != first.Get());
            CHECK(atlas.size() == 2048);
            CHECK(atlas.texture()->getDesc().height == 2048);
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }

    TEST_CASE("sizes no atlas can have are refused, and leave the atlas as it was") {
        AtlasFixture fixture;
        {
            ShadowAtlas atlas = fixture.createAtlas();
            REQUIRE(atlas.resize(512).has_value());
            const nvrhi::ITexture* before = atlas.texture();

            for (const uint32_t size : std::initializer_list<uint32_t>{0, 1, kMinShadowTileSize - 1, 16385, 1u << 20}) {
                CAPTURE(size);
                const Status resized = atlas.resize(size);
                REQUIRE_FALSE(resized.has_value());
                CHECK(resized.error().code == ErrorCode::InvalidArgument);
                CHECK(atlas.size() == 512);
                CHECK(atlas.texture() == before);
            }
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }

    TEST_CASE("an atlas is cleared and drawn into, tile by tile") {
        AtlasFixture fixture;
        {
            ShadowAtlas atlas = fixture.createAtlas();
            REQUIRE(atlas.resize(256).has_value());

            // A cube, seen from above by a map that fills a quarter of the atlas.
            const GpuMesh* cube = fixture.resources->mesh(builtinMeshId(BuiltinMesh::Cube));
            REQUIRE(cube != nullptr);
            const std::vector<MeshDraw> draws = {{.mesh = cube,
                                                  .submesh = 0,
                                                  .material = &fixture.resources->material(Uuid()),
                                                  .localToWorld = Mat4(1.0f)}};
            ShadowView shadow;
            shadow.viewProjection = orthographic(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f) *
                                    lookAt(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f));
            shadow.tileOrigin = UVec2(128, 0);
            shadow.tileSize = 128;

            MeshDrawList list(fixture.context.device());
            MeshPassStatistics drawn;
            MeshPassStatistics nothing;
            test::recordAndExecute(fixture.context, [&](nvrhi::ICommandList* commandList) {
                fixture.resources->flushUploads();
                atlas.clear(commandList);
                list.build(commandList, draws, Vec3(0.0f), kWorldForward);
                drawn = atlas.draw(commandList, shadow, list.solidBatches(), list.instanceBuffer());
                // Nothing to draw is not an error, whatever the tile.
                nothing = atlas.draw(commandList, ShadowView(), {}, nullptr);
            });
            CHECK(drawn == MeshPassStatistics{.drawCalls = 1, .instances = 1, .triangles = 12});
            CHECK(nothing == MeshPassStatistics{});

#if defined(CORDITE_ENABLE_ASSERTS)
            // A tile that does not lie inside the atlas is a mistake of the caller.
            ShadowView outside = shadow;
            outside.tileOrigin = UVec2(192, 192);
            test::recordAndExecute(fixture.context, [&](nvrhi::ICommandList* commandList) {
                CHECK_THROWS_AS(atlas.draw(commandList, outside, list.solidBatches(), list.instanceBuffer()),
                                test::AssertionFailure);
            });
#endif
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }
}
