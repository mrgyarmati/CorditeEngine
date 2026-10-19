#pragma once

/// @file
/// Compiles Slang shader source to SPIR-V for Vulkan, and on Windows to DXIL for
/// Direct3D 12 - through DXC, which Slang loads from beside itself.
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
/// CORDITE_TRY_ASSIGN(const ShaderBinary binary, compiler->compile(request, ShaderTarget::Spirv));
/// @endcode
///
/// Shader conventions the compiler is configured for:
/// - Matrices are column-major in memory, matching glm. Write `mul(matrix, vector)`.
/// - Resources are declared with HLSL registers. For Vulkan they map to bindings with
///   NVRHI's default offsets: textures `tN` -> N, samplers `sN` -> 128 + N, constant
///   buffers `bN` -> 256 + N, unordered access views `uN` -> 384 + N; `spaceN` is
///   descriptor set N. Direct3D 12 takes the registers as they are.
/// - DXIL is of shader model 6.0, the least that Direct3D 12 takes: the machines without
///   Vulkan that it is for are old ones.

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

    /// Run the optimizer. Disable to keep the generated code close to the source when
    /// debugging a shader in a graphics debugger.
    bool optimize = true;

    /// Embed source-level debug information in the code.
    bool debugInfo = false;
};

/// Whether the program can compile shaders to `target`: SPIR-V everywhere, DXIL on
/// Windows, where DXC is beside it.
[[nodiscard]] bool canCompileTo(ShaderTarget target);

class SlangShaderCompiler final : public IShaderCompiler {
public:
    /// Fails with Unsupported when the Slang runtime library cannot be initialized.
    [[nodiscard]] static Result<std::unique_ptr<SlangShaderCompiler>> create(const SlangShaderCompilerDesc& desc);
    ~SlangShaderCompiler() override;
    CORDITE_NON_MOVABLE(SlangShaderCompiler);

    /// Slang's build, the options, and for DXIL the version of DXC that writes it.
    [[nodiscard]] std::string version(ShaderTarget target) const override;

    /// Thread-safe; compilations are serialized internally. Fails with Unsupported for a
    /// target that cannot be compiled to here (canCompileTo()).
    [[nodiscard]] Result<ShaderBinary> compile(const ShaderRequest& request, ShaderTarget target) override;

private:
    struct Impl;

    explicit SlangShaderCompiler(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
