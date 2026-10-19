#pragma once

/// @file
/// The interface between the shader library and whatever turns source into the code of a
/// device: SPIR-V, or DXIL (ShaderTarget).
///
/// The engine's only implementation is SlangShaderCompiler in the shader_compiler
/// module. Keeping the interface here lets exported games, which ship precompiled
/// shaders, link the shader library without linking a compiler; tests use it to
/// substitute a fake compiler.
///
/// @code
/// class CountingCompiler final : public IShaderCompiler {
/// public:
///     std::string version(ShaderTarget target) const override { return "counting-1"; }
///     Result<ShaderBinary> compile(const ShaderRequest& request, ShaderTarget target) override {
///         ++compilations;
///         return ShaderBinary{.target = target};
///     }
///     int compilations = 0;
/// };
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/shaders/ShaderTypes.h"

#include <string>

namespace cordite {

class IShaderCompiler {
public:
    virtual ~IShaderCompiler() = default;

    /// A string that changes whenever the compiler or its options could produce
    /// different output for the same request and target: a cached shader that another
    /// version made is made again.
    [[nodiscard]] virtual std::string version(ShaderTarget target) const = 0;

    /// Compiles one normalized request to the code of `target`. On failure the error
    /// message is the compiler's diagnostic output, with file names and line numbers; a
    /// target the compiler cannot write is Unsupported.
    [[nodiscard]] virtual Result<ShaderBinary> compile(const ShaderRequest& request, ShaderTarget target) = 0;
};

} // namespace cordite
