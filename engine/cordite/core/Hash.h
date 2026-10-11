#pragma once

/// @file
/// Hashing.
///
/// - fnv1a64 is constexpr and meant for compile-time string identifiers.
/// - hashBytes / hashString are fast 64-bit hashes for in-memory lookup tables.
/// - ContentHasher produces 128-bit digests that are stable across runs, platforms and
///   engine versions. Asset and shader cache keys are built from it.
///
/// @code
/// ContentHasher hasher;
/// hasher.update(shaderSource).update(defines).updateValue(compilerVersion);
/// const std::string cacheKey = hasher.finish().toHex();
/// @endcode

#include "cordite/core/Base.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace cordite {

/// FNV-1a over the bytes of a string, usable in constant expressions.
constexpr uint64_t fnv1a64(std::string_view text) noexcept {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char character : text) {
        hash ^= static_cast<uint8_t>(character);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// Mixes a value into a running hash. Order-dependent.
constexpr uint64_t hashCombine(uint64_t seed, uint64_t value) noexcept {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 12) + (seed >> 4));
}

uint64_t hashBytes(std::span<const std::byte> data, uint64_t seed = 0);
uint64_t hashString(std::string_view text, uint64_t seed = 0);

/// A 128-bit content digest.
struct Hash128 {
    uint64_t high = 0;
    uint64_t low = 0;

    auto operator<=>(const Hash128&) const = default;

    /// Returns 32 lowercase hexadecimal characters.
    [[nodiscard]] std::string toHex() const;

    /// Parses the output of toHex(). Returns nullopt for anything else.
    static std::optional<Hash128> fromHex(std::string_view text);
};

Hash128 hashBytes128(std::span<const std::byte> data);

/// Incremental 128-bit hasher for keys assembled from several parts.
class ContentHasher {
public:
    ContentHasher();
    ~ContentHasher();
    CORDITE_NON_MOVABLE(ContentHasher);

    /// Feeds raw bytes.
    ContentHasher& update(std::span<const std::byte> data);

    /// Feeds a string together with its length, so that ("ab", "c") and ("a", "bc")
    /// produce different digests.
    ContentHasher& update(std::string_view text);

    /// Feeds the object representation of a trivially copyable value. Do not pass types
    /// with padding bytes: their contents are unspecified.
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    ContentHasher& updateValue(const T& value) {
        return update(std::as_bytes(std::span<const T, 1>(&value, 1)));
    }

    /// Returns the digest of everything fed so far. The hasher can keep accepting data.
    [[nodiscard]] Hash128 finish() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace cordite
