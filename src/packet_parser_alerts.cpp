/**
 * Alert table parsing and priority-selection path for PacketParser.
 */

#include "packet_parser.h"
#include "../include/band_utils.h"
#include <algorithm>
#include "debug_logger.h"

#ifndef UNIT_TEST
#include "perf_metrics.h"
#define PARSER_PERF_INC(counter) PERF_INC(counter)
#define PARSER_TRACE_ENABLED() (perfDebugEnabled)
#else
#define PARSER_PERF_INC(counter) do { } while (0)
#define PARSER_TRACE_ENABLED() (false)
#endif

namespace {
uint16_t combineMSBLSB(uint8_t msb, uint8_t lsb) {
    return (static_cast<uint16_t>(msb) << 8) | lsb;
}

// Freshness guards prevent old partial rows from being reused as "complete" data.
static constexpr uint32_t kAlertRowFreshnessMs = 1500;
static constexpr uint32_t kAlertAssemblyTimeoutMs = 1800;

const char* bandToString(Band band) {
    return bandName(band);
}

const char* directionToString(Direction dir) {
    switch (dir) {
        case DIR_FRONT: return "Front";
        case DIR_SIDE:  return "Side";
        case DIR_REAR:  return "Rear";
        default:        return "None";
    }
}

// Static flag to signal alert count tracker reset on next parseAlertData() call
// Used on V1 disconnect to ensure clean state on reconnect.
bool s_resetAlertCountFlag = false;
} // namespace

void PacketParser::resetAlertAssembly() {
    // Drop any partially collected alert rows without altering display state.
    clearAlertCache();
}

void PacketParser::clearAlertCache() {
    alertChunkPresent.fill(false);
    alertChunkCountTag.fill(0);
    alertChunkRxMs.fill(0);
    alertTableFirstSeenMs.fill(0);
}

void PacketParser::clearAlertCacheForCount(uint8_t count) {
    if (count == 0 || count > MAX_ALERTS) {
        return;
    }
    for (size_t i = 0; i < RAW_ALERT_INDEX_SLOTS; ++i) {
        if (alertChunkPresent[i] && alertChunkCountTag[i] == count) {
            alertChunkPresent[i] = false;
            alertChunkCountTag[i] = 0;
            alertChunkRxMs[i] = 0;
        }
    }
    alertTableFirstSeenMs[count] = 0;
}

void PacketParser::resetAlertCountTracker() {
    s_resetAlertCountFlag = true;
}

Band PacketParser::decodeBand(uint8_t bandArrow) const {
    if (bandArrow & 0b00000001) return BAND_LASER;
    if (bandArrow & 0b00000010) return BAND_KA;
    if (bandArrow & 0b00000100) return BAND_K;
    if (bandArrow & 0b00001000) return BAND_X;
    return BAND_NONE;
}

Direction PacketParser::decodeDirection(uint8_t bandArrow) const {
    if (bandArrow & 0b00100000) return DIR_FRONT;
    if (bandArrow & 0b01000000) return DIR_SIDE;
    if (bandArrow & 0b10000000) return DIR_REAR;
    return DIR_NONE;
}

uint8_t PacketParser::mapStrengthToBars(Band band, uint8_t raw) const {
    // Convert raw RSSI to 0-6 bars for individual alert strength tracking.
    // Main display signal bars come from V1's LED bitmap in parseDisplayData().
    static constexpr uint8_t kaThresholds[] = {0x7F, 0x88, 0x92, 0x9C, 0xA6, 0xB0, 0xFF};
    static constexpr uint8_t kThresholds[]  = {0x7F, 0x86, 0x90, 0x9A, 0xA4, 0xAE, 0xFF};
    static constexpr uint8_t xThresholds[]  = {0x7F, 0x8A, 0x98, 0xA6, 0xB4, 0xC2, 0xFF};

    const uint8_t* table = nullptr;
    switch (band) {
        case BAND_KA: table = kaThresholds; break;
        case BAND_K:  table = kThresholds;  break;
        case BAND_X:  table = xThresholds;  break;
        case BAND_LASER: {
            uint8_t bars = (raw > 0x10) ? 6 : 0; // Treat tiny noise as zero.
            return bars;
        }
        default:
            return 0;
    }

    uint8_t bars = 0;
    for (uint8_t i = 0; i < 7; ++i) {
        if (raw <= table[i]) {
            bars = i;
            break;
        }
    }
    if (bars == 0 && raw > table[0]) {
        bars = 6;
    }

    return bars;
}

bool PacketParser::parseAlertData(const uint8_t* payload, size_t length) {
    if (!payload || length < 1) {
        return false;
    }

    // Byte0: high nibble = alert index, low nibble = alert count.
    // VR-style streams can be seen as one-based (1..count) or zero-based
    // (0..count-1). Keep rows by raw index and publish when either complete set
    // is present to avoid mode-locking on partial tables.
    uint8_t alertIndex = (payload[0] >> 4) & 0x0F;
    uint8_t receivedAlertCount = payload[0] & 0x0F;
    if (receivedAlertCount == 0) {
        alertCount = 0;
        clearAlertCache();
        // DON'T clear signalBars here - parseDisplayData reads V1's LED bitmap.
        displayState.arrows = DIR_NONE;
        displayState.muted = false;
        displayState.hasJunkAlert = false;
        displayState.hasPhotoAlert = false;
        return true;
    }

    // Each alert row is 7-8 bytes in V1G2 captures; require at least 7.
    if (length < 7) {
        return false;
    }

    // Check if reset was requested (e.g., on V1 disconnect).
    if (s_resetAlertCountFlag) {
        clearAlertCache();
        s_resetAlertCountFlag = false;
    }

    if (receivedAlertCount > MAX_ALERTS) {
        // Invalid table size - drop and wait for next valid row.
        return false;
    }

    const uint32_t nowMs = millis();

    // Drop stale cached rows so a missing row from a prior cycle cannot be
    // reused to fake a "complete" table later.
    for (size_t i = 0; i < RAW_ALERT_INDEX_SLOTS; ++i) {
        if (!alertChunkPresent[i]) {
            continue;
        }
        if ((nowMs - alertChunkRxMs[i]) > kAlertRowFreshnessMs) {
            const uint8_t staleCount = alertChunkCountTag[i];
            alertChunkPresent[i] = false;
            alertChunkCountTag[i] = 0;
            alertChunkRxMs[i] = 0;
            if (staleCount > 0 && staleCount <= MAX_ALERTS) {
                bool anyRowsForCount = false;
                for (size_t j = 0; j < RAW_ALERT_INDEX_SLOTS; ++j) {
                    if (alertChunkPresent[j] && alertChunkCountTag[j] == staleCount) {
                        anyRowsForCount = true;
                        break;
                    }
                }
                if (!anyRowsForCount) {
                    alertTableFirstSeenMs[staleCount] = 0;
                }
            }
        }
    }

    const bool indexValidOneBased = (alertIndex >= 1 && alertIndex <= receivedAlertCount);
    const bool indexValidZeroBased = (alertIndex < receivedAlertCount);
    if (!indexValidOneBased && !indexValidZeroBased) {
        if (PARSER_TRACE_ENABLED()) {
            Serial.printf("[AlertAsm] drop idx=%u cnt=%u (invalid raw index)\n",
                          static_cast<unsigned>(alertIndex),
                          static_cast<unsigned>(receivedAlertCount));
        }
        return true;
    }
    const size_t rawSlot = static_cast<size_t>(alertIndex);

    bool hadRowsForCount = false;
    for (size_t i = 0; i < RAW_ALERT_INDEX_SLOTS; ++i) {
        if (alertChunkPresent[i] && alertChunkCountTag[i] == receivedAlertCount) {
            hadRowsForCount = true;
            break;
        }
    }

    const bool replacingRow =
        alertChunkPresent[rawSlot] && (alertChunkCountTag[rawSlot] == receivedAlertCount);
    if (replacingRow) {
        PARSER_PERF_INC(alertTableRowReplacements);
    }
    alertChunkPresent[rawSlot] = true;
    alertChunkCountTag[rawSlot] = receivedAlertCount;
    alertChunkRxMs[rawSlot] = nowMs;
    if (!hadRowsForCount || alertTableFirstSeenMs[receivedAlertCount] == 0) {
        alertTableFirstSeenMs[receivedAlertCount] = nowMs;
    }
    std::array<uint8_t, 8>& chunk = alertChunks[rawSlot];
    chunk.fill(0);
    size_t copyLen = std::min<size_t>(8, length);
    for (size_t i = 0; i < copyLen; ++i) {
        chunk[i] = payload[i];
    }

    auto countRowsForMode = [&](bool zeroBased) -> size_t {
        size_t rows = 0;
        for (uint8_t logicalIdx = 0; logicalIdx < receivedAlertCount; ++logicalIdx) {
            const size_t expectedRawIndex = zeroBased
                ? static_cast<size_t>(logicalIdx)
                : static_cast<size_t>(logicalIdx + 1);
            if (expectedRawIndex >= RAW_ALERT_INDEX_SLOTS) {
                continue;
            }
            if (alertChunkPresent[expectedRawIndex] &&
                alertChunkCountTag[expectedRawIndex] == receivedAlertCount &&
                (nowMs - alertChunkRxMs[expectedRawIndex]) <= kAlertRowFreshnessMs) {
                ++rows;
            }
        }
        return rows;
    };

    const size_t rowsZeroBased = countRowsForMode(true);
    const size_t rowsOneBased = countRowsForMode(false);
    const bool completeZeroBased = (rowsZeroBased == receivedAlertCount);
    const bool completeOneBased = (rowsOneBased == receivedAlertCount);
    const size_t rowsForCount = std::max(rowsZeroBased, rowsOneBased);

    if (PARSER_TRACE_ENABLED()) {
        Serial.printf(
            "[AlertRow] idx=%u cnt=%u rawSlot=%u rows0=%u rows1=%u repl=%u raw0=0x%02X f=%u bandArrow=0x%02X aux0=0x%02X\n",
            static_cast<unsigned>(alertIndex),
            static_cast<unsigned>(receivedAlertCount),
            static_cast<unsigned>(rawSlot),
            static_cast<unsigned>(rowsZeroBased),
            static_cast<unsigned>(rowsOneBased),
            replacingRow ? 1u : 0u,
            static_cast<unsigned>(payload[0]),
            static_cast<unsigned>(combineMSBLSB(payload[1], payload[2])),
            static_cast<unsigned>(payload[5]),
            static_cast<unsigned>(payload[6]));
    }

    // Rolling raw-index cache: only publish when at least one full scheme is ready.
    if (!completeZeroBased && !completeOneBased) {
        if ((alertTableFirstSeenMs[receivedAlertCount] != 0) &&
            ((nowMs - alertTableFirstSeenMs[receivedAlertCount]) > kAlertAssemblyTimeoutMs)) {
            PARSER_PERF_INC(alertTableAssemblyTimeouts);
            if (PARSER_TRACE_ENABLED()) {
                Serial.printf("[AlertAsm] timeout cnt=%u rows=%u/%u ageMs=%lu\n",
                              static_cast<unsigned>(receivedAlertCount),
                              static_cast<unsigned>(rowsForCount),
                              static_cast<unsigned>(receivedAlertCount),
                              static_cast<unsigned long>(
                                  nowMs - alertTableFirstSeenMs[receivedAlertCount]));
            }
            clearAlertCacheForCount(receivedAlertCount);
            return true;
        }
        if (PARSER_TRACE_ENABLED()) {
            Serial.printf("[AlertAsm] partial rows=%u/%u (rows0=%u rows1=%u)\n",
                          static_cast<unsigned>(rowsForCount),
                          static_cast<unsigned>(receivedAlertCount),
                          static_cast<unsigned>(rowsZeroBased),
                          static_cast<unsigned>(rowsOneBased));
        }
        return true;
    }

    enum class AlertIndexMode : uint8_t {
        ZeroBased = 0,
        OneBased = 1,
    };
    AlertIndexMode decodeMode = completeZeroBased ? AlertIndexMode::ZeroBased : AlertIndexMode::OneBased;
    if (completeZeroBased && completeOneBased) {
        PARSER_PERF_INC(prioritySelectAmbiguousIndex);
    }

    if (PARSER_TRACE_ENABLED()) {
        const char* mode = (decodeMode == AlertIndexMode::ZeroBased) ? "zero" : "one";
        Serial.printf("[AlertAsm] publish mode=%s cnt=%u rows0=%u rows1=%u\n",
                      mode,
                      static_cast<unsigned>(receivedAlertCount),
                      static_cast<unsigned>(rowsZeroBased),
                      static_cast<unsigned>(rowsOneBased));
    }

    std::array<AlertData, MAX_ALERTS> nextAlerts{};
    size_t nextAlertCount = 0;
    bool anyJunk = false;
    bool anyPhoto = false;

    // We have enough rows; validate all required row slots for this count and
    // decode into a new table buffer before publishing.
    for (size_t i = 0; i < receivedAlertCount && i < MAX_ALERTS; ++i) {
        const size_t expectedRawIndex = (decodeMode == AlertIndexMode::ZeroBased) ? i : (i + 1);
        const bool rowFresh =
            (expectedRawIndex < RAW_ALERT_INDEX_SLOTS) &&
            alertChunkPresent[expectedRawIndex] &&
            alertChunkCountTag[expectedRawIndex] == receivedAlertCount &&
            ((nowMs - alertChunkRxMs[expectedRawIndex]) <= kAlertRowFreshnessMs);
        if (!rowFresh) {
            if (PARSER_TRACE_ENABLED()) {
                Serial.printf("[AlertAsm] missing row rawIdx=%u cnt=%u rows=%u ageMs=%lu\n",
                              static_cast<unsigned>(expectedRawIndex),
                              static_cast<unsigned>(receivedAlertCount),
                              static_cast<unsigned>(rowsForCount),
                              static_cast<unsigned long>(
                                  (expectedRawIndex < RAW_ALERT_INDEX_SLOTS &&
                                   alertChunkPresent[expectedRawIndex])
                                      ? (nowMs - alertChunkRxMs[expectedRawIndex])
                                      : 0));
            }
            return true;
        }

        const auto& a = alertChunks[expectedRawIndex];
        uint8_t bandArrow = a[5];  // band + arrow bits (low-5 contains raw band encoding)
        uint8_t aux0 = a[6];       // aux0: bit7=priority, bit6=junk, low nibble=photo type
        const uint8_t rawBandBits = static_cast<uint8_t>(bandArrow & 0x1F);
        const bool isKu = (rawBandBits == 0x10);

        Band band = decodeBand(bandArrow);
        if (isKu) {
            PARSER_PERF_INC(parserRowsKuRaw);
        }
        if (band == BAND_NONE) {
            PARSER_PERF_INC(parserRowsBandNone);
        }
        Direction dir = decodeDirection(bandArrow);
        bool isPriority = (aux0 & 0x80) != 0;  // (aux0 & 128) != 0
        // Match official Android/iOS library behavior:
        // junk flag is valid on V1 4.1032+, photo type on 4.1037+.
        const bool junkSupported =
            !displayState.hasV1Version || (displayState.v1FirmwareVersion >= 41032);
        const bool photoSupported =
            !displayState.hasV1Version || (displayState.v1FirmwareVersion >= 41037);
        bool isJunk = junkSupported && ((aux0 & 0x40) != 0);
        uint8_t photoType = photoSupported ? static_cast<uint8_t>(aux0 & 0x0F) : 0;

        if (nextAlertCount < MAX_ALERTS) {
            AlertData& alert = nextAlerts[nextAlertCount++];
            alert.band = band;
            alert.direction = dir;
            alert.isPriority = isPriority;
            alert.isJunk = isJunk;
            alert.photoType = photoType;
            alert.rawBandBits = rawBandBits;
            alert.isKu = isKu;

            // Bytes 3 and 4 are raw RSSI values for front/rear antennas.
            alert.frontStrength = mapStrengthToBars(band, a[3]);
            alert.rearStrength = mapStrengthToBars(band, a[4]);

            alert.frequency = (band == BAND_LASER) ? 0 : combineMSBLSB(a[1], a[2]); // MHz
            alert.isValid = true;

            anyJunk |= isJunk;
            anyPhoto |= (photoType != 0);
        }
    }

    alerts = nextAlerts;
    alertCount = nextAlertCount;

    if (alertCount > 0) {
        // Priority resolution order:
        // 1) Per-row aux0 bit7 (matches AndroidESPLibrary2 AlertData::isPriority())
        // 2) First usable alert
        // 3) Entry 0 as last-resort safety fallback
        auto isUsableAlert = [this](int idx) -> bool {
            if (idx < 0 || idx >= static_cast<int>(alertCount)) return false;
            const AlertData& a = alerts[static_cast<size_t>(idx)];
            if (!a.isValid || a.band == BAND_NONE) return false;
            if (a.band != BAND_LASER && a.frequency == 0) return false;
            return true;
        };

        int priorityFromRowFlag = -1;
        for (size_t i = 0; i < alertCount; ++i) {
            if (alerts[i].isPriority) {
                priorityFromRowFlag = static_cast<int>(i);
                break;
            }
        }
        if (priorityFromRowFlag >= 0 && !isUsableAlert(priorityFromRowFlag)) {
            PARSER_PERF_INC(prioritySelectUnusableIndex);
        }

        enum class PrioritySource : uint8_t { RowFlag = 2, FirstUsable = 3, FirstEntry = 4 };

        int priorityIdx = -1;
        PrioritySource source = PrioritySource::FirstEntry;
        if (isUsableAlert(priorityFromRowFlag)) {
            priorityIdx = priorityFromRowFlag;
            source = PrioritySource::RowFlag;
        }
        if (priorityIdx < 0) {
            for (size_t i = 0; i < alertCount; ++i) {
                if (isUsableAlert(static_cast<int>(i))) {
                    priorityIdx = static_cast<int>(i);
                    source = PrioritySource::FirstUsable;
                    break;
                }
            }
        }
        if (priorityIdx < 0) {
            priorityIdx = 0;
            source = PrioritySource::FirstEntry;
        }

        switch (source) {
            case PrioritySource::RowFlag:
                PARSER_PERF_INC(prioritySelectRowFlag);
                break;
            case PrioritySource::FirstUsable:
                PARSER_PERF_INC(prioritySelectFirstUsable);
                break;
            case PrioritySource::FirstEntry:
            default:
                PARSER_PERF_INC(prioritySelectFirstEntry);
                break;
        }

        if (!isUsableAlert(priorityIdx)) {
            PARSER_PERF_INC(prioritySelectInvalidChosen);
        }

        if (PARSER_TRACE_ENABLED()) {
            const char* src =
                (source == PrioritySource::RowFlag) ? "rowFlag" :
                (source == PrioritySource::FirstUsable) ? "firstUsable" : "firstEntry";
            Serial.printf("[AlertPri] src=%s idx=%u cnt=%u rowFlagIdx=%d\n",
                          src,
                          static_cast<unsigned>(priorityIdx),
                          static_cast<unsigned>(alertCount),
                          priorityFromRowFlag);
        }

        displayState.v1PriorityIndex = priorityIdx;
        displayState.priorityArrow = alerts[priorityIdx].direction;
    }

    // Keep mute authoritative from display packets (InfDisplayData/Aux0 soft mute).
    displayState.hasJunkAlert = anyJunk;
    displayState.hasPhotoAlert = anyPhoto;

    PARSER_PERF_INC(alertTablePublishes);
    if (receivedAlertCount == 3) {
        PARSER_PERF_INC(alertTablePublishes3Bogey);
    }

    // Match VR AlertDataProcessor behavior: clear per-count rows after a complete
    // table publish so the next table is assembled from fresh rows.
    clearAlertCacheForCount(receivedAlertCount);
    return true;
}

AlertData PacketParser::getPriorityAlert() const {
    if (alertCount == 0) {
        return AlertData();
    }

    // displayState.v1PriorityIndex is resolved in parseAlertData():
    // alert-row aux0 bit7 first, then safety fallbacks.
    uint8_t idx = displayState.v1PriorityIndex;
    if (idx < alertCount) {
        return alerts[idx];
    }
    return alerts[0];  // Fallback
}

bool PacketParser::getRenderablePriorityAlert(AlertData& out) const {
    out = AlertData();
    if (alertCount == 0) {
        return false;
    }

    auto isRenderable = [](const AlertData& a) -> bool {
        if (!a.isValid || a.band == BAND_NONE) {
            return false;
        }
        return (a.band == BAND_LASER) || (a.frequency != 0);
    };

    const AlertData priority = getPriorityAlert();
    if (isRenderable(priority)) {
        out = priority;
        return true;
    }

    for (size_t i = 0; i < alertCount; ++i) {
        if (isRenderable(alerts[i])) {
            out = alerts[i];
            return true;
        }
    }

    return false;
}
