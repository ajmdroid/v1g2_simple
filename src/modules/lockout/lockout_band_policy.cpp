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

bool lockoutKLearningEnabled() {
    return (lockoutSupportedBandMask() & static_cast<uint8_t>(0x04)) != 0;
}

bool lockoutXLearningEnabled() {
    return (lockoutSupportedBandMask() & static_cast<uint8_t>(0x08)) != 0;
}

void lockoutSetKaLearningEnabled(bool enabled) {
    uint8_t mask = gLockoutSupportedBandMask.load(std::memory_order_relaxed);
    if (enabled) mask |= 0x02; else mask &= ~0x02;
    gLockoutSupportedBandMask.store(mask, std::memory_order_relaxed);
}

void lockoutSetKLearningEnabled(bool enabled) {
    uint8_t mask = gLockoutSupportedBandMask.load(std::memory_order_relaxed);
    if (enabled) mask |= 0x04; else mask &= ~0x04;
    gLockoutSupportedBandMask.store(mask, std::memory_order_relaxed);
}

void lockoutSetXLearningEnabled(bool enabled) {
    uint8_t mask = gLockoutSupportedBandMask.load(std::memory_order_relaxed);
    if (enabled) mask |= 0x08; else mask &= ~0x08;
    gLockoutSupportedBandMask.store(mask, std::memory_order_relaxed);
}

uint8_t lockoutSanitizeBandMask(uint8_t bandMask) {
    return static_cast<uint8_t>(bandMask & lockoutSupportedBandMask());
}

bool lockoutBandSupported(uint8_t bandMask) {
    return lockoutSanitizeBandMask(bandMask) != 0;
}
