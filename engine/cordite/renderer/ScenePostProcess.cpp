#include "cordite/renderer/ScenePostProcess.h"

#include "cordite/world/Components.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tuple>

namespace cordite {
namespace {

/// A component that applies to the view, and how much.
struct Applied {
    int32_t priority = 0;
    Uuid id;
    const PostProcessComponent* component = nullptr;
    float amount = 0.0f;
};

/// More components than this apply to one view only in a scene that has gone wrong;
/// the ones of the highest priority are the ones that count, and are kept.
constexpr size_t kMaxApplied = 64;

float finiteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

} // namespace

float postProcessBoxWeight(const Mat4& placement, float blendDistance, const Vec3& position) {
    // The box is a cube of one metre in the entity's own space. How far outside it the
    // position is, is measured there and brought back to metres of the world axis by
    // axis: a box that is scaled is as far away as its sides are.
    const Vec3 axes(glm::length(Vec3(placement[0])), glm::length(Vec3(placement[1])), glm::length(Vec3(placement[2])));
    if (!(axes.x > 0.0f) || !(axes.y > 0.0f) || !(axes.z > 0.0f) || !isFinite(axes) || !isFinite(position)) {
        // A box without a size holds nothing.
        return 0.0f;
    }
    const Vec3 local = Vec3(glm::inverse(placement) * Vec4(position, 1.0f));
    const Vec3 outside = glm::max(glm::abs(local) - Vec3(0.5f), Vec3(0.0f)) * axes;
    const float distance = glm::length(outside);
    if (!std::isfinite(distance)) {
        return 0.0f;
    }
    const float fade = std::max(finiteOr(blendDistance, 0.0f), 0.0f);
    if (distance <= 0.0f) {
        return 1.0f;
    }
    return fade > 0.0f ? std::clamp(1.0f - distance / fade, 0.0f, 1.0f) : 0.0f;
}

PostProcessSettings resolvePostProcess(const World& world, const PostProcessView& view) {
    // Found in whatever order the components are stored in; applied in the order of
    // their priorities and ids. Nothing here allocates memory: this runs for every view
    // of every frame.
    std::array<Applied, kMaxApplied> applied{};
    size_t count = 0;
    // What the camera of the view says for itself, if anything.
    Applied own;

    const auto before = [](const Applied& a, const Applied& b) {
        return std::tie(a.priority, a.id) < std::tie(b.priority, b.id);
    };
    const auto components = world.registry().view<const PostProcessComponent, const WorldTransform>();
    for (const auto [handle, component, placement] : components.each()) {
        const Entity entity{handle};
        if (!world.isEnabledInHierarchy(entity)) {
            continue;
        }
        const float weight = std::clamp(finiteOr(component.weight, 0.0f), 0.0f, 1.0f);
        Applied candidate{.priority = component.priority, .id = world.id(entity), .component = &component};

        // A camera's own: for the views through that camera, and for no other.
        if (world.tryGet<CameraComponent>(entity) != nullptr) {
            if (!view.camera.isNil() && candidate.id == view.camera) {
                candidate.amount = weight;
                own = candidate;
            }
            continue;
        }

        candidate.amount =
            weight *
            (component.global ? 1.0f : postProcessBoxWeight(placement.matrix, component.blendDistance, view.position));
        if (candidate.amount <= 0.0f) {
            continue;
        }
        if (count < applied.size()) {
            applied[count++] = candidate;
        } else {
            // Full: the new one takes the place of the one that would be applied
            // first, if it comes after it.
            const auto first = std::ranges::min_element(applied, before);
            if (before(*first, candidate)) {
                *first = candidate;
            }
        }
    }
    std::sort(applied.begin(), applied.begin() + static_cast<std::ptrdiff_t>(count), before);

    PostProcessSettings look;
    for (size_t index = 0; index < count; ++index) {
        look = blend(look, sanitized(*applied[index].component), applied[index].amount);
    }
    if (own.component != nullptr && own.amount > 0.0f) {
        look = blend(look, sanitized(*own.component), own.amount);
    }
    return look;
}

} // namespace cordite
