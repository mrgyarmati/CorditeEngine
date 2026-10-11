#pragma once

/// @file
/// 128-bit universally unique identifiers.
///
/// Uuids identify assets and entities independently of file paths and names, so
/// references survive renames and moves.
///
/// @code
/// const Uuid id = Uuid::generate();
/// const std::string text = id.toString();          // "3f2b8c1e-...."
/// const std::optional<Uuid> parsed = Uuid::parse(text);
/// @endcode

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace cordite {

class Uuid {
public:
    /// Constructs the nil Uuid, which means "no object".
    constexpr Uuid() noexcept = default;

    constexpr Uuid(uint64_t high, uint64_t low) noexcept : m_high(high), m_low(low) {}

    /// Returns a new random (RFC 9562 version 4) Uuid. Thread-safe.
    static Uuid generate();

    /// Derives a Uuid deterministically from a namespace and a name. The same inputs
    /// always give the same result, which keeps sub-asset identifiers stable when a
    /// source file is re-imported. The result is an RFC 9562 version 8 Uuid.
    static Uuid fromName(const Uuid& namespaceId, std::string_view name);

    /// Parses the canonical 8-4-4-4-12 hexadecimal form, in either letter case.
    static std::optional<Uuid> parse(std::string_view text);

    /// Returns the canonical lowercase 8-4-4-4-12 form.
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] constexpr bool isNil() const noexcept { return m_high == 0 && m_low == 0; }

    [[nodiscard]] constexpr uint64_t high() const noexcept { return m_high; }

    [[nodiscard]] constexpr uint64_t low() const noexcept { return m_low; }

    constexpr auto operator<=>(const Uuid&) const noexcept = default;

private:
    uint64_t m_high = 0;
    uint64_t m_low = 0;
};

} // namespace cordite

template <>
struct std::hash<cordite::Uuid> {
    size_t operator()(const cordite::Uuid& id) const noexcept {
        return static_cast<size_t>(id.high() ^ (id.low() * 0x9e3779b97f4a7c15ull));
    }
};
