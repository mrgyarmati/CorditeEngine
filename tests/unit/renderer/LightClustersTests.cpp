#include "cordite/renderer/LightClusters.h"

#include "cordite/math/Projection.h"
#include "cordite/math/Random.h"
#include "cordite/world/Components.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <vector>

using namespace cordite;

namespace {

/// A camera at `eye` looking at `target`, with a 16:9 picture.
CameraView perspectiveView(const Vec3& eye, const Vec3& target, float nearPlane = 0.1f, float farPlane = 100.0f) {
    CameraComponent camera;
    camera.verticalFovDegrees = 60.0f;
    camera.nearPlane = nearPlane;
    camera.farPlane = farPlane;
    return makeCameraView(glm::inverse(lookAt(eye, target)), camera, 16.0f / 9.0f);
}

CameraView orthographicView(const Vec3& eye, const Vec3& target, float height = 9.0f) {
    CameraComponent camera;
    camera.projection = CameraProjection::Orthographic;
    camera.orthographicHeight = height;
    camera.nearPlane = 0.5f;
    camera.farPlane = 48.5f;
    return makeCameraView(glm::inverse(lookAt(eye, target)), camera, 16.0f / 9.0f);
}

SceneLight pointLight(const Vec3& position, float range) {
    return {.type = GpuLightType::Point, .position = position, .range = range};
}

/// Where a point of the world is in the picture, and how far in front of the camera.
struct Seen {
    Vec2 uv;
    float depth;
};

Seen seenAt(const CameraView& view, const Vec3& point) {
    const Vec4 clip = view.viewProjection * Vec4(point, 1.0f);
    const Vec2 ndc = Vec2(clip) / clip.w;
    return {Vec2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f), glm::dot(point - view.position, view.forward)};
}

/// True when the list of the cluster a point falls in names the light.
bool reaches(const LightClusters& clusters, const CameraView& view, const Vec3& point, uint32_t light) {
    const Seen seen = seenAt(view, point);
    const std::span<const uint32_t> list = clusters.lightsOf(clusterOf(seen.uv, seen.depth, clusters.depthMapping));
    return std::ranges::find(list, light) != list.end();
}

} // namespace

TEST_SUITE("LightClusters") {
    TEST_CASE("the grid is 16 by 9 by 24, indexed column first") {
        CHECK(kClusterCount == 3456);
        CHECK(clusterIndex(0, 0, 0) == 0);
        CHECK(clusterIndex(15, 0, 0) == 15);
        CHECK(clusterIndex(0, 1, 0) == 16);
        CHECK(clusterIndex(0, 0, 1) == 144);
        CHECK(clusterIndex(15, 8, 23) == kClusterCount - 1);
    }

    TEST_CASE("slices of a perspective view grow by the same factor each") {
        const ClusterDepthMapping mapping = clusterDepthMapping(perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f)));
        CHECK_FALSE(mapping.orthographic);

        // From the near plane to the far plane.
        CHECK(mapping.sliceStart(0) == doctest::Approx(0.1f));
        CHECK(mapping.sliceStart(kClusterSlices) == doctest::Approx(100.0f).epsilon(0.001));
        // A thousandfold in 24 steps: each slice is the 24th root of 1000 deeper.
        const float factor = std::pow(1000.0f, 1.0f / 24.0f);
        for (uint32_t slice = 0; slice < kClusterSlices; ++slice) {
            CAPTURE(slice);
            CHECK(mapping.sliceStart(slice + 1) / mapping.sliceStart(slice) == doctest::Approx(factor).epsilon(0.001));
        }
    }

    TEST_CASE("a depth is in the slice that starts at or before it") {
        for (const CameraView& view : {perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f)),
                                       orthographicView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f))}) {
            const ClusterDepthMapping mapping = clusterDepthMapping(view);
            for (uint32_t slice = 0; slice < kClusterSlices; ++slice) {
                CAPTURE(slice);
                const float start = mapping.sliceStart(slice);
                const float end = mapping.sliceStart(slice + 1);
                CHECK(mapping.sliceOf(start + (end - start) * 0.01f) == slice);
                CHECK(mapping.sliceOf((start + end) * 0.5f) == slice);
                CHECK(mapping.sliceOf(end - (end - start) * 0.01f) == slice);
            }
        }
    }

    TEST_CASE("depths outside the grid count to its first and last slice") {
        const ClusterDepthMapping mapping = clusterDepthMapping(perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f)));
        CHECK(mapping.sliceOf(0.05f) == 0);
        CHECK(mapping.sliceOf(0.0f) == 0);
        CHECK(mapping.sliceOf(-3.0f) == 0);
        CHECK(mapping.sliceOf(std::numeric_limits<float>::quiet_NaN()) == 0);
        CHECK(mapping.sliceOf(5000.0f) == kClusterSlices - 1);
        CHECK(mapping.sliceOf(std::numeric_limits<float>::infinity()) == kClusterSlices - 1);
    }

    TEST_CASE("slices of an orthographic view are equally deep") {
        const ClusterDepthMapping mapping = clusterDepthMapping(orthographicView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f)));
        CHECK(mapping.orthographic);
        // 48 metres between the planes: two metres a slice.
        for (uint32_t slice = 0; slice <= kClusterSlices; ++slice) {
            CHECK(mapping.sliceStart(slice) == doctest::Approx(0.5f + 2.0f * static_cast<float>(slice)));
        }
    }

    TEST_CASE("the grid does not reach further than kMaxClusterDepth, however far the camera sees") {
        const ClusterDepthMapping mapping =
            clusterDepthMapping(perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), 0.1f, 100000.0f));
        CHECK(mapping.sliceStart(kClusterSlices) == doctest::Approx(kMaxClusterDepth).epsilon(0.001));
        CHECK(mapping.sliceOf(90000.0f) == kClusterSlices - 1);
    }

    TEST_CASE("a point of the picture is in the cluster of its column, row and slice") {
        const ClusterDepthMapping mapping = clusterDepthMapping(perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f)));
        const float depth = (mapping.sliceStart(5) + mapping.sliceStart(6)) * 0.5f;

        CHECK(clusterOf(Vec2(0.0f, 0.0f), depth, mapping) == clusterIndex(0, 0, 5));
        CHECK(clusterOf(Vec2(0.5f, 0.5f), depth, mapping) == clusterIndex(8, 4, 5));
        CHECK(clusterOf(Vec2(0.99f, 0.99f), depth, mapping) == clusterIndex(15, 8, 5));
        // The right and bottom edges belong to the last column and row; so does what
        // rounding puts just outside.
        CHECK(clusterOf(Vec2(1.0f, 1.0f), depth, mapping) == clusterIndex(15, 8, 5));
        CHECK(clusterOf(Vec2(1.5f, -0.2f), depth, mapping) == clusterIndex(15, 0, 5));
    }

    TEST_CASE("without lights every list is empty") {
        LightClusters clusters;
        buildLightClusters(perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f)), {}, clusters);
        CHECK(clusters.lights.empty());
        CHECK(clusters.indices.empty());
        CHECK(clusters.directionalCount == 0);
        REQUIRE(clusters.grid.size() == kClusterCount);
        CHECK(std::ranges::all_of(clusters.grid, [](const ClusterRange& range) { return range.count == 0; }));
    }

    TEST_CASE("a light is in the list of every cluster it can light, and of few others") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f, 1.0f, 0.0f));
        const Vec3 position(1.0f, 1.0f, -2.0f);
        const float reach = 2.0f;
        const std::vector<SceneLight> lights = {pointLight(position, reach)};
        LightClusters clusters;
        buildLightClusters(view, lights, clusters);
        REQUIRE(clusters.lights.size() == 1);

        // Points all through the light's reach: each one's cluster must name the light.
        Random random(7);
        for (int sample = 0; sample < 2000; ++sample) {
            const Vec3 offset = random.insideUnitSphere() * reach;
            CAPTURE(offset);
            CHECK(reaches(clusters, view, position + offset, 0));
        }

        // Far from it, on the other side of the picture or much deeper, it is not listed.
        CHECK_FALSE(reaches(clusters, view, Vec3(-6.0f, 1.0f, -2.0f), 0));
        CHECK_FALSE(reaches(clusters, view, Vec3(1.0f, 1.0f, -40.0f), 0));
        CHECK_FALSE(reaches(clusters, view, Vec3(1.0f, 1.0f, 6.0f), 0));

        // It is in far fewer lists than there are clusters.
        size_t listed = 0;
        for (const ClusterRange& range : clusters.grid) {
            listed += range.count;
        }
        CHECK(listed > 0);
        CHECK(listed < kClusterCount / 10);
        CHECK(clusters.indices.size() == listed);
    }

    TEST_CASE("the same holds without perspective") {
        const CameraView view = orthographicView(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f));
        const Vec3 position(3.0f, -1.0f, 2.0f);
        const std::vector<SceneLight> lights = {pointLight(position, 1.5f)};
        LightClusters clusters;
        buildLightClusters(view, lights, clusters);
        REQUIRE(clusters.lights.size() == 1);

        Random random(11);
        for (int sample = 0; sample < 1000; ++sample) {
            CHECK(reaches(clusters, view, position + random.insideUnitSphere() * 1.5f, 0));
        }
        CHECK_FALSE(reaches(clusters, view, Vec3(-3.0f, -1.0f, 2.0f), 0));
        CHECK_FALSE(reaches(clusters, view, Vec3(3.0f, -1.0f, -20.0f), 0));
    }

    TEST_CASE("the lists of the clusters follow each other and hold their lights in order") {
        const CameraView view = perspectiveView(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f));
        std::vector<SceneLight> lights;
        Random random(3);
        for (int index = 0; index < 64; ++index) {
            lights.push_back(
                pointLight(Vec3(random.range(-6.0f, 6.0f), random.range(-3.0f, 3.0f), random.range(-8.0f, 4.0f)),
                           random.range(0.5f, 3.0f)));
        }
        LightClusters clusters;
        buildLightClusters(view, lights, clusters);

        uint32_t expectedOffset = 0;
        for (const ClusterRange& range : clusters.grid) {
            REQUIRE(range.offset == expectedOffset);
            expectedOffset += range.count;
        }
        CHECK(expectedOffset == clusters.indices.size());
        for (uint32_t cluster = 0; cluster < kClusterCount; ++cluster) {
            const std::span<const uint32_t> list = clusters.lightsOf(cluster);
            CHECK(std::ranges::is_sorted(list));
            CHECK(std::ranges::adjacent_find(list) == list.end());
            for (const uint32_t light : list) {
                REQUIRE(light < clusters.lights.size());
            }
        }

        // Building again, into the same object, gives the same result.
        const LightClusters first = clusters;
        buildLightClusters(view, lights, clusters);
        CHECK(clusters.grid == first.grid);
        CHECK(clusters.indices == first.indices);
    }

    TEST_CASE("directional lights come first and are in no list") {
        const CameraView view = perspectiveView(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f));
        const std::vector<SceneLight> lights = {
            pointLight(Vec3(0.0f), 2.0f),
            {.type = GpuLightType::Directional, .direction = Vec3(0.0f, -1.0f, 0.0f), .color = Vec3(2.0f)},
            pointLight(Vec3(1.0f, 0.0f, 0.0f), 2.0f),
        };
        LightClusters clusters;
        buildLightClusters(view, lights, clusters);

        REQUIRE(clusters.lights.size() == 3);
        CHECK(clusters.directionalCount == 1);
        CHECK((clusters.lights[0].typeAndChannels & 0xFFu) == static_cast<uint32_t>(GpuLightType::Directional));
        CHECK(clusters.lights[0].color == Vec3(2.0f));
        CHECK(clusters.lights[1].position == Vec3(0.0f));
        CHECK(clusters.lights[2].position == Vec3(1.0f, 0.0f, 0.0f));
        // The lists name the point lights only, by their place in the list of lights.
        CHECK(std::ranges::all_of(clusters.indices, [](uint32_t light) { return light == 1 || light == 2; }));
        CHECK(std::ranges::find(clusters.indices, 1u) != clusters.indices.end());
    }

    TEST_CASE("lights that cannot light anything are left out") {
        const CameraView view = perspectiveView(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f));
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::vector<SceneLight> lights = {
            {.type = GpuLightType::Point, .position = Vec3(0.0f), .color = Vec3(0.0f), .range = 5.0f}, // dark
            {.type = GpuLightType::Point, .position = Vec3(0.0f), .range = 0.0f},                      // no reach
            {.type = GpuLightType::Point, .position = Vec3(nan, 0.0f, 0.0f), .range = 5.0f},           // nowhere
            {.type = GpuLightType::Point, .position = Vec3(0.0f, 0.0f, 30.0f), .range = 5.0f},         // behind
            {.type = GpuLightType::Point, .position = Vec3(500.0f, 0.0f, 0.0f), .range = 5.0f},        // beside
            {.type = GpuLightType::Directional, .color = Vec3(-1.0f)},                                 // dark
        };
        LightClusters clusters;
        buildLightClusters(view, lights, clusters);
        CHECK(clusters.lights.empty());
        CHECK(clusters.indices.empty());
        CHECK(clusters.droppedLights == 0);
    }

    TEST_CASE("a spot light is only listed where its cone reaches") {
        const CameraView view = perspectiveView(Vec3(0.0f, 0.0f, 20.0f), Vec3(0.0f));
        // Shining to the right, in a narrow cone, ten metres far.
        const SceneLight spot{.type = GpuLightType::Spot,
                              .position = Vec3(0.0f),
                              .direction = Vec3(1.0f, 0.0f, 0.0f),
                              .range = 10.0f,
                              .innerConeAngle = toRadians(10.0f),
                              .outerConeAngle = toRadians(15.0f)};
        const std::vector<SceneLight> lights = {spot};
        LightClusters clusters;
        buildLightClusters(view, lights, clusters);
        REQUIRE(clusters.lights.size() == 1);

        // Along the cone it is listed ...
        for (const float distance : {0.5f, 3.0f, 6.0f, 9.5f}) {
            CAPTURE(distance);
            CHECK(reaches(clusters, view, Vec3(distance, 0.0f, 0.0f), 0));
            CHECK(reaches(clusters, view, Vec3(distance, distance * std::tan(toRadians(14.0f)), 0.0f), 0));
        }
        // ... and behind the light, where a point light of that range would be, it is not.
        CHECK_FALSE(reaches(clusters, view, Vec3(-6.0f, 0.0f, 0.0f), 0));
        CHECK_FALSE(reaches(clusters, view, Vec3(-2.0f, 6.0f, 0.0f), 0));

        // What the shaders need of the cone: nothing at the outer angle, all at the inner.
        const GpuLight& gpu = clusters.lights[0];
        CHECK(std::cos(toRadians(15.0f)) * gpu.spotScale + gpu.spotOffset == doctest::Approx(0.0f).epsilon(0.001));
        CHECK(std::cos(toRadians(10.0f)) * gpu.spotScale + gpu.spotOffset == doctest::Approx(1.0f).epsilon(0.001));
        CHECK(gpu.direction == Vec3(1.0f, 0.0f, 0.0f));
    }

    TEST_CASE("a light is described to the shaders by type, channels, color and reach") {
        const CameraView view = perspectiveView(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f));
        const std::vector<SceneLight> lights = {{.type = GpuLightType::Point,
                                                 .position = Vec3(1.0f, 2.0f, 3.0f),
                                                 .color = Vec3(4.0f, 5.0f, 6.0f),
                                                 .range = 7.0f,
                                                 .channels = 0x1A5}};
        LightClusters clusters;
        buildLightClusters(view, lights, clusters);
        REQUIRE(clusters.lights.size() == 1);
        const GpuLight& gpu = clusters.lights[0];
        CHECK(gpu.position == Vec3(1.0f, 2.0f, 3.0f));
        CHECK(gpu.color == Vec3(4.0f, 5.0f, 6.0f));
        CHECK(gpu.range == 7.0f);
        // Type in the low byte, the eight channels in the next; more than eight are cut off.
        CHECK(gpu.typeAndChannels == (1u | (0xA5u << 8)));
    }

    TEST_CASE("a builder that is kept gives the same lists as a new one, whatever it built before") {
        // Lights all over, seeded: some in every view below, some in none.
        Random random(42);
        std::vector<SceneLight> lights;
        for (int index = 0; index < 200; ++index) {
            SceneLight light =
                pointLight(Vec3(random.range(-15.0f, 15.0f), random.range(0.0f, 4.0f), random.range(-15.0f, 15.0f)),
                           random.range(0.5f, 6.0f));
            if (index % 4 == 0) {
                light.type = GpuLightType::Spot;
                light.direction = glm::normalize(Vec3(random.range(-1.0f, 1.0f), -1.0f, random.range(-1.0f, 1.0f)));
                light.innerConeAngle = random.range(0.1f, 0.5f);
                light.outerConeAngle = light.innerConeAngle + random.range(0.05f, 0.8f);
            }
            lights.push_back(light);
        }
        lights.push_back({.type = GpuLightType::Directional});

        const std::vector<CameraView> views = {
            perspectiveView(Vec3(0.0f, 3.0f, 14.0f), Vec3(0.0f)),
            // The same lens somewhere else: the boxes of the clusters are the same.
            perspectiveView(Vec3(-9.0f, 1.0f, 2.0f), Vec3(4.0f, 0.5f, -3.0f)),
            // Other lenses: they are not.
            perspectiveView(Vec3(0.0f, 3.0f, 14.0f), Vec3(0.0f), 0.5f, 40.0f),
            orthographicView(Vec3(0.0f, 12.0f, 9.0f), Vec3(0.0f), 20.0f),
        };

        LightClusterBuilder builder;
        LightClusters kept;
        // Twice through, so that every view is also built after every other.
        for (const size_t index : std::initializer_list<size_t>{0, 1, 2, 3, 3, 1, 0, 2, 0, 0}) {
            CAPTURE(index);
            builder.build(views[index], lights, kept);
            LightClusters fresh;
            buildLightClusters(views[index], lights, fresh);

            CHECK(kept.directionalCount == 1);
            CHECK(kept.directionalCount == fresh.directionalCount);
            CHECK(kept.droppedLights == fresh.droppedLights);
            CHECK(kept.grid == fresh.grid);
            CHECK(kept.indices == fresh.indices);
            REQUIRE(kept.lights.size() == fresh.lights.size());
            for (size_t light = 0; light < kept.lights.size(); ++light) {
                CHECK(kept.lights[light].position == fresh.lights[light].position);
                CHECK(kept.lights[light].range == fresh.lights[light].range);
            }
            // There is something in the lists to compare.
            CHECK(kept.lights.size() > 20);
            CHECK(kept.indices.size() > kept.lights.size());
        }
    }

    TEST_CASE("more lights than a view can hold are dropped, the last ones first, and counted") {
        const CameraView view = perspectiveView(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f));
        std::vector<SceneLight> lights;
        for (uint32_t index = 0; index < kMaxDirectionalLights + 2; ++index) {
            lights.push_back({.type = GpuLightType::Directional, .color = Vec3(static_cast<float>(index + 1))});
        }
        for (uint32_t index = 0; index < kMaxLocalLights + 5; ++index) {
            lights.push_back(pointLight(Vec3(0.0f, 0.0f, 0.001f * static_cast<float>(index)), 1.0f));
        }
        // Lights that reach nothing in view do not count towards the limit.
        lights.insert(lights.begin(), pointLight(Vec3(0.0f, 0.0f, 500.0f), 1.0f));

        LightClusters clusters;
        buildLightClusters(view, lights, clusters);
        CHECK(clusters.directionalCount == kMaxDirectionalLights);
        CHECK(clusters.lights.size() == kMaxDirectionalLights + kMaxLocalLights);
        CHECK(clusters.droppedLights == 2 + 5);
        // The first ones are the ones kept.
        CHECK(clusters.lights[kMaxDirectionalLights - 1].color == Vec3(static_cast<float>(kMaxDirectionalLights)));
        CHECK(clusters.lights.back().position.z == doctest::Approx(0.001f * static_cast<float>(kMaxLocalLights - 1)));
        for (const uint32_t light : clusters.indices) {
            REQUIRE(light < clusters.lights.size());
        }
    }
}
