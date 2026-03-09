#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "mock_heap_caps_state.h"

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
    g_mock_heap_caps_malloc_calls++;
    g_mock_heap_caps_last_malloc_size = size;
    g_mock_heap_caps_last_malloc_caps = caps;
    if (g_mock_heap_caps_fail_malloc) {
        g_mock_heap_caps_fail_malloc = false;
        return nullptr;
    }
    return std::malloc(size);
}

inline void heap_caps_free(void* ptr) {
    g_mock_heap_caps_free_calls++;
    std::free(ptr);
}

inline uint32_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return g_mock_heap_caps_largest_block;
}
