#include "cordite/renderer/ViewBindings.h"

#include "cordite/core/Assert.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/LightClusters.h"
#include "cordite/renderer/ShadowPlan.h"
#include "cordite/world/Components.h"

#include "render/RenderTestSupport.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::recordAndExecute;
using cordite::test::RenderContext;
using cordite::test::unwrap;

namespace {

/// A depth texture that a view can bind as its shadow atlas.
nvrhi::TextureHandle createAtlasTexture(nvrhi::IDevice* device, const char* name) {
    nvrhi::TextureHandle texture = device->createTexture(nvrhi::TextureDesc()
                                                             .setWidth(64)
                                                             .setHeight(64)
                                                             .setFormat(nvrhi::Format::D32)
                                                             .setIsRenderTarget(true)
                                                             .setDebugName(name)
                                                             .setInitialState(nvrhi::ResourceStates::ShaderResource)
                                                             .setKeepInitialState(true));
    REQUIRE(texture != nullptr);
    return texture;
}

/// A view, and lights sorted into its clusters: none to begin with.
struct ViewFixture {
    ViewFixture() {
        view.camera = makeCameraView(Mat4(1.0f), CameraComponent(), 1.0f);
        view.targetSize = UVec2(64, 64);
        buildLightClusters(view.camera, {}, clusters);
    }

    SceneView view;
    LightClusters clusters;
};

} // namespace

TEST_SUITE("ViewBindings") {
    TEST_CASE("the binding set is kept from one update to the next, and replaced when what it holds is") {
        RenderContext context;
        {
            ViewBindings bindings = unwrap(ViewBindings::create(context.device()));
            REQUIRE(bindings.layout() != nullptr);
            const nvrhi::TextureHandle atlas = createAtlasTexture(context.device(), "First atlas");
            const nvrhi::TextureHandle otherAtlas = createAtlasTexture(context.device(), "Second atlas");
            ViewFixture fixture;

            // The sets are held on to, so that a new one cannot be at the address of an
            // old one that has gone.
            nvrhi::BindingSetHandle first;
            nvrhi::BindingSetHandle withOtherAtlas;
            nvrhi::BindingSetHandle withManyLights;
            recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
                bindings.update(commandList, fixture.view, fixture.clusters, {.atlas = atlas});
                first = bindings.bindings();
                REQUIRE(first != nullptr);

                // Another view of the same kind: new values, the same set.
                fixture.view.targetSize = UVec2(128, 32);
                bindings.update(commandList, fixture.view, fixture.clusters, {.atlas = atlas});
                CHECK(bindings.bindings() == first.Get());

                // A view may come with another atlas.
                bindings.update(commandList, fixture.view, fixture.clusters, {.atlas = otherAtlas});
                withOtherAtlas = bindings.bindings();
                REQUIRE(withOtherAtlas != nullptr);
                CHECK(withOtherAtlas != first);

                // More lights than the buffer was made for: it grows, and the set follows.
                fixture.clusters.lights.resize(500);
                bindings.update(commandList, fixture.view, fixture.clusters, {.atlas = otherAtlas});
                withManyLights = bindings.bindings();
                REQUIRE(withManyLights != nullptr);
                CHECK(withManyLights != withOtherAtlas);
                // Fewer again: the buffer is large enough and stays.
                fixture.clusters.lights.resize(3);
                bindings.update(commandList, fixture.view, fixture.clusters, {.atlas = otherAtlas});
                CHECK(bindings.bindings() == withManyLights.Get());

                // As many shadow maps as a view can have: their buffer grows as well.
                ShadowPlan plan;
                plan.views.resize(kMaxShadowViews);
                bindings.update(commandList, fixture.view, fixture.clusters, {.plan = &plan, .atlas = otherAtlas});
                REQUIRE(bindings.bindings() != nullptr);
                CHECK(bindings.bindings() != withManyLights.Get());
            });
        }
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("a shadow plan with more maps than a view can have is a mistake of the caller") {
        RenderContext context;
        {
            ViewBindings bindings = unwrap(ViewBindings::create(context.device()));
            const nvrhi::TextureHandle atlas = createAtlasTexture(context.device(), "Atlas");
            ViewFixture fixture;
            ShadowPlan plan;
            plan.views.resize(static_cast<size_t>(kMaxShadowViews) + 1);

            recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
#if defined(CORDITE_ENABLE_ASSERTS)
                CHECK_THROWS_AS(
                    bindings.update(commandList, fixture.view, fixture.clusters, {.plan = &plan, .atlas = atlas}),
                    test::AssertionFailure);
#else
                // Without assertions the view is described as one without shadows.
                bindings.update(commandList, fixture.view, fixture.clusters, {.plan = &plan, .atlas = atlas});
                CHECK(bindings.bindings() != nullptr);
#endif
            });

#if defined(CORDITE_ENABLE_ASSERTS)
            // Without an atlas or a command list there is nothing to update.
            recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
                CHECK_THROWS_AS(bindings.update(commandList, fixture.view, fixture.clusters, {}),
                                test::AssertionFailure);
            });
            CHECK_THROWS_AS(bindings.update(nullptr, fixture.view, fixture.clusters, {.atlas = atlas}),
                            test::AssertionFailure);
#endif
        }
        checkNoValidationIssues(*context.graphics);
    }
}
