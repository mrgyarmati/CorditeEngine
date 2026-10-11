#pragma once

/// @file
/// Helpers shared by the shader tests: fake SPIR-V and a compiler that does not need
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

/// Stands in for the real compiler. "Compiling" reads the requested file from `root`,
/// records it as the only dependency and returns fake SPIR-V derived from its content
/// and the defines, so different inputs give different output.
class FakeShaderCompiler final : public IShaderCompiler {
public:
    explicit FakeShaderCompiler(std::filesystem::path root) : m_root(std::move(root)) {}

    [[nodiscard]] std::string version() const override { return compilerVersion; }

    [[nodiscard]] Result<ShaderBinary> compile(const ShaderRequest& request) override {
        ++compileCount;
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
        binary.entryPoint = request.entryPoint;
        binary.compilerVersion = compilerVersion;
        binary.code =
            makeFakeSpirv({static_cast<uint32_t>(sourceHash.low), static_cast<uint32_t>(request.defines.size())});
        binary.dependencies.push_back({request.path, sourceHash});
        return binary;
    }

    std::string compilerVersion = "fake-1";
    int compileCount = 0;
    /// When set, every compile fails with this error.
    std::optional<Error> failWith;

private:
    std::filesystem::path m_root;
};

} // namespace cordite::test
