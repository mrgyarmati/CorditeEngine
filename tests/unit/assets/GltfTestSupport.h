#pragma once

/// @file
/// Builds small glTF files for tests, piece by piece.

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"

#include "AssetTestSupport.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cordite::test {

// Component types as glTF numbers them.
inline constexpr int kGltfByte = 5120;
inline constexpr int kGltfUnsignedByte = 5121;
inline constexpr int kGltfShort = 5122;
inline constexpr int kGltfUnsignedShort = 5123;
inline constexpr int kGltfUnsignedInt = 5125;
inline constexpr int kGltfFloat = 5126;

/// A glTF document under construction: JSON that the test fills in, and a binary
/// buffer that accessors are appended to.
///
///     GltfFile file;
///     const int positions = file.addAccessor<float>("VEC3", kGltfFloat, {0, 0, 0, 1, 0, 0, 0, 1, 0});
///     file.json["meshes"] = parseOrFail(R"([{"primitives": [{"attributes": {"POSITION": 0}}]}])");
///     const std::filesystem::path path = file.write(directory.path(), "triangle");
class GltfFile {
public:
    GltfFile() {
        json = Json::object();
        json["asset"]["version"] = "2.0";
    }

    /// Everything but buffers, buffer views and accessors, which write() adds.
    Json json;

    /// Appends `values` to the buffer and returns the index of an accessor for them.
    template <typename T>
    int addAccessor(std::string_view type, int componentType, const std::vector<T>& values, bool normalized = false) {
        while (m_bytes.size() % 4 != 0) {
            m_bytes.push_back(std::byte{0});
        }
        const size_t offset = m_bytes.size();
        const auto* bytes = reinterpret_cast<const std::byte*>(values.data());
        m_bytes.insert(m_bytes.end(), bytes, bytes + values.size() * sizeof(T));

        Json view = Json::object();
        view["buffer"] = 0;
        view["byteOffset"] = offset;
        view["byteLength"] = values.size() * sizeof(T);
        m_views.push_back(std::move(view));

        Json accessor = Json::object();
        accessor["bufferView"] = m_views.size() - 1;
        accessor["componentType"] = componentType;
        accessor["count"] = values.size() / componentCount(type);
        accessor["type"] = type;
        if (normalized) {
            accessor["normalized"] = true;
        }
        m_accessors.push_back(std::move(accessor));
        return static_cast<int>(m_accessors.size() - 1);
    }

    /// Appends raw bytes (an embedded image, say) and returns the index of their view.
    int addView(std::span<const std::byte> bytes) {
        while (m_bytes.size() % 4 != 0) {
            m_bytes.push_back(std::byte{0});
        }
        Json view = Json::object();
        view["buffer"] = 0;
        view["byteOffset"] = m_bytes.size();
        view["byteLength"] = bytes.size();
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
        m_views.push_back(std::move(view));
        return static_cast<int>(m_views.size() - 1);
    }

    /// The accessor and view descriptions, for tests that corrupt them before writing.
    Json& accessors() { return m_accessors; }

    Json& views() { return m_views; }

    std::vector<std::byte>& bytes() { return m_bytes; }

    /// How the buffer is stored when the file is written.
    enum class Buffer {
        /// In `<name>.bin` next to the .gltf file.
        File,
        /// Inside the .gltf file, as a base64 data URI.
        DataUri,
    };

    /// Writes `<name>.gltf` into `directory` and returns its path.
    std::filesystem::path write(const std::filesystem::path& directory, std::string_view name,
                                Buffer buffer = Buffer::File) const {
        Json document = finished();
        if (!m_bytes.empty()) {
            if (buffer == Buffer::File) {
                document["buffers"][0]["uri"] = fmt::format("{}.bin", name);
                REQUIRE(writeBinaryFile(directory / pathFromUtf8(fmt::format("{}.bin", name)), m_bytes).has_value());
            } else {
                document["buffers"][0]["uri"] =
                    fmt::format("data:application/octet-stream;base64,{}", encodeBase64(m_bytes));
            }
        }
        const std::filesystem::path path = directory / pathFromUtf8(fmt::format("{}.gltf", name));
        REQUIRE(writeTextFile(path, dumpJson(document)).has_value());
        return path;
    }

    /// Writes `<name>.glb` into `directory` and returns its path.
    std::filesystem::path writeBinary(const std::filesystem::path& directory, std::string_view name) const {
        std::string text = dumpJson(finished(), -1);
        while (text.size() % 4 != 0) {
            text += ' ';
        }
        std::vector<std::byte> binary = m_bytes;
        while (binary.size() % 4 != 0) {
            binary.push_back(std::byte{0});
        }

        std::vector<std::byte> file;
        const auto total = static_cast<uint32_t>(12 + 8 + text.size() + (binary.empty() ? 0 : 8 + binary.size()));
        appendBytes<uint32_t>(file, {0x46546C67u, 2u, total, static_cast<uint32_t>(text.size()), 0x4E4F534Au});
        const auto* characters = reinterpret_cast<const std::byte*>(text.data());
        file.insert(file.end(), characters, characters + text.size());
        if (!binary.empty()) {
            appendBytes<uint32_t>(file, {static_cast<uint32_t>(binary.size()), 0x004E4942u});
            file.insert(file.end(), binary.begin(), binary.end());
        }

        const std::filesystem::path path = directory / pathFromUtf8(fmt::format("{}.glb", name));
        REQUIRE(writeBinaryFile(path, file).has_value());
        return path;
    }

private:
    static size_t componentCount(std::string_view type) {
        if (type == "SCALAR") {
            return 1;
        }
        if (type == "VEC2") {
            return 2;
        }
        if (type == "VEC3") {
            return 3;
        }
        if (type == "VEC4") {
            return 4;
        }
        return 16;
    }

    [[nodiscard]] Json finished() const {
        Json document = json;
        if (!m_bytes.empty()) {
            document["buffers"][0]["byteLength"] = m_bytes.size();
        }
        if (!m_views.empty()) {
            document["bufferViews"] = m_views;
        }
        if (!m_accessors.empty()) {
            document["accessors"] = m_accessors;
        }
        return document;
    }

    std::vector<std::byte> m_bytes;
    Json m_views = Json::array();
    Json m_accessors = Json::array();
};

inline Json parseOrFail(std::string_view text) {
    return unwrap(parseJson(text));
}

} // namespace cordite::test
