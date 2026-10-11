#pragma once

/// @file
/// Forward declaration of the Json type for headers that only pass it by reference.
/// Include cordite/core/Json.h in the source file that actually uses it.

#include <nlohmann/json_fwd.hpp>

namespace cordite {

/// JSON value that preserves the insertion order of object keys, which keeps saved
/// files stable and diff-friendly.
using Json = nlohmann::ordered_json;

} // namespace cordite
