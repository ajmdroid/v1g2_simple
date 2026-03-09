#pragma once

#include <stdint.h>

namespace clamp_utils {

inline uint16_t clampU16Value(int value, int minVal, int maxVal) {
    if (value < minVal) return static_cast<uint16_t>(minVal);
    if (value > maxVal) return static_cast<uint16_t>(maxVal);
    return static_cast<uint16_t>(value);
}

}  // namespace clamp_utils
