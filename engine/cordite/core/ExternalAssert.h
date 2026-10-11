#pragma once

/// @file
/// Assertions for third-party libraries that let the application supply their assert
/// macro.
///
/// Such a library compiles the macro into its own sources, so the header that provides
/// it must not pull the engine into the library's build. This one includes nothing. Its
/// failures go to the same handler as CORDITE_ASSERT (see cordite/core/Assert.h), which
/// means they are logged, and thrown as test failures under the test harness, instead of
/// popping up a C runtime dialog.
///
/// @code
/// // In the configuration header of a library:
/// #include "cordite/core/ExternalAssert.h"
/// #define IM_ASSERT(expression) CORDITE_EXTERNAL_ASSERT(expression)
/// @endcode
///
/// Engine code uses CORDITE_ASSERT, which also takes a formatted message.

namespace cordite::detail {

void reportExternalAssertionFailure(const char* expression, const char* file, int line);

} // namespace cordite::detail

#if defined(CORDITE_ENABLE_ASSERTS)
#define CORDITE_EXTERNAL_ASSERT(expression)                                                                            \
    (static_cast<bool>(expression)                                                                                     \
         ? static_cast<void>(0)                                                                                        \
         : ::cordite::detail::reportExternalAssertionFailure(#expression, __FILE__, __LINE__))
#else
// sizeof keeps the expression "used" without evaluating it.
#define CORDITE_EXTERNAL_ASSERT(expression) static_cast<void>(sizeof(!(expression)))
#endif
