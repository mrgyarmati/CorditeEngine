#include "cordite/engine/EngineComponents.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"

namespace cordite {

Status registerEngineComponents(ComponentRegistry& registry) {
    // A module that gains components adds its registration function here.
    CORDITE_TRY(registerWorldComponents(registry));
    CORDITE_TRY(registerRendererComponents(registry));
    CORDITE_TRY(registerAnimationComponents(registry));
    CORDITE_TRY(registerPhysicsComponents(registry));
    return {};
}

} // namespace cordite
