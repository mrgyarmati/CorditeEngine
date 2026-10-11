#include "cordite/core/Uuid.h"

#include "cordite/core/Hash.h"
#include "cordite/core/StringUtils.h"

#include <fmt/format.h>

#include <array>
#include <random>

namespace cordite {
namespace {

constexpr uint64_t kVersionMask = 0x000000000000f000ull;
constexpr uint64_t kVariantMask = 0xc000000000000000ull;
constexpr uint64_t kVariantRfc = 0x8000000000000000ull;

Uuid withVersion(uint64_t high, uint64_t low, uint64_t version) {
    high = (high & ~kVersionMask) | (version << 12);
    low = (low & ~kVariantMask) | kVariantRfc;
    return {high, low};
}

std::mt19937_64 makeSeededGenerator() {
    std::random_device device;
    std::array<uint32_t, 8> seedData{};
    for (uint32_t& word : seedData) {
        word = device();
    }
    std::seed_seq seed(seedData.begin(), seedData.end());
    return std::mt19937_64(seed);
}

} // namespace

Uuid Uuid::generate() {
    static thread_local std::mt19937_64 generator = makeSeededGenerator();
    return withVersion(generator(), generator(), 4);
}

Uuid Uuid::fromName(const Uuid& namespaceId, std::string_view name) {
    ContentHasher hasher;
    hasher.updateValue(namespaceId.high()).updateValue(namespaceId.low()).update(name);
    const Hash128 digest = hasher.finish();
    return withVersion(digest.high, digest.low, 8);
}

std::optional<Uuid> Uuid::parse(std::string_view text) {
    constexpr size_t kLength = 36;
    if (text.size() != kLength) {
        return std::nullopt;
    }

    std::array<uint64_t, 2> parts{};
    size_t digitCount = 0;
    for (size_t index = 0; index < kLength; ++index) {
        const bool expectDash = index == 8 || index == 13 || index == 18 || index == 23;
        if (expectDash) {
            if (text[index] != '-') {
                return std::nullopt;
            }
            continue;
        }

        const int digit = hexDigitValue(text[index]);
        if (digit < 0) {
            return std::nullopt;
        }
        uint64_t& part = parts[digitCount / 16];
        part = (part << 4) | static_cast<uint64_t>(digit);
        ++digitCount;
    }
    return Uuid{parts[0], parts[1]};
}

std::string Uuid::toString() const {
    return fmt::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", m_high >> 32, (m_high >> 16) & 0xffffull,
                       m_high & 0xffffull, m_low >> 48, m_low & 0xffffffffffffull);
}

} // namespace cordite
