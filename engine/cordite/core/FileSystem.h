#pragma once

/// @file
/// File I/O helpers that report failures as Results and treat paths as UTF-8.
///
/// @code
/// CORDITE_TRY_ASSIGN(auto text, readTextFile(projectDir / "settings.json"));
/// CORDITE_TRY(writeTextFile(projectDir / "settings.json", updated));
/// @endcode

#include "cordite/core/Result.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

/// Converts a path to UTF-8 using forward slashes, suitable for logs and serialized data.
std::string pathToUtf8(const std::filesystem::path& path);

/// Builds a path from UTF-8 text. Never construct a path from std::string directly: on
/// Windows that interprets the bytes in the active code page instead of UTF-8.
std::filesystem::path pathFromUtf8(std::string_view text);

Result<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path);
Result<std::string> readTextFile(const std::filesystem::path& path);

/// Writes a file atomically: the data goes to a temporary sibling that then replaces the
/// target, so readers never observe a partially written file. Creates parent directories.
Status writeBinaryFile(const std::filesystem::path& path, std::span<const std::byte> data);
Status writeTextFile(const std::filesystem::path& path, std::string_view text);

/// Creates a directory and any missing parents. Succeeds when it already exists.
Status ensureDirectory(const std::filesystem::path& path);

/// Returns true when the path names an existing regular file.
bool fileExists(const std::filesystem::path& path);

/// Returns true when the path names an existing directory.
bool directoryExists(const std::filesystem::path& path);

/// Returns `path` relative to `base` using forward slashes, or an error when `path` is
/// not inside `base`. Used to validate that user-supplied paths stay inside a project.
Result<std::string> relativePathWithin(const std::filesystem::path& base, const std::filesystem::path& path);

} // namespace cordite
