#include "debug_api_service.h"
#include "debug_perf_files_service.h"

#include <ArduinoJson.h>
#include "../wifi/wifi_json_document.h"
#include <cmath>
#include "json_stream_response.h"
#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

#include "../../../include/config.h"
#include "../../perf_metrics.h"
#include "../../settings.h"
#include "../../ble_client.h"
#include "../../storage_manager.h"
#include "../../perf_sd_logger.h"
#include "../display/display_pipeline_module.h"
#include "../ble/ble_queue_module.h"
#include "../gps/gps_runtime_module.h"
#include "../gps/gps_observation_log.h"
#include "../speed/speed_source_selector.h"
#include "../system/system_event_bus.h"
#include "debug_api_service_deps.inc"
namespace {

bool isTruthyArgValue(const String& value) {
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "on" || value == "ON";
}

constexpr uint16_t kScenarioCharShort = 0xB2CE;
constexpr uint16_t kScenarioCharLong = 0xB4E0;
constexpr uint8_t kScenarioDest = 0x04;
constexpr uint8_t kScenarioSrc = ESP_PACKET_ORIGIN_V1;

// Frequency values are rendered as freq/1000.0 in UI, so 24150 => 24.150 GHz.
constexpr uint16_t kFreqKBase = 24150;
constexpr uint16_t kFreqKa = 34700;
constexpr uint16_t kFreqX = 10525;
constexpr uint16_t kFreqKJunkLow = 24089;
constexpr uint16_t kFreqKJunkHigh = 24205;

// Stretch scenario timing so alerts persist long enough to resemble live traffic.
// Keeps generated scenarios in roughly 2-30s windows.
constexpr uint32_t kScenarioTimeScale = 10;

constexpr uint8_t kBandKFront = 0x24;
constexpr uint8_t kBandKSide = 0x44;
constexpr uint8_t kBandKRear = 0x84;
constexpr uint8_t kBandKaFront = 0x22;
constexpr uint8_t kBandKaSide = 0x42;
constexpr uint8_t kBandKaRear = 0x82;
constexpr uint8_t kBandXRear = 0x88;        // X + rear
constexpr uint8_t kDisplayXRearMuted = 0x98;  // Display image bit: X + rear + mute
constexpr uint8_t kBandLaserFront = 0x21;

constexpr uint8_t kBogeyOne = 6;
constexpr uint8_t kBogeyJunk = 30;
constexpr uint8_t kBogeyPhoto = 115;
constexpr uint8_t kBogeyLaser = 73;

struct ScenarioCatalogEntry {
    const char* id;
    const char* category;
    const char* description;
};

struct ScenarioPacket {
    uint32_t atMs = 0;
    std::vector<uint8_t> bytes;
    uint16_t charUuid = kScenarioCharLong;
};

struct ScenarioPlaybackState {
    bool loaded = false;
    bool running = false;
    bool loop = false;
    String scenarioId = "";
    String category = "";
    String description = "";
    std::vector<ScenarioPacket> events;
    size_t nextEventIndex = 0;
    uint32_t durationMs = 0;
    uint32_t loadedAtMs = 0;
    uint32_t startMs = 0;
    uint32_t startedAtMs = 0;
    uint32_t finishedAtMs = 0;
    uint32_t emittedPackets = 0;
    uint32_t completedRuns = 0;
    uint16_t streamRepeatMs = 700;
    uint16_t durationScalePct = 100;
};

struct AlertRowSpec {
    uint8_t index;
    uint16_t frequency;
    uint8_t frontRaw;
    uint8_t rearRaw;
    uint8_t bandArrow;
    uint8_t aux0;
};

constexpr ScenarioCatalogEntry kScenarioCatalog[] = {
    {"RAD-01", "standard-radar", "Single K ramp from weak to strong"},
    {"RAD-02", "standard-radar", "K baseline with Ka priority takeover"},
    {"RAD-03", "standard-radar", "Muted X rear alert with clear"},
    {"PHO-01", "photo", "Single priority photo alert"},
    {"PHO-02", "photo", "Photo + standard mixed alert table"},
    {"JNK-01", "junk", "Single junked radar alert"},
    {"JNK-02", "junk", "Junk K plus clean K arbitration"},
    {"LAS-01", "laser", "Immediate single laser hit"},
    {"LAS-02", "laser", "Laser preemption over active K"},
    {"MBP-01", "multi-bogey", "4 weak K then faint growing Ka priority"},
    {"MBP-02", "multi-bogey", "Immediate low-strength Ka under K load"},
    {"MBP-03", "multi-bogey", "K clutter then instant laser takeover"},
    {"ASM-01", "assembly-fault", "Missing row timeout path"},
    {"ASM-02", "assembly-fault", "Duplicate row replacement path"},
    {"ASM-03", "assembly-fault", "Out-of-order row assembly"},
    {"ASM-04", "assembly-fault", "Zero-based row index compatibility"},
    {"MET-01", "metrics", "Ambiguous zero/one-based table completion"},
    {"MET-02", "metrics", "Unusable row-priority fallback path"},
    {"TRN-01", "transport", "Burst of simultaneous packets"},
    {"TRN-02", "transport", "Split-frame reassembly"},
    {"TRN-03", "transport", "Burst + split mixed pressure"},
};

ScenarioPlaybackState gScenarioPlayback;

constexpr uint16_t kScenarioStreamRepeatMsDefault = 700;
constexpr uint16_t kScenarioStreamRepeatMsMax = 5000;
constexpr uint16_t kScenarioDurationScalePctDefault = 100;
constexpr uint16_t kScenarioDurationScalePctMin = 25;
constexpr uint16_t kScenarioDurationScalePctMax = 1000;
constexpr size_t kScenarioExpandedEventCap = 4096;

String normalizedScenarioId(const String& raw) {
    String id = raw;
    id.trim();
    id.toUpperCase();
    return id;
}

const ScenarioCatalogEntry* findScenarioCatalogEntry(const String& id) {
    for (const auto& entry : kScenarioCatalog) {
        if (id.equalsIgnoreCase(entry.id)) {
            return &entry;
        }
    }
    return nullptr;
}

uint16_t clampScenarioStreamRepeatMs(int value) {
    if (value <= 0) {
        return 0;
    }
    if (value > kScenarioStreamRepeatMsMax) {
        return kScenarioStreamRepeatMsMax;
    }
    return static_cast<uint16_t>(value);
}

uint16_t clampScenarioDurationScalePct(int value) {
    if (value < static_cast<int>(kScenarioDurationScalePctMin)) {
        return kScenarioDurationScalePctMin;
    }
    if (value > static_cast<int>(kScenarioDurationScalePctMax)) {
        return kScenarioDurationScalePctMax;
    }
    return static_cast<uint16_t>(value);
}

bool scenarioPacketHasId(const ScenarioPacket& packet, uint8_t packetId) {
    return packet.bytes.size() >= 6 && packet.bytes[3] == packetId;
}

bool isScenarioAlertClearPacket(const ScenarioPacket& packet) {
    if (!scenarioPacketHasId(packet, PACKET_ID_ALERT_DATA)) {
        return false;
    }
    // payload[0] at bytes[5]: 0 means clear.
    return packet.bytes.size() >= 7 && packet.bytes[5] == 0x00;
}

bool isScenarioDisplayClearPacket(const ScenarioPacket& packet) {
    if (!scenarioPacketHasId(packet, PACKET_ID_DISPLAY_DATA)) {
        return false;
    }
    // payload: [bogey][bogey2][led][img1][img2]...
    return packet.bytes.size() >= 10 && packet.bytes[5] == 0x00 &&
           packet.bytes[7] == 0x00 && packet.bytes[8] == 0x00 &&
           packet.bytes[9] == 0x00;
}

void applyScenarioDurationScale(std::vector<ScenarioPacket>& events,
                                uint16_t durationScalePct) {
    if (events.empty() || durationScalePct == 100) {
        return;
    }
    for (ScenarioPacket& event : events) {
        event.atMs = static_cast<uint32_t>(
            (static_cast<uint64_t>(event.atMs) * durationScalePct) / 100ULL);
    }
    std::stable_sort(events.begin(),
                     events.end(),
                     [](const ScenarioPacket& a, const ScenarioPacket& b) {
                         return a.atMs < b.atMs;
                     });
}

void injectScenarioHoldRepeats(std::vector<ScenarioPacket>& events,
                               uint16_t streamRepeatMs) {
    if (events.size() < 2 || streamRepeatMs == 0) {
        return;
    }

    std::vector<ScenarioPacket> expanded;
    expanded.reserve(std::min(kScenarioExpandedEventCap, events.size() * 4));

    ScenarioPacket lastDisplay;
    ScenarioPacket lastAlert;
    bool haveDisplay = false;
    bool haveAlert = false;

    for (size_t i = 0; i < events.size(); ++i) {
        const ScenarioPacket& current = events[i];
        expanded.push_back(current);

        if (scenarioPacketHasId(current, PACKET_ID_DISPLAY_DATA)) {
            if (isScenarioDisplayClearPacket(current)) {
                haveDisplay = false;
            } else {
                lastDisplay = current;
                haveDisplay = true;
            }
        } else if (scenarioPacketHasId(current, PACKET_ID_ALERT_DATA)) {
            if (isScenarioAlertClearPacket(current)) {
                haveAlert = false;
            } else {
                lastAlert = current;
                haveAlert = true;
            }
        }

        if (i + 1 >= events.size()) {
            continue;
        }
        const uint32_t nextAtMs = events[i + 1].atMs;
        if (nextAtMs <= current.atMs + streamRepeatMs) {
            continue;
        }

        uint32_t repeatAtMs = current.atMs + streamRepeatMs;
        while (repeatAtMs < nextAtMs && expanded.size() < kScenarioExpandedEventCap) {
            if (haveDisplay) {
                ScenarioPacket displayRepeat = lastDisplay;
                displayRepeat.atMs = repeatAtMs;
                expanded.push_back(std::move(displayRepeat));
            }
            if (haveAlert && expanded.size() < kScenarioExpandedEventCap) {
                ScenarioPacket alertRepeat = lastAlert;
                alertRepeat.atMs = repeatAtMs + 2;
                if (alertRepeat.atMs < nextAtMs) {
                    expanded.push_back(std::move(alertRepeat));
                }
            }
            repeatAtMs += streamRepeatMs;
        }

        if (expanded.size() >= kScenarioExpandedEventCap) {
            break;
        }
    }

    std::stable_sort(expanded.begin(),
                     expanded.end(),
                     [](const ScenarioPacket& a, const ScenarioPacket& b) {
                         return a.atMs < b.atMs;
                     });
    events.swap(expanded);
}

std::vector<uint8_t> makeFramedPacket(uint8_t packetId,
                                      std::initializer_list<uint8_t> payload) {
    std::vector<uint8_t> packet;
    packet.reserve(6 + payload.size());
    packet.push_back(ESP_PACKET_START);
    packet.push_back(kScenarioDest);
    packet.push_back(kScenarioSrc);
    packet.push_back(packetId);
    packet.push_back(static_cast<uint8_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    packet.push_back(ESP_PACKET_END);
    return packet;
}

std::vector<uint8_t> makeDisplayPacket(uint8_t bogeyByte,
                                       uint8_t ledBitmap,
                                       uint8_t image1,
                                       uint8_t image2,
                                       uint8_t aux0 = 0x00,
                                       uint8_t aux1 = 0x00,
                                       uint8_t aux2 = 0x42) {
    return makeFramedPacket(PACKET_ID_DISPLAY_DATA,
                            {bogeyByte, 0x00, ledBitmap, image1, image2, aux0, aux1, aux2});
}

std::vector<uint8_t> makeAlertPacket(uint8_t rowIndex,
                                     uint8_t rowCount,
                                     uint16_t frequencyMHz,
                                     uint8_t frontRaw,
                                     uint8_t rearRaw,
                                     uint8_t bandArrow,
                                     uint8_t aux0) {
    const uint8_t rowMeta = static_cast<uint8_t>(((rowIndex & 0x0F) << 4) | (rowCount & 0x0F));
    return makeFramedPacket(PACKET_ID_ALERT_DATA,
                            {rowMeta,
                             static_cast<uint8_t>((frequencyMHz >> 8) & 0xFF),
                             static_cast<uint8_t>(frequencyMHz & 0xFF),
                             frontRaw,
                             rearRaw,
                             bandArrow,
                             aux0});
}

std::vector<uint8_t> makeAlertClearPacket() {
    // Keep payload >=2 bytes so PacketParser::validatePacket(length>=8) accepts it.
    return makeFramedPacket(PACKET_ID_ALERT_DATA, {0x00, 0x00});
}

void addScenarioPacket(std::vector<ScenarioPacket>& events,
                       uint32_t atMs,
                       std::vector<uint8_t> packet,
                       uint16_t charUuid = kScenarioCharLong) {
    if (packet.empty()) {
        return;
    }
    events.push_back(ScenarioPacket{atMs * kScenarioTimeScale, std::move(packet), charUuid});
}

void addSplitScenarioPacket(std::vector<ScenarioPacket>& events,
                            uint32_t atMs,
                            const std::vector<uint8_t>& packet,
                            std::initializer_list<size_t> chunkSizes,
                            uint16_t chunkGapMs = 4,
                            uint16_t charUuid = kScenarioCharLong) {
    if (packet.empty()) {
        return;
    }

    size_t offset = 0;
    uint32_t tsMs = atMs;
    for (size_t requested : chunkSizes) {
        if (offset >= packet.size()) {
            break;
        }
        const size_t size = std::min(requested, packet.size() - offset);
        if (size == 0) {
            continue;
        }
        std::vector<uint8_t> chunk(packet.begin() + offset,
                                   packet.begin() + offset + size);
        addScenarioPacket(events, tsMs, std::move(chunk), charUuid);
        offset += size;
        tsMs += chunkGapMs;
    }

    if (offset < packet.size()) {
        std::vector<uint8_t> remainder(packet.begin() + offset, packet.end());
        addScenarioPacket(events, tsMs, std::move(remainder), charUuid);
    }
}

void addAlertRows(std::vector<ScenarioPacket>& events,
                  uint32_t atMs,
                  uint8_t count,
                  std::initializer_list<AlertRowSpec> rows,
                  uint16_t perRowStepMs = 8) {
    uint32_t rowTs = atMs;
    for (const AlertRowSpec& row : rows) {
        addScenarioPacket(events,
                          rowTs,
                          makeAlertPacket(row.index,
                                          count,
                                          row.frequency,
                                          row.frontRaw,
                                          row.rearRaw,
                                          row.bandArrow,
                                          row.aux0),
                          kScenarioCharLong);
        rowTs += perRowStepMs;
    }
}

void addScenarioClearStart(std::vector<ScenarioPacket>& events, uint32_t atMs = 0) {
    addScenarioPacket(events, atMs, makeDisplayPacket(0x00, 0x00, 0x00, 0x00), kScenarioCharShort);
    addScenarioPacket(events, atMs + 2, makeAlertClearPacket(), kScenarioCharLong);
}

void addScenarioClearEnd(std::vector<ScenarioPacket>& events, uint32_t atMs) {
    addScenarioPacket(events, atMs, makeAlertClearPacket(), kScenarioCharLong);
    addScenarioPacket(events, atMs + 2, makeDisplayPacket(0x00, 0x00, 0x00, 0x00), kScenarioCharShort);
}

bool buildScenarioPackets(const String& scenarioId,
                          std::vector<ScenarioPacket>& outEvents,
                          String& outCategory,
                          String& outDescription,
                          uint32_t& outDurationMs) {
    outEvents.clear();
    outDurationMs = 0;

    const String normalizedId = normalizedScenarioId(scenarioId);
    const ScenarioCatalogEntry* catalog = findScenarioCatalogEntry(normalizedId);
    if (!catalog) {
        return false;
    }

    outCategory = catalog->category;
    outDescription = catalog->description;

    addScenarioClearStart(outEvents, 0);

    if (normalizedId == "RAD-01") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyOne, 0x03, kBandKFront, kBandKFront), kScenarioCharShort);
        addAlertRows(outEvents, 108, 1, {{1, kFreqKBase, 0x86, 0x7F, kBandKFront, 0x80}});
        addScenarioPacket(outEvents, 360, makeDisplayPacket(kBogeyOne, 0x07, kBandKSide, kBandKSide), kScenarioCharShort);
        addAlertRows(outEvents, 368, 1, {{1, kFreqKBase, 0x8E, 0x86, kBandKSide, 0x80}});
        addScenarioPacket(outEvents, 620, makeDisplayPacket(kBogeyOne, 0x0F, kBandKFront, kBandKFront), kScenarioCharShort);
        addAlertRows(outEvents, 628, 1, {{1, kFreqKBase, 0x9A, 0x82, kBandKFront, 0x80}});
        addScenarioPacket(outEvents, 860, makeDisplayPacket(kBogeyOne, 0x1F, kBandKRear, kBandKRear), kScenarioCharShort);
        addAlertRows(outEvents, 868, 1, {{1, kFreqKBase, 0x8A, 0x98, kBandKRear, 0x80}});
        addScenarioClearEnd(outEvents, 1080);
    } else if (normalizedId == "RAD-02") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyOne, 0x07, kBandKFront, kBandKFront), kScenarioCharShort);
        addAlertRows(outEvents, 108, 1, {{1, kFreqKBase, 0x92, 0x7F, kBandKFront, 0x80}});
        addScenarioPacket(outEvents, 420, makeDisplayPacket(0x3F, 0x0F, static_cast<uint8_t>(kBandKFront | 0x02), static_cast<uint8_t>(kBandKFront | 0x02)), kScenarioCharShort);
        addAlertRows(outEvents,
                     430,
                     2,
                     {{1, kFreqKBase, 0x8E, 0x7F, kBandKFront, 0x00},
                      {2, kFreqKa, 0x9A, 0x7F, kBandKaFront, 0x80}});
        addScenarioClearEnd(outEvents, 980);
    } else if (normalizedId == "RAD-03") {
        addScenarioPacket(outEvents, 120, makeDisplayPacket(kBogeyOne, 0x03, kDisplayXRearMuted, kDisplayXRearMuted), kScenarioCharShort);
        addAlertRows(outEvents, 128, 1, {{1, kFreqX, 0x8A, 0x96, kBandXRear, 0x80}});
        addScenarioPacket(outEvents, 500, makeDisplayPacket(kBogeyOne, 0x07, kDisplayXRearMuted, kDisplayXRearMuted), kScenarioCharShort);
        addAlertRows(outEvents, 508, 1, {{1, kFreqX, 0x84, 0x9C, kBandXRear, 0x80}});
        addScenarioClearEnd(outEvents, 860);
    } else if (normalizedId == "PHO-01") {
        addScenarioPacket(outEvents, 110, makeDisplayPacket(kBogeyPhoto, 0x07, kBandKaFront, kBandKaFront), kScenarioCharShort);
        addAlertRows(outEvents, 118, 1, {{1, kFreqKa, 0x90, 0x7F, kBandKaFront, 0x81}});
        addScenarioPacket(outEvents, 360, makeDisplayPacket(kBogeyPhoto, 0x0F, kBandKaSide, kBandKaSide), kScenarioCharShort);
        addAlertRows(outEvents, 368, 1, {{1, kFreqKa, 0x96, 0x82, kBandKaSide, 0x81}});
        addScenarioPacket(outEvents, 620, makeDisplayPacket(kBogeyPhoto, 0x1F, kBandKaRear, kBandKaRear), kScenarioCharShort);
        addAlertRows(outEvents, 628, 1, {{1, kFreqKa, 0x8A, 0x9A, kBandKaRear, 0x81}});
        addScenarioClearEnd(outEvents, 930);
    } else if (normalizedId == "PHO-02") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyPhoto, 0x0F, static_cast<uint8_t>(kBandKaFront | 0x04), static_cast<uint8_t>(kBandKaFront | 0x04)), kScenarioCharShort);
        addAlertRows(outEvents,
                     110,
                     2,
                     {{1, kFreqKa, 0x96, 0x7F, kBandKaFront, 0x81},
                      {2, static_cast<uint16_t>(kFreqKBase + 4), 0x90, 0x7F, kBandKSide, 0x00}});
        addScenarioClearEnd(outEvents, 980);
    } else if (normalizedId == "JNK-01") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyJunk, 0x03, kBandKFront, kBandKFront), kScenarioCharShort);
        addAlertRows(outEvents, 108, 1, {{1, kFreqKJunkLow, 0x88, 0x7F, kBandKFront, 0xC0}});
        addScenarioPacket(outEvents, 360, makeDisplayPacket(kBogeyJunk, 0x07, kBandKSide, kBandKSide), kScenarioCharShort);
        addAlertRows(outEvents, 368, 1, {{1, kFreqKJunkHigh, 0x8C, 0x86, kBandKSide, 0xC0}});
        addScenarioPacket(outEvents, 620, makeDisplayPacket(kBogeyJunk, 0x0F, kBandKRear, kBandKRear), kScenarioCharShort);
        addAlertRows(outEvents, 628, 1, {{1, kFreqKJunkLow, 0x84, 0x92, kBandKRear, 0xC0}});
        addScenarioClearEnd(outEvents, 820);
    } else if (normalizedId == "JNK-02") {
        addScenarioPacket(outEvents,
                          110,
                          makeDisplayPacket(kBogeyJunk,
                                            0x0F,
                                            static_cast<uint8_t>(kBandKFront | 0x40),
                                            static_cast<uint8_t>(kBandKFront | 0x40)),
                          kScenarioCharShort);
        addAlertRows(outEvents,
                     118,
                     2,
                     {{1, kFreqKJunkLow, 0x8C, 0x7F, kBandKFront, 0x40},
                      {2, kFreqKJunkHigh, 0x98, 0x7F, kBandKSide, 0x80}});
        addScenarioClearEnd(outEvents, 980);
    } else if (normalizedId == "LAS-01") {
        addScenarioPacket(outEvents, 80, makeDisplayPacket(kBogeyLaser, 0x3F, kBandLaserFront, kBandLaserFront), kScenarioCharShort);
        addAlertRows(outEvents, 88, 1, {{1, 0, 0xFF, 0x00, kBandLaserFront, 0x80}});
        addScenarioPacket(outEvents, 250, makeDisplayPacket(kBogeyLaser, 0x3F, kBandLaserFront, kBandLaserFront), kScenarioCharShort);
        addAlertRows(outEvents, 258, 1, {{1, 0, 0xFE, 0x00, kBandLaserFront, 0x80}});
        addScenarioPacket(outEvents, 430, makeDisplayPacket(kBogeyLaser, 0x3F, kBandLaserFront, kBandLaserFront), kScenarioCharShort);
        addAlertRows(outEvents, 438, 1, {{1, 0, 0xFD, 0x00, kBandLaserFront, 0x80}});
        addScenarioClearEnd(outEvents, 620);
    } else if (normalizedId == "LAS-02") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyOne, 0x07, kBandKFront, kBandKFront), kScenarioCharShort);
        addAlertRows(outEvents,
                     108,
                     3,
                     {{1, static_cast<uint16_t>(kFreqKBase - 3), 0x88, 0x7F, kBandKFront, 0x00},
                      {2, kFreqKBase, 0x8A, 0x7F, kBandKSide, 0x00},
                      {3, static_cast<uint16_t>(kFreqKBase + 3), 0x8C, 0x7F, kBandKRear, 0x00}});
        addScenarioPacket(outEvents, 420, makeDisplayPacket(kBogeyLaser, 0x3F, static_cast<uint8_t>(kBandLaserFront | 0x04), static_cast<uint8_t>(kBandLaserFront | 0x04)), kScenarioCharShort);
        addAlertRows(outEvents,
                     430,
                     4,
                     {{1, static_cast<uint16_t>(kFreqKBase - 3), 0x88, 0x7F, kBandKFront, 0x00},
                      {2, kFreqKBase, 0x8A, 0x7F, kBandKSide, 0x00},
                      {3, static_cast<uint16_t>(kFreqKBase + 3), 0x8C, 0x7F, kBandKRear, 0x00},
                      {4, 0, 0xFF, 0x00, kBandLaserFront, 0x80}});
        addScenarioClearEnd(outEvents, 1080);
    } else if (normalizedId == "MBP-01") {
        addScenarioPacket(outEvents, 120, makeDisplayPacket(kBogeyOne, 0x07, static_cast<uint8_t>(0xE4), static_cast<uint8_t>(0xE4)), kScenarioCharShort);
        addAlertRows(outEvents,
                     130,
                     4,
                     {{1, static_cast<uint16_t>(kFreqKBase - 4), 0x88, 0x7F, kBandKFront, 0x00},
                      {2, static_cast<uint16_t>(kFreqKBase - 1), 0x8A, 0x7F, kBandKSide, 0x00},
                      {3, static_cast<uint16_t>(kFreqKBase + 2), 0x8C, 0x7F, kBandKRear, 0x00},
                      {4, static_cast<uint16_t>(kFreqKBase + 5), 0x8E, 0x7F, kBandKFront, 0x00}});
        addScenarioPacket(outEvents, 640, makeDisplayPacket(kBogeyOne, 0x0F, static_cast<uint8_t>(0xE6), static_cast<uint8_t>(0xE6)), kScenarioCharShort);
        addAlertRows(outEvents,
                     650,
                     5,
                     {{1, static_cast<uint16_t>(kFreqKBase - 4), 0x88, 0x7F, kBandKFront, 0x00},
                      {2, static_cast<uint16_t>(kFreqKBase - 1), 0x8A, 0x7F, kBandKSide, 0x00},
                      {3, static_cast<uint16_t>(kFreqKBase + 2), 0x8C, 0x7F, kBandKRear, 0x00},
                      {4, static_cast<uint16_t>(kFreqKBase + 5), 0x8E, 0x7F, kBandKFront, 0x00},
                      {5, kFreqKa, 0x8A, 0x7F, kBandKaFront, 0x80}});
        addAlertRows(outEvents,
                     1080,
                     5,
                     {{1, static_cast<uint16_t>(kFreqKBase - 4), 0x88, 0x7F, kBandKFront, 0x00},
                      {2, static_cast<uint16_t>(kFreqKBase - 1), 0x8A, 0x7F, kBandKSide, 0x00},
                      {3, static_cast<uint16_t>(kFreqKBase + 2), 0x8C, 0x7F, kBandKRear, 0x00},
                      {4, static_cast<uint16_t>(kFreqKBase + 5), 0x8E, 0x7F, kBandKFront, 0x00},
                      {5, kFreqKa, 0xA4, 0x7F, kBandKaFront, 0x80}});
        addScenarioClearEnd(outEvents, 1540);
    } else if (normalizedId == "MBP-02") {
        addScenarioPacket(outEvents, 120, makeDisplayPacket(kBogeyOne, 0x07, static_cast<uint8_t>(0x64), static_cast<uint8_t>(0x64)), kScenarioCharShort);
        addAlertRows(outEvents,
                     130,
                     2,
                     {{1, kFreqKBase, 0x8C, 0x7F, kBandKFront, 0x00},
                      {2, kFreqKa, 0x8A, 0x7F, kBandKaSide, 0x80}});
        addAlertRows(outEvents,
                     500,
                     2,
                     {{1, kFreqKBase, 0x8E, 0x7F, kBandKFront, 0x00},
                      {2, kFreqKa, 0x98, 0x7F, kBandKaSide, 0x80}});
        addScenarioClearEnd(outEvents, 980);
    } else if (normalizedId == "MBP-03") {
        addScenarioPacket(outEvents, 110, makeDisplayPacket(kBogeyOne, 0x07, static_cast<uint8_t>(0xE4), static_cast<uint8_t>(0xE4)), kScenarioCharShort);
        addAlertRows(outEvents,
                     120,
                     4,
                     {{1, static_cast<uint16_t>(kFreqKBase - 2), 0x88, 0x7F, kBandKFront, 0x00},
                      {2, kFreqKBase, 0x8A, 0x7F, kBandKSide, 0x00},
                      {3, static_cast<uint16_t>(kFreqKBase + 2), 0x8C, 0x7F, kBandKRear, 0x00},
                      {4, static_cast<uint16_t>(kFreqKBase + 4), 0x8E, 0x7F, kBandKFront, 0x00}});
        addScenarioPacket(outEvents, 520, makeDisplayPacket(kBogeyLaser, 0x3F, kBandLaserFront, kBandLaserFront), kScenarioCharShort);
        addAlertRows(outEvents, 528, 1, {{1, 0, 0xFF, 0x00, kBandLaserFront, 0x80}});
        addScenarioClearEnd(outEvents, 980);
    } else if (normalizedId == "ASM-01") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyOne, 0x07, static_cast<uint8_t>(kBandKFront | 0x22), static_cast<uint8_t>(kBandKFront | 0x22)), kScenarioCharShort);
        addAlertRows(outEvents,
                     110,
                     3,
                     {{1, kFreqKBase, 0x90, 0x7F, kBandKFront, 0x00},
                      {3, kFreqKa, 0x98, 0x7F, kBandKaFront, 0x80}});
        addAlertRows(outEvents, 2230, 3, {{1, kFreqKBase, 0x90, 0x7F, kBandKFront, 0x00}});
        addAlertRows(outEvents, 2410, 1, {{1, kFreqKa, 0x9A, 0x7F, kBandKaFront, 0x80}});
        addScenarioClearEnd(outEvents, 2860);
    } else if (normalizedId == "ASM-02") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyOne, 0x07, static_cast<uint8_t>(kBandKFront | 0x02), static_cast<uint8_t>(kBandKFront | 0x02)), kScenarioCharShort);
        addAlertRows(outEvents, 110, 2, {{1, kFreqKBase, 0x90, 0x7F, kBandKFront, 0x00}});
        addAlertRows(outEvents, 140, 2, {{1, kFreqKBase, 0x92, 0x7F, kBandKFront, 0x00}});
        addAlertRows(outEvents, 170, 2, {{2, kFreqKa, 0x9A, 0x7F, kBandKaFront, 0x80}});
        addScenarioClearEnd(outEvents, 900);
    } else if (normalizedId == "ASM-03") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyOne, 0x0F, static_cast<uint8_t>(0xE6), static_cast<uint8_t>(0xE6)), kScenarioCharShort);
        addAlertRows(outEvents,
                     112,
                     3,
                     {{3, kFreqKa, 0x98, 0x7F, kBandKaFront, 0x80},
                      {1, kFreqKBase, 0x8E, 0x7F, kBandKFront, 0x00},
                      {2, static_cast<uint16_t>(kFreqKBase + 3), 0x8F, 0x7F, kBandKRear, 0x00}});
        addScenarioClearEnd(outEvents, 880);
    } else if (normalizedId == "ASM-04") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyOne, 0x07, static_cast<uint8_t>(kBandKFront | 0x02), static_cast<uint8_t>(kBandKFront | 0x02)), kScenarioCharShort);
        addAlertRows(outEvents,
                     110,
                     2,
                     {{0, kFreqKBase, 0x90, 0x7F, kBandKFront, 0x00},
                      {1, kFreqKa, 0x98, 0x7F, kBandKaFront, 0x80}});
        addScenarioClearEnd(outEvents, 860);
    } else if (normalizedId == "MET-01") {
        // Count=2 with raw rows 0,2,1 causes both index schemes to be complete
        // on the final row and should increment prioritySelectAmbiguousIndex.
        addScenarioPacket(outEvents, 100, makeDisplayPacket(0x3F, 0x07, static_cast<uint8_t>(kBandKFront | 0x02), static_cast<uint8_t>(kBandKFront | 0x02)), kScenarioCharShort);
        addAlertRows(outEvents, 110, 2, {{0, kFreqKBase, 0x90, 0x7F, kBandKFront, 0x00}});
        addAlertRows(outEvents, 145, 2, {{2, kFreqKa, 0x98, 0x7F, kBandKaFront, 0x00}});
        addAlertRows(outEvents, 180, 2, {{1, static_cast<uint16_t>(kFreqKBase + 2), 0x94, 0x7F, kBandKSide, 0x80}});
        addScenarioClearEnd(outEvents, 900);
    } else if (normalizedId == "MET-02") {
        // Row1 carries row-priority bit but BAND_NONE, forcing first-usable fallback
        // and incrementing prioritySelectUnusableIndex.
        addScenarioPacket(outEvents, 100, makeDisplayPacket(0x3F, 0x07, static_cast<uint8_t>(kBandKFront | 0x02), static_cast<uint8_t>(kBandKFront | 0x02)), kScenarioCharShort);
        addAlertRows(outEvents,
                     112,
                     2,
                     {{1, kFreqKBase, 0x90, 0x7F, 0x20, 0x80},
                      {2, kFreqKa, 0x98, 0x7F, kBandKaFront, 0x00}});
        addScenarioClearEnd(outEvents, 900);
    } else if (normalizedId == "TRN-01") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyOne, 0x0F, kBandKFront, kBandKFront), kScenarioCharShort);
        for (uint8_t i = 0; i < 18; ++i) {
            const uint16_t freq = static_cast<uint16_t>(kFreqKBase + i);
            const uint8_t front = static_cast<uint8_t>(0x88 + (i % 6) * 4);
            addScenarioPacket(outEvents, 130, makeAlertPacket(1, 1, freq, front, 0x7F, kBandKFront, 0x80), kScenarioCharLong);
        }
        addScenarioPacket(outEvents, 160, makeDisplayPacket(kBogeyOne, 0x1F, kBandKFront, kBandKFront), kScenarioCharShort);
        addScenarioClearEnd(outEvents, 920);
    } else if (normalizedId == "TRN-02") {
        const std::vector<uint8_t> splitAlert =
            makeAlertPacket(1, 1, kFreqKa, 0xA0, 0x7F, kBandKaFront, 0x80);
        const std::vector<uint8_t> splitDisplay =
            makeDisplayPacket(kBogeyOne, 0x0F, kBandKaFront, kBandKaFront);
        addSplitScenarioPacket(outEvents, 120, splitDisplay, {3, 2, 2}, 5, kScenarioCharShort);
        addSplitScenarioPacket(outEvents, 170, splitAlert, {2, 4, 3}, 4, kScenarioCharLong);
        addScenarioClearEnd(outEvents, 940);
    } else if (normalizedId == "TRN-03") {
        addScenarioPacket(outEvents, 100, makeDisplayPacket(kBogeyOne, 0x0F, static_cast<uint8_t>(kBandKFront | 0x02), static_cast<uint8_t>(kBandKFront | 0x02)), kScenarioCharShort);
        for (uint8_t i = 0; i < 8; ++i) {
            const uint32_t ts = 130 + static_cast<uint32_t>(i * 14);
            const bool ka = (i % 2) != 0;
            const uint16_t freq = ka ? static_cast<uint16_t>(kFreqKa + i) : static_cast<uint16_t>(kFreqKBase + i);
            const uint8_t bandArrow = ka ? kBandKaFront : kBandKFront;
            const uint8_t aux0 = ka ? 0x80 : 0x00;
            const std::vector<uint8_t> packet = makeAlertPacket(1, 1, freq, static_cast<uint8_t>(0x8A + i), 0x7F, bandArrow, aux0);
            addSplitScenarioPacket(outEvents, ts, packet, {2, 3}, 3, kScenarioCharLong);
        }
        addScenarioClearEnd(outEvents, 980);
    } else {
        return false;
    }

    std::stable_sort(outEvents.begin(),
                     outEvents.end(),
                     [](const ScenarioPacket& a, const ScenarioPacket& b) {
                         return a.atMs < b.atMs;
                     });
    outDurationMs = outEvents.empty() ? 0 : outEvents.back().atMs;
    return true;
}

bool parseRequestBody(WebServer& server, JsonDocument& body, bool& hasBody) {
    hasBody = false;
    if (!server.hasArg("plain")) {
        return true;
    }
    const String payload = server.arg("plain");
    if (payload.length() == 0) {
        return true;
    }
    const DeserializationError err = deserializeJson(body, payload.c_str());
    if (err) {
        return false;
    }
    hasBody = true;
    return true;
}

bool jsonTruthy(const JsonVariantConst& value, bool fallback) {
    if (value.isNull()) {
        return fallback;
    }
    if (value.is<bool>()) {
        return value.as<bool>();
    }
    if (value.is<int>()) {
        return value.as<int>() != 0;
    }
    if (value.is<const char*>()) {
        return isTruthyArgValue(String(value.as<const char*>()));
    }
    return fallback;
}

String requestScenarioId(WebServer& server,
                         const JsonDocument* body,
                         const char* key = "id") {
    if (server.hasArg(key)) {
        String fromArg = server.arg(key);
        fromArg.trim();
        if (fromArg.length() > 0) {
            return fromArg;
        }
    }
    if (server.hasArg("name")) {
        String fromName = server.arg("name");
        fromName.trim();
        if (fromName.length() > 0) {
            return fromName;
        }
    }
    if (body) {
        const JsonVariantConst vId = (*body)[key];
        if (vId.is<const char*>()) {
            String out = vId.as<const char*>();
            out.trim();
            if (out.length() > 0) {
                return out;
            }
        }
        const JsonVariantConst vName = (*body)["name"];
        if (vName.is<const char*>()) {
            String out = vName.as<const char*>();
            out.trim();
            if (out.length() > 0) {
                return out;
            }
        }
    }
    return "";
}

bool requestBoolArg(WebServer& server,
                    const JsonDocument* body,
                    const char* key,
                    bool fallback) {
    if (server.hasArg(key)) {
        return isTruthyArgValue(server.arg(key));
    }
    if (body) {
        return jsonTruthy((*body)[key], fallback);
    }
    return fallback;
}

uint16_t requestScenarioStreamRepeatMs(WebServer& server,
                                       const JsonDocument* body,
                                       uint16_t fallback) {
    auto parseRaw = [&](const JsonVariantConst& value, uint16_t base) -> uint16_t {
        if (value.isNull()) {
            return base;
        }
        if (value.is<int>()) {
            return clampScenarioStreamRepeatMs(value.as<int>());
        }
        if (value.is<const char*>()) {
            return clampScenarioStreamRepeatMs(String(value.as<const char*>()).toInt());
        }
        return base;
    };

    if (server.hasArg("streamRepeatMs")) {
        return clampScenarioStreamRepeatMs(server.arg("streamRepeatMs").toInt());
    }
    if (server.hasArg("repeatMs")) {
        return clampScenarioStreamRepeatMs(server.arg("repeatMs").toInt());
    }
    if (body) {
        const JsonVariantConst repeatMs = (*body)["streamRepeatMs"];
        if (!repeatMs.isNull()) {
            return parseRaw(repeatMs, fallback);
        }
        return parseRaw((*body)["repeatMs"], fallback);
    }
    return fallback;
}

uint16_t requestScenarioDurationScalePct(WebServer& server,
                                         const JsonDocument* body,
                                         uint16_t fallback) {
    auto parsePct = [&](const JsonVariantConst& value, uint16_t base) -> uint16_t {
        if (value.isNull()) {
            return base;
        }
        if (value.is<int>()) {
            return clampScenarioDurationScalePct(value.as<int>());
        }
        if (value.is<const char*>()) {
            return clampScenarioDurationScalePct(String(value.as<const char*>()).toInt());
        }
        return base;
    };

    auto parseScale = [&](const JsonVariantConst& value, uint16_t base) -> uint16_t {
        if (value.isNull()) {
            return base;
        }
        float scale = -1.0f;
        if (value.is<float>()) {
            scale = value.as<float>();
        } else if (value.is<double>()) {
            scale = static_cast<float>(value.as<double>());
        } else if (value.is<const char*>()) {
            scale = String(value.as<const char*>()).toFloat();
        }
        if (!(scale > 0.0f)) {
            return base;
        }
        const int pct = static_cast<int>(scale * 100.0f + 0.5f);
        return clampScenarioDurationScalePct(pct);
    };

    if (server.hasArg("durationScalePct")) {
        return clampScenarioDurationScalePct(server.arg("durationScalePct").toInt());
    }
    if (server.hasArg("durationScale")) {
        const float scale = server.arg("durationScale").toFloat();
        if (scale > 0.0f) {
            return clampScenarioDurationScalePct(static_cast<int>(scale * 100.0f + 0.5f));
        }
    }
    if (body) {
        const JsonVariantConst pct = (*body)["durationScalePct"];
        if (!pct.isNull()) {
            return parsePct(pct, fallback);
        }
        return parseScale((*body)["durationScale"], fallback);
    }
    return fallback;
}

void loadScenarioState(const String& requestedId,
                       bool loop,
                       uint16_t streamRepeatMs,
                       uint16_t durationScalePct) {
    gScenarioPlayback.events.clear();
    String category;
    String description;
    uint32_t durationMs = 0;
    std::vector<ScenarioPacket> events;
    const bool ok = buildScenarioPackets(requestedId, events, category, description, durationMs);
    if (!ok) {
        gScenarioPlayback.loaded = false;
        gScenarioPlayback.running = false;
        gScenarioPlayback.scenarioId = "";
        gScenarioPlayback.category = "";
        gScenarioPlayback.description = "";
        gScenarioPlayback.nextEventIndex = 0;
        gScenarioPlayback.durationMs = 0;
        gScenarioPlayback.streamRepeatMs = streamRepeatMs;
        gScenarioPlayback.durationScalePct = durationScalePct;
        return;
    }

    applyScenarioDurationScale(events, durationScalePct);
    injectScenarioHoldRepeats(events, streamRepeatMs);
    durationMs = events.empty() ? 0 : events.back().atMs;

    gScenarioPlayback.loaded = true;
    gScenarioPlayback.running = false;
    gScenarioPlayback.loop = loop;
    gScenarioPlayback.scenarioId = normalizedScenarioId(requestedId);
    gScenarioPlayback.category = category;
    gScenarioPlayback.description = description;
    gScenarioPlayback.events = std::move(events);
    gScenarioPlayback.nextEventIndex = 0;
    gScenarioPlayback.durationMs = durationMs;
    gScenarioPlayback.loadedAtMs = millis();
    gScenarioPlayback.startMs = 0;
    gScenarioPlayback.startedAtMs = 0;
    gScenarioPlayback.finishedAtMs = 0;
    gScenarioPlayback.emittedPackets = 0;
    gScenarioPlayback.completedRuns = 0;
    gScenarioPlayback.streamRepeatMs = streamRepeatMs;
    gScenarioPlayback.durationScalePct = durationScalePct;
}

bool startScenarioPlayback() {
    if (!gScenarioPlayback.loaded || gScenarioPlayback.events.empty()) {
        return false;
    }
    const uint32_t nowMs = millis();
    gScenarioPlayback.running = true;
    gScenarioPlayback.nextEventIndex = 0;
    gScenarioPlayback.startMs = nowMs;
    gScenarioPlayback.startedAtMs = nowMs;
    gScenarioPlayback.finishedAtMs = 0;
    gScenarioPlayback.emittedPackets = 0;
    gScenarioPlayback.completedRuns = 0;
    return true;
}

void stopScenarioPlayback() {
    if (!gScenarioPlayback.running) {
        return;
    }
    gScenarioPlayback.running = false;
    gScenarioPlayback.finishedAtMs = millis();
}

void pumpScenarioPlayback(uint32_t nowMs) {
    if (!gScenarioPlayback.running || gScenarioPlayback.events.empty()) {
        return;
    }

    const uint32_t elapsedMs = nowMs - gScenarioPlayback.startMs;
    while (gScenarioPlayback.nextEventIndex < gScenarioPlayback.events.size()) {
        const ScenarioPacket& event = gScenarioPlayback.events[gScenarioPlayback.nextEventIndex];
        if (elapsedMs < event.atMs) {
            break;
        }
        if (!event.bytes.empty() && event.bytes.size() <= 256) {
            DebugApiService::deps::bleQueue->onNotify(event.bytes.data(), event.bytes.size(), event.charUuid);
            gScenarioPlayback.emittedPackets++;
        }
        gScenarioPlayback.nextEventIndex++;
    }

    if (gScenarioPlayback.nextEventIndex < gScenarioPlayback.events.size()) {
        return;
    }

    if (gScenarioPlayback.loop) {
        gScenarioPlayback.completedRuns++;
        gScenarioPlayback.nextEventIndex = 0;
        gScenarioPlayback.startMs = nowMs;
        return;
    }

    gScenarioPlayback.running = false;
    gScenarioPlayback.finishedAtMs = nowMs;
    gScenarioPlayback.completedRuns++;
}

}  // anonymous namespace

namespace DebugApiService {

void appendScenarioStatus(JsonDocument& doc, uint32_t nowMs) {
    doc["loaded"] = gScenarioPlayback.loaded;
    doc["running"] = gScenarioPlayback.running;
    doc["loop"] = gScenarioPlayback.loop;
    doc["scenarioId"] = gScenarioPlayback.scenarioId;
    doc["category"] = gScenarioPlayback.category;
    doc["description"] = gScenarioPlayback.description;
    doc["eventsTotal"] = static_cast<uint32_t>(gScenarioPlayback.events.size());
    doc["eventsEmitted"] = gScenarioPlayback.emittedPackets;
    doc["nextEventIndex"] = static_cast<uint32_t>(gScenarioPlayback.nextEventIndex);
    doc["durationMs"] = gScenarioPlayback.durationMs;
    doc["streamRepeatMs"] = gScenarioPlayback.streamRepeatMs;
    doc["durationScalePct"] = gScenarioPlayback.durationScalePct;
    doc["durationScale"] = static_cast<float>(gScenarioPlayback.durationScalePct) / 100.0f;
    doc["completedRuns"] = gScenarioPlayback.completedRuns;
    doc["loadedAtMs"] = gScenarioPlayback.loadedAtMs;
    doc["startedAtMs"] = gScenarioPlayback.startedAtMs;
    doc["finishedAtMs"] = gScenarioPlayback.finishedAtMs;
    doc["elapsedMs"] = gScenarioPlayback.running
                           ? static_cast<uint32_t>(nowMs - gScenarioPlayback.startMs)
                           : (gScenarioPlayback.finishedAtMs >= gScenarioPlayback.startMs
                                  ? static_cast<uint32_t>(gScenarioPlayback.finishedAtMs -
                                                          gScenarioPlayback.startMs)
                                  : 0);
}

void sendV1ScenarioList(WebServer& server) {
    WifiJson::Document doc;
    doc["success"] = true;
    doc["count"] = static_cast<uint32_t>(sizeof(kScenarioCatalog) / sizeof(kScenarioCatalog[0]));
    JsonObject tuning = doc["timingTuning"].to<JsonObject>();
    tuning["streamRepeatMsDefault"] = kScenarioStreamRepeatMsDefault;
    tuning["streamRepeatMsMax"] = kScenarioStreamRepeatMsMax;
    tuning["durationScalePctDefault"] = kScenarioDurationScalePctDefault;
    tuning["durationScalePctMin"] = kScenarioDurationScalePctMin;
    tuning["durationScalePctMax"] = kScenarioDurationScalePctMax;
    JsonArray scenarios = doc["scenarios"].to<JsonArray>();
    for (const auto& row : kScenarioCatalog) {
        JsonObject item = scenarios.add<JsonObject>();
        item["id"] = row.id;
        item["category"] = row.category;
        item["description"] = row.description;
    }
    sendJsonStream(server, doc);
}

void sendV1ScenarioStatus(WebServer& server) {
    WifiJson::Document doc;
    doc["success"] = true;
    appendScenarioStatus(doc, millis());
    sendJsonStream(server, doc);
}

void handleV1ScenarioLoad(WebServer& server) {
    WifiJson::Document body;
    bool hasBody = false;
    if (!parseRequestBody(server, body, hasBody)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON body\"}");
        return;
    }
    const JsonDocument* bodyPtr = hasBody ? &body : nullptr;

    const String requestedId = requestScenarioId(server, bodyPtr);
    if (requestedId.length() == 0) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing scenario id\"}");
        return;
    }
    const String normalizedId = normalizedScenarioId(requestedId);
    if (!findScenarioCatalogEntry(normalizedId)) {
        server.send(404, "application/json", "{\"success\":false,\"error\":\"Unknown scenario id\"}");
        return;
    }

    const bool loop = requestBoolArg(server, bodyPtr, "loop", false);
    const bool autoStart = requestBoolArg(server, bodyPtr, "start", false) ||
                           requestBoolArg(server, bodyPtr, "autostart", false);
    const uint16_t streamRepeatMs =
        requestScenarioStreamRepeatMs(server, bodyPtr, kScenarioStreamRepeatMsDefault);
    const uint16_t durationScalePct =
        requestScenarioDurationScalePct(server, bodyPtr, kScenarioDurationScalePctDefault);

    loadScenarioState(normalizedId, loop, streamRepeatMs, durationScalePct);
    if (!gScenarioPlayback.loaded) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Failed to load scenario\"}");
        return;
    }

    if (autoStart) {
        startScenarioPlayback();
    }

    WifiJson::Document doc;
    doc["success"] = true;
    doc["action"] = "load";
    appendScenarioStatus(doc, millis());
    sendJsonStream(server, doc);
}

void handleV1ScenarioStart(WebServer& server) {
    WifiJson::Document body;
    bool hasBody = false;
    if (!parseRequestBody(server, body, hasBody)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON body\"}");
        return;
    }
    const JsonDocument* bodyPtr = hasBody ? &body : nullptr;

    const String requestedId = requestScenarioId(server, bodyPtr);
    const bool loopRequested = requestBoolArg(server, bodyPtr, "loop", gScenarioPlayback.loop);
    const bool hasRequestedId = requestedId.length() > 0;
    const uint16_t repeatFallback =
        hasRequestedId ? kScenarioStreamRepeatMsDefault : gScenarioPlayback.streamRepeatMs;
    const uint16_t scaleFallback =
        hasRequestedId ? kScenarioDurationScalePctDefault : gScenarioPlayback.durationScalePct;
    const uint16_t streamRepeatMs =
        requestScenarioStreamRepeatMs(server, bodyPtr, repeatFallback);
    const uint16_t durationScalePct =
        requestScenarioDurationScalePct(server, bodyPtr, scaleFallback);

    if (hasRequestedId) {
        const String normalizedId = normalizedScenarioId(requestedId);
        if (!findScenarioCatalogEntry(normalizedId)) {
            server.send(404, "application/json", "{\"success\":false,\"error\":\"Unknown scenario id\"}");
            return;
        }
        loadScenarioState(normalizedId, loopRequested, streamRepeatMs, durationScalePct);
    } else if (!gScenarioPlayback.loaded) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"No scenario loaded\"}");
        return;
    } else {
        gScenarioPlayback.loop = loopRequested;
        const bool timingChanged =
            (gScenarioPlayback.streamRepeatMs != streamRepeatMs) ||
            (gScenarioPlayback.durationScalePct != durationScalePct);
        if (timingChanged) {
            loadScenarioState(gScenarioPlayback.scenarioId,
                              loopRequested,
                              streamRepeatMs,
                              durationScalePct);
        }
    }

    if (!startScenarioPlayback()) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Scenario has no events\"}");
        return;
    }

    WifiJson::Document doc;
    doc["success"] = true;
    doc["action"] = "start";
    appendScenarioStatus(doc, millis());
    sendJsonStream(server, doc);
}

void handleV1ScenarioStop(WebServer& server) {
    stopScenarioPlayback();
    WifiJson::Document doc;
    doc["success"] = true;
    doc["action"] = "stop";
    appendScenarioStatus(doc, millis());
    sendJsonStream(server, doc);
}

void handleApiV1ScenarioList(WebServer& server) {
    sendV1ScenarioList(server);
}

void handleApiV1ScenarioStatus(WebServer& server) {
    sendV1ScenarioStatus(server);
}

void handleApiV1ScenarioLoad(WebServer& server,
                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleV1ScenarioLoad(server);
}

void handleApiV1ScenarioStart(WebServer& server,
                              bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleV1ScenarioStart(server);
}

void handleApiV1ScenarioStop(WebServer& server,
                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleV1ScenarioStop(server);
}
void process(uint32_t nowMs) {
    pumpScenarioPlayback(nowMs);
}

}  // namespace DebugApiService
