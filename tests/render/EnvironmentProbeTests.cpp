#include "cordite/math/SphericalHarmonics.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/EnvironmentProbes.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/rhi/TextureReadback.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <fmt/format.h>
#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::HalfPicture;
using cordite::test::SceneFixture;
using cordite::test::sceneLight;
using cordite::test::unwrap;

namespace {

/// Takes the pictures of the probes of the scene that need them, as a frame of a game
/// would before its view.
void takeProbes(SceneFixture& scene, const SceneRenderSettings& settings = SceneFixture::flatSettings()) {
    scene.world.updateWorldTransforms();
    test::recordAndExecute(scene.context, [&](nvrhi::ICommandList* commandList) {
        scene.renderer->renderCaptures(commandList, scene.world, settings);
    });
    scene.context.graphics->waitIdle();
}

/// Takes the pictures of every probe that needs one, a frame after another as a game
/// would, until none is left: a frame takes kMaxProbeCapturesPerFrame, in the order of the
/// ids of their entities, which are random.
void takeAllProbes(SceneFixture& scene) {
    for (uint32_t frame = 0; frame <= kMaxEnvironmentProbes; ++frame) {
        const uint32_t before = scene.renderer->probes().captureCount();
        takeProbes(scene);
        if (scene.renderer->probes().captureCount() == before) {
            return;
        }
    }
    FAIL("the probes are still being taken after a frame for every probe there can be");
}

/// The mean of the four pixels that meet at the corner (x, y). On a ball whose middle is
/// there, the four lean away from the middle's normal alike, each the other way: what one
/// leans towards a light another leans away from.
Vec3 meanAround(const HalfPicture& picture, uint32_t x, uint32_t y) {
    return (picture.at(x - 1, y - 1) + picture.at(x, y - 1) + picture.at(x - 1, y) + picture.at(x, y)) / 4.0f;
}

/// The axes of the world, in the order of the faces of a cube map.
constexpr std::array<Vec3, kCubeFaceCount> kAxes = {Vec3(1.0f, 0.0f, 0.0f), Vec3(-1.0f, 0.0f, 0.0f),
                                                    Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f),
                                                    Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, -1.0f)};
constexpr std::array<Vec3, kCubeFaceCount> kWallColors = {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f),
                                                          Vec3(0.0f, 0.0f, 1.0f), Vec3(1.0f, 1.0f, 0.0f),
                                                          Vec3(0.0f, 1.0f, 1.0f), Vec3(1.0f, 0.0f, 1.0f)};

/// A closed room about the origin, `half` metres to each wall, of flat materials: each wall
/// of `colors` by the face of a cube map it faces.
void buildRoom(SceneFixture& scene, float half, const std::array<Vec3, kCubeFaceCount>& colors) {
    for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
        const Vec3 axis = kAxes[face];
        // As thick as a tenth of a metre, and wide enough to close the corners.
        const Vec3 size = glm::mix(Vec3(2.0f * half + 0.2f), Vec3(0.1f), glm::abs(axis));
        scene.add(BuiltinMesh::Cube, scene.material(fmt::format("wall{}", face)), at(axis * (half + 0.05f), size));
        (void)colors;
    }
}

void writeRoomMaterials(SceneFixture& scene, const std::array<Vec3, kCubeFaceCount>& colors) {
    for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
        scene.writeFlatMaterial(fmt::format("wall{}", face), colors[face]);
    }
}

/// Puts an environment probe at `position`, its box `size` metres a side.
Entity addProbe(SceneFixture& scene, const Vec3& position, float size,
                const EnvironmentProbeComponent& component = {}) {
    const Entity entity = scene.world.createEntity("Probe");
    scene.world.transform(entity) = at(position, Vec3(size));
    scene.world.add<EnvironmentProbeComponent>(entity) = component;
    return entity;
}

/// The texel of a face of `size` texels whose direction from the middle of the cube is
/// nearest to `direction`.
UVec2 texelTowards(uint32_t face, const Vec3& direction, uint32_t size) {
    UVec2 best(0);
    float closest = -2.0f;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const Vec2 uv = (Vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) / static_cast<float>(size);
            const float alike = glm::dot(cubeFaceDirection(face, uv), direction);
            if (alike > closest) {
                closest = alike;
                best = UVec2(x, y);
            }
        }
    }
    return best;
}

} // namespace

TEST_SUITE("EnvironmentProbes") {
    TEST_CASE("a probe's picture has every face of the cube where a cube map has it, each the right way round") {
        SceneFixture scene;
        writeRoomMaterials(scene, kWallColors);
        scene.writeFlatMaterial("mark", Vec3(1.0f));
        scene.open();
        buildRoom(scene, 1.0f, kWallColors);
        // A white mark on each wall, off its middle along both of its other axes.
        std::array<Vec3, kCubeFaceCount> marks{};
        for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
            const Vec3 axis = kAxes[face];
            const Vec3 first = axis.x != 0.0f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
            const Vec3 second = glm::cross(glm::abs(axis), first);
            marks[face] = axis * 0.97f + first * 0.5f + second * 0.3f;
            scene.add(BuiltinMesh::Cube, scene.material("mark"), at(marks[face], Vec3(0.12f)));
        }
        addProbe(scene, Vec3(0.0f), 2.0f);
        takeProbes(scene);
        REQUIRE(scene.renderer->probes().captureCount() == 1);

        for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
            CAPTURE(face);
            const HalfPicture picture{
                unwrap(readTexture(*scene.context.graphics, scene.renderer->probes().captureCube(), 0, face))};
            REQUIRE(picture.pixels.width == kProbeFaceSize);
            // The middle of the face is the wall it faces.
            CHECK_NEAR(picture.at(kProbeFaceSize / 2, kProbeFaceSize / 2), kWallColors[face], 1.0e-3f);
            // And the mark is where its direction is on the face.
            const UVec2 mark = texelTowards(face, glm::normalize(marks[face]), kProbeFaceSize);
            CAPTURE(mark.x);
            CAPTURE(mark.y);
            CHECK_NEAR(picture.at(mark.x, mark.y), Vec3(1.0f), 1.0e-3f);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a probe in a red room lights a white, matte ball in its box red, and nothing outside its box") {
        SceneFixture scene;
        std::array<Vec3, kCubeFaceCount> red{};
        red.fill(Vec3(1.0f, 0.0f, 0.0f));
        writeRoomMaterials(scene, red);
        scene.writeMatteMaterial("white");
        scene.open();
        buildRoom(scene, 3.0f, red);
        // The probe's box two metres a side about the origin; one small ball inside it, one
        // outside it but in the room.
        addProbe(scene, Vec3(0.0f), 2.0f, {.blendDistance = 0.0f});
        scene.add(BuiltinMesh::Sphere, scene.material("white"), at(Vec3(-0.6f, 0.0f, 0.0f), Vec3(0.2f)));
        scene.add(BuiltinMesh::Sphere, scene.material("white"), at(Vec3(1.6f, 0.0f, 0.0f), Vec3(0.2f)));
        takeProbes(scene);

        // Seen from inside the room, along -Z, without perspective.
        const CameraView view = scene.orthographicView(Vec3(0.5f, 0.0f, 2.5f), Vec3(0.5f, 0.0f, 0.0f), 4.0f);
        scene.render(view);
        const HalfPicture light = sceneLight(scene);
        // The middles of the balls: x = -0.6 and 1.6 of a view four metres wide about 0.5.
        const auto column = [](float x) { return static_cast<uint32_t>((x - 0.5f + 2.0f) / 4.0f * 64.0f); };
        const Vec3 inside = light.at(column(-0.6f), 32);
        const Vec3 outside = light.at(column(1.6f), 32);
        CAPTURE(inside.r);
        CAPTURE(outside.r);
        // The room, as a white matte surface gives it back - but for the little of it the
        // ball itself hides from the probe.
        CHECK(std::abs(inside.r - 1.0f) < 0.02f);
        CHECK(inside.g < 1.0e-3f);
        CHECK(inside.b < 1.0e-3f);
        // No environment, no light: nothing.
        CHECK(outside == Vec3(0.0f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a mirror reflects the walls of a probe's room where its reflection meets them, with box projection or "
              "as if far away") {
        SceneFixture scene;
        writeRoomMaterials(scene, kWallColors);
        scene.writeMaterial("mirror", R"("baseColor": [1, 1, 1, 1], "metallic": 1, "roughness": 0)");
        scene.open();
        buildRoom(scene, 1.0f, kWallColors);
        const Entity probe = addProbe(scene, Vec3(0.0f), 2.0f, {.blendDistance = 0.0f});
        const Vec3 ballCentre(-0.55f, -0.3f, 0.4f);
        constexpr float kRadius = 0.15f;
        scene.add(BuiltinMesh::Sphere, scene.material("mirror"), at(ballCentre, Vec3(2.0f * kRadius)));
        // Inside the room, in front of the ball, looking at it along -Z without perspective.
        constexpr float kHeight = 0.4f;
        const CameraView view = scene.orthographicView(ballCentre + Vec3(0.0f, 0.0f, 0.5f), ballCentre, kHeight);

        uint32_t decisive = 0;
        for (const bool projected : {true, false}) {
            CAPTURE(projected);
            scene.world.tryGet<EnvironmentProbeComponent>(probe)->boxProjection = projected;
            scene.renderer->retakeProbes();
            takeProbes(scene);
            scene.render(view);
            const HalfPicture light = sceneLight(scene);
            const GpuEnvironmentProbe described = gpuProbeOf(scene.world.tryGet<WorldTransform>(probe)->matrix,
                                                             *scene.world.tryGet<EnvironmentProbeComponent>(probe), 0);
            const GpuEnvironmentProbe other = gpuProbeOf(scene.world.tryGet<WorldTransform>(probe)->matrix,
                                                         {.blendDistance = 0.0f, .boxProjection = !projected}, 0);
            uint32_t checked = 0;
            for (uint32_t y = 4; y < 64; y += 3) {
                for (uint32_t x = 4; x < 64; x += 3) {
                    // Where the pixel's line of sight meets the ball, and which way the ball faces there.
                    const Vec2 offset = (Vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) / 64.0f - 0.5f;
                    const Vec2 across(offset.x * kHeight, -offset.y * kHeight);
                    const float inside = kRadius * kRadius - glm::dot(across, across);
                    if (inside <= 0.0f) {
                        continue;
                    }
                    const Vec3 normal = glm::normalize(Vec3(across, std::sqrt(inside)));
                    // Seen head-on enough to be well inside the outline of the ball, which is a
                    // mesh and not quite a sphere.
                    if (normal.z < 0.6f) {
                        continue;
                    }
                    const Vec3 point = ballCentre + normal * kRadius;
                    const Vec3 reflected = glm::reflect(Vec3(0.0f, 0.0f, -1.0f), normal);
                    const Vec3 looked = probeReflectionDirection(described, point, reflected);
                    // Not where the picture sees the ball itself, and clearly towards one wall.
                    if (glm::dot(looked, glm::normalize(ballCentre)) > std::cos(0.3f)) {
                        continue;
                    }
                    const Vec3 size = glm::abs(looked);
                    const float largest = std::max({size.x, size.y, size.z});
                    const float second = size.x + size.y + size.z - largest - std::min({size.x, size.y, size.z});
                    if (second > 0.8f * largest) {
                        continue;
                    }
                    uint32_t face = 0;
                    if (size.y == largest) {
                        face = looked.y > 0.0f ? 2 : 3;
                    } else if (size.z == largest) {
                        face = looked.z > 0.0f ? 4 : 5;
                    } else {
                        face = looked.x > 0.0f ? 0 : 1;
                    }
                    CAPTURE(x);
                    CAPTURE(y);
                    CAPTURE(face);
                    CHECK_NEAR(light.at(x, y), kWallColors[face], 0.08f);
                    ++checked;
                    // Where the other way of looking it up would have found another wall.
                    const Vec3 otherLooked = probeReflectionDirection(other, point, reflected);
                    const Vec3 otherSize = glm::abs(otherLooked);
                    const float otherLargest = std::max({otherSize.x, otherSize.y, otherSize.z});
                    const bool sameAxis = (otherSize.x == otherLargest && size.x == largest) ||
                                          (otherSize.y == otherLargest && size.y == largest) ||
                                          (otherSize.z == otherLargest && size.z == largest);
                    decisive += sameAxis ? 0 : 1;
                }
            }
            CHECK(checked > 60);
        }
        // The two ways see different walls in enough of the ball to tell them apart.
        CHECK(decisive > 10);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a probe's harmonics are those of its picture: a room lit from above lights a ball's top more than "
              "its side") {
        SceneFixture scene;
        // A white ceiling, and the rest of the room black.
        std::array<Vec3, kCubeFaceCount> ceiling{};
        ceiling[2] = Vec3(1.0f);
        writeRoomMaterials(scene, ceiling);
        scene.writeMatteMaterial("white");
        scene.open();
        buildRoom(scene, 3.0f, ceiling);
        addProbe(scene, Vec3(0.0f), 6.0f, {.blendDistance = 0.0f});
        const Vec3 ball(-0.8f, 0.0f, 0.0f);
        scene.add(BuiltinMesh::Sphere, scene.material("white"), at(ball, Vec3(0.1f)));
        takeProbes(scene);

        // The same nine numbers on the CPU, from the same picture: texel by texel, each by
        // the part of the sphere it covers.
        SphericalHarmonics9 light;
        constexpr uint32_t kSize = kProbeFaceSize;
        for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
            for (uint32_t y = 0; y < kSize; ++y) {
                for (uint32_t x = 0; x < kSize; ++x) {
                    const Vec2 uv =
                        (Vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) / static_cast<float>(kSize);
                    const Vec2 onFace = uv * 2.0f - 1.0f;
                    const float step = 2.0f / static_cast<float>(kSize);
                    const float solidAngle = step * step / std::pow(1.0f + glm::dot(onFace, onFace), 1.5f);
                    light.add(cubeFaceDirection(face, uv), ceiling[face], solidAngle);
                }
            }
        }
        const auto expected = [&](const Vec3& normal) { return light.irradiance(normal).x / kPi; };
        // From in front, the middle of the ball faces +Z; from above, +Y. The ball is sixteen
        // pixels wide, and a pixel beside its middle leans towards the ceiling or away by
        // enough to change its light by a sixth: the four around the middle are taken.
        scene.render(scene.orthographicView(ball + Vec3(0.0f, 0.0f, 1.0f), ball, 0.4f));
        const float side = meanAround(sceneLight(scene), 32, 32).x;
        scene.render(scene.orthographicView(ball + Vec3(0.0f, 1.0f, 0.0f), ball, 0.4f, Vec3(0.0f, 0.0f, -1.0f)));
        const float top = meanAround(sceneLight(scene), 32, 32).x;
        CAPTURE(side);
        CAPTURE(top);
        CHECK(std::abs(side - expected(Vec3(0.0f, 0.0f, 1.0f))) < 0.01f);
        CHECK(std::abs(top - expected(Vec3(0.0f, 1.0f, 0.0f))) < 0.01f);
        CHECK(top > 2.0f * side);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a rough metal reflects a probe blurred: white walls as brightly as an even environment, and of a "
              "black wall the white ones around it") {
        SceneFixture scene;
        // White walls, and a black one at +Z.
        std::array<Vec3, kCubeFaceCount> walls{};
        walls.fill(Vec3(1.0f));
        walls[4] = Vec3(0.0f);
        writeRoomMaterials(scene, walls);
        constexpr std::array<int, 3> kTenths = {0, 5, 10};
        for (const int tenths : kTenths) {
            scene.writeMaterial(fmt::format("metal{}", tenths),
                                fmt::format(R"("baseColor": [1, 1, 1, 1], "metallic": 1, "roughness": {})",
                                            static_cast<float>(tenths) / 10.0f));
        }
        scene.open();
        buildRoom(scene, 2.0f, walls);
        addProbe(scene, Vec3(0.0f), 4.0f, {.blendDistance = 0.0f, .boxProjection = false});
        // A mirror at -X, the roughest at +X, a metre apart: in a picture 3.2 metres wide
        // their middles are where pixels meet, twenty pixels apart.
        for (size_t index = 0; index < kTenths.size(); ++index) {
            scene.add(BuiltinMesh::Sphere, scene.material(fmt::format("metal{}", kTenths[index])),
                      at(Vec3(static_cast<float>(index) - 1.0f, -1.0f, 0.0f), Vec3(0.6f)));
        }
        takeProbes(scene);

        // Seen from -Z, a ball's middle reflects white walls, however blurred. What a white
        // metal throws back of an even light is one less 0.55 times its roughness
        // (environmentReflectance in Brdf.slang), as of an even environment: a level of the
        // probe that lost or made light when it was blurred would show here. From that side
        // +X is on the left.
        scene.render(scene.orthographicView(Vec3(0.0f, -1.0f, -1.5f), Vec3(0.0f, -1.0f, 0.0f), 3.2f));
        const HalfPicture ofWhite = sceneLight(scene);
        // Seen from +Z, the middles reflect the black wall: a mirror that alone, a rough
        // ball the white walls around it as well.
        scene.render(scene.orthographicView(Vec3(0.0f, -1.0f, 1.5f), Vec3(0.0f, -1.0f, 0.0f), 3.2f));
        const HalfPicture ofBlack = sceneLight(scene);
        std::array<float, kTenths.size()> blurred{};
        for (size_t index = 0; index < kTenths.size(); ++index) {
            CAPTURE(kTenths[index]);
            const float white = meanAround(ofWhite, 52 - static_cast<uint32_t>(index) * 20, 32).r;
            CAPTURE(white);
            CHECK(std::abs(white - (1.0f - 0.055f * static_cast<float>(kTenths[index]))) < 0.02f);
            blurred[index] = meanAround(ofBlack, 12 + static_cast<uint32_t>(index) * 20, 32).r;
        }
        CAPTURE(blurred[0]);
        CAPTURE(blurred[1]);
        CAPTURE(blurred[2]);
        CHECK(blurred[0] < 0.01f);
        CHECK(blurred[1] > blurred[0]);
        CHECK(blurred[2] > blurred[1] + 0.05f);
        // At a roughness of one, what a surface reflects is spread over the half of
        // everything around that it faces as light that falls on it is - by the cosine. Of
        // that, the black wall is 0.554 (what a face of a cube is of the light at its
        // middle); the rest is thrown back as of an even light.
        CHECK(std::abs(blurred[2] - (1.0f - 0.554f) * 0.45f) < 0.02f);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("towards the faces of its box a probe fades into the environment, evenly over its blend distance") {
        SceneFixture scene;
        std::array<Vec3, kCubeFaceCount> red{};
        red.fill(Vec3(1.0f, 0.0f, 0.0f));
        writeRoomMaterials(scene, red);
        scene.writeMatteMaterial("white");
        // An environment that is blue everywhere: nothing in a scene keeps its light out.
        Image blue = unwrap(Image::create(8, 4, ImageFormat::Rgba32F));
        for (uint32_t y = 0; y < blue.height(); ++y) {
            for (uint32_t x = 0; x < blue.width(); ++x) {
                blue.setPixel(x, y, Vec4(0.0f, 0.0f, 1.0f, 1.0f));
            }
        }
        scene.writeHdrImage("blue", blue);
        scene.open();
        buildRoom(scene, 3.0f, red);
        scene.setEnvironment(scene.asset("blue.hdr"));
        addProbe(scene, Vec3(0.0f), 2.0f, {.blendDistance = 0.5f});
        // In the middle of the box, a fifth of a metre from its +X face, and outside it.
        const std::array<float, 3> places = {0.0f, 0.8f, 1.6f};
        for (const float x : places) {
            scene.add(BuiltinMesh::Sphere, scene.material("white"), at(Vec3(x, 0.0f, 0.0f), Vec3(0.2f)));
        }
        takeProbes(scene);

        const CameraView view = scene.orthographicView(Vec3(0.5f, 0.0f, 2.5f), Vec3(0.5f, 0.0f, 0.0f), 4.0f);
        scene.render(view);
        const HalfPicture light = sceneLight(scene);
        const auto column = [](float x) { return static_cast<uint32_t>((x - 0.5f + 2.0f) / 4.0f * 64.0f); };
        // A pixel by the middle of a ball shows the front of the ball, near enough to the
        // middle of the box along Y and Z: its share goes by how far the middle of the pixel
        // is from the +X face, half a metre of blend.
        for (size_t ball = 0; ball < places.size(); ++ball) {
            CAPTURE(ball);
            const uint32_t x = column(places[ball]);
            const float pixelX = (static_cast<float>(x) + 0.5f) / 64.0f * 4.0f - 1.5f;
            const float share = std::clamp((1.0f - pixelX) / 0.5f, 0.0f, 1.0f);
            CAPTURE(share);
            const Vec3 seen = light.at(x, 32);
            CAPTURE(seen.r);
            CAPTURE(seen.b);
            CHECK(std::abs(seen.r - share) < 0.02f);
            CHECK(std::abs(seen.b - (1.0f - share)) < 0.02f);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("of probes whose boxes overlap, the higher priority is laid over the lower, and of the same, the lower "
              "id") {
        SceneFixture scene;
        std::array<Vec3, kCubeFaceCount> red{};
        red.fill(Vec3(1.0f, 0.0f, 0.0f));
        writeRoomMaterials(scene, red);
        scene.writeMatteMaterial("white");
        scene.open();
        buildRoom(scene, 3.0f, red);
        const Entity half = addProbe(scene, Vec3(0.0f), 2.0f, {.blendDistance = 0.0f, .intensity = 0.5f});
        const Entity whole = addProbe(scene, Vec3(0.0f), 2.0f, {.blendDistance = 0.0f, .intensity = 1.0f});
        scene.add(BuiltinMesh::Sphere, scene.material("white"), at(Vec3(-0.6f, 0.0f, 0.0f), Vec3(0.2f)));
        const CameraView view = scene.orthographicView(Vec3(0.5f, 0.0f, 2.5f), Vec3(0.5f, 0.0f, 0.0f), 4.0f);
        const auto ballRed = [&] {
            takeAllProbes(scene);
            scene.render(view);
            return sceneLight(scene).at(static_cast<uint32_t>((-0.6f - 0.5f + 2.0f) / 4.0f * 64.0f), 32).r;
        };

        scene.world.tryGet<EnvironmentProbeComponent>(half)->priority = 1;
        CHECK(std::abs(ballRed() - 0.5f) < 0.01f);
        scene.world.tryGet<EnvironmentProbeComponent>(half)->priority = -1;
        CHECK(std::abs(ballRed() - 1.0f) < 0.02f);
        // The same priority: the one whose entity has the lower id.
        scene.world.tryGet<EnvironmentProbeComponent>(half)->priority = 0;
        const float first = scene.world.id(half) < scene.world.id(whole) ? 0.5f : 1.0f;
        CHECK(std::abs(ballRed() - first) < 0.02f);

        // Where the higher fades, the lower has what it leaves. The pixel shows the front of
        // the ball about 0.4 metres from the -X face of both boxes, and the higher fades over
        // 0.8 metres: about half of the light is the higher's, and the rest the lower's.
        scene.world.tryGet<EnvironmentProbeComponent>(half)->priority = 1;
        scene.world.tryGet<EnvironmentProbeComponent>(half)->blendDistance = 0.8f;
        const uint32_t column = static_cast<uint32_t>((-0.6f - 0.5f + 2.0f) / 4.0f * 64.0f);
        const float share = ((static_cast<float>(column) + 0.5f) / 64.0f * 4.0f - 1.5f + 1.0f) / 0.8f;
        CAPTURE(share);
        CHECK(std::abs(ballRed() - (0.5f * share + (1.0f - share))) < 0.02f);
        // The view of the probes shows the same shares, in the colors of the first two probes
        // of the view: the higher first.
        SceneRenderSettings shares = SceneFixture::flatSettings();
        shares.debugView = RenderDebugView::Probes;
        scene.render(view, shares);
        const Vec3 seen = sceneLight(scene).at(column, 32);
        CAPTURE(seen);
        CHECK(glm::distance(seen, share * Vec3(1.0f, 0.2f, 0.2f) + (1.0f - share) * Vec3(0.2f, 1.0f, 0.2f)) < 0.02f);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a probe's picture is taken without reflections from the screen, whether the views have them or not") {
        SceneFixture scene;
        scene.writeMaterial("mirror", R"("baseColor": [1, 1, 1, 1], "metallic": 1, "roughness": 0)");
        scene.writeFlatMaterial("wall", Vec3(0.2f, 0.8f, 0.4f));
        scene.open();
        // A mirror floor a metre under the probe, and a wall of light on it in front of the
        // probe: the screen would show the wall in the floor.
        scene.add(BuiltinMesh::Plane, scene.material("mirror"), at(Vec3(0.0f, -1.0f, 0.0f), Vec3(10.0f, 1.0f, 10.0f)));
        scene.add(BuiltinMesh::Cube, scene.material("wall"), at(Vec3(0.0f, 0.0f, -2.05f), Vec3(6.0f, 2.0f, 0.1f)));
        addProbe(scene, Vec3(0.0f), 8.0f);
        const auto faces = [&](bool screenReflections) {
            SceneRenderSettings settings = SceneFixture::flatSettings();
            settings.screenEffectsQuality = ScreenEffectsQuality::High;
            settings.screenReflections = screenReflections;
            scene.renderer->retakeProbes();
            takeProbes(scene, settings);
            std::vector<std::vector<std::byte>> pictures;
            for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
                pictures.push_back(
                    unwrap(readTexture(*scene.context.graphics, scene.renderer->probes().captureCube(), 0, face)).data);
            }
            return pictures;
        };
        CHECK(faces(true) == faces(false));
        // The floor under the wall, as the face towards -Z sees it, reflects nothing there is.
        const HalfPicture front{
            unwrap(readTexture(*scene.context.graphics, scene.renderer->probes().captureCube(), 0, 5))};
        CHECK(front.at(kProbeFaceSize / 2, kProbeFaceSize * 7 / 8) == Vec3(0.0f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a probe is taken once, and again when it moves, when the assets are read again or when it is asked to "
              "be, a few in a frame") {
        SceneFixture scene;
        std::array<Vec3, kCubeFaceCount> red{};
        red.fill(Vec3(1.0f, 0.0f, 0.0f));
        writeRoomMaterials(scene, red);
        scene.open();
        buildRoom(scene, 3.0f, red);
        const Entity probe = addProbe(scene, Vec3(0.0f), 2.0f);
        const EnvironmentProbes& probes = scene.renderer->probes();
        takeProbes(scene);
        CHECK(probes.captureCount() == 1);
        CHECK(scene.renderer->takeFrameStatistics().probeCaptures == 1);
        takeProbes(scene);
        CHECK(probes.captureCount() == 1);
        // Turned and scaled, its picture is the same; moved, it is not.
        scene.world.transform(probe).rotation = glm::angleAxis(0.5f, Vec3(0.0f, 1.0f, 0.0f));
        scene.world.transform(probe).scale = Vec3(3.0f);
        takeProbes(scene);
        CHECK(probes.captureCount() == 1);
        scene.world.transform(probe).position = Vec3(0.1f, 0.0f, 0.0f);
        takeProbes(scene);
        CHECK(probes.captureCount() == 2);
        scene.renderer->retakeProbes();
        takeProbes(scene);
        CHECK(probes.captureCount() == 3);
        takeProbes(scene);
        CHECK(probes.captureCount() == 3);

        // Three more, as many in a frame as a frame takes.
        for (int index = 0; index < 3; ++index) {
            addProbe(scene, Vec3(0.5f * static_cast<float>(index), 1.0f, 0.0f), 1.0f);
        }
        for (uint32_t frame = 1; frame <= 3; ++frame) {
            takeProbes(scene);
            CHECK(probes.captureCount() == 3 + std::min(frame * kMaxProbeCapturesPerFrame, 3u));
        }
        takeProbes(scene);
        CHECK(probes.captureCount() == 6);
        // Six views a picture: five pictures since the statistics were last taken.
        CHECK(scene.renderer->takeFrameStatistics().views == 5 * kCubeFaceCount);
        // Assets that were read again may look otherwise: every probe is taken again.
        REQUIRE(scene.assets->refresh().has_value());
        for (uint32_t frame = 1; frame <= 4; ++frame) {
            takeProbes(scene);
            CHECK(probes.captureCount() == 6 + std::min(frame * kMaxProbeCapturesPerFrame, 4u));
        }
        takeProbes(scene);
        CHECK(probes.captureCount() == 10);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE(
        "light bounces once: a probe's picture is not lit by probes, and a view told not to use them is as before") {
        SceneFixture scene;
        // Five walls of light, and one white, matte wall, which only a probe could light.
        std::array<Vec3, kCubeFaceCount> red{};
        red.fill(Vec3(1.0f, 0.0f, 0.0f));
        writeRoomMaterials(scene, red);
        scene.writeMatteMaterial("wall5");
        scene.writeMatteMaterial("white");
        scene.open();
        buildRoom(scene, 3.0f, red);
        addProbe(scene, Vec3(0.0f), 2.0f, {.blendDistance = 0.0f});
        scene.add(BuiltinMesh::Sphere, scene.material("white"), at(Vec3(-0.6f, 0.0f, 0.0f), Vec3(0.2f)));
        const CameraView view = scene.orthographicView(Vec3(0.5f, 0.0f, 2.5f), Vec3(0.5f, 0.0f, 0.0f), 4.0f);
        takeProbes(scene);
        scene.render(view);
        const std::vector<std::byte> once = sceneLight(scene).pixels.data;
        // Taken again, it sees the white wall as dark as before: the first picture lights it
        // in no picture.
        scene.renderer->retakeProbes();
        takeProbes(scene);
        scene.render(view);
        CHECK(sceneLight(scene).pixels.data == once);

        // Told to use no probes, a view is drawn as if there were none.
        SceneRenderSettings without = SceneFixture::flatSettings();
        without.environmentProbes = false;
        scene.render(view, without);
        const std::vector<std::byte> unlit = sceneLight(scene).pixels.data;
        CHECK_FALSE(unlit == once);
        const auto probeView = scene.world.registry().view<EnvironmentProbeComponent>();
        const std::vector<entt::entity> handles(probeView.begin(), probeView.end());
        for (const entt::entity handle : handles) {
            scene.world.remove<EnvironmentProbeComponent>(Entity{handle});
        }
        scene.render(view);
        CHECK(sceneLight(scene).pixels.data == unlit);
        checkNoValidationIssues(*scene.context.graphics);
    }
}
