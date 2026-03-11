#pragma once

#include <stdint.h>

#include "lockout_entry.h"

namespace lockout_signature {

static constexpr uint8_t kInvalidLocalHour = 0xFF;

struct HourWindow {
    bool valid = false;
    uint8_t startHour = 0;
    uint8_t spanHours = 0;
};

inline uint8_t localHourFromEpochMs(int64_t epochMs, int32_t tzOffsetMinutes) {
    if (epochMs <= 0) {
        return kInvalidLocalHour;
    }
    const int64_t localMs = epochMs + static_cast<int64_t>(tzOffsetMinutes) * 60000LL;
    int64_t localHour = (localMs / 3600000LL) % 24LL;
    if (localHour < 0) {
        localHour += 24LL;
    }
    return static_cast<uint8_t>(localHour);
}

inline uint32_t addHourToMask(uint32_t hourMask, uint8_t localHour) {
    if (localHour >= 24) {
        return hourMask;
    }
    return hourMask | (1UL << localHour);
}

inline HourWindow deriveHourWindow(uint32_t hourMask) {
    HourWindow window;
    if ((hourMask & 0x00FFFFFFUL) == 0) {
        return window;
    }

    uint8_t seenHours[24] = {};
    uint8_t seenCount = 0;
    for (uint8_t hour = 0; hour < 24; ++hour) {
        if ((hourMask & (1UL << hour)) != 0) {
            seenHours[seenCount++] = hour;
        }
    }
    if (seenCount == 0) {
        return window;
    }

    int bestGap = -1;
    uint8_t startHour = seenHours[0];
    for (uint8_t i = 0; i < seenCount; ++i) {
        const uint8_t current = seenHours[i];
        const uint8_t next = seenHours[(i + 1) % seenCount];
        const int wrappedNext = (i + 1 < seenCount) ? next : static_cast<int>(next) + 24;
        const int gap = wrappedNext - static_cast<int>(current) - 1;
        if (gap > bestGap) {
            bestGap = gap;
            startHour = next;
        }
    }

    window.valid = true;
    window.startHour = startHour;
    window.spanHours = static_cast<uint8_t>(24 - bestGap);
    return window;
}

inline bool shouldMarkAllTime(uint32_t hourMask) {
    const HourWindow window = deriveHourWindow(hourMask);
    return window.valid && window.spanHours >= 12;
}

inline bool hourIsExpected(const LockoutEntry& entry, uint8_t localHour) {
    if (localHour >= 24) {
        return true;
    }
    if (entry.isAllTime()) {
        return true;
    }
    const HourWindow window = deriveHourWindow(entry.activeHourMask);
    if (!window.valid || window.spanHours == 0) {
        return false;
    }
    const uint8_t offset =
        static_cast<uint8_t>((localHour + 24 - window.startHour) % 24);
    return offset < window.spanHours;
}

inline void updateAdaptiveWindow(LockoutEntry& entry, uint16_t observedFreqMHz) {
    const uint16_t center = entry.freqMHz;
    const uint16_t cap = entry.freqTolMHz;
    const uint16_t lowerCap = (center > cap) ? static_cast<uint16_t>(center - cap) : 0;
    const uint16_t upperCap =
        (center > static_cast<uint16_t>(UINT16_MAX - cap)) ? UINT16_MAX
                                                           : static_cast<uint16_t>(center + cap);

    if (entry.freqWindowMinMHz == 0 || entry.freqWindowMinMHz > center) {
        entry.freqWindowMinMHz = center;
    }
    if (entry.freqWindowMaxMHz == 0 || entry.freqWindowMaxMHz < center) {
        entry.freqWindowMaxMHz = center;
    }

    uint16_t clampedFreq = observedFreqMHz;
    if (clampedFreq < lowerCap) clampedFreq = lowerCap;
    if (clampedFreq > upperCap) clampedFreq = upperCap;

    if (clampedFreq < entry.freqWindowMinMHz) {
        entry.freqWindowMinMHz = clampedFreq;
    }
    if (clampedFreq > entry.freqWindowMaxMHz) {
        entry.freqWindowMaxMHz = clampedFreq;
    }
}

inline bool freqMatches(const LockoutEntry& entry, uint16_t alertFreqMHz) {
    if (entry.freqMHz == 0) {
        return false;
    }
    const uint16_t lower =
        (entry.freqWindowMinMHz == 0) ? entry.freqMHz : entry.freqWindowMinMHz;
    const uint16_t upper =
        (entry.freqWindowMaxMHz == 0) ? entry.freqMHz : entry.freqWindowMaxMHz;
    return alertFreqMHz >= lower && alertFreqMHz <= upper;
}

inline bool signatureEquivalent(const LockoutEntry& lhs, const LockoutEntry& rhs) {
    return lhs.areaId == rhs.areaId &&
           lhs.latE5 == rhs.latE5 &&
           lhs.lonE5 == rhs.lonE5 &&
           lhs.radiusE5 == rhs.radiusE5 &&
           lhs.bandMask == rhs.bandMask &&
           lhs.freqMHz == rhs.freqMHz &&
           lhs.directionMode == rhs.directionMode &&
           lhs.headingDeg == rhs.headingDeg &&
           lhs.headingTolDeg == rhs.headingTolDeg;
}

}  // namespace lockout_signature
