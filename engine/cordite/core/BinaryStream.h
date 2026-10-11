#pragma once

/// @file
/// Little-endian binary serialization for cooked (runtime) asset formats.
///
/// BinaryReader treats its input as untrusted: every read is bounds-checked, and once a
/// read fails the reader stays in a failed state and returns zero values. Callers read
/// the whole structure and check status() once at the end.
///
/// @code
/// BinaryWriter writer;
/// writer.write(kMeshMagic);
/// writer.writeString(name);
/// writer.writeArray<Vertex>(vertices);
///
/// BinaryReader reader(bytes);
/// const uint32_t magic = reader.read<uint32_t>();
/// const std::string name = reader.readString();
/// std::vector<Vertex> vertices = reader.readArray<Vertex>();
/// CORDITE_TRY(reader.status("mesh"));
/// @endcode

#include "cordite/core/Result.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace cordite {

static_assert(std::endian::native == std::endian::little, "Cordite's binary formats assume a little-endian host.");

class BinaryWriter {
public:
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void write(const T& value) {
        writeBytes(std::as_bytes(std::span<const T, 1>(&value, 1)));
    }

    void writeBytes(std::span<const std::byte> bytes);

    /// Writes a 32-bit length followed by the bytes of the string. Strings must be
    /// shorter than 4 GiB.
    void writeString(std::string_view text);

    /// Writes a 64-bit element count followed by the raw elements.
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void writeArray(std::span<const T> elements) {
        write(static_cast<uint64_t>(elements.size()));
        writeBytes(std::as_bytes(elements));
    }

    [[nodiscard]] const std::vector<std::byte>& data() const { return m_data; }

    [[nodiscard]] size_t size() const { return m_data.size(); }

    /// Moves the written bytes out, leaving the writer empty.
    [[nodiscard]] std::vector<std::byte> take();

private:
    std::vector<std::byte> m_data;
};

class BinaryReader {
public:
    /// The reader views `data`; the caller keeps it alive while reading.
    explicit BinaryReader(std::span<const std::byte> data) : m_data(data) {}

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    T read() {
        T value{};
        readBytes(std::as_writable_bytes(std::span<T, 1>(&value, 1)));
        return value;
    }

    /// Copies exactly out.size() bytes. On failure the output is zero-filled.
    bool readBytes(std::span<std::byte> out);

    /// Reads a string written by BinaryWriter::writeString.
    std::string readString();

    /// Reads an array written by BinaryWriter::writeArray. The stored count is validated
    /// against the remaining input before any memory is allocated.
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    std::vector<T> readArray() {
        const auto count = read<uint64_t>();
        if (m_failed || count > remaining() / sizeof(T)) {
            m_failed = true;
            return {};
        }
        std::vector<T> elements(static_cast<size_t>(count));
        readBytes(std::as_writable_bytes(std::span<T>(elements)));
        return elements;
    }

    /// Returns a view of the next `size` bytes without copying, or an empty span on failure.
    std::span<const std::byte> readSpan(size_t size);

    [[nodiscard]] bool failed() const { return m_failed; }

    [[nodiscard]] size_t position() const { return m_position; }

    [[nodiscard]] size_t remaining() const { return m_data.size() - m_position; }

    /// Returns a ParseError naming `what` when any read ran past the end of the input.
    [[nodiscard]] Status status(std::string_view what) const;

private:
    std::span<const std::byte> m_data;
    size_t m_position = 0;
    bool m_failed = false;
};

} // namespace cordite
