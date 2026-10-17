#include "cordite/renderer/MotionVectors.h"

#include "cordite/math/Color.h"

#include <algorithm>
#include <cmath>

namespace cordite {

bool MotionHistory::continuesWith(const CameraView& next, const Uuid& seenThrough) const {
    return valid && seenThrough == camera && !isCameraCut(view, next);
}

namespace {

/// The entry of `id` in `entries`, which are in the order of their ids, or null.
template <typename Value>
const Value* findById(const std::vector<std::pair<Uuid, Value>>& entries, const Uuid& id) {
    const auto found = std::ranges::lower_bound(entries, id, {}, &std::pair<Uuid, Value>::first);
    return found != entries.end() && found->first == id ? &found->second : nullptr;
}

} // namespace

const Mat4* MotionHistory::placementOf(const Uuid& id) const {
    return findById(placements, id);
}

const std::vector<Mat4>* MotionHistory::poseOf(const Uuid& id) const {
    return findById(poses, id);
}

void MotionHistory::remember(const CameraView& drawn, const Uuid& seenThrough) {
    view = drawn;
    camera = seenThrough;
    valid = true;
    std::ranges::sort(placements, {}, &std::pair<Uuid, Mat4>::first);
    std::ranges::sort(poses, {}, &std::pair<Uuid, std::vector<Mat4>>::first);
}

void MotionHistory::forget() {
    valid = false;
    camera = Uuid();
    view = CameraView();
    placements.clear();
    poses.clear();
}

bool isCameraCut(const CameraView& before, const CameraView& after) {
    if (before.orthographic != after.orthographic) {
        return true;
    }
    const float distance = glm::length(after.position - before.position);
    // How far the camera turned, about whatever axis - rolling too, which leaves the way
    // it looks as it was: the angle of the rotation from the one orientation to the
    // other, by the trace of its matrix.
    const Mat3 turn = Mat3(after.view) * glm::transpose(Mat3(before.view));
    const float cosine = (turn[0][0] + turn[1][1] + turn[2][2] - 1.0f) * 0.5f;
    // How much larger things are in the picture: the scale of the projection, which a
    // field of view - or how much a camera without perspective sees - sets.
    const float zoom = after.projection[1][1] / before.projection[1][1];
    // A camera that is not made of numbers is no camera to go on from.
    if (!std::isfinite(distance) || !std::isfinite(cosine) || !std::isfinite(zoom) || zoom <= 0.0f) {
        return true;
    }
    return distance > kCameraCutDistance || cosine < std::cos(kCameraCutAngle) || zoom > kCameraCutZoom ||
           zoom < 1.0f / kCameraCutZoom;
}

Vec3 motionDebugColor(Vec2 motion, UVec2 targetSize) {
    // In pixels, and with y up, as angles are counted.
    const Vec2 pixels(motion.x * static_cast<float>(targetSize.x), -motion.y * static_cast<float>(targetSize.y));
    const float length = glm::length(pixels);
    // Also what is no number: nothing that can be seen moved.
    if (!(length > 0.0f) || !std::isfinite(length)) {
        return Vec3(0.0f);
    }
    const float hue = std::atan2(pixels.y, pixels.x) / (2.0f * kPi);
    return hsvToRgb(Vec3(hue, 1.0f, std::min(length / kMotionDebugFullLength, 1.0f)));
}

} // namespace cordite
