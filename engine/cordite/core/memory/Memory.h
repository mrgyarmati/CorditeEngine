#pragma once

/// @file
/// Low-level memory helpers shared by the allocators.

#include <cstddef>

namespace cordite {

inline constexpr size_t kKibibyte = 1024;
inline constexpr size_t kMebibyte = 1024 * kKibibyte;

constexpr bool isPowerOfTwo(size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

/// Rounds `value` up to a multiple of `alignment`, which must be a power of two.
constexpr size_t alignUp(size_t value, size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

/// Allocates `size` bytes aligned to `alignment` (a power of two). Returns nullptr when
/// the system is out of memory. Release with alignedFree.
[[nodiscard]] void* alignedAllocate(size_t size, size_t alignment);

/// Releases memory from alignedAllocate. Accepts nullptr.
void alignedFree(void* pointer) noexcept;

} // namespace cordite
