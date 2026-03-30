/**
 * Shared input-sanitization helpers used by both settings.cpp and wifi_manager.cpp.
 *
 * All functions are inline so including this header adds no linkage overhead.
 * Keep only truly-shared logic here; file-specific sanitizers stay in their
 * respective .cpp files.
 */

#pragma once
#ifndef SETTINGS_SANITIZE_H
#define SETTINGS_SANITIZE_H

#include <Arduino.h>
#include <algorithm>
#include "settings.h"   // V1Mode enum

// ── Length limits ────────────────────────────────────────────────────────────

inline constexpr size_t MAX_WIFI_SSID_LEN = 32;
inline constexpr size_t MAX_AP_PASSWORD_LEN = 63;
inline constexpr size_t MIN_AP_PASSWORD_LEN = 8;
inline constexpr size_t MAX_PROXY_NAME_LEN = 32;
inline constexpr size_t MAX_SLOT_NAME_LEN = 20;
inline constexpr size_t MAX_PROFILE_NAME_LEN = 64;
inline constexpr size_t MAX_PROFILE_DESCRIPTION_LEN = 160;

// ── Numeric clamps ──────────────────────────────────────────────────────────

inline uint8_t clampU8(int value, int minVal, int maxVal) {
    return static_cast<uint8_t>(std::max(minVal, std::min(value, maxVal)));
}

inline uint8_t clampSlotVolumeValue(int value) {
    // 0xFF means "no change"; otherwise valid range is 0..9.
    if (value == 0xFF) {
        return 0xFF;
    }
    return clampU8(value, 0, 9);
}

inline uint8_t clampApTimeoutValue(int value) {
    // 0 means always-on, otherwise enforce 5..60 minutes.
    if (value == 0) {
        return 0;
    }
    return clampU8(value, 5, 60);
}

inline V1Mode normalizeV1ModeValue(int raw) {
    switch (raw) {
        case V1_MODE_UNKNOWN:
        case V1_MODE_ALL_BOGEYS:
        case V1_MODE_LOGIC:
        case V1_MODE_ADVANCED_LOGIC:
            return static_cast<V1Mode>(raw);
        default:
            return V1_MODE_UNKNOWN;
    }
}

// ── String sanitizers ───────────────────────────────────────────────────────

inline String clampStringLength(const String& value, size_t maxLen) {
    if (value.length() <= maxLen) {
        return value;
    }
    return value.substring(0, maxLen);
}

inline String sanitizeApSsidValue(const String& raw) {
    String value = clampStringLength(raw, MAX_WIFI_SSID_LEN);
    if (value.length() == 0) {
        return "V1-Simple";
    }
    return value;
}

inline String sanitizeWifiClientSsidValue(const String& raw) {
    return clampStringLength(raw, MAX_WIFI_SSID_LEN);
}

inline String sanitizeProxyNameValue(const String& raw) {
    String value = clampStringLength(raw, MAX_PROXY_NAME_LEN);
    if (value.length() == 0) {
        return "V1-Proxy";
    }
    return value;
}

inline String sanitizeSlotNameValue(const String& raw) {
    String value = clampStringLength(raw, MAX_SLOT_NAME_LEN);
    value.toUpperCase();
    return value;
}

inline String sanitizeProfileNameValue(const String& raw) {
    return clampStringLength(raw, MAX_PROFILE_NAME_LEN);
}

inline String sanitizeProfileDescriptionValue(const String& raw) {
    return clampStringLength(raw, MAX_PROFILE_DESCRIPTION_LEN);
}

#endif // SETTINGS_SANITIZE_H
