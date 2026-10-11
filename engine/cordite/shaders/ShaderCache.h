#pragma once

/// @file
/// On-disk store of compiled shaders, one file per permutation.
///
/// @code
/// ShaderCache cache(projectDir / ".cordite/shaders");
/// if (std::optional<ShaderBinary> binary = cache.load(request.key())) { ... }
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

    /// Returns the entry for `key`, or nullopt when there is none or it is unreadable.
    [[nodiscard]] std::optional<ShaderBinary> load(const Hash128& key) const;

    /// Writes or replaces the entry for `key` atomically.
    [[nodiscard]] Status store(const Hash128& key, const ShaderBinary& binary) const;

    /// Deletes every cache entry. Other files in the directory are left alone.
    [[nodiscard]] Status clear() const;

    [[nodiscard]] const std::filesystem::path& directory() const { return m_directory; }

    /// File that holds the entry for `key`.
    [[nodiscard]] std::filesystem::path entryPath(const Hash128& key) const;

private:
    std::filesystem::path m_directory;
};

/// Serializes a binary to the cache file format. Exposed for the packaging pipeline,
/// which writes the same format into exported games.
[[nodiscard]] std::vector<std::byte> encodeShaderBinary(const ShaderBinary& binary);

/// Parses the cache file format. Fails with ParseError for corrupt data and with
/// VersionMismatch for data written by an incompatible engine version.
[[nodiscard]] Result<ShaderBinary> decodeShaderBinary(std::span<const std::byte> data);

} // namespace cordite
