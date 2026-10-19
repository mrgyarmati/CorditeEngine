#include "cordite/shaders/ShaderCache.h"

#include "cordite/core/BinaryStream.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"

#include <fmt/format.h>

#include <optional>
#include <string>
#include <system_error>

namespace cordite {
namespace {

constexpr uint32_t kMagic = 0x44485343; // "CSHD" in little-endian byte order
constexpr uint32_t kFormatVersion = 1;
constexpr const char* kExtension = ".cshader";

/// Upper bounds for counts read from a file, so that corrupt data cannot make the
/// reader loop or allocate without limit.
constexpr uint32_t kMaxDependencies = 4096;
constexpr uint8_t kStageCount = 3;

} // namespace

std::vector<std::byte> encodeShaderBinary(const ShaderBinary& binary) {
    BinaryWriter writer;
    writer.write(kMagic);
    writer.write(kFormatVersion);
    writer.write(static_cast<uint8_t>(binary.stage));
    writer.writeString(binary.entryPoint);
    writer.writeString(binary.compilerVersion);

    writer.write(static_cast<uint32_t>(binary.dependencies.size()));
    for (const ShaderDependency& dependency : binary.dependencies) {
        writer.writeString(dependency.path);
        writer.write(dependency.contentHash.high);
        writer.write(dependency.contentHash.low);
    }

    writer.writeArray<std::byte>(binary.code);
    return writer.take();
}

Result<ShaderBinary> decodeShaderBinary(std::span<const std::byte> data) {
    BinaryReader reader(data);
    if (reader.read<uint32_t>() != kMagic) {
        return makeError(ErrorCode::ParseError, "not a compiled shader file");
    }
    const auto version = reader.read<uint32_t>();
    if (version != kFormatVersion) {
        return makeError(ErrorCode::VersionMismatch, "compiled shader format {} is not supported (expected {})",
                         version, kFormatVersion);
    }

    ShaderBinary binary;
    const auto stage = reader.read<uint8_t>();
    if (stage >= kStageCount) {
        return makeError(ErrorCode::ParseError, "compiled shader has an invalid stage ({})", stage);
    }
    binary.stage = static_cast<ShaderStage>(stage);
    binary.entryPoint = reader.readString();
    binary.compilerVersion = reader.readString();

    const auto dependencyCount = reader.read<uint32_t>();
    if (dependencyCount > kMaxDependencies) {
        return makeError(ErrorCode::ParseError, "compiled shader lists an implausible {} dependencies",
                         dependencyCount);
    }
    binary.dependencies.reserve(dependencyCount);
    for (uint32_t index = 0; index < dependencyCount && !reader.failed(); ++index) {
        ShaderDependency dependency;
        dependency.path = reader.readString();
        dependency.contentHash.high = reader.read<uint64_t>();
        dependency.contentHash.low = reader.read<uint64_t>();
        binary.dependencies.push_back(std::move(dependency));
    }

    binary.code = reader.readArray<std::byte>();
    CORDITE_TRY(reader.status("compiled shader"));

    if (reader.remaining() != 0) {
        return makeError(ErrorCode::ParseError, "compiled shader has {} unexpected trailing bytes", reader.remaining());
    }
    const std::optional<ShaderTarget> target = targetOf(binary.code);
    if (!target) {
        return makeError(ErrorCode::ParseError, "compiled shader holds neither SPIR-V nor signed DXIL");
    }
    binary.target = *target;
    return binary;
}

ShaderCache::ShaderCache(std::filesystem::path directory) : m_directory(std::move(directory)) {}

std::filesystem::path ShaderCache::entryPath(const Hash128& key, ShaderTarget target) const {
    // SPIR-V where it always was: the caches made before there were other targets stay.
    const std::string suffix = target == ShaderTarget::Spirv ? std::string() : fmt::format(".{}", nameOf(target));
    return m_directory / pathFromUtf8(key.toHex() + suffix + kExtension);
}

std::optional<ShaderBinary> ShaderCache::load(const Hash128& key, ShaderTarget target) const {
    const std::filesystem::path path = entryPath(key, target);
    if (!fileExists(path)) {
        return std::nullopt;
    }

    Result<std::vector<std::byte>> data = readBinaryFile(path);
    if (!data) {
        CORDITE_LOG_WARN("Shaders", "ignoring unreadable shader cache entry: {}", data.error().message);
        return std::nullopt;
    }

    Result<ShaderBinary> binary = decodeShaderBinary(*data);
    if (!binary) {
        // A stale format is expected after an engine update; anything else is worth a warning.
        if (binary.error().code == ErrorCode::VersionMismatch) {
            CORDITE_LOG_DEBUG("Shaders", "discarding {}: {}", pathToUtf8(path), binary.error().message);
        } else {
            CORDITE_LOG_WARN("Shaders", "discarding corrupt shader cache entry {}: {}", pathToUtf8(path),
                             binary.error().message);
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return std::nullopt;
    }
    if (binary->target != target) {
        // Code of another target under this one's name was not written by a cache.
        CORDITE_LOG_WARN("Shaders", "discarding {}: it holds {}, not {}", pathToUtf8(path), nameOf(binary->target),
                         nameOf(target));
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return std::nullopt;
    }
    return std::move(*binary);
}

Status ShaderCache::store(const Hash128& key, const ShaderBinary& binary) const {
    if (!isCodeOf(binary.target, binary.code)) {
        return makeError(ErrorCode::InvalidArgument, "refusing to cache a shader that is not valid {}",
                         nameOf(binary.target));
    }
    return writeBinaryFile(entryPath(key, binary.target), encodeShaderBinary(binary));
}

Status ShaderCache::clear() const {
    std::error_code error;
    if (!std::filesystem::is_directory(m_directory, error)) {
        return {};
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(m_directory, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == kExtension) {
            std::filesystem::remove(entry.path(), error);
            if (error) {
                return makeError(ErrorCode::IoError, "cannot delete {}: {}", pathToUtf8(entry.path()), error.message());
            }
        }
    }
    if (error) {
        return makeError(ErrorCode::IoError, "cannot list {}: {}", pathToUtf8(m_directory), error.message());
    }
    return {};
}

} // namespace cordite
