#pragma once

/// @file
/// Bump ("arena") allocator for short-lived data.
///
/// Allocation is a pointer increment and individual allocations are never freed: the
/// whole arena is reset at once, or rewound to an earlier marker. This suits per-frame
/// scratch data such as the render queue built from the scene every frame.
///
/// @code
/// LinearAllocator frame(4 * kMebibyte);
/// std::span<DrawItem> items = frame.allocateArray<DrawItem>(visibleCount);
/// ...
/// frame.reset();   // at the end of the frame
/// @endcode
///
/// Not thread-safe. Objects are not destroyed, so only trivially destructible types may
/// be created in the arena.

#include "cordite/core/Base.h"

#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace cordite {

class LinearAllocator {
public:
    /// Position in the arena, obtained from mark() and passed to rewind().
    using Marker = size_t;

    /// Reserves `capacity` bytes up front. The arena never grows.
    explicit LinearAllocator(size_t capacity);
    ~LinearAllocator();
    CORDITE_NON_MOVABLE(LinearAllocator);

    /// Returns `size` bytes aligned to `alignment` (a power of two), or nullptr when the
    /// arena does not have enough space left.
    [[nodiscard]] void* allocate(size_t size, size_t alignment = alignof(std::max_align_t));

    /// Constructs a T in the arena. Returns nullptr when the arena is full.
    template <typename T, typename... Args>
        requires std::is_trivially_destructible_v<T>
    [[nodiscard]] T* create(Args&&... args) {
        void* memory = allocate(sizeof(T), alignof(T));
        return memory != nullptr ? new (memory) T(std::forward<Args>(args)...) : nullptr;
    }

    /// Allocates `count` value-initialized elements. Returns an empty span when the
    /// arena is full or `count` is zero.
    template <typename T>
        requires std::is_trivially_destructible_v<T>
    [[nodiscard]] std::span<T> allocateArray(size_t count) {
        if (count == 0 || count > capacity() / sizeof(T)) {
            return {};
        }
        auto* elements = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
        if (elements == nullptr) {
            return {};
        }
        // Not placement array-new: that form may prepend a size cookie on some compilers.
        std::uninitialized_value_construct_n(elements, count);
        return {elements, count};
    }

    [[nodiscard]] Marker mark() const { return m_offset; }

    /// Releases everything allocated after `marker` was taken.
    void rewind(Marker marker);

    /// Releases every allocation.
    void reset() { m_offset = 0; }

    [[nodiscard]] size_t capacity() const { return m_capacity; }

    [[nodiscard]] size_t used() const { return m_offset; }

    /// Largest value used() has reached, for sizing the arena.
    [[nodiscard]] size_t peak() const { return m_peak; }

    /// Returns true when the pointer lies inside the arena.
    [[nodiscard]] bool owns(const void* pointer) const;

private:
    std::byte* m_buffer = nullptr;
    size_t m_capacity = 0;
    size_t m_offset = 0;
    size_t m_peak = 0;
};

} // namespace cordite
