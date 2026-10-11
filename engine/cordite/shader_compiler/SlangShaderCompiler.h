#pragma once

/// @file
/// Compiles Slang shader source to SPIR-V.
///
/// This module is linked by the editor, the tools and development builds of the runtime.
/// Exported games do not link it: they load shaders that were compiled at export time.
///
/// @code
/// SlangShaderCompilerDesc desc;
/// desc.sourceRoots = {engineDir / "shaders"};
/// CORDITE_TRY_ASSIGN(auto compiler, SlangShaderCompiler::create(desc));
///
/// ShaderRequest request{.path = "passes/Tonemap.slang", .entryPoint = "pixelMain", .stage = ShaderStage::Pixel};
/// CORDITE_TRY(request.normalize());
/// CORDITE_TRY_ASSIGN(const ShaderBinary binary, compiler->compile(request));
/// @endcode
///
/// Shader conventions the compiler is configured for:
/// - Matrices are column-major in memory, matching glm. Write `mul(matrix, vector)`.
/// - Resources are declared with HLSL registers and map to Vulkan bindings with NVRHI's
///   default offsets: textures `tN` -> N, samplers `sN` -> 128 + N, constant buffers
///   `bN` -> 256 + N, unordered access views `uN` -> 384 + N. `spaceN` is descriptor set N.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/shaders/ShaderCompiler.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cordite {

struct SlangShaderCompilerDesc {
    /// Directories searched for the requested file and for anything it includes or imports.
    std::vector<std::filesystem::path> sourceRoots;

    /// Run the SPIR-V optimizer. Disable to keep the generated code close to the source
    /// when debugging a shader in a graphics debugger.
    bool optimize = true;

    /// Embed source-level debug information in the SPIR-V.
    bool debugInfo = false;
};

class SlangShaderCompiler final : public IShaderCompiler {
public:
    /// Fails with Unsupported when the Slang runtime library cannot be initialized.
    [[nodiscard]] static Result<std::unique_ptr<SlangShaderCompiler>> create(const SlangShaderCompilerDesc& desc);
    ~SlangShaderCompiler() override;
    CORDITE_NON_MOVABLE(SlangShaderCompiler);

    [[nodiscard]] std::string version() const override;

    /// Thread-safe; compilations are serialized internally.
    [[nodiscard]] Result<ShaderBinary> compile(const ShaderRequest& request) override;

private:
    struct Impl;

    explicit SlangShaderCompiler(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
