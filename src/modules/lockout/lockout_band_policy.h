#pragma once

#include <stdint.h>

// Band enum bits are defined in packet_parser.h:
// Laser=0x01, Ka=0x02, K=0x04, X=0x08.
static constexpr uint8_t kLockoutBandMaskKxOnly = static_cast<uint8_t>(0x04 | 0x08);
static constexpr uint8_t kLockoutBandMaskKaKx = static_cast<uint8_t>(0x02 | 0x04 | 0x08);

// Runtime policy gate (default: K/X only). Ka is opt-in and safety-gated in UI/API.
uint8_t lockoutSupportedBandMask();
bool lockoutKaLearningEnabled();
bool lockoutKLearningEnabled();
bool lockoutXLearningEnabled();
void lockoutSetKaLearningEnabled(bool enabled);
void lockoutSetKLearningEnabled(bool enabled);
void lockoutSetXLearningEnabled(bool enabled);

uint8_t lockoutSanitizeBandMask(uint8_t bandMask);
bool lockoutBandSupported(uint8_t bandMask);
