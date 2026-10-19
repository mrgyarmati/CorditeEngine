#pragma once

/// @file
/// Helpers shared by the shader tests: fake SPIR-V and DXIL, and a compiler that does not need
/// Slang, so the cache and library logic can be tested in isolation.

#include "cordite/core/FileSystem.h"
#include "cordite/core/Hash.h"
#include "cordite/shaders/ShaderCompiler.h"

#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace cordite::test {

/// Bytes that pass isSpirv(): the magic number followed by `payload` as extra words.
inline std::vector<std::byte> makeFakeSpirv(std::initializer_list<uint32_t> payload = {1, 2, 3}) {
    std::vector<uint32_t> words = {0x07230203u};
    words.insert(words.end(), payload);

    std::vector<std::byte> bytes(words.size() * sizeof(uint32_t));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

/// Bytes that pass isDxil(): a container - "DXBC", a hash that is not nothing, its version,
/// size and one part - whose part is "DXIL", holding `payload` as words.
inline std::vector<std::byte> makeFakeDxil(std::initializer_list<uint32_t> payload = {1, 2, 3}) {
    const auto payloadBytes = static_cast<uint32_t>(payload.size() * sizeof(uint32_t));
    std::vector<uint32_t> words = {0x43425844u, // "DXBC"
                                   0x01234567u,
                                   0x89abcdefu,
                                   0x0f1e2d3cu,
                                   0x4b5a6978u,        // the hash
                                   0x00000001u,        // version 1.0
                                   44u + payloadBytes, // the size of all of it
                                   1u,                 // one part
                                   36u,                // where it is
                                   0x4C495844u,        // "DXIL"
                                   payloadBytes};
    words.insert(words.end(), payload);

    std::vector<std::byte> bytes(words.size() * sizeof(uint32_t));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

/// Stands in for the real compiler. "Compiling" reads the requested file from `root`,
/// records it as the only dependency and returns fake code of the target derived from its
/// content and the defines, so different inputs give different output.
class FakeShaderCompiler final : public IShaderCompiler {
public:
    explicit FakeShaderCompiler(std::filesystem::path root) : m_root(std::move(root)) {}

    [[nodiscard]] std::string version(ShaderTarget target) const override {
        return target == ShaderTarget::Spirv ? compilerVersion : compilerVersion + "-dxil";
    }

    [[nodiscard]] Result<ShaderBinary> compile(const ShaderRequest& request, ShaderTarget target) override {
        ++compileCount;
        targets.push_back(target);
        if (failWith) {
            return tl::unexpected(*failWith);
        }

        const Result<std::vector<std::byte>> source = readBinaryFile(m_root / pathFromUtf8(request.path));
        if (!source) {
            return tl::unexpected(source.error());
        }

        const Hash128 sourceHash = hashBytes128(*source);
        ShaderBinary binary;
        binary.stage = request.stage;
        binary.target = target;
        binary.entryPoint = request.entryPoint;
        binary.compilerVersion = version(target);
        const std::initializer_list<uint32_t> payload = {static_cast<uint32_t>(sourceHash.low),
                                                         static_cast<uint32_t>(request.defines.size())};
        binary.code = target == ShaderTarget::Spirv ? makeFakeSpirv(payload) : makeFakeDxil(payload);
        binary.dependencies.push_back({request.path, sourceHash});
        return binary;
    }

    std::string compilerVersion = "fake-1";
    int compileCount = 0;
    /// The target of every compile, in order.
    std::vector<ShaderTarget> targets;
    /// When set, every compile fails with this error.
    std::optional<Error> failWith;

private:
    std::filesystem::path m_root;
};

} // namespace cordite::test
