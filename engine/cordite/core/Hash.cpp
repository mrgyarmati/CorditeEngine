#include "cordite/core/Hash.h"

#include "cordite/core/Assert.h"
#include "cordite/core/StringUtils.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace cordite {
namespace {

/// Parses lowercase hexadecimal only: digests are compared and used as file names in
/// their canonical form, so uppercase input is treated as malformed.
std::optional<uint64_t> parseHex64(std::string_view text) {
    uint64_t value = 0;
    for (const char character : text) {
        const int digit = hexDigitValue(character);
        if (digit < 0 || (character >= 'A' && character <= 'F')) {
            return std::nullopt;
        }
        value = (value << 4) | static_cast<uint64_t>(digit);
    }
    return value;
}

} // namespace

uint64_t hashBytes(std::span<const std::byte> data, uint64_t seed) {
    return XXH3_64bits_withSeed(data.data(), data.size(), seed);
}

uint64_t hashString(std::string_view text, uint64_t seed) {
    return XXH3_64bits_withSeed(text.data(), text.size(), seed);
}

std::string Hash128::toHex() const {
    return fmt::format("{:016x}{:016x}", high, low);
}

std::optional<Hash128> Hash128::fromHex(std::string_view text) {
    if (text.size() != 32) {
        return std::nullopt;
    }
    const std::optional<uint64_t> high = parseHex64(text.substr(0, 16));
    const std::optional<uint64_t> low = parseHex64(text.substr(16, 16));
    if (!high || !low) {
        return std::nullopt;
    }
    return Hash128{*high, *low};
}

Hash128 hashBytes128(std::span<const std::byte> data) {
    const XXH128_hash_t digest = XXH3_128bits(data.data(), data.size());
    return Hash128{digest.high64, digest.low64};
}

struct ContentHasher::State {
    XXH3_state_t* xxh = nullptr;
};

ContentHasher::ContentHasher() : m_state(std::make_unique<State>()) {
    m_state->xxh = XXH3_createState();
    CORDITE_VERIFY(m_state->xxh != nullptr, "out of memory creating hash state");
    XXH3_128bits_reset(m_state->xxh);
}

ContentHasher::~ContentHasher() {
    XXH3_freeState(m_state->xxh);
}

ContentHasher& ContentHasher::update(std::span<const std::byte> data) {
    if (!data.empty()) {
        XXH3_128bits_update(m_state->xxh, data.data(), data.size());
    }
    return *this;
}

ContentHasher& ContentHasher::update(std::string_view text) {
    const uint64_t length = text.size();
    updateValue(length);
    return update(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

Hash128 ContentHasher::finish() const {
    const XXH128_hash_t digest = XXH3_128bits_digest(m_state->xxh);
    return Hash128{digest.high64, digest.low64};
}

} // namespace cordite
