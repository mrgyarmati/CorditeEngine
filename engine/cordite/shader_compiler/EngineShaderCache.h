#pragma once

/// @file
/// The engine's shaders, compiled ahead: what a game that is exported has instead of a
/// compiler.
///
/// An engine made without a shader compiler (EngineDesc::shaderCompiler left empty) reads
/// every shader from its cache, trusted as it is. compileEngineShaders() makes such a cache
/// - through a ShaderLibrary with the Slang compiler, so that every entry is kept exactly as
/// a library will look for it - and makeSlangShaderCompiler() is the compiler that an
/// engine which compiles is given.
///
/// @code
/// // Exporting: the cache the game ships.
/// CORDITE_TRY(compileEngineShaders(engineShaderDirectory, package / "cache" / "shaders"));
///
/// // Developing: an engine that compiles what changed.
/// EngineDesc desc;
/// desc.shaderCompiler = makeSlangShaderCompiler;
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/shaders/ShaderCompiler.h"

#include <filesystem>
#include <memory>

namespace cordite {

/// The Slang compiler of the shaders under `sourceDirectory`, as an engine asks for one
/// (EngineDesc::shaderCompiler). Fails as SlangShaderCompiler::create() does.
[[nodiscard]] Result<std::unique_ptr<IShaderCompiler>>
makeSlangShaderCompiler(const std::filesystem::path& sourceDirectory);

/// Compiles every permutation of the engine's shaders (engineShaderRequests()), from the
/// sources in `sourceDirectory`, into a shader cache in `cacheDirectory` - kept as a
/// ShaderLibrary keeps one, which an engine without a compiler reads from
/// `EngineDesc::cacheDirectory / "shaders"`. What is in the cache already, and up to date,
/// stays. Fails with the first permutation that does not compile, naming it.
[[nodiscard]] Status compileEngineShaders(const std::filesystem::path& sourceDirectory,
                                          const std::filesystem::path& cacheDirectory);

} // namespace cordite
