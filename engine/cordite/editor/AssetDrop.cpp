#include "cordite/editor/AssetDrop.h"

#include "cordite/core/Reflection.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/editor/PropertyEditor.h"
#include "cordite/math/MathJson.h"
#include "cordite/renderer/RenderComponents.h"

#include <fmt/format.h>
#include <imgui.h>

#include <cstring>

namespace cordite {

ViewportDrop viewportDropOf(AssetType type) {
    if (canBePlaced(type)) {
        return ViewportDrop::Placed;
    }
    return type == AssetType::Material ? ViewportDrop::OntoMesh : ViewportDrop::None;
}

Vec3 dropPoint(const Ray& ray, std::optional<float> surface, const DropReach& reach) {
    if (surface) {
        return ray.origin + ray.direction * *surface;
    }
    // A line of sight along the grid never meets it, and one that nearly does meets it
    // further away than it is drawn.
    if (ray.direction.y != 0.0f) {
        const float distance = -ray.origin.y / ray.direction.y;
        if (distance >= 0.0f && distance <= reach.grid) {
            Vec3 point = ray.origin + ray.direction * distance;
            // On the grid, not a rounding error above or below it.
            point.y = 0.0f;
            return point;
        }
    }
    return ray.origin + ray.direction * reach.ahead;
}

std::vector<Uuid> materialsWith(std::span<const Uuid> materials, uint32_t submesh, const Uuid& material) {
    std::vector<Uuid> result(materials.begin(), materials.end());
    if (result.size() <= submesh) {
        result.resize(static_cast<size_t>(submesh) + 1);
    }
    result[submesh] = material;
    return result;
}

std::optional<std::string> viewportDropCommand(const EditorContext& context, const AssetInfo& asset, const Ray& ray,
                                               const std::optional<WorldMeshHit>& under, const DropReach& reach) {
    switch (viewportDropOf(asset.type)) {
        case ViewportDrop::Placed: {
            const std::optional<float> surface = under ? std::optional<float>(under->hit.distance) : std::nullopt;
            Json parameters = Json::object();
            parameters["asset"] = asset.id.toString();
            parameters["position"] = toJson(dropPoint(ray, surface, reach));
            return fmt::format("asset.instantiate {}", parameters.dump());
        }
        case ViewportDrop::OntoMesh: {
            if (!under) {
                return std::nullopt;
            }
            const World& world = context.world();
            const auto* renderer = world.tryGet<MeshRendererComponent>(under->entity);
            if (renderer == nullptr) {
                return std::nullopt;
            }
            const std::vector<Uuid> materials = materialsWith(renderer->materials, under->hit.submesh, asset.id);
            if (materials == renderer->materials) {
                return std::nullopt;
            }
            Json properties = Json::object();
            // As the reflection writes them: a nil id is null.
            properties["materials"] = PropertyCodec<std::vector<Uuid>>::toJson(materials);
            Json parameters = Json::object();
            parameters["entity"] = world.id(under->entity).toString();
            parameters["component"] = "MeshRenderer";
            parameters["properties"] = std::move(properties);
            return fmt::format("component.set {}", parameters.dump());
        }
        case ViewportDrop::None:
            break;
    }
    return std::nullopt;
}

const AssetInfo* draggedAsset(const AssetDatabase& database) {
    const ImGuiPayload* dragged = ImGui::GetDragDropPayload();
    if (dragged == nullptr || !dragged->IsDataType(PropertyEditor::kAssetPayload) ||
        dragged->DataSize != static_cast<int>(sizeof(Uuid))) {
        return nullptr;
    }
    Uuid id;
    std::memcpy(&id, dragged->Data, sizeof(Uuid));
    return database.find(id);
}

} // namespace cordite
