#pragma once

/// @file
/// Platform detection and small utility macros shared by every engine module.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define CORDITE_PLATFORM_WINDOWS 1
#define CORDITE_PLATFORM_LINUX 0
#define CORDITE_PLATFORM_MACOS 0
#elif defined(__APPLE__)
#define CORDITE_PLATFORM_WINDOWS 0
#define CORDITE_PLATFORM_LINUX 0
#define CORDITE_PLATFORM_MACOS 1
#elif defined(__linux__)
#define CORDITE_PLATFORM_WINDOWS 0
#define CORDITE_PLATFORM_LINUX 1
#define CORDITE_PLATFORM_MACOS 0
#else
#error "Cordite supports Windows, Linux and macOS only."
#endif

#if defined(_MSC_VER)
#define CORDITE_DEBUG_BREAK() __debugbreak()
#else
#define CORDITE_DEBUG_BREAK() __builtin_trap()
#endif

// The macro argument is a type name used in declarations, which cannot be parenthesized.
// NOLINTBEGIN(bugprone-macro-parentheses)

/// Deletes the copy operations of a class. Place in the public section.
#define CORDITE_NON_COPYABLE(Type)                                                                                     \
    Type(const Type&) = delete;                                                                                        \
    Type& operator=(const Type&) = delete

/// Deletes the copy and move operations of a class. Place in the public section.
#define CORDITE_NON_MOVABLE(Type)                                                                                      \
    Type(const Type&) = delete;                                                                                        \
    Type& operator=(const Type&) = delete;                                                                             \
    Type(Type&&) = delete;                                                                                             \
    Type& operator=(Type&&) = delete

// NOLINTEND(bugprone-macro-parentheses)

#define CORDITE_CONCAT_IMPL(a, b) a##b
#define CORDITE_CONCAT(a, b) CORDITE_CONCAT_IMPL(a, b)
