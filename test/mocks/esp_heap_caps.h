#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM 0x01
#endif

#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL 0x02
#endif

#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT 0x04
#endif

inline void* heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return std::malloc(size);
}

inline void heap_caps_free(void* ptr) {
    std::free(ptr);
}

inline uint32_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return 8u * 1024u * 1024u;
}
