#include "cordite/renderer/ScreenReflections.h"

#include "cordite/math/Transform.h"
#include "cordite/world/Components.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

using namespace cordite;

namespace {

constexpr UVec2 kSize(320, 180);
constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// A camera a metre and a half above a floor, three metres in front of a wall that stands
/// on it, looking at the wall.
const Vec3 kEye(0.0f, 1.5f, 3.0f);

CameraView sceneCamera(float farPlane = 100.0f) {
    CameraComponent camera;
    camera.verticalFovDegrees = 60.0f;
    camera.nearPlane = 0.1f;
    camera.farPlane = farPlane;
    const Transform placement{.position = kEye,
                              .rotation = lookRotation(glm::normalize(Vec3(0.0f, 0.8f, -2.0f) - kEye))};
    return makeCameraView(placement.toMatrix(), camera, static_cast<float>(kSize.x) / static_cast<float>(kSize.y));
}

/// A rectangle of a scene: the plane through `point` that faces `normal`, where it is
/// between `lower` and `upper`.
struct Rectangle {
    Vec3 point;
    Vec3 normal;
    Vec3 lower;
    Vec3 upper;
};

/// The floor, ten metres a side; the wall at z = -2, six metres wide and two high; and a
/// strip twenty centimetres high, two metres in front of the wall.
const Rectangle kFloor{
    .point = Vec3(0.0f), .normal = Vec3(0.0f, 1.0f, 0.0f), .lower = Vec3(-5.0f, 0.0f, -5.0f), .upper = Vec3(5.0f)};
const Rectangle kWall{.point = Vec3(0.0f, 0.0f, -2.0f),
                      .normal = Vec3(0.0f, 0.0f, 1.0f),
                      .lower = Vec3(-3.0f, 0.0f, -2.0f),
                      .upper = Vec3(3.0f, 2.0f, -2.0f)};
const Rectangle kStrip{.point = Vec3(0.0f),
                       .normal = Vec3(0.0f, 0.0f, 1.0f),
                       .lower = Vec3(-3.0f, 0.9f, 0.0f),
                       .upper = Vec3(3.0f, 1.1f, 0.0f)};

/// Where a point of the world is in the picture of `view`: x and y from (0, 0) at the top
/// left to (1, 1), z its reversed depth.
Vec3 inPicture(const CameraView& view, const Vec3& point) {
    const Vec4 clip = view.viewProjection * Vec4(point, 1.0f);
    const Vec3 ndc = Vec3(clip) / clip.w;
    return {ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f, ndc.z};
}

/// The depth a picture of `scene` holds in the pixel at `uv`: that of what the line of sight
/// through the middle of the pixel meets first, as a GPU draws it; 0 where it meets
/// nothing, or only beyond the far plane, and outside the picture.
float depthOfPixel(const CameraView& view, std::span<const Rectangle> scene, Vec2 uv) {
    const Vec2 pixel = glm::floor(uv * Vec2(kSize));
    if (pixel.x < 0.0f || pixel.y < 0.0f || pixel.x >= static_cast<float>(kSize.x) ||
        pixel.y >= static_cast<float>(kSize.y)) {
        return 0.0f;
    }
    const Vec2 middle = (pixel + 0.5f) / Vec2(kSize);
    const Vec4 far = view.inverseViewProjection * Vec4(middle.x * 2.0f - 1.0f, 1.0f - middle.y * 2.0f, 0.5f, 1.0f);
    const Vec3 sight = glm::normalize(Vec3(far) / far.w - view.position);
    float nearest = std::numeric_limits<float>::infinity();
    for (const Rectangle& rectangle : scene) {
        const float facing = glm::dot(sight, rectangle.normal);
        if (std::abs(facing) < 1.0e-6f) {
            continue;
        }
        const float distance = glm::dot(rectangle.point - view.position, rectangle.normal) / facing;
        const Vec3 met = view.position + sight * distance;
        const bool inside = glm::all(glm::greaterThanEqual(met, rectangle.lower - 1.0e-4f)) &&
                            glm::all(glm::lessThanEqual(met, rectangle.upper + 1.0e-4f));
        if (distance > 0.0f && inside) {
            nearest = std::min(nearest, distance);
        }
    }
    return std::isfinite(nearest) ? std::max(inPicture(view, view.position + sight * nearest).z, 0.0f) : 0.0f;
}

ScreenReflectionSettings walk(uint32_t steps = 64) {
    return {.intensity = 1.0f, .maxRoughness = 0.4f, .maxDistance = 20.0f, .thickness = 0.3f, .steps = steps};
}

/// The reflection of the line of sight off the floor at `point`.
Vec3 offTheFloor(const Vec3& point) {
    return glm::reflect(glm::normalize(point - kEye), Vec3(0.0f, 1.0f, 0.0f));
}

} // namespace

TEST_SUITE("ScreenReflections") {
    TEST_CASE("a reflection is trusted fully inside the picture, and less over its last tenth at each border") {
        CHECK(screenEdgeFade(Vec2(0.5f)) == 1.0f);
        CHECK(std::abs(screenEdgeFade(Vec2(0.1f, 0.5f)) - 1.0f) < 1.0e-5f);
        CHECK(std::abs(screenEdgeFade(Vec2(0.05f, 0.5f)) - 0.5f) < 1.0e-5f);
        CHECK(std::abs(screenEdgeFade(Vec2(0.5f, 0.975f)) - 0.25f) < 1.0e-4f);
        // The border that is nearest decides.
        CHECK(std::abs(screenEdgeFade(Vec2(0.02f, 0.97f)) - 0.2f) < 1.0e-4f);
        CHECK(screenEdgeFade(Vec2(0.0f, 0.5f)) == 0.0f);
        CHECK(screenEdgeFade(Vec2(1.2f, 0.5f)) == 0.0f);
        CHECK(screenEdgeFade(Vec2(0.5f, -0.1f)) == 0.0f);
        CHECK(screenEdgeFade(Vec2(kNotANumber, 0.5f)) == 0.0f);
        CHECK(screenEdgeFade(Vec2(0.5f, kNotANumber)) == 0.0f);
        CHECK(screenEdgeFade(Vec2(0.5f, std::numeric_limits<float>::infinity())) == 0.0f);
    }

    TEST_CASE("a reflection is trusted fully for three quarters of its way, and a surface up to half as rough as "
              "the roughest takes all of it") {
        CHECK(reflectionDistanceFade(0.0f, 20.0f) == 1.0f);
        CHECK(reflectionDistanceFade(15.0f, 20.0f) == 1.0f);
        CHECK(std::abs(reflectionDistanceFade(17.5f, 20.0f) - 0.5f) < 1.0e-5f);
        CHECK(reflectionDistanceFade(20.0f, 20.0f) == 0.0f);
        CHECK(reflectionDistanceFade(25.0f, 20.0f) == 0.0f);
        CHECK(reflectionDistanceFade(1.0f, 0.0f) == 0.0f);
        CHECK(reflectionDistanceFade(kNotANumber, 20.0f) == 0.0f);
        CHECK(reflectionDistanceFade(1.0f, std::numeric_limits<float>::infinity()) == 0.0f);

        CHECK(reflectionRoughnessFade(0.0f, 0.4f) == 1.0f);
        CHECK(reflectionRoughnessFade(0.2f, 0.4f) == 1.0f);
        CHECK(std::abs(reflectionRoughnessFade(0.3f, 0.4f) - 0.5f) < 1.0e-5f);
        CHECK(reflectionRoughnessFade(0.4f, 0.4f) == 0.0f);
        CHECK(reflectionRoughnessFade(0.9f, 0.4f) == 0.0f);
        CHECK(reflectionRoughnessFade(0.0f, 0.0f) == 0.0f);
        CHECK(reflectionRoughnessFade(kNotANumber, 0.4f) == 0.0f);
    }

    TEST_CASE("a view takes the look of its scene, and its quality how finely the reflections are followed") {
        PostProcessSettings look;
        look.reflectionIntensity = 0.75f;
        look.reflectionMaxRoughness = 0.3f;
        look.reflectionMaxDistance = 12.0f;
        look.reflectionThickness = 0.2f;
        const ScreenReflectionSettings medium = screenReflectionSettingsFor(look, ScreenEffectsQuality::Medium);
        CHECK(medium.intensity == 0.75f);
        CHECK(medium.maxRoughness == 0.3f);
        CHECK(medium.maxDistance == 12.0f);
        CHECK(medium.thickness == 0.2f);
        CHECK(medium.divisor == screenEffectsSettingsFor(ScreenEffectsQuality::Medium).reflectionDivisor);
        CHECK(medium.steps == screenEffectsSettingsFor(ScreenEffectsQuality::Medium).reflectionSteps);
        // Off draws none, whatever the scene says.
        CHECK(screenReflectionSettingsFor(look, ScreenEffectsQuality::Off).intensity == 0.0f);
        // Higher qualities follow in more steps, over more pixels.
        const ScreenReflectionSettings low = screenReflectionSettingsFor(look, ScreenEffectsQuality::Low);
        const ScreenReflectionSettings high = screenReflectionSettingsFor(look, ScreenEffectsQuality::High);
        CHECK(low.steps < medium.steps);
        CHECK(medium.steps < high.steps);
        CHECK(low.divisor >= medium.divisor);
        CHECK(high.divisor == 1);
        // A scene as it comes reflects what is on the screen.
        CHECK(PostProcessSettings{}.reflectionIntensity == 1.0f);
    }

    TEST_CASE("a reflection off a floor meets the wall that stands on it where the mirror image of the wall is") {
        const CameraView view = sceneCamera();
        const std::array<Rectangle, 2> scene = {kFloor, kWall};
        const DepthAt depthAt = [&](Vec2 uv) { return depthOfPixel(view, scene, uv); };
        int checked = 0;
        for (const float x : {-0.6f, 0.0f, 0.6f}) {
            for (const float z : {-0.5f, 0.0f, 0.5f}) {
                const Vec3 origin(x, 0.0f, z);
                const Vec3 direction = offTheFloor(origin);
                // Where the mirrored line of sight meets the plane of the wall.
                const Vec3 met = origin + direction * ((-2.0f - z) / direction.z);
                CAPTURE(origin);
                CAPTURE(met);
                REQUIRE(met.y > 0.1f);
                REQUIRE(met.y < 1.9f);
                const ScreenReflectionHit hit =
                    traceScreenReflection(view, kSize, walk(), origin, kWorldUp, direction, depthAt);
                REQUIRE(hit.found);
                // Within a pixel of the mirror image, and of the way there in metres as well.
                CHECK(glm::length((hit.uv - Vec2(inPicture(view, met))) * Vec2(kSize)) < 1.0f);
                CHECK(std::abs(hit.travelled - glm::distance(origin, met)) < 0.05f);
                // In the middle of the picture, and well within its distance: trusted fully.
                CHECK(hit.confidence == 1.0f);
                ++checked;
            }
        }
        CHECK(checked == 9);
    }

    TEST_CASE("a reflection that leaves over the top of the wall meets nothing") {
        const CameraView view = sceneCamera();
        const std::array<Rectangle, 2> scene = {kFloor, kWall};
        const DepthAt depthAt = [&](Vec2 uv) { return depthOfPixel(view, scene, uv); };
        // Nearer the camera the line of sight falls more steeply, and its reflection rises as
        // steeply: at the wall it is higher than two metres.
        const Vec3 origin(0.0f, 0.0f, 1.0f);
        const Vec3 direction = offTheFloor(origin);
        REQUIRE((origin + direction * ((-2.0f - origin.z) / direction.z)).y > 2.0f);
        const ScreenReflectionHit hit =
            traceScreenReflection(view, kSize, walk(), origin, kWorldUp, direction, depthAt);
        CHECK_FALSE(hit.found);
        CHECK(hit.confidence == 0.0f);
    }

    TEST_CASE("a reflection that passes behind something thinner than the thickness goes on, and meets something "
              "thicker") {
        const CameraView view = sceneCamera();
        const std::array<Rectangle, 3> scene = {kFloor, kWall, kStrip};
        const DepthAt depthAt = [&](Vec2 uv) { return depthOfPixel(view, scene, uv); };
        const Vec3 origin(0.0f, 0.0f, 0.5f);
        const Vec3 direction = offTheFloor(origin);
        const Vec3 onTheWall = origin + direction * ((-2.0f - origin.z) / direction.z);
        // In the picture, the way to the wall crosses the strip, a metre and a half in front
        // of where the reflection is then.
        const float stripTop = inPicture(view, Vec3(0.0f, 1.1f, 0.0f)).y;
        const float stripBottom = inPicture(view, Vec3(0.0f, 0.9f, 0.0f)).y;
        REQUIRE(inPicture(view, origin).y > stripBottom);
        REQUIRE(inPicture(view, onTheWall).y < stripTop);

        const ScreenReflectionHit wall =
            traceScreenReflection(view, kSize, walk(), origin, kWorldUp, direction, depthAt);
        REQUIRE(wall.found);
        CHECK(glm::length((wall.uv - Vec2(inPicture(view, onTheWall))) * Vec2(kSize)) < 1.0f);

        // Taken to be two metres thick, the strip is what it meets, where the way enters it.
        ScreenReflectionSettings thick = walk();
        thick.thickness = 2.0f;
        const ScreenReflectionHit strip =
            traceScreenReflection(view, kSize, thick, origin, kWorldUp, direction, depthAt);
        REQUIRE(strip.found);
        CHECK(strip.uv.y < stripBottom + 1.0f / static_cast<float>(kSize.y));
        CHECK(strip.uv.y > stripTop);
        CHECK(strip.travelled < wall.travelled);
    }

    TEST_CASE("a reflection is not met by the surface it leaves, wherever in its pixel it leaves it") {
        const CameraView view = sceneCamera();
        const std::array<Rectangle, 2> scene = {kFloor, kWall};
        const DepthAt depthAt = [&](Vec2 uv) { return depthOfPixel(view, scene, uv); };
        // Along the floor, rising slowly towards the wall: at first it is less than a pixel
        // above the floor, and the steps are finer than a pixel.
        const Vec3 direction = glm::normalize(Vec3(0.0f, 0.2f, -1.0f));
        const Vec3 onTheWall(0.0f, 0.4f, -2.0f);
        int checked = 0;
        for (const float across : {-0.4f, -0.2f, 0.0f, 0.2f, 0.4f}) {
            // A point of the floor that far from the middle of its pixel, up the picture.
            const Vec2 pixel = glm::floor(Vec2(inPicture(view, Vec3(0.0f))) * Vec2(kSize));
            const Vec2 uv = (pixel + Vec2(0.5f, 0.5f + across)) / Vec2(kSize);
            const Vec4 far = view.inverseViewProjection * Vec4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.5f, 1.0f);
            const Vec3 sight = glm::normalize(Vec3(far) / far.w - view.position);
            const Vec3 origin = view.position + sight * (-view.position.y / sight.y);
            CAPTURE(across);
            const ScreenReflectionHit hit =
                traceScreenReflection(view, kSize, walk(512), origin, kWorldUp, direction, depthAt);
            REQUIRE(hit.found);
            CHECK(std::abs(hit.travelled - glm::distance(origin, onTheWall)) < 0.05f);
            ++checked;
        }
        CHECK(checked == 5);
    }

    TEST_CASE("a reflection that goes on past the far plane into the sky meets nothing there") {
        // What is further than six metres is not drawn: the far corners of the floor are
        // sky, and the reflection over the wall goes on into the sky past the far plane.
        const CameraView view = sceneCamera(6.0f);
        const std::array<Rectangle, 2> scene = {kFloor, kWall};
        const DepthAt depthAt = [&](Vec2 uv) { return depthOfPixel(view, scene, uv); };
        const Vec3 origin(0.0f, 0.0f, 1.0f);
        const Vec3 direction = offTheFloor(origin);
        REQUIRE(glm::distance(origin + direction * 20.0f, view.position) > 6.0f);
        CHECK_FALSE(traceScreenReflection(view, kSize, walk(), origin, kWorldUp, direction, depthAt).found);
    }

    TEST_CASE("only the part of a reflection that is in the picture is walked: its steps find a wall in it") {
        const CameraView view = sceneCamera();
        // A wall to the right, two metres from the middle, which the picture shows.
        const Rectangle side{.point = Vec3(2.0f, 0.0f, 0.0f),
                             .normal = Vec3(-1.0f, 0.0f, 0.0f),
                             .lower = Vec3(2.0f, 0.0f, -2.0f),
                             .upper = Vec3(2.0f, 2.0f, 3.0f)};
        const std::array<Rectangle, 3> scene = {kFloor, kWall, side};
        const DepthAt depthAt = [&](Vec2 uv) { return depthOfPixel(view, scene, uv); };
        // Across the view, rising a little: two hundred metres along, it is ten times as far
        // to the right as the picture is wide, and the wall is in its first metres.
        const Vec3 origin(0.0f, 0.0f, 0.5f);
        const Vec3 direction = glm::normalize(Vec3(1.0f, 0.15f, -0.05f));
        ScreenReflectionSettings far = walk(16);
        far.maxDistance = 200.0f;
        REQUIRE(inPicture(view, origin + direction * 200.0f).x > 10.0f);
        const Vec3 onTheSide = origin + direction * (2.0f / direction.x);
        REQUIRE(inPicture(view, onTheSide).x < 0.95f);
        const ScreenReflectionHit hit = traceScreenReflection(view, kSize, far, origin, kWorldUp, direction, depthAt);
        REQUIRE(hit.found);
        CHECK(std::abs(hit.travelled - glm::distance(origin, onTheSide)) < 0.05f);
    }

    TEST_CASE("a reflection towards the camera ends at the near plane, and without steps or distance nothing is "
              "followed") {
        const CameraView view = sceneCamera();
        const std::array<Rectangle, 2> scene = {kFloor, kWall};
        const DepthAt depthAt = [&](Vec2 uv) { return depthOfPixel(view, scene, uv); };
        const Vec3 onTheWall(0.5f, 1.0f, -2.0f);
        // Twenty metres along this would be far behind the camera.
        const Vec3 back = glm::normalize(Vec3(0.0f, 0.1f, 1.0f));
        const ScreenReflectionHit towards =
            traceScreenReflection(view, kSize, walk(), onTheWall, Vec3(0.0f, 0.0f, 1.0f), back, depthAt);
        CHECK_FALSE(towards.found);
        CHECK(std::isfinite(towards.uv.x));
        CHECK(std::isfinite(towards.travelled));

        const Vec3 origin(0.0f, 0.0f, 0.5f);
        const Vec3 direction = offTheFloor(origin);
        CHECK_FALSE(traceScreenReflection(view, kSize, walk(0), origin, kWorldUp, direction, depthAt).found);
        ScreenReflectionSettings nowhere = walk();
        nowhere.maxDistance = 0.0f;
        CHECK_FALSE(traceScreenReflection(view, kSize, nowhere, origin, kWorldUp, direction, depthAt).found);
        nowhere.maxDistance = kNotANumber;
        CHECK_FALSE(traceScreenReflection(view, kSize, nowhere, origin, kWorldUp, direction, depthAt).found);
        // Straight away from the camera a reflection stays in one pixel: nothing to follow.
        const Vec3 away = glm::normalize(origin - view.position);
        CHECK_FALSE(traceScreenReflection(view, kSize, walk(), origin, kWorldUp, away, depthAt).found);
        CHECK_FALSE(traceScreenReflection(view, UVec2(0), walk(), origin, kWorldUp, direction, depthAt).found);
    }
}
