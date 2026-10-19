#pragma once

/// @file
/// Value types describing a shader to compile and the result of compiling it.
///
/// A shader is identified by a ShaderRequest: a source file, an entry point, a stage and
/// a set of preprocessor defines. Each distinct set of defines is a "permutation" of the
/// same source. The request has a deterministic key, so the same permutation always maps
/// to the same cache entry on every machine.
///
/// @code
/// ShaderRequest request;
/// request.path = "passes/GBuffer.slang";
/// request.entryPoint = "pixelMain";
/// request.stage = ShaderStage::Pixel;
/// request.defines = {{"ALPHA_TEST", "1"}, {"NORMAL_MAP", "1"}};
/// CORDITE_TRY(request.normalize());
/// const std::string key = request.key().toHex();
/// @endcode

#include "cordite/core/Hash.h"
#include "cordite/core/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

enum class ShaderStage : uint8_t {
    Vertex,
    Pixel,
    Compute,
};

std::string_view nameOf(ShaderStage stage);

/// What a shader is compiled to: the code of one kind of device.
enum class ShaderTarget : uint8_t {
    /// SPIR-V, for Vulkan.
    Spirv,
    /// DXIL, signed, for Direct3D 12 (Windows).
    Dxil,
};

inline constexpr std::array<ShaderTarget, 2> kShaderTargets = {ShaderTarget::Spirv, ShaderTarget::Dxil};

/// "spirv" or "dxil".
std::string_view nameOf(ShaderTarget target);

/// A preprocessor definition, `#define name value`.
struct ShaderDefine {
    std::string name;
    std::string value = "1";

    bool operator==(const ShaderDefine&) const = default;
};

struct ShaderRequest {
    /// Source file relative to a shader root, with forward slashes: "passes/Tonemap.slang".
    std::string path;
    std::string entryPoint = "main";
    ShaderStage stage = ShaderStage::Vertex;
    std::vector<ShaderDefine> defines;

    /// Validates the request and sorts the defines by name, so that two requests listing
    /// the same defines in a different order are the same permutation. Fails with
    /// InvalidArgument for an empty or escaping path, an invalid identifier, or a define
    /// that appears twice.
    [[nodiscard]] Status normalize();

    /// Deterministic identity of this permutation. The request must be normalized.
    [[nodiscard]] Hash128 key() const;

    /// A short human-readable description for logs: "passes/GBuffer.slang:pixelMain [A=1 B=1]".
    [[nodiscard]] std::string describe() const;

    bool operator==(const ShaderRequest&) const = default;
};

/// A source file a compiled shader was built from, and its content at that time.
struct ShaderDependency {
    /// Relative to a shader root, with forward slashes.
    std::string path;
    Hash128 contentHash;

    bool operator==(const ShaderDependency&) const = default;
};

/// A compiled shader: its code plus what is needed to tell whether it is still up to date.
struct ShaderBinary {
    ShaderStage stage = ShaderStage::Vertex;
    /// What the code is.
    ShaderTarget target = ShaderTarget::Spirv;
    std::string entryPoint;
    /// IShaderCompiler::version() of the compiler that produced the code, for its target.
    std::string compilerVersion;
    /// A SPIR-V module, whose size is a multiple of four bytes; or a DXIL container.
    std::vector<std::byte> code;
    /// Every file that was read to compile the shader, the main file included.
    std::vector<ShaderDependency> dependencies;

    bool operator==(const ShaderBinary&) const = default;
};

/// Returns true when `code` starts with the SPIR-V magic number and holds whole words.
[[nodiscard]] bool isSpirv(std::span<const std::byte> code);

/// Returns true when `code` is a DXIL container that is signed: the magic "DXBC", a hash
/// that is not all nothing - Direct3D 12 refuses a shader that is unsigned - and the size
/// the container says it has.
[[nodiscard]] bool isDxil(std::span<const std::byte> code);

/// Whether `code` is the code of `target` (isSpirv(), isDxil()).
[[nodiscard]] bool isCodeOf(ShaderTarget target, std::span<const std::byte> code);

/// The target whose code `code` is; none for what is the code of none.
[[nodiscard]] std::optional<ShaderTarget> targetOf(std::span<const std::byte> code);

} // namespace cordite
