#include "cordite/core/memory/LinearAllocator.h"

#include "cordite/core/Assert.h"
#include "cordite/core/memory/Memory.h"

#include <algorithm>
#include <cstdint>

namespace cordite {

LinearAllocator::LinearAllocator(size_t capacity) : m_capacity(capacity) {
    m_buffer = static_cast<std::byte*>(alignedAllocate(capacity, alignof(std::max_align_t)));
    CORDITE_VERIFY(m_buffer != nullptr, "out of memory allocating a {} byte arena", capacity);
    if (m_buffer == nullptr) {
        m_capacity = 0;
    }
}

LinearAllocator::~LinearAllocator() {
    alignedFree(m_buffer);
}

void* LinearAllocator::allocate(size_t size, size_t alignment) {
    CORDITE_ASSERT(isPowerOfTwo(alignment), "alignment {} is not a power of two", alignment);

    // Align the absolute address rather than the offset, so that alignments stricter
    // than the alignment of the buffer itself are honoured.
    const auto base = reinterpret_cast<uintptr_t>(m_buffer);
    const size_t alignedOffset = alignUp(base + m_offset, alignment) - base;
    if (alignedOffset > m_capacity || size > m_capacity - alignedOffset) {
        return nullptr;
    }

    m_offset = alignedOffset + size;
    m_peak = std::max(m_peak, m_offset);
    return m_buffer + alignedOffset;
}

void LinearAllocator::rewind(Marker marker) {
    CORDITE_ASSERT(marker <= m_offset, "marker {} is ahead of the current offset {}", marker, m_offset);
    m_offset = std::min(marker, m_offset);
}

bool LinearAllocator::owns(const void* pointer) const {
    const auto* address = static_cast<const std::byte*>(pointer);
    return address >= m_buffer && address < m_buffer + m_capacity;
}

} // namespace cordite
