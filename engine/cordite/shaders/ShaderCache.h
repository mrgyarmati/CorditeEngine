#pragma once

/// @file
/// On-disk store of compiled shaders, one file per permutation and target.
///
/// @code
/// ShaderCache cache(projectDir / ".cordite/shaders");
/// if (std::optional<ShaderBinary> binary = cache.load(request.key(), ShaderTarget::Spirv)) { ... }
/// CORDITE_TRY(cache.store(request.key(), compiled));
/// @endcode
///
/// Cache files are treated as untrusted input: an entry that is truncated, corrupt or
/// written by a different format version is ignored and deleted, never trusted.

#include "cordite/core/Hash.h"
#include "cordite/core/Result.h"
#include "cordite/shaders/ShaderTypes.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace cordite {

class ShaderCache {
public:
    /// The directory is created on the first store().
    explicit ShaderCache(std::filesystem::path directory);

    /// Returns the entry for `key` compiled to `target`, or nullopt when there is none, it
    /// is unreadable, or it holds the code of another target.
    [[nodiscard]] std::optional<ShaderBinary> load(const Hash128& key, ShaderTarget target) const;

    /// Writes or replaces the entry for `key` and the target of `binary` atomically.
    /// Fails with InvalidArgument when its code is not the code of its target.
    [[nodiscard]] Status store(const Hash128& key, const ShaderBinary& binary) const;

    /// Deletes every cache entry, of every target. Other files in the directory are left
    /// alone.
    [[nodiscard]] Status clear() const;

    [[nodiscard]] const std::filesystem::path& directory() const { return m_directory; }

    /// File that holds the entry for `key` and `target`: `<key>.cshader` for SPIR-V, as
    /// caches have always kept it, and `<key>.<target>.cshader` for the others.
    [[nodiscard]] std::filesystem::path entryPath(const Hash128& key, ShaderTarget target) const;

private:
    std::filesystem::path m_directory;
};

/// Serializes a binary to the cache file format. Exposed for the packaging pipeline,
/// which writes the same format into exported games.
[[nodiscard]] std::vector<std::byte> encodeShaderBinary(const ShaderBinary& binary);

/// Parses the cache file format. The target is what the code is, which says so itself:
/// the format has no field for it, so the caches of SPIR-V written before there was DXIL
/// are read as they were. Fails with ParseError for corrupt data or code of no target, and
/// with VersionMismatch for data written by an incompatible engine version.
[[nodiscard]] Result<ShaderBinary> decodeShaderBinary(std::span<const std::byte> data);

} // namespace cordite
