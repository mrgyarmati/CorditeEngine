#pragma once

/// @file
/// Assertions.
///
/// - CORDITE_ASSERT checks an invariant in Debug and RelWithDebInfo builds and compiles to
///   nothing in Release. Use it for programmer errors.
/// - CORDITE_VERIFY is checked in every configuration. Use it where continuing would
///   corrupt state even in a shipping build.
///
/// Neither is a substitute for handling expected failures (bad files, missing assets,
/// invalid user input): those return a cordite::Result.
///
/// @code
/// CORDITE_ASSERT(index < m_items.size(), "index {} out of range ({} items)", index, m_items.size());
/// @endcode

#include "cordite/core/Base.h"

#include <fmt/format.h>

#include <string>
#include <string_view>
#include <utility>

namespace cordite {

/// Describes a failed assertion. The views are only valid during the handler call.
struct AssertionInfo {
    std::string_view expression;
    std::string_view file;
    int line = 0;
    std::string_view message;
};

/// Called when an assertion fails. If the handler returns, execution continues after the
/// failed assertion; the default handler logs the failure and aborts the process.
using AssertionHandler = void (*)(const AssertionInfo& info);

/// Replaces the assertion handler and returns the previous one. Passing nullptr restores
/// the default handler. Tests install a throwing handler to exercise failure paths.
AssertionHandler setAssertionHandler(AssertionHandler handler);

namespace detail {

void reportAssertionFailure(std::string_view expression, std::string_view file, int line, std::string_view message);

inline std::string formatAssertionMessage() {
    return {};
}

template <typename... Args>
std::string formatAssertionMessage(fmt::format_string<Args...> pattern, Args&&... args) {
    return fmt::format(pattern, std::forward<Args>(args)...);
}

/// Only ever named inside sizeof by a compiled-out CORDITE_ASSERT; never called.
template <typename... Args>
constexpr int ignoreAssertion(Args&&... /*unused*/) noexcept {
    return 0;
}

} // namespace detail
} // namespace cordite

// Both macros expand to a void expression, so they can be used anywhere an expression
// can, including inside other macros and comma expressions.
#define CORDITE_VERIFY(condition, ...)                                                                                 \
    (static_cast<bool>(condition)                                                                                      \
         ? static_cast<void>(0)                                                                                        \
         : ::cordite::detail::reportAssertionFailure(#condition, __FILE__, __LINE__,                                   \
                                                     ::cordite::detail::formatAssertionMessage(__VA_ARGS__)))

#if defined(CORDITE_ENABLE_ASSERTS)
#define CORDITE_ASSERT(condition, ...) CORDITE_VERIFY(condition, __VA_ARGS__)
#else
// sizeof keeps the condition and the message arguments "used" without evaluating them,
// so variables that only feed assertions do not trigger unused warnings in Release.
#define CORDITE_ASSERT(condition, ...)                                                                                 \
    static_cast<void>(sizeof(::cordite::detail::ignoreAssertion(!(condition)__VA_OPT__(, ) __VA_ARGS__)))
#endif
