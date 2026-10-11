#include "cordite/core/memory/PoolAllocator.h"

#include "cordite/core/Assert.h"
#include "cordite/core/memory/Memory.h"

#include <algorithm>

namespace cordite {

PoolAllocator::PoolAllocator(size_t blockSize, size_t blockAlignment, size_t blocksPerChunk)
    : m_blockAlignment(std::max(blockAlignment, alignof(FreeNode))),
      m_blocksPerChunk(std::max<size_t>(1, blocksPerChunk)) {
    CORDITE_ASSERT(isPowerOfTwo(blockAlignment), "alignment {} is not a power of two", blockAlignment);

    // A free block stores the free-list link in place, so it must be able to hold one.
    m_blockSize = alignUp(std::max(blockSize, sizeof(FreeNode)), m_blockAlignment);
}

PoolAllocator::~PoolAllocator() {
    CORDITE_ASSERT(m_allocatedCount == 0, "pool destroyed with {} blocks still allocated", m_allocatedCount);
    for (std::byte* chunk : m_chunks) {
        alignedFree(chunk);
    }
}

void* PoolAllocator::allocate() {
    if (m_freeList == nullptr && !grow()) {
        return nullptr;
    }

    FreeNode* block = m_freeList;
    m_freeList = block->next;
    ++m_allocatedCount;
    return block;
}

void PoolAllocator::free(void* block) {
    if (block == nullptr) {
        return;
    }
    CORDITE_ASSERT(owns(block), "block does not belong to this pool");
    CORDITE_ASSERT(m_allocatedCount > 0, "free called more often than allocate");

    auto* node = static_cast<FreeNode*>(block);
    node->next = m_freeList;
    m_freeList = node;
    --m_allocatedCount;
}

bool PoolAllocator::owns(const void* pointer) const {
    const auto* address = static_cast<const std::byte*>(pointer);
    const size_t chunkSize = m_blockSize * m_blocksPerChunk;
    return std::ranges::any_of(m_chunks,
                               [&](const std::byte* chunk) { return address >= chunk && address < chunk + chunkSize; });
}

bool PoolAllocator::grow() {
    auto* chunk = static_cast<std::byte*>(alignedAllocate(m_blockSize * m_blocksPerChunk, m_blockAlignment));
    if (chunk == nullptr) {
        return false;
    }
    m_chunks.push_back(chunk);

    // Thread the new blocks onto the free list in address order.
    for (size_t index = m_blocksPerChunk; index-- > 0;) {
        auto* node = reinterpret_cast<FreeNode*>(chunk + index * m_blockSize);
        node->next = m_freeList;
        m_freeList = node;
    }
    return true;
}

} // namespace cordite
