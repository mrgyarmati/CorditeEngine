#pragma once

/// @file
/// The one place that includes EnTT, the library that stores entities and components.
///
/// EnTT is configured through macros that must be defined before its first header is
/// seen, and that must be the same in every translation unit. Going through this header
/// guarantees both, so engine and game code includes it instead of any `<entt/...>`
/// header (the module_layering test enforces that):
///
/// @code
/// #include "cordite/world/Entt.h"
///
/// void countCameras(const entt::registry& registry);
/// @endcode
///
/// The configuration routes EnTT's internal checks into the engine's assertion system:
/// misuse such as touching a destroyed entity is reported like any other failed
/// CORDITE_ASSERT in Debug and RelWithDebInfo builds, and costs nothing in Release.

#include "cordite/core/Assert.h"

#if defined(ENTT_CONFIG_CONFIG_H)
#error "An EnTT header was included directly. Include cordite/world/Entt.h instead."
#endif

#define ENTT_ASSERT(condition, message) CORDITE_ASSERT(condition, "{}", message)

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>
