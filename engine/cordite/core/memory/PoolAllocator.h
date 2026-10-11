#pragma once

/// @file
/// Fixed-size block allocator.
///
/// All blocks have the same size, so allocating and freeing are O(1) and never fragment.
/// The pool grows by whole chunks and returns memory to the system only when destroyed.
/// Use it for many small objects of one type that are created and destroyed often.
///
/// @code
/// ObjectPool<Particle> particles;
/// Particle* particle = particles.create(position, velocity);
/// ...
/// particles.destroy(particle);
/// @endcode
///
/// Not thread-safe.

#include "cordite/core/Base.h"

#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace cordite {

class PoolAllocator {
public:
    /// `blockSize` and `blockAlignment` describe one block; `blocksPerChunk` is how many
    /// blocks are obtained from the system at a time.
    PoolAllocator(size_t blockSize, size_t blockAlignment, size_t blocksPerChunk = 256);
    ~PoolAllocator();
    CORDITE_NON_MOVABLE(PoolAllocator);

    /// Returns an uninitialized block, or nullptr when the system is out of memory.
    [[nodiscard]] void* allocate();

    /// Returns a block to the pool. Accepts nullptr. The block must come from this pool
    /// and must not be freed twice.
    void free(void* block);

    [[nodiscard]] size_t blockSize() const { return m_blockSize; }

    /// Number of blocks currently handed out.
    [[nodiscard]] size_t allocatedCount() const { return m_allocatedCount; }

    /// Number of blocks the pool can hand out without growing.
    [[nodiscard]] size_t capacity() const { return m_chunks.size() * m_blocksPerChunk; }

    /// Returns true when the pointer lies inside memory owned by this pool.
    [[nodiscard]] bool owns(const void* pointer) const;

private:
    struct FreeNode {
        FreeNode* next;
    };

    bool grow();

    size_t m_blockSize;
    size_t m_blockAlignment;
    size_t m_blocksPerChunk;
    size_t m_allocatedCount = 0;
    FreeNode* m_freeList = nullptr;
    std::vector<std::byte*> m_chunks;
};

/// Typed wrapper over PoolAllocator that runs constructors and destructors.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t objectsPerChunk = 256) : m_allocator(sizeof(T), alignof(T), objectsPerChunk) {}

    /// Constructs a T. Returns nullptr when the system is out of memory.
    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        void* memory = m_allocator.allocate();
        return memory != nullptr ? new (memory) T(std::forward<Args>(args)...) : nullptr;
    }

    /// Destroys an object created by this pool. Accepts nullptr.
    void destroy(T* object) {
        if (object != nullptr) {
            object->~T();
            m_allocator.free(object);
        }
    }

    [[nodiscard]] size_t liveCount() const { return m_allocator.allocatedCount(); }

    [[nodiscard]] size_t capacity() const { return m_allocator.capacity(); }

private:
    PoolAllocator m_allocator;
};

} // namespace cordite
