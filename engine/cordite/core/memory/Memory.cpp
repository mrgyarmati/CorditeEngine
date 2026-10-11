#include "cordite/core/memory/Memory.h"

#include "cordite/core/Assert.h"

#include <algorithm>
#include <cstdlib>

#if CORDITE_PLATFORM_WINDOWS
#include <malloc.h>
#endif

namespace cordite {

void* alignedAllocate(size_t size, size_t alignment) {
    CORDITE_ASSERT(isPowerOfTwo(alignment), "alignment {} is not a power of two", alignment);

#if CORDITE_PLATFORM_WINDOWS
    return _aligned_malloc(std::max<size_t>(size, 1), alignment);
#else
    // posix_memalign requires the alignment to be a multiple of sizeof(void*).
    void* pointer = nullptr;
    const size_t effectiveAlignment = std::max(alignment, sizeof(void*));
    if (posix_memalign(&pointer, effectiveAlignment, std::max<size_t>(size, 1)) != 0) {
        return nullptr;
    }
    return pointer;
#endif
}

void alignedFree(void* pointer) noexcept {
#if CORDITE_PLATFORM_WINDOWS
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

} // namespace cordite
