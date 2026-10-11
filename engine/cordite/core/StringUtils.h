#pragma once

/// @file
/// Small string helpers. All functions treat text as ASCII-compatible UTF-8 and only
/// classify ASCII characters, so they are locale-independent.

#include <string>
#include <string_view>
#include <vector>

namespace cordite {

/// Returns the text without leading and trailing ASCII whitespace.
std::string_view trim(std::string_view text);

/// Splits on a delimiter. Empty parts are kept: split("a,,b", ',') yields {"a", "", "b"}.
/// The views point into `text`.
std::vector<std::string_view> split(std::string_view text, char delimiter);

/// Lowercases ASCII letters and leaves every other byte untouched.
std::string toLowerAscii(std::string_view text);

/// Case-insensitive comparison of ASCII letters.
bool equalsIgnoreCase(std::string_view a, std::string_view b);

/// Replaces every occurrence of `from` with `to`. `from` must not be empty.
std::string replaceAll(std::string_view text, std::string_view from, std::string_view to);

/// Returns the value (0-15) of a hexadecimal digit in either letter case, or -1 when the
/// character is not a hexadecimal digit.
constexpr int hexDigitValue(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace cordite
