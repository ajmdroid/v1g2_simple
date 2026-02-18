#pragma once

#include <cstdint>

// Shared heap-capability mock state used by native tests.
// Defaults represent a healthy heap unless tests override them.
inline uint32_t g_mock_heap_caps_free_size = 320000u;
inline uint32_t g_mock_heap_caps_largest_block = 8u * 1024u * 1024u;

inline void mock_set_heap_caps(uint32_t free_size, uint32_t largest_block) {
    g_mock_heap_caps_free_size = free_size;
    g_mock_heap_caps_largest_block = largest_block;
}

inline void mock_reset_heap_caps() {
    g_mock_heap_caps_free_size = 320000u;
    g_mock_heap_caps_largest_block = 8u * 1024u * 1024u;
}
