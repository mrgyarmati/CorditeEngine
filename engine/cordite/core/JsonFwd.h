#pragma once

/// @file
/// Forward declaration of the Json type for headers that only pass it by reference.
/// Include cordite/core/Json.h in the source file that actually uses it.
///
/// @code
/// // Widget.h
/// #include "cordite/core/JsonFwd.h"
/// [[nodiscard]] Json describe(const Widget& widget);
///
/// // Widget.cpp
/// #include "cordite/core/Json.h"
/// @endcode

#include <nlohmann/json_fwd.hpp>

namespace cordite {

/// JSON value that preserves the insertion order of object keys, which keeps saved
/// files stable and diff-friendly.
using Json = nlohmann::ordered_json;

} // namespace cordite
