#include "cordite/core/FileSystem.h"

#include "cordite/core/Uuid.h"

#include <cstring>
#include <exception>
#include <fstream>
#include <system_error>

namespace cordite {
namespace {

Result<std::vector<std::byte>> readFileBytes(const std::filesystem::path& path) {
    // Some platforms let a directory be opened as a stream and then report a nonsensical
    // size for it, so rule out everything that is not a regular file first.
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (!std::filesystem::exists(status)) {
        return makeError(ErrorCode::NotFound, "{} does not exist", pathToUtf8(path));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return makeError(ErrorCode::IoError, "{} is not a regular file", pathToUtf8(path));
    }

    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return makeError(ErrorCode::IoError, "cannot open {} for reading", pathToUtf8(path));
    }

    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return makeError(ErrorCode::IoError, "cannot determine the size of {}", pathToUtf8(path));
    }

    std::vector<std::byte> data;
    try {
        data.resize(static_cast<size_t>(size));
    } catch (const std::exception&) {
        return makeError(ErrorCode::IoError, "not enough memory to read {} ({} bytes)", pathToUtf8(path), size);
    }

    stream.seekg(0);
    if (size > 0 && !stream.read(reinterpret_cast<char*>(data.data()), size)) {
        return makeError(ErrorCode::IoError, "failed to read {} bytes from {}", size, pathToUtf8(path));
    }
    return data;
}

/// Returns a path next to `path` that no other writer, in this or another process, will
/// pick at the same time.
std::filesystem::path temporarySiblingOf(const std::filesystem::path& path) {
    std::filesystem::path temporary = path;
    temporary += pathFromUtf8(fmt::format(".{}.tmp", Uuid::generate().toString()));
    return temporary;
}

} // namespace

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path pathFromUtf8(std::string_view text) {
    return {std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size())};
}

Result<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path) {
    return readFileBytes(path);
}

Result<std::string> readTextFile(const std::filesystem::path& path) {
    CORDITE_TRY_ASSIGN(auto bytes, readFileBytes(path));

    std::string text(bytes.size(), '\0');
    if (!bytes.empty()) {
        std::memcpy(text.data(), bytes.data(), bytes.size());
    }
    return text;
}

Status writeBinaryFile(const std::filesystem::path& path, std::span<const std::byte> data) {
    if (path.has_parent_path()) {
        CORDITE_TRY(ensureDirectory(path.parent_path()));
    }

    const std::filesystem::path temporary = temporarySiblingOf(path);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return makeError(ErrorCode::IoError, "cannot open {} for writing", pathToUtf8(temporary));
        }
        if (!data.empty()) {
            stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        stream.flush();
        if (!stream) {
            stream.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return makeError(ErrorCode::IoError, "failed to write {} bytes to {}", data.size(), pathToUtf8(path));
        }
    }

    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return makeError(ErrorCode::IoError, "cannot replace {}: {}", pathToUtf8(path), error.message());
    }
    return {};
}

Status writeTextFile(const std::filesystem::path& path, std::string_view text) {
    return writeBinaryFile(path, std::as_bytes(std::span<const char>(text.data(), text.size())));
}

Status ensureDirectory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        return makeError(ErrorCode::IoError, "cannot create directory {}: {}", pathToUtf8(path), error.message());
    }
    if (!std::filesystem::is_directory(path, error)) {
        return makeError(ErrorCode::IoError, "{} exists but is not a directory", pathToUtf8(path));
    }
    return {};
}

bool fileExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool directoryExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

Result<std::string> relativePathWithin(const std::filesystem::path& base, const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonicalBase = std::filesystem::weakly_canonical(base, error);
    if (error) {
        return makeError(ErrorCode::InvalidArgument, "cannot resolve {}: {}", pathToUtf8(base), error.message());
    }
    const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return makeError(ErrorCode::InvalidArgument, "cannot resolve {}: {}", pathToUtf8(path), error.message());
    }

    // "." means the path is the base itself, which is not a location inside it.
    const std::filesystem::path relative = canonicalPath.lexically_relative(canonicalBase);
    if (relative.empty() || relative == "." || *relative.begin() == "..") {
        return makeError(ErrorCode::InvalidArgument, "{} is outside {}", pathToUtf8(path), pathToUtf8(base));
    }
    return pathToUtf8(relative);
}

} // namespace cordite
