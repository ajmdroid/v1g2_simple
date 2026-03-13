#pragma once

#include <cstdint>

extern "C" inline uint32_t esp_random() {
    return 0x12345678u;
}
