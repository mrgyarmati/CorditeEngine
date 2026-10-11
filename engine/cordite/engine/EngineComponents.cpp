#include "cordite/engine/EngineComponents.h"

#include "cordite/world/Components.h"

namespace cordite {

Status registerEngineComponents(ComponentRegistry& registry) {
    // A module that gains components adds its registration function here.
    return registerWorldComponents(registry);
}

} // namespace cordite
