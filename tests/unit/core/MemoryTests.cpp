#include "cordite/core/memory/Memory.h"

#include "cordite/core/memory/LinearAllocator.h"
#include "cordite/core/memory/PoolAllocator.h"

#include "support/TestSupport.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

using namespace cordite;
using cordite::test::AssertionFailure;

namespace {

bool isAligned(const void* pointer, size_t alignment) {
    return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
}

struct Tracked {
    explicit Tracked(int& liveCounter, int payload) : counter(&liveCounter), value(payload) { ++*counter; }

    ~Tracked() { --*counter; }

    int* counter;
    int value;
};

} // namespace

TEST_SUITE("Memory") {
    TEST_CASE("alignment helpers") {
        static_assert(isPowerOfTwo(1) && isPowerOfTwo(64) && !isPowerOfTwo(0) && !isPowerOfTwo(48));
        static_assert(alignUp(0, 16) == 0);
        static_assert(alignUp(1, 16) == 16);
        static_assert(alignUp(16, 16) == 16);
        static_assert(alignUp(17, 8) == 24);
        CHECK(alignUp(1000, 256) == 1024);
    }

    TEST_CASE("alignedAllocate honours large alignments") {
        for (const size_t alignment : {size_t{8}, size_t{64}, size_t{4096}}) {
            void* memory = alignedAllocate(100, alignment);
            REQUIRE(memory != nullptr);
            CHECK(isAligned(memory, alignment));
            std::memset(memory, 0xab, 100);
            alignedFree(memory);
        }
        CHECK_NOTHROW(alignedFree(nullptr));
    }
}

TEST_SUITE("LinearAllocator") {
    TEST_CASE("allocations are sequential, aligned and tracked") {
        LinearAllocator arena(1024);
        CHECK(arena.capacity() == 1024);
        CHECK(arena.used() == 0);

        void* first = arena.allocate(10, 1);
        void* second = arena.allocate(4, 64);
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        CHECK(isAligned(second, 64));
        CHECK(static_cast<std::byte*>(second) >= static_cast<std::byte*>(first) + 10);
        CHECK(arena.owns(first));
        CHECK(arena.owns(second));
        CHECK(arena.used() >= 14);
        CHECK(arena.peak() == arena.used());
    }

    TEST_CASE("an exhausted arena returns nullptr and stays usable") {
        LinearAllocator arena(64);
        CHECK(arena.allocate(65, 1) == nullptr);
        CHECK(arena.used() == 0);

        REQUIRE(arena.allocate(64, 1) != nullptr);
        CHECK(arena.allocate(1, 1) == nullptr);
        CHECK(arena.used() == 64);

        arena.reset();
        CHECK(arena.allocate(32, 1) != nullptr);
    }

    TEST_CASE("an absurd size does not overflow the bounds check") {
        LinearAllocator arena(64);
        REQUIRE(arena.allocate(8, 1) != nullptr);
        CHECK(arena.allocate(SIZE_MAX - 4, 1) == nullptr);
        CHECK(arena.allocateArray<uint64_t>(SIZE_MAX / 2).empty());
        CHECK(arena.used() == 8);
    }

    TEST_CASE("mark and rewind release later allocations only") {
        LinearAllocator arena(256);
        REQUIRE(arena.allocate(16, 1) != nullptr);
        const LinearAllocator::Marker marker = arena.mark();
        void* scratch = arena.allocate(100, 1);
        REQUIRE(scratch != nullptr);
        CHECK(arena.used() == 116);

        arena.rewind(marker);
        CHECK(arena.used() == 16);
        CHECK(arena.peak() == 116);

        // The released space is handed out again.
        CHECK(arena.allocate(100, 1) == scratch);
    }

    TEST_CASE("create and allocateArray construct objects") {
        struct Item {
            int id = 7;
            float weight = 1.5f;
        };

        LinearAllocator arena(1024);
        Item* item = arena.create<Item>();
        REQUIRE(item != nullptr);
        CHECK(item->id == 7);
        CHECK(item->weight == 1.5f);

        const std::span<uint32_t> values = arena.allocateArray<uint32_t>(16);
        REQUIRE(values.size() == 16);
        for (const uint32_t value : values) {
            CHECK(value == 0);
        }

        CHECK(arena.allocateArray<uint32_t>(0).empty());
        CHECK(arena.allocateArray<uint32_t>(100000).empty());
    }

    TEST_CASE("owns rejects foreign pointers") {
        LinearAllocator arena(64);
        int local = 0;
        CHECK_FALSE(arena.owns(&local));
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("misuse is caught by assertions") {
        LinearAllocator arena(64);
        CHECK_THROWS_AS((void)arena.allocate(8, 3), AssertionFailure);
        CHECK_THROWS_AS(arena.rewind(32), AssertionFailure);
    }
#endif
}

TEST_SUITE("PoolAllocator") {
    TEST_CASE("blocks are distinct, aligned and reused after free") {
        PoolAllocator pool(24, 16, 8);
        CHECK(pool.blockSize() >= 24);
        CHECK(pool.blockSize() % 16 == 0);
        CHECK(pool.capacity() == 0);

        std::set<void*> blocks;
        for (int index = 0; index < 8; ++index) {
            void* block = pool.allocate();
            REQUIRE(block != nullptr);
            CHECK(isAligned(block, 16));
            CHECK(pool.owns(block));
            std::memset(block, index, 24);
            CHECK(blocks.insert(block).second);
        }
        CHECK(pool.allocatedCount() == 8);
        CHECK(pool.capacity() == 8);

        void* recycled = *blocks.begin();
        pool.free(recycled);
        CHECK(pool.allocatedCount() == 7);
        CHECK(pool.allocate() == recycled);
        CHECK(pool.capacity() == 8);

        for (void* block : blocks) {
            pool.free(block);
        }
        CHECK(pool.allocatedCount() == 0);
    }

    TEST_CASE("the pool grows by whole chunks") {
        PoolAllocator pool(8, 8, 4);
        std::vector<void*> blocks;
        for (int index = 0; index < 9; ++index) {
            blocks.push_back(pool.allocate());
            REQUIRE(blocks.back() != nullptr);
        }
        CHECK(pool.capacity() == 12);

        for (void* block : blocks) {
            pool.free(block);
        }
    }

    TEST_CASE("tiny blocks are widened to hold the free-list link") {
        PoolAllocator pool(1, 1, 4);
        CHECK(pool.blockSize() >= sizeof(void*));
        void* a = pool.allocate();
        void* b = pool.allocate();
        CHECK(a != b);
        pool.free(a);
        pool.free(b);
    }

    TEST_CASE("freeing nullptr is a no-op") {
        PoolAllocator pool(16, 8, 4);
        CHECK_NOTHROW(pool.free(nullptr));
        CHECK(pool.allocatedCount() == 0);
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("freeing a foreign pointer is caught") {
        PoolAllocator pool(16, 8, 4);
        void* block = pool.allocate();
        int local = 0;
        CHECK_THROWS_AS(pool.free(&local), AssertionFailure);
        pool.free(block);
    }
#endif

    TEST_CASE("ObjectPool runs constructors and destructors") {
        int live = 0;
        ObjectPool<Tracked> pool(4);

        Tracked* first = pool.create(live, 1);
        Tracked* second = pool.create(live, 2);
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        CHECK(live == 2);
        CHECK(first->value == 1);
        CHECK(second->value == 2);
        CHECK(pool.liveCount() == 2);

        pool.destroy(first);
        CHECK(live == 1);
        pool.destroy(second);
        pool.destroy(nullptr);
        CHECK(live == 0);
        CHECK(pool.liveCount() == 0);
    }
}
