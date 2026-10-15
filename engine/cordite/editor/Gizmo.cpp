#include "cordite/editor/Gizmo.h"

#include "cordite/core/StringUtils.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cordite {
namespace {

constexpr Vec4 kXColor(0.95f, 0.30f, 0.30f, 1.0f);
constexpr Vec4 kYColor(0.40f, 0.85f, 0.35f, 1.0f);
constexpr Vec4 kZColor(0.35f, 0.55f, 1.00f, 1.0f);
/// The color of what is selected in the viewport: the handle that is held or pointed at.
constexpr Vec4 kHighlightColor(1.0f, 0.78f, 0.20f, 1.0f);

/// Where the square of a plane handle lies along its two axes, in lengths of an axis.
constexpr float kPlaneNear = 0.25f;
constexpr float kPlaneFar = 0.45f;
/// Half the size of the box at the end of a scale handle, and of the centre one.
constexpr float kScaleBox = 0.06f;
/// Segments of a ring of the rotation gizmo, drawn and picked alike.
constexpr uint32_t kRingSegments = 64;
/// Below this a direction counts as none: an axis seen end on, a ray along a plane.
constexpr float kParallel = 1.0e-4f;
/// The least area, in square pixels, of a plane handle on the screen that can be taken
/// hold of: one seen edge on is a line, which points along an axis would fall into.
constexpr float kMinimumHandleArea = 16.0f;

constexpr std::array<GizmoHandle, 3> kAxes = {GizmoHandle::X, GizmoHandle::Y, GizmoHandle::Z};
constexpr std::array<GizmoHandle, 3> kPlanes = {GizmoHandle::XY, GizmoHandle::YZ, GizmoHandle::XZ};

/// How many pixels a metre is at `point`, on a viewport `height` pixels high.
float pixelsPerMetre(const CameraView& view, const Vec3& point, float height) {
    // 1 / tan(half the field of view) with perspective, 2 / the height seen without.
    const float scale = view.projection[1][1];
    if (view.orthographic) {
        return height * scale * 0.5f;
    }
    const float depth = std::max(glm::dot(point - view.position, view.forward), view.nearPlane);
    return height * scale * 0.5f / depth;
}

/// The two axes of a plane handle.
std::pair<Vec3, Vec3> planeAxes(const GizmoFrame& frame, GizmoHandle handle) {
    switch (handle) {
        case GizmoHandle::XY:
            return {frame.x, frame.y};
        case GizmoHandle::YZ:
            return {frame.y, frame.z};
        case GizmoHandle::XZ:
            return {frame.x, frame.z};
        default:
            break;
    }
    return {frame.x, frame.y};
}

/// The normal of the plane of a plane handle: the third axis.
Vec3 planeNormal(const GizmoFrame& frame, GizmoHandle handle) {
    switch (handle) {
        case GizmoHandle::XY:
            return frame.z;
        case GizmoHandle::YZ:
            return frame.x;
        case GizmoHandle::XZ:
            return frame.y;
        default:
            break;
    }
    return frame.z;
}

float distanceToSegment(Vec2 point, Vec2 from, Vec2 to) {
    const Vec2 along = to - from;
    const float lengthSquared = glm::dot(along, along);
    const float t = lengthSquared > 0.0f ? std::clamp(glm::dot(point - from, along) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    return glm::length(point - (from + along * t));
}

/// Whether a point is inside a convex quadrilateral on the screen, whichever way round. A
/// quadrilateral that has no area - a plane seen edge on - has no inside.
bool insideQuad(Vec2 point, const std::array<Vec2, 4>& corners) {
    float twiceArea = 0.0f;
    for (size_t index = 0; index < corners.size(); ++index) {
        const Vec2& from = corners[index];
        const Vec2& to = corners[(index + 1) % corners.size()];
        twiceArea += from.x * to.y - to.x * from.y;
    }
    if (std::abs(twiceArea) < kMinimumHandleArea * 2.0f) {
        return false;
    }
    float sign = 0.0f;
    for (size_t index = 0; index < corners.size(); ++index) {
        const Vec2 edge = corners[(index + 1) % corners.size()] - corners[index];
        const Vec2 toPoint = point - corners[index];
        const float cross = edge.x * toPoint.y - edge.y * toPoint.x;
        if (cross != 0.0f) {
            if (sign == 0.0f) {
                sign = cross;
            } else if ((cross > 0.0f) != (sign > 0.0f)) {
                return false;
            }
        }
    }
    return true;
}

/// The distance on the screen from a pixel to a segment of the world; none when the
/// segment is behind the camera.
std::optional<float> screenDistance(const CameraView& view, Vec2 viewportSize, Vec2 pixel, const Vec3& from,
                                    const Vec3& to) {
    const std::optional<Vec2> start = worldToScreen(view, from, viewportSize);
    const std::optional<Vec2> end = worldToScreen(view, to, viewportSize);
    if (!start || !end) {
        return std::nullopt;
    }
    return distanceToSegment(pixel, *start, *end);
}

/// A unit vector at right angles to `normal`, and a second one at right angles to both.
std::pair<Vec3, Vec3> ringBasis(const Vec3& normal) {
    const Vec3 helper = std::abs(normal.y) < 0.9f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 first = glm::normalize(glm::cross(normal, helper));
    return {first, glm::cross(normal, first)};
}

/// The nearest pixel distance from a pointer to a ring of the rotation gizmo.
std::optional<float> ringDistance(const CameraView& view, Vec2 viewportSize, Vec2 pixel, const Vec3& center,
                                  const Vec3& normal, float radius) {
    const auto [u, v] = ringBasis(normal);
    std::optional<float> nearest;
    Vec3 previous = center + u * radius;
    for (uint32_t segment = 1; segment <= kRingSegments; ++segment) {
        const float angle = kTwoPi * static_cast<float>(segment) / static_cast<float>(kRingSegments);
        const Vec3 point = center + (u * std::cos(angle) + v * std::sin(angle)) * radius;
        if (const std::optional<float> distance = screenDistance(view, viewportSize, pixel, previous, point)) {
            nearest = nearest ? std::min(*nearest, *distance) : *distance;
        }
        previous = point;
    }
    return nearest;
}

/// The point of the line through `origin` along `axis` (of unit length) that is nearest
/// to the ray; none while the two run alongside each other.
std::optional<Vec3> nearestOnAxis(const Vec3& origin, const Vec3& axis, const Ray& ray) {
    const float along = glm::dot(axis, ray.direction);
    const float denominator = 1.0f - along * along;
    if (denominator < kParallel) {
        return std::nullopt;
    }
    const Vec3 between = origin - ray.origin;
    const float t = (along * glm::dot(ray.direction, between) - glm::dot(axis, between)) / denominator;
    return origin + axis * t;
}

/// Where along an axis of a gizmo the pointer at `pixel` is: the pointer is first slid onto
/// the axis as it is seen, so that only how far it moved along the axis counts - and the
/// handle stays under it, with perspective too. None for an axis seen end on.
std::optional<Vec3> pointOnAxis(const GizmoFrame& frame, GizmoHandle handle, const CameraView& view, Vec2 viewportSize,
                                Vec2 pixel) {
    const Vec3 axis = frame.axis(handle);
    const std::optional<Vec2> from = worldToScreen(view, frame.origin, viewportSize);
    const std::optional<Vec2> to = worldToScreen(view, frame.origin + axis * frame.size, viewportSize);
    if (!from || !to || glm::length(*to - *from) < 1.0f) {
        return std::nullopt;
    }
    const Vec2 along = glm::normalize(*to - *from);
    const Vec2 onAxis = *from + along * glm::dot(pixel - *from, along);
    return nearestOnAxis(frame.origin, axis, screenRay(view, onAxis, viewportSize));
}

/// Where the ray meets the plane through `origin` at right angles to `normal`; none when it
/// runs along the plane or away from it.
std::optional<Vec3> onPlane(const Vec3& origin, const Vec3& normal, const Ray& ray) {
    if (std::abs(glm::dot(normal, ray.direction)) < kParallel) {
        return std::nullopt;
    }
    const std::optional<float> distance = intersect(ray, Plane::fromPointNormal(origin, normal));
    if (!distance) {
        return std::nullopt;
    }
    return ray.at(*distance);
}

/// Which component of a vector an axis handle is: 0 for X, 1 for Y, 2 for Z.
int axisIndex(GizmoHandle handle) {
    switch (handle) {
        case GizmoHandle::Y:
            return 1;
        case GizmoHandle::Z:
            return 2;
        default:
            break;
    }
    return 0;
}

Vec4 colorOf(GizmoHandle handle, GizmoHandle highlighted) {
    if (handle == highlighted) {
        return kHighlightColor;
    }
    switch (handle) {
        case GizmoHandle::X:
            return kXColor;
        case GizmoHandle::Y:
            return kYColor;
        case GizmoHandle::Z:
            return kZColor;
        default:
            break;
    }
    return Vec4(0.85f, 0.85f, 0.85f, 1.0f);
}

/// The corners of the square of a plane handle.
std::array<Vec3, 4> planeCorners(const GizmoFrame& frame, GizmoHandle handle) {
    const auto [first, second] = planeAxes(frame, handle);
    const float near = frame.size * kPlaneNear;
    const float far = frame.size * kPlaneFar;
    return {frame.origin + first * near + second * near, frame.origin + first * far + second * near,
            frame.origin + first * far + second * far, frame.origin + first * near + second * far};
}

} // namespace

std::string_view nameOf(GizmoMode mode) {
    switch (mode) {
        case GizmoMode::Translate:
            return "translate";
        case GizmoMode::Rotate:
            return "rotate";
        case GizmoMode::Scale:
            return "scale";
    }
    return "unknown";
}

std::optional<GizmoMode> gizmoModeFromName(std::string_view name) {
    for (const GizmoMode mode : kGizmoModes) {
        if (equalsIgnoreCase(name, nameOf(mode))) {
            return mode;
        }
    }
    return std::nullopt;
}

std::string_view nameOf(GizmoSpace space) {
    return space == GizmoSpace::Local ? "local" : "world";
}

std::optional<GizmoSpace> gizmoSpaceFromName(std::string_view name) {
    for (const GizmoSpace space : {GizmoSpace::World, GizmoSpace::Local}) {
        if (equalsIgnoreCase(name, nameOf(space))) {
            return space;
        }
    }
    return std::nullopt;
}

Vec3 GizmoFrame::axis(GizmoHandle handle) const {
    switch (handle) {
        case GizmoHandle::Y:
            return y;
        case GizmoHandle::Z:
            return z;
        default:
            break;
    }
    return x;
}

GizmoFrame gizmoFrame(const Mat4& worldMatrix, GizmoSpace space, GizmoMode mode, const CameraView& view,
                      Vec2 viewportSize, float pixelScale) {
    GizmoFrame frame;
    frame.origin = Vec3(worldMatrix[3]);
    // Scale is kept along the entity's own axes, and is changed along them.
    if (space == GizmoSpace::Local || mode == GizmoMode::Scale) {
        const Vec3 columnX(worldMatrix[0]);
        const Vec3 columnY(worldMatrix[1]);
        const Vec3 columnZ(worldMatrix[2]);
        const float lengthX = glm::length(columnX);
        if (lengthX > kParallel) {
            const Vec3 x = columnX / lengthX;
            const Vec3 yOff = columnY - x * glm::dot(columnY, x);
            const float lengthY = glm::length(yOff);
            if (lengthY > kParallel) {
                const Vec3 y = yOff / lengthY;
                const Vec3 zOff = columnZ - x * glm::dot(columnZ, x) - y * glm::dot(columnZ, y);
                const float lengthZ = glm::length(zOff);
                // An entity scaled to nothing along an axis keeps the axes of the world.
                if (lengthZ > kParallel) {
                    frame.x = x;
                    frame.y = y;
                    frame.z = zOff / lengthZ;
                }
            }
        }
    }
    const float height = std::max(viewportSize.y, 1.0f);
    frame.size = kGizmoScreenSize * std::max(pixelScale, kParallel) / pixelsPerMetre(view, frame.origin, height);
    return frame;
}

GizmoHandle pickHandle(GizmoMode mode, const GizmoFrame& frame, const CameraView& view, Vec2 viewportSize, Vec2 pixel,
                       float pixelScale) {
    GizmoHandle nearest = GizmoHandle::None;
    float nearestDistance = kGizmoPickPixels * std::max(pixelScale, kParallel);
    const auto consider = [&](GizmoHandle handle, std::optional<float> distance) {
        if (distance && *distance <= nearestDistance) {
            nearest = handle;
            nearestDistance = *distance;
        }
    };

    if (mode == GizmoMode::Rotate) {
        for (const GizmoHandle axis : kAxes) {
            consider(axis, ringDistance(view, viewportSize, pixel, frame.origin, frame.axis(axis), frame.size));
        }
        return nearest;
    }
    for (const GizmoHandle axis : kAxes) {
        consider(axis,
                 screenDistance(view, viewportSize, pixel, frame.origin, frame.origin + frame.axis(axis) * frame.size));
    }
    if (mode == GizmoMode::Translate) {
        for (const GizmoHandle plane : kPlanes) {
            std::array<Vec2, 4> corners{};
            bool visible = true;
            const std::array<Vec3, 4> world = planeCorners(frame, plane);
            for (size_t index = 0; index < world.size(); ++index) {
                const std::optional<Vec2> corner = worldToScreen(view, world[index], viewportSize);
                visible = visible && corner.has_value();
                corners[index] = corner.value_or(Vec2(0.0f));
            }
            if (visible && insideQuad(pixel, corners)) {
                consider(plane, 0.0f);
            }
        }
    } else if (const std::optional<Vec2> center = worldToScreen(view, frame.origin, viewportSize)) {
        // The box at the centre scales along all three axes.
        const float box = kScaleBox * frame.size * pixelsPerMetre(view, frame.origin, std::max(viewportSize.y, 1.0f));
        if (std::abs(pixel.x - center->x) <= box && std::abs(pixel.y - center->y) <= box) {
            consider(GizmoHandle::All, 0.0f);
        }
    }
    return nearest;
}

GizmoDrag beginDrag(GizmoMode mode, GizmoHandle handle, const GizmoFrame& frame, const CameraView& view,
                    Vec2 viewportSize, Vec2 pixel) {
    GizmoDrag drag{.mode = mode, .handle = handle, .frame = frame, .grabbed = frame.origin, .grabbedPixel = pixel};
    const Ray ray = screenRay(view, pixel, viewportSize);
    if (handle == GizmoHandle::X || handle == GizmoHandle::Y || handle == GizmoHandle::Z) {
        drag.grabbed = pointOnAxis(frame, handle, view, viewportSize, pixel).value_or(frame.origin);
    } else if (handle == GizmoHandle::XY || handle == GizmoHandle::YZ || handle == GizmoHandle::XZ) {
        drag.grabbed = onPlane(frame.origin, planeNormal(frame, handle), ray).value_or(frame.origin);
    }
    return drag;
}

Vec3 dragTranslation(const GizmoDrag& drag, const CameraView& view, Vec2 viewportSize, Vec2 pixel) {
    const Ray ray = screenRay(view, pixel, viewportSize);
    const GizmoFrame& frame = drag.frame;
    switch (drag.handle) {
        case GizmoHandle::X:
        case GizmoHandle::Y:
        case GizmoHandle::Z: {
            const Vec3 axis = frame.axis(drag.handle);
            const std::optional<Vec3> point = pointOnAxis(frame, drag.handle, view, viewportSize, pixel);
            return point ? axis * glm::dot(*point - drag.grabbed, axis) : Vec3(0.0f);
        }
        case GizmoHandle::XY:
        case GizmoHandle::YZ:
        case GizmoHandle::XZ: {
            const std::optional<Vec3> point = onPlane(frame.origin, planeNormal(frame, drag.handle), ray);
            return point ? *point - drag.grabbed : Vec3(0.0f);
        }
        default:
            break;
    }
    return Vec3(0.0f);
}

Quat dragRotation(const GizmoDrag& drag, const CameraView& view, Vec2 viewportSize, Vec2 pixel) {
    const std::optional<Vec2> center = worldToScreen(view, drag.frame.origin, viewportSize);
    if (!center || drag.handle == GizmoHandle::None || drag.handle == GizmoHandle::All) {
        return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const Vec2 from = drag.grabbedPixel - *center;
    const Vec2 to = pixel - *center;
    if (glm::dot(from, from) < kParallel || glm::dot(to, to) < kParallel) {
        return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    // The screen's y points down, so an angle that grows turns clockwise as seen; seen from
    // the tip of an axis that points at the viewer, a positive rotation turns the other way.
    const float swept = std::atan2(from.x * to.y - from.y * to.x, glm::dot(from, to));
    const Vec3 axis = drag.frame.axis(drag.handle);
    const Vec3 towardViewer = view.orthographic ? -view.forward : view.position - drag.frame.origin;
    const float angle = glm::dot(axis, towardViewer) >= 0.0f ? -swept : swept;
    return glm::angleAxis(angle, axis);
}

Vec3 dragScale(const GizmoDrag& drag, const CameraView& view, Vec2 viewportSize, Vec2 pixel) {
    const std::optional<Vec2> center = worldToScreen(view, drag.frame.origin, viewportSize);
    if (!center || drag.handle == GizmoHandle::None) {
        return Vec3(1.0f);
    }
    const Vec2 moved = pixel - drag.grabbedPixel;
    if (drag.handle == GizmoHandle::All) {
        // Up and to the right is larger, as the box at the end of an axis.
        const float unit = drag.frame.size * pixelsPerMetre(view, drag.frame.origin, std::max(viewportSize.y, 1.0f));
        const float along = (moved.x - moved.y) * 0.70710678f;
        return Vec3(std::max(1.0f + along / std::max(unit, 1.0f), kGizmoMinimumScale));
    }
    const std::optional<Vec2> tip =
        worldToScreen(view, drag.frame.origin + drag.frame.axis(drag.handle) * drag.frame.size, viewportSize);
    if (!tip) {
        return Vec3(1.0f);
    }
    const Vec2 onScreen = *tip - *center;
    const float unit = glm::length(onScreen);
    // An axis seen end on has no direction on the screen to drag along.
    if (unit < 1.0f) {
        return Vec3(1.0f);
    }
    const float factor = std::max(1.0f + glm::dot(moved, onScreen / unit) / unit, kGizmoMinimumScale);
    Vec3 scale(1.0f);
    scale[axisIndex(drag.handle)] = factor;
    return scale;
}

void drawGizmo(DebugDraw& lines, GizmoMode mode, const GizmoFrame& frame, GizmoHandle highlighted) {
    switch (mode) {
        case GizmoMode::Translate:
            for (const GizmoHandle axis : kAxes) {
                lines.arrow(frame.origin, frame.origin + frame.axis(axis) * frame.size, colorOf(axis, highlighted));
            }
            for (const GizmoHandle plane : kPlanes) {
                const std::array<Vec3, 4> corners = planeCorners(frame, plane);
                const Vec4 color = colorOf(plane, highlighted);
                for (size_t index = 0; index < corners.size(); ++index) {
                    lines.line(corners[index], corners[(index + 1) % corners.size()], color);
                }
            }
            break;
        case GizmoMode::Rotate:
            for (const GizmoHandle axis : kAxes) {
                lines.circle(frame.origin, frame.axis(axis), frame.size, colorOf(axis, highlighted), kRingSegments);
            }
            break;
        case GizmoMode::Scale: {
            const Mat4 orientation(Vec4(frame.x, 0.0f), Vec4(frame.y, 0.0f), Vec4(frame.z, 0.0f), Vec4(0, 0, 0, 1));
            const Vec3 half(kScaleBox * frame.size);
            for (const GizmoHandle axis : kAxes) {
                const Vec3 tip = frame.origin + frame.axis(axis) * frame.size;
                const Vec4 color = colorOf(axis, highlighted);
                lines.line(frame.origin, tip, color);
                lines.box(Aabb{.min = -half, .max = half}, glm::translate(Mat4(1.0f), tip) * orientation, color);
            }
            lines.box(Aabb{.min = -half, .max = half}, glm::translate(Mat4(1.0f), frame.origin) * orientation,
                      colorOf(GizmoHandle::All, highlighted));
            break;
        }
    }
}

} // namespace cordite
