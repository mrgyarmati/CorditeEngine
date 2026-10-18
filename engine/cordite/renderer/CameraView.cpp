#include "cordite/renderer/CameraView.h"

#include "cordite/animation/CameraShake.h"
#include "cordite/core/Assert.h"
#include "cordite/math/Projection.h"
#include "cordite/math/Transform.h"

#include <algorithm>
#include <cmath>

namespace cordite {
namespace {

constexpr float kMinimumNearPlane = 1.0e-4f;
constexpr float kMinimumAspectRatio = 1.0e-3f;

/// The rotation and translation of a matrix, with any scale removed.
Mat4 withoutScale(const Mat4& matrix) {
    Mat4 result(1.0f);
    result[0] = Vec4(normalizeOr(Vec3(matrix[0]), kWorldRight), 0.0f);
    result[1] = Vec4(normalizeOr(Vec3(matrix[1]), kWorldUp), 0.0f);
    result[2] = Vec4(normalizeOr(Vec3(matrix[2]), -kWorldForward), 0.0f);
    result[3] = Vec4(Vec3(matrix[3]), 1.0f);
    return result;
}

} // namespace

CameraView makeCameraView(const Mat4& cameraToWorld, const CameraComponent& camera, float aspectRatio) {
    const Mat4 placement = withoutScale(cameraToWorld);
    const float aspect = std::isfinite(aspectRatio) ? std::max(aspectRatio, kMinimumAspectRatio) : 1.0f;

    CameraView view;
    view.nearPlane = std::max(camera.nearPlane, kMinimumNearPlane);
    // The far plane must lie beyond the near plane, or the projection divides by zero.
    view.farPlane = std::max(camera.farPlane, view.nearPlane * 1.001f);
    view.position = Vec3(placement[3]);
    view.forward = -Vec3(placement[2]);
    view.view = glm::inverse(placement);

    view.orthographic = camera.projection == CameraProjection::Orthographic;
    if (view.orthographic) {
        const float halfHeight = 0.5f * std::max(camera.orthographicHeight, kMinimumNearPlane);
        const float halfWidth = halfHeight * aspect;
        view.projection =
            orthographicReverseZ(-halfWidth, halfWidth, -halfHeight, halfHeight, view.nearPlane, view.farPlane);
    } else {
        const float fieldOfView = toRadians(std::clamp(camera.verticalFovDegrees, 1.0f, 179.0f));
        view.projection = perspectiveReverseZ(fieldOfView, aspect, view.nearPlane, view.farPlane);
    }

    view.viewProjection = view.projection * view.view;
    view.inverseViewProjection = glm::inverse(view.viewProjection);
    return view;
}

Mat4 cameraPlacement(const World& world, Entity camera) {
    const Mat4& placement = world.worldMatrix(camera);
    const auto* shake = world.tryGet<CameraShakeComponent>(camera);
    if (shake == nullptr) {
        return placement;
    }
    const Transform offset = cameraShakeOffset(*shake);
    if (offset == Transform{}) {
        return placement;
    }
    // In the camera's own space, without its scale: what makeCameraView leaves out would
    // otherwise scale the move, and skew the turn.
    return withoutScale(placement) * offset.toMatrix();
}

CameraView makeCameraView(const World& world, Entity camera, float aspectRatio) {
    const auto* lens = world.tryGet<CameraComponent>(camera);
    CORDITE_ASSERT(lens != nullptr, "the view of an entity is that of its camera, and it has none");
    return makeCameraView(cameraPlacement(world, camera), lens != nullptr ? *lens : CameraComponent{}, aspectRatio);
}

Entity findPrimaryCamera(const World& world) {
    Entity found;
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (!found.isNull()) {
            return;
        }
        const auto* camera = world.tryGet<CameraComponent>(entity);
        if (camera != nullptr && camera->primary && world.isEnabledInHierarchy(entity)) {
            found = entity;
        }
    });
    return found;
}

Ray screenRay(const CameraView& view, Vec2 pixel, Vec2 targetSize) {
    // Pixel (0, 0) is the top-left corner; clip space has +Y up.
    const Vec2 normalized((pixel.x / targetSize.x) * 2.0f - 1.0f, 1.0f - (pixel.y / targetSize.y) * 2.0f);

    // Reversed depth: 1 is the near plane. A point halfway to the far plane gives the
    // direction without the precision loss of unprojecting the far plane itself.
    const Vec4 nearPoint = view.inverseViewProjection * Vec4(normalized, 1.0f, 1.0f);
    const Vec4 farPoint = view.inverseViewProjection * Vec4(normalized, 0.5f, 1.0f);
    const Vec3 origin = Vec3(nearPoint) / nearPoint.w;
    const Vec3 target = Vec3(farPoint) / farPoint.w;
    return Ray{origin, normalizeOr(target - origin, view.forward)};
}

std::optional<Vec2> worldToScreen(const CameraView& view, const Vec3& point, Vec2 targetSize) {
    const Vec4 clip = view.viewProjection * Vec4(point, 1.0f);
    if (clip.w <= kEpsilon) {
        return std::nullopt;
    }
    const Vec2 normalized = Vec2(clip) / clip.w;
    return Vec2((normalized.x * 0.5f + 0.5f) * targetSize.x, (0.5f - normalized.y * 0.5f) * targetSize.y);
}

} // namespace cordite
