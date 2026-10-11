#pragma once

/// @file
/// The renderer's source of compiled shaders.
///
/// A ShaderLibrary answers "give me this permutation" from, in order: memory, the disk
/// cache, and the compiler. During development every lookup also checks that the source
/// files the shader was built from are unchanged, and recompiles when they are not. An
/// exported game has no compiler and no sources; it runs the same library in cache-only
/// mode against the shaders packaged with it.
///
/// @code
/// ShaderLibraryDesc desc;
/// desc.sourceRoots = {engineDir / "shaders"};
/// desc.cacheDirectory = projectDir / ".cordite/shaders";
/// desc.compiler = &slangCompiler;
/// ShaderLibrary library(desc);
///
/// CORDITE_TRY_ASSIGN(auto shader, library.load({.path = "passes/Tonemap.slang",
///                                               .entryPoint = "pixelMain",
///                                               .stage = ShaderStage::Pixel}));
/// device->createShader(shaderDesc, shader->code.data(), shader->code.size());
/// @endcode
///
/// Thread-safe: load() may be called from several threads.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/shaders/ShaderCache.h"
#include "cordite/shaders/ShaderCompiler.h"
#include "cordite/shaders/ShaderTypes.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cordite {

struct ShaderLibraryDesc {
    /// Directories searched, in order, for shader source files. May be empty when
    /// `compiler` is null.
    std::vector<std::filesystem::path> sourceRoots;

    /// Where compiled shaders are stored between runs. Empty keeps them in memory only.
    std::filesystem::path cacheDirectory;

    /// Compiles shaders that are missing from the cache or out of date. Not owned; must
    /// outlive the library. Null runs the library in cache-only mode.
    IShaderCompiler* compiler = nullptr;

    /// Accept cache entries without checking them against the source files. Set for
    /// exported games, which ship a cache but no sources.
    bool trustCache = false;
};

/// Counters describing where shaders came from, for tests and the profiler.
struct ShaderLibraryStats {
    uint64_t memoryHits = 0;
    uint64_t diskHits = 0;
    uint64_t compilations = 0;
    uint64_t failures = 0;
};

class ShaderLibrary {
public:
    explicit ShaderLibrary(ShaderLibraryDesc desc);
    ~ShaderLibrary() = default;
    CORDITE_NON_MOVABLE(ShaderLibrary);

    /// Returns the compiled shader for a request, compiling it if necessary. The request
    /// does not need to be normalized. Fails with NotFound in cache-only mode when the
    /// permutation is not in the cache, and with the compiler's diagnostics when
    /// compilation fails.
    [[nodiscard]] Result<std::shared_ptr<const ShaderBinary>> load(ShaderRequest request);

    /// Forgets every in-memory shader whose source files changed on disk and returns the
    /// affected requests, so the renderer can rebuild the pipelines that use them.
    [[nodiscard]] std::vector<ShaderRequest> invalidateChanged();

    /// Forgets all in-memory shaders. The disk cache is untouched.
    void clearMemory();

    [[nodiscard]] ShaderLibraryStats stats() const;

    [[nodiscard]] const std::vector<std::filesystem::path>& sourceRoots() const { return m_desc.sourceRoots; }

private:
    struct Entry {
        ShaderRequest request;
        std::shared_ptr<const ShaderBinary> binary;
    };

    /// True when every dependency of `binary` still has the content it was compiled from.
    [[nodiscard]] bool isUpToDate(const ShaderBinary& binary) const;

    /// Finds a source file under the roots. Returns an empty path when there is none.
    [[nodiscard]] std::filesystem::path resolveSource(std::string_view relativePath) const;

    ShaderLibraryDesc m_desc;
    std::optional<ShaderCache> m_cache;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
    ShaderLibraryStats m_stats;
};

} // namespace cordite
