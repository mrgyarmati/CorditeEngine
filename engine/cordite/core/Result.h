#pragma once

/// @file
/// Error handling for expected failures.
///
/// Functions that can fail for reasons outside the programmer's control (I/O, malformed
/// data, missing assets, device loss) return Result<T> or Status instead of throwing.
///
/// @code
/// Result<Texture> loadTexture(const std::filesystem::path& path) {
///     CORDITE_TRY_ASSIGN(auto bytes, readBinaryFile(path));
///     if (bytes.empty()) {
///         return makeError(ErrorCode::ParseError, "{} is empty", pathToUtf8(path));
///     }
///     return decodeTexture(bytes);
/// }
/// @endcode

#include "cordite/core/Base.h"

#include <fmt/format.h>
#include <tl/expected.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace cordite {

enum class ErrorCode : uint8_t {
    Unknown,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    IoError,
    ParseError,
    Unsupported,
    OutOfRange,
    InvalidState,
    VersionMismatch,
    DeviceError,
    ScriptError,
};

/// Returns the identifier of an error code, e.g. "NotFound".
std::string_view nameOf(ErrorCode code);

/// An error code plus a human-readable message that says what failed and why.
struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;

    /// Returns "<Code>: <message>".
    [[nodiscard]] std::string describe() const;

    /// Returns a copy whose message is prefixed with "<context>: ", for adding the
    /// operation that was in progress as an error travels up the call stack.
    [[nodiscard]] Error withContext(std::string_view context) const;
};

/// A value of type T, or the Error explaining why it could not be produced.
template <typename T>
using Result = tl::expected<T, Error>;

/// Result of an operation that produces no value.
using Status = Result<void>;

/// Builds the error alternative of any Result from a format string.
template <typename... Args>
[[nodiscard]] tl::unexpected<Error> makeError(ErrorCode code, fmt::format_string<Args...> pattern, Args&&... args) {
    return tl::unexpected<Error>(Error{code, fmt::format(pattern, std::forward<Args>(args)...)});
}

} // namespace cordite

/// Evaluates an expression returning a Result and returns its error from the enclosing
/// function when it failed.
#define CORDITE_TRY(expression)                                                                                        \
    do {                                                                                                               \
        auto&& corditeTryResult = (expression);                                                                        \
        if (!corditeTryResult) {                                                                                       \
            return ::tl::unexpected<::cordite::Error>(std::move(corditeTryResult).error());                            \
        }                                                                                                              \
    } while (false)

/// Like CORDITE_TRY, but binds the value on success:
/// `CORDITE_TRY_ASSIGN(auto text, readTextFile(path));`
// `declaration` introduces a variable, so it cannot be parenthesized.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define CORDITE_TRY_ASSIGN(declaration, expression)                                                                    \
    auto CORDITE_CONCAT(corditeTryResult, __LINE__) = (expression);                                                    \
    if (!CORDITE_CONCAT(corditeTryResult, __LINE__)) {                                                                 \
        return ::tl::unexpected<::cordite::Error>(std::move(CORDITE_CONCAT(corditeTryResult, __LINE__)).error());      \
    }                                                                                                                  \
    declaration = std::move(*CORDITE_CONCAT(corditeTryResult, __LINE__))
// NOLINTEND(bugprone-macro-parentheses)
