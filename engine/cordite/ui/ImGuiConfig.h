#pragma once

/// @file
/// Compile-time configuration of Dear ImGui.
///
/// The build passes this file to ImGui as IMGUI_USER_CONFIG, so `imgui.h` includes it
/// wherever it is used - in ImGui's own sources as well as in the engine and the editor -
/// and all of them agree on the settings. Do not include it yourself; include `<imgui.h>`.
///
/// @code
/// #include <imgui.h>      // already configured
/// @endcode

#include "cordite/core/ExternalAssert.h"

// ImGui checks how it is used very thoroughly. Report what it finds like any other
// failed engine assertion instead of through the C runtime.
#define IM_ASSERT(expression) CORDITE_EXTERNAL_ASSERT(expression)

// Keeps names that ImGui has already renamed out of new code.
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
