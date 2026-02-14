#include "lockout_band_policy.h"

#include <atomic>

namespace {

std::atomic<uint8_t> gLockoutSupportedBandMask{kLockoutBandMaskKxOnly};

}  // namespace

uint8_t lockoutSupportedBandMask() {
    return gLockoutSupportedBandMask.load(std::memory_order_relaxed);
}

bool lockoutKaLearningEnabled() {
    return (lockoutSupportedBandMask() & static_cast<uint8_t>(0x02)) != 0;
}

void lockoutSetKaLearningEnabled(bool enabled) {
    gLockoutSupportedBandMask.store(enabled ? kLockoutBandMaskKaKx : kLockoutBandMaskKxOnly,
                                    std::memory_order_relaxed);
}

uint8_t lockoutSanitizeBandMask(uint8_t bandMask) {
    return static_cast<uint8_t>(bandMask & lockoutSupportedBandMask());
}

bool lockoutBandSupported(uint8_t bandMask) {
    return lockoutSanitizeBandMask(bandMask) != 0;
}
