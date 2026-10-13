#pragma once

/// @file
/// The component that puts a script on an entity.
///
/// @code
/// ComponentRegistry components;
/// CORDITE_TRY(registerScriptComponents(components));
///
/// ScriptComponent& script = world.add<ScriptComponent>(door);
/// script.script = assets.database().findByReference("scripts/door.lua")->id;
/// script.properties = Json{{"speed", 2.5}};       // values for what the script declares
/// @endcode
///
/// The component only says which script, and what it is told. ScriptSystem
/// (ScriptSystem.h) is what runs it.

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/world/ComponentRegistry.h"

namespace cordite {

/// Runs a script for the entity while the game runs.
struct ScriptComponent {
    /// The script: a `.lua` asset. Without one, nothing runs.
    Uuid script;
    /// What the script is told for this entity: values for the `properties` it
    /// declares. One that it does not declare, or of another kind than it declares,
    /// keeps the script from starting.
    Json properties = Json::object();

    bool operator==(const ScriptComponent&) const = default;
};

/// The description of ScriptComponent: "Script".
[[nodiscard]] const TypeInfo& scriptComponentType();

/// Registers the Script component. Fails as ComponentRegistry::add() does.
[[nodiscard]] Status registerScriptComponents(ComponentRegistry& registry);

} // namespace cordite
