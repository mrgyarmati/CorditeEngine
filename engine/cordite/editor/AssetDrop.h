#pragma once

/// @file
/// What an asset dragged from the Assets panel does where it is dropped onto a viewport.
///
/// What can be placed (canBePlaced: a model, a prefab, a mesh) is placed at the point under
/// the pointer, as a root of the scene with its origin there: where the line of sight
/// through the pointer meets a mesh of the scene, or else the ground grid of the editor,
/// or else in front of the camera, as far away as what it looks at. A material is given to
/// the part of a mesh it is dropped on - the entry of MeshRendererComponent::materials of
/// the submesh under the pointer - and is not taken where there is no mesh. Nothing else is
/// taken. Dropped onto the hierarchy instead, what can be placed hangs under the entity of
/// the row, or is a root below the rows (HierarchyPanel).
///
/// What a drop does is a command line, as what a click in the Assets panel does is
/// (AssetBrowser.h): the viewport queues it, so that what a person does by dragging, an
/// agent does by name - and it is decided here, where no pixel is involved.
///
/// @code
/// const Ray ray = screenRay(view, pixel, targetSize);
/// const std::optional<WorldMeshHit> under = raycastMeshes(context.world(), context.assets(), ray);
/// const DropReach reach{.grid = 50.0f, .ahead = camera.focusDistance()};
/// if (std::optional<std::string> line = viewportDropCommand(context, *asset, ray, under, reach)) {
///     submittedLines.push_back(std::move(*line));
/// }
/// @endcode

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/editor/PosedMesh.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cordite {

/// What an asset of a kind does when it is dropped onto a viewport.
enum class ViewportDrop : uint8_t {
    /// Nothing: it is not taken.
    None,
    /// It is placed where it is dropped: a model, a prefab, a mesh.
    Placed,
    /// It is given to the part of a mesh it is dropped on: a material. It is taken only
    /// where there is a mesh.
    OntoMesh,
};

[[nodiscard]] ViewportDrop viewportDropOf(AssetType type);

/// How far along the line of sight through the pointer something dropped may land, in
/// metres.
struct DropReach {
    /// How far away the ground grid - the plane y = 0 - may be met: as far as it is drawn.
    /// Beyond, nobody sees it there.
    float grid = 50.0f;
    /// Where something lands that meets neither a mesh nor the grid: how far ahead the
    /// camera looks (EditorCamera::focusDistance()).
    float ahead = 10.0f;
};

/// Where something dropped along `ray` lands: `surface` along it, where the ray meets a
/// mesh; or else where it meets the ground grid ahead, from above or from below, within
/// `reach.grid`; or else `reach.ahead` along it. `ray.direction` is of length one.
[[nodiscard]] Vec3 dropPoint(const Ray& ray, std::optional<float> surface, const DropReach& reach);

/// `materials` with `material` in place `submesh` - and nil, the default material, in the
/// places before it that `materials` does not reach.
[[nodiscard]] std::vector<Uuid> materialsWith(std::span<const Uuid> materials, uint32_t submesh, const Uuid& material);

/// The command line that does what dropping `asset` onto a viewport along `ray` does,
/// `under` being the mesh the ray meets first: asset.instantiate at dropPoint() for what is
/// placed, and component.set of the MeshRenderer's materials for a material dropped on a
/// mesh. Nothing where it does nothing: for an asset of another kind, for a material where
/// there is no mesh, and for one that that part of the mesh has already.
[[nodiscard]] std::optional<std::string> viewportDropCommand(const EditorContext& context, const AssetInfo& asset,
                                                             const Ray& ray, const std::optional<WorldMeshHit>& under,
                                                             const DropReach& reach);

/// The asset that the drag of Dear ImGui in progress carries (PropertyEditor::kAssetPayload),
/// when it carries one that `database` has; null otherwise. For the target of a drop to ask
/// what it would be given, before it takes it.
[[nodiscard]] const AssetInfo* draggedAsset(const AssetDatabase& database);

} // namespace cordite
