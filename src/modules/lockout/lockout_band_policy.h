#pragma once

#include <stdint.h>

// Lockout policy: only K and X bands are eligible for lockout learning/enforcement.
// Band enum bits are defined in packet_parser.h (Laser=0x01, Ka=0x02, K=0x04, X=0x08).
static constexpr uint8_t kLockoutSupportedBandMask = static_cast<uint8_t>(0x04 | 0x08);

inline uint8_t lockoutSanitizeBandMask(uint8_t bandMask) {
    return static_cast<uint8_t>(bandMask & kLockoutSupportedBandMask);
}

inline bool lockoutBandSupported(uint8_t bandMask) {
    return lockoutSanitizeBandMask(bandMask) != 0;
}
