#pragma once

/// @file
/// The interface between the shader library and whatever turns source into SPIR-V.
///
/// The engine's only implementation is SlangShaderCompiler in the shader_compiler
/// module. Keeping the interface here lets exported games, which ship precompiled
/// shaders, link the shader library without linking a compiler; tests use it to
/// substitute a fake compiler.

#include "cordite/core/Result.h"
#include "cordite/shaders/ShaderTypes.h"

#include <string>

namespace cordite {

class IShaderCompiler {
public:
    virtual ~IShaderCompiler() = default;

    /// A string that changes whenever the compiler or its options could produce
    /// different output for the same request. It is part of every cache key.
    [[nodiscard]] virtual std::string version() const = 0;

    /// Compiles one normalized request. On failure the error message is the compiler's
    /// diagnostic output, with file names and line numbers.
    [[nodiscard]] virtual Result<ShaderBinary> compile(const ShaderRequest& request) = 0;
};

} // namespace cordite
