#include "cordite/renderer/ViewBindings.h"

#include "cordite/core/Assert.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/EnvironmentMaps.h"
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

/// A texture array that a view can bind as its cookies: one white texel.
nvrhi::TextureHandle createCookieArray(nvrhi::IDevice* device, const char* name) {
    nvrhi::TextureHandle texture = device->createTexture(nvrhi::TextureDesc()
                                                             .setDimension(nvrhi::TextureDimension::Texture2DArray)
                                                             .setWidth(1)
                                                             .setHeight(1)
                                                             .setArraySize(1)
                                                             .setFormat(nvrhi::Format::SRGBA8_UNORM)
                                                             .setDebugName(name)
                                                             .setInitialState(nvrhi::ResourceStates::ShaderResource)
                                                             .setKeepInitialState(true));
    REQUIRE(texture != nullptr);
    return texture;
}

/// An environment that a view can bind: one texel of a cube map, and one of a sky.
GpuEnvironment createEnvironment(nvrhi::IDevice* device, const char* name) {
    GpuEnvironment environment;
    environment.radiance = device->createTexture(nvrhi::TextureDesc()
                                                     .setDimension(nvrhi::TextureDimension::TextureCube)
                                                     .setWidth(1)
                                                     .setHeight(1)
                                                     .setArraySize(6)
                                                     .setFormat(nvrhi::Format::RGBA16_FLOAT)
                                                     .setDebugName(name)
                                                     .setInitialState(nvrhi::ResourceStates::ShaderResource)
                                                     .setKeepInitialState(true));
    environment.sky = device->createTexture(nvrhi::TextureDesc()
                                                .setWidth(1)
                                                .setHeight(1)
                                                .setFormat(nvrhi::Format::RGBA16_FLOAT)
                                                .setDebugName(name)
                                                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                                                .setKeepInitialState(true));
    REQUIRE(environment.radiance != nullptr);
    REQUIRE(environment.sky != nullptr);
    return environment;
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
            const GpuEnvironment environment = createEnvironment(context.device(), "First environment");
            const GpuEnvironment otherEnvironment = createEnvironment(context.device(), "Second environment");
            const nvrhi::TextureHandle cookies = createCookieArray(context.device(), "First cookies");
            const nvrhi::TextureHandle otherCookies = createCookieArray(context.device(), "Second cookies");
            ViewFixture fixture;

            // The sets are held on to, so that a new one cannot be at the address of an
            // old one that has gone.
            nvrhi::BindingSetHandle first;
            nvrhi::BindingSetHandle withOtherAtlas;
            nvrhi::BindingSetHandle withOtherEnvironment;
            nvrhi::BindingSetHandle withManyLights;
            recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
                bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.atlas = atlas},
                                {.environment = &environment});
                first = bindings.bindings();
                REQUIRE(first != nullptr);

                // Another view of the same kind: new values, the same set.
                fixture.view.targetSize = UVec2(128, 32);
                bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.atlas = atlas},
                                {.environment = &environment});
                CHECK(bindings.bindings() == first.Get());

                // A view may come with another atlas.
                bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.atlas = otherAtlas},
                                {.environment = &environment});
                withOtherAtlas = bindings.bindings();
                REQUIRE(withOtherAtlas != nullptr);
                CHECK(withOtherAtlas != first);

                // And with another environment; how bright it is, how it is turned and
                // whether it is seen are numbers, which need no new set.
                bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.atlas = otherAtlas},
                                {.environment = &otherEnvironment});
                withOtherEnvironment = bindings.bindings();
                REQUIRE(withOtherEnvironment != nullptr);
                CHECK(withOtherEnvironment != withOtherAtlas);
                bindings.update(
                    commandList, fixture.view, fixture.clusters, cookies, {.atlas = otherAtlas},
                    {.environment = &otherEnvironment, .intensity = 2.0f, .rotation = 1.0f, .visible = true});
                CHECK(bindings.bindings() == withOtherEnvironment.Get());
                bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.atlas = otherAtlas},
                                {.environment = &environment});
                CHECK(bindings.bindings() != withOtherEnvironment.Get());

                // And with other cookies.
                bindings.update(commandList, fixture.view, fixture.clusters, otherCookies, {.atlas = otherAtlas},
                                {.environment = &environment});
                CHECK(bindings.bindings() != withOtherEnvironment.Get());
                const nvrhi::BindingSetHandle withOtherCookies = bindings.bindings();
                bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.atlas = otherAtlas},
                                {.environment = &environment});
                CHECK(bindings.bindings() != withOtherCookies.Get());

                // More lights than the buffer was made for: it grows, and the set follows.
                fixture.clusters.lights.resize(500);
                bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.atlas = otherAtlas},
                                {.environment = &environment});
                withManyLights = bindings.bindings();
                REQUIRE(withManyLights != nullptr);
                CHECK(withManyLights != withOtherAtlas);
                CHECK(withManyLights != withOtherEnvironment);
                // Fewer again: the buffer is large enough and stays.
                fixture.clusters.lights.resize(3);
                bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.atlas = otherAtlas},
                                {.environment = &environment});
                CHECK(bindings.bindings() == withManyLights.Get());

                // As many shadow maps as a view can have: their buffer grows as well.
                ShadowPlan plan;
                plan.views.resize(kMaxShadowViews);
                bindings.update(commandList, fixture.view, fixture.clusters, cookies,
                                {.plan = &plan, .atlas = otherAtlas}, {.environment = &environment});
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
            const GpuEnvironment environment = createEnvironment(context.device(), "Environment");
            const nvrhi::TextureHandle cookies = createCookieArray(context.device(), "Cookies");
            ViewFixture fixture;
            ShadowPlan plan;
            plan.views.resize(static_cast<size_t>(kMaxShadowViews) + 1);

            recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
#if defined(CORDITE_ENABLE_ASSERTS)
                CHECK_THROWS_AS(bindings.update(commandList, fixture.view, fixture.clusters, cookies,
                                                {.plan = &plan, .atlas = atlas}, {.environment = &environment}),
                                test::AssertionFailure);
#else
                // Without assertions the view is described as one without shadows.
                bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.plan = &plan, .atlas = atlas},
                                    {.environment = &environment});
                CHECK(bindings.bindings() != nullptr);
#endif
            });

#if defined(CORDITE_ENABLE_ASSERTS)
            // Without cookies, an atlas, an environment or a command list there is nothing
            // to update.
            recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
                CHECK_THROWS_AS(bindings.update(commandList, fixture.view, fixture.clusters, nullptr, {.atlas = atlas},
                                                {.environment = &environment}),
                                test::AssertionFailure);
                CHECK_THROWS_AS(bindings.update(commandList, fixture.view, fixture.clusters, cookies, {},
                                                {.environment = &environment}),
                                test::AssertionFailure);
                CHECK_THROWS_AS(
                    bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.atlas = atlas}, {}),
                    test::AssertionFailure);
                const GpuEnvironment withoutTextures;
                CHECK_THROWS_AS(bindings.update(commandList, fixture.view, fixture.clusters, cookies, {.atlas = atlas},
                                                {.environment = &withoutTextures}),
                                test::AssertionFailure);
            });
            CHECK_THROWS_AS(bindings.update(nullptr, fixture.view, fixture.clusters, cookies, {.atlas = atlas},
                                            {.environment = &environment}),
                            test::AssertionFailure);
#endif
        }
        checkNoValidationIssues(*context.graphics);
    }
}
