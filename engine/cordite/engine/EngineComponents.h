#pragma once

/// @file
/// The complete set of component types the engine provides.
///
/// Each module that defines components has a function that registers its own; this one
/// calls them all, in the order the components appear in scene files and in the editor's
/// inspector. An Engine does it for its own registry at start-up. Tools and tests that
/// work with scenes but need no running engine call it directly:
///
/// @code
/// ComponentRegistry components;
/// CORDITE_TRY(registerEngineComponents(components));
/// CORDITE_TRY(loadScene(path, world, components));
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/world/ComponentRegistry.h"

namespace cordite {

/// Fails with AlreadyExists when one of the names is taken in `registry` already.
[[nodiscard]] Status registerEngineComponents(ComponentRegistry& registry);

} // namespace cordite
