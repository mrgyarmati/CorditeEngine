#pragma once

/// @file
/// The permutations of the engine's own shaders: every one that the engine asks for.
///
/// A game that is exported has no shader compiler; it has a cache of exactly these
/// (ShaderLibraryDesc::trustCache). So a pass that asks for another permutation adds it
/// here in the same change - a test drives the renderer, the interface and the engine
/// through their paths and fails for one that is not on the list, and another compiles
/// every one of them.
///
/// @code
/// for (const ShaderRequest& request : engineShaderRequests()) {
///     CORDITE_TRY(library.load(request));   // fills a cache that a game can ship
/// }
/// @endcode

#include "cordite/shaders/ShaderTypes.h"

#include <vector>

namespace cordite {

/// Every permutation of the shaders in `shaders/` that the engine asks for: normalized,
/// each once, in the order of ShaderRequest::describe().
[[nodiscard]] std::vector<ShaderRequest> engineShaderRequests();

} // namespace cordite
