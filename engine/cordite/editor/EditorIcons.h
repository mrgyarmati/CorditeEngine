#pragma once

/// @file
/// Which icon the editor shows for what: an asset by its type, a directory of assets by
/// whether it is open, an entity by what it is.
///
/// @code
/// ImGui::Text("%s %s", iconOf(asset.type).text(), asset.name.c_str());
/// ImGui::Text("%s %s", entityIcon(world, entity).text(), world.name(entity).c_str());
/// @endcode

#include "cordite/assets/AssetDatabase.h"
#include "cordite/ui/UiIcons.h"
#include "cordite/world/World.h"

namespace cordite {

/// The icon of an asset of a type.
[[nodiscard]] UiIcon iconOf(AssetType type);

/// The icon of a directory of assets, open or closed.
[[nodiscard]] UiIcon directoryIcon(bool open);

/// The icon an entity is shown with: what it is most. In this order: a placed prefab, a
/// camera, a light - a sun when it is directional - what lights a whole world (an
/// environment or ambient light), fog, what finishes the picture, a mesh, an animated
/// figure, a character, a body or a joint, something heard or that hears, a script; and a
/// plain entity, which an entity without a component of these is. An entity that is no
/// longer valid is a plain entity too.
[[nodiscard]] UiIcon entityIcon(const World& world, Entity entity);

} // namespace cordite
