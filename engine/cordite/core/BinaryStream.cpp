#include "cordite/core/BinaryStream.h"

#include "cordite/core/Assert.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace cordite {

void BinaryWriter::writeBytes(std::span<const std::byte> bytes) {
    m_data.insert(m_data.end(), bytes.begin(), bytes.end());
}

void BinaryWriter::writeString(std::string_view text) {
    CORDITE_ASSERT(text.size() <= std::numeric_limits<uint32_t>::max(), "string of {} bytes exceeds the format limit",
                   text.size());
    const size_t length = std::min<size_t>(text.size(), std::numeric_limits<uint32_t>::max());
    write(static_cast<uint32_t>(length));
    writeBytes(std::as_bytes(std::span(text.substr(0, length))));
}

std::vector<std::byte> BinaryWriter::take() {
    return std::exchange(m_data, {});
}

bool BinaryReader::readBytes(std::span<std::byte> out) {
    if (m_failed || out.size() > remaining()) {
        m_failed = true;
        std::ranges::fill(out, std::byte{0});
        return false;
    }
    if (!out.empty()) {
        std::memcpy(out.data(), m_data.data() + m_position, out.size());
        m_position += out.size();
    }
    return true;
}

std::string BinaryReader::readString() {
    const auto length = read<uint32_t>();
    if (m_failed || length > remaining()) {
        m_failed = true;
        return {};
    }
    std::string text(length, '\0');
    readBytes(std::as_writable_bytes(std::span<char>(text.data(), text.size())));
    return text;
}

std::span<const std::byte> BinaryReader::readSpan(size_t size) {
    if (m_failed || size > remaining()) {
        m_failed = true;
        return {};
    }
    const std::span<const std::byte> view = m_data.subspan(m_position, size);
    m_position += size;
    return view;
}

Status BinaryReader::status(std::string_view what) const {
    if (m_failed) {
        return makeError(ErrorCode::ParseError, "{} data is truncated or corrupt", what);
    }
    return {};
}

} // namespace cordite
