#include "cordite/scripting/ScriptComponents.h"

namespace cordite {

const TypeInfo& scriptComponentType() {
    static const TypeInfo kType =
        TypeBuilder<ScriptComponent>("Script")
            .property("script", &ScriptComponent::script,
                      {.description = "The script that runs for the entity: a .lua file of the project.",
                       .semantic = PropertySemantic::AssetReference,
                       .referenceType = "Script"})
            .property("properties", &ScriptComponent::properties,
                      {.description = "What the script is told for this entity: a value for each of the "
                                      "properties it declares that is to be other than the script says."})
            .build();
    return kType;
}

Status registerScriptComponents(ComponentRegistry& registry) {
    return registry.add<ScriptComponent>(
        scriptComponentType(),
        {.description = "Runs a script for the entity while the game runs: its `start` once, its `update` every "
                        "frame, its `destroy` when the script is taken off or the entity is destroyed by a script."});
}

} // namespace cordite
