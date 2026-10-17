#include "cordite/renderer/LevelOfDetail.h"

#include <algorithm>
#include <cmath>

namespace cordite {

LodView lodViewOf(const CameraView& view, uint32_t targetHeight, float errorPixels) {
    // The projection maps a metre (with perspective: one metre in front of the camera)
    // to projection[1][1] halves of the height of the picture.
    return {.cameraPosition = view.position,
            .orthographic = view.orthographic,
            .pixelsPerMetre = std::abs(view.projection[1][1]) * static_cast<float>(targetHeight) * 0.5f,
            .nearPlane = view.nearPlane,
            .errorPixels = errorPixels};
}

float scaleOf(const Mat4& localToWorld) {
    return std::max(
        {glm::length(Vec3(localToWorld[0])), glm::length(Vec3(localToWorld[1])), glm::length(Vec3(localToWorld[2]))});
}

uint32_t selectLod(std::span<const float> errors, const Aabb& worldBounds, float worldScale, const LodView& view) {
    if (errors.empty() || !(view.errorPixels > 0.0f) || !std::isfinite(view.errorPixels) || !(worldScale > 0.0f) ||
        !std::isfinite(worldScale) || worldBounds.isEmpty()) {
        return 0;
    }
    float pixelsPerMetre = view.pixelsPerMetre;
    if (!view.orthographic) {
        const Vec3 center = worldBounds.center();
        const float radius = glm::length(worldBounds.extents());
        const float distance = glm::distance(center, view.cameraPosition) - radius;
        if (!(distance > view.nearPlane)) {
            return 0;
        }
        pixelsPerMetre /= distance;
    }
    if (!(pixelsPerMetre > 0.0f) || !std::isfinite(pixelsPerMetre)) {
        return 0;
    }
    // The largest error that looks small enough, in the mesh's own units.
    const float allowed = view.errorPixels / (pixelsPerMetre * worldScale);
    uint32_t level = 0;
    for (size_t index = 0; index < errors.size(); ++index) {
        if (!(errors[index] <= allowed)) {
            break;
        }
        level = static_cast<uint32_t>(index + 1);
    }
    return level;
}

} // namespace cordite
