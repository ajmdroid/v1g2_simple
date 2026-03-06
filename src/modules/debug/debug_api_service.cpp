#include "debug_api_service.h"
#include "debug_perf_files_service.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cmath>
#include "json_stream_response.h"
#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

#include "../../../include/config.h"
#include "../../perf_metrics.h"
#include "../../audio_beep.h"
#include "../../settings.h"
#include "../../ble_client.h"
#include "../../storage_manager.h"
#include "../../perf_sd_logger.h"
#include "../display/display_pipeline_module.h"
#include "../ble/ble_queue_module.h"
#include "../gps/gps_runtime_module.h"
#include "../gps/gps_observation_log.h"
#include "../gps/gps_lockout_safety.h"
#include "../lockout/lockout_learner.h"
#include "../lockout/lockout_band_policy.h"
#include "../speed/speed_source_selector.h"
#include "../system/system_event_bus.h"
#include "../../../include/camera_alert_types.h"

// Extern globals (defined in main.cpp / module .cpp files).
extern V1BLEClient bleClient;
extern BleQueueModule bleQueueModule;
extern SystemEventBus systemEventBus;
#ifndef UNIT_TEST
extern DisplayPipelineModule displayPipelineModule;
#endif

// Conditionally-compiled perf latency externs (must be at file scope, not inside namespace).
#if PERF_METRICS && PERF_MONITORING
extern PerfLatency perfLatency;
extern bool perfDebugEnabled;
#endif

namespace {

bool isTruthyArgValue(const String& value) {
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "on" || value == "ON";
}

bool parseUint32Arg(const String& token, uint32_t& outValue) {
    if (token.length() == 0) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < token.length(); ++i) {
        const char ch = token.charAt(i);
        if (ch < '0' || ch > '9') {
            return false;
        }
        const uint32_t nextValue = (value * 10U) + static_cast<uint32_t>(ch - '0');
        if (nextValue < value) {
            return false;
        }
        value = nextValue;
    }

    outValue = value;
    return true;
}

CameraType parseCameraTypeArg(const String& token) {
    String normalized = token;
    normalized.trim();
    normalized.toLowerCase();
    if (normalized == "speed") {
        return CameraType::SPEED;
    }
    if (normalized == "red_light" || normalized == "red-light" || normalized == "redlight") {
        return CameraType::RED_LIGHT;
    }
    if (normalized == "bus_lane" || normalized == "bus-lane" || normalized == "buslane") {
        return CameraType::BUS_LANE;
    }
    if (normalized == "alpr") {
        return CameraType::ALPR;
    }
    return CameraType::INVALID;
}

enum class DebugCameraVoiceStage : uint8_t {
    NONE = 0,
    FAR,
    NEAR,
};

bool parseDebugCameraVoiceStageArg(const String& token, DebugCameraVoiceStage& outStage) {
    String normalized = token;
    normalized.trim();
    normalized.toLowerCase();
    if (normalized.length() == 0 || normalized == "far") {
        outStage = DebugCameraVoiceStage::FAR;
        return true;
    }
    if (normalized == "none" || normalized == "off") {
        outStage = DebugCameraVoiceStage::NONE;
        return true;
    }
    if (normalized == "near" || normalized == "close") {
        outStage = DebugCameraVoiceStage::NEAR;
        return true;
    }
    return false;
}

const char* debugCameraVoiceStageName(DebugCameraVoiceStage stage) {
    switch (stage) {
        case DebugCameraVoiceStage::NONE:
            return "none";
        case DebugCameraVoiceStage::NEAR:
            return "near";
        case DebugCameraVoiceStage::FAR:
        default:
            return "far";
    }
}

struct PanicFileSnapshot {
    bool loaded = false;
    bool hasPanicFile = false;
    String panicInfo = "";
};

PanicFileSnapshot gPanicFileSnapshot;

const PanicFileSnapshot& getPanicFileSnapshot() {
    if (gPanicFileSnapshot.loaded) {
        return gPanicFileSnapshot;
    }
    gPanicFileSnapshot.loaded = true;
    gPanicFileSnapshot.hasPanicFile = LittleFS.exists("/panic.txt");
    if (!gPanicFileSnapshot.hasPanicFile) {
        return gPanicFileSnapshot;
    }

    File f = LittleFS.open("/panic.txt", "r");
    if (!f) {
        // If open fails, surface a conservative "present but unreadable" snapshot.
        gPanicFileSnapshot.panicInfo = "";
        return gPanicFileSnapshot;
    }
    gPanicFileSnapshot.panicInfo = f.readString();
    f.close();
    return gPanicFileSnapshot;
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
            bleQueueModule.onNotify(event.bytes.data(), event.bytes.size(), event.charUuid);
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

static void sendMetrics(WebServer& server) {
    // Get base perf metrics
    JsonDocument doc;
    
    // Core counters (always available)
    doc["rxPackets"] = perfCounters.rxPackets.load();
    doc["rxBytes"] = perfCounters.rxBytes.load();
    doc["parseSuccesses"] = perfCounters.parseSuccesses.load();
    doc["parseFailures"] = perfCounters.parseFailures.load();
    doc["queueDrops"] = perfCounters.queueDrops.load();
    doc["perfDrop"] = perfCounters.perfDrop.load();
    doc["perfSdLockFail"] = perfCounters.perfSdLockFail.load();
    doc["perfSdDirFail"] = perfCounters.perfSdDirFail.load();
    doc["perfSdOpenFail"] = perfCounters.perfSdOpenFail.load();
    doc["perfSdHeaderFail"] = perfCounters.perfSdHeaderFail.load();
    doc["perfSdMarkerFail"] = perfCounters.perfSdMarkerFail.load();
    doc["perfSdWriteFail"] = perfCounters.perfSdWriteFail.load();
    doc["oversizeDrops"] = perfCounters.oversizeDrops.load();
    doc["queueHighWater"] = perfCounters.queueHighWater.load();
    doc["cmdBleBusy"] = perfCounters.cmdBleBusy.load();
    doc["bleMutexTimeout"] = perfCounters.bleMutexTimeout.load();
    doc["displayUpdates"] = perfCounters.displayUpdates.load();
    doc["displaySkips"] = perfCounters.displaySkips.load();
    doc["audioPlayCount"] = perfCounters.audioPlayCount.load();
    doc["audioPlayBusy"] = perfCounters.audioPlayBusy.load();
    doc["audioTaskFail"] = perfCounters.audioTaskFail.load();
    doc["reconnects"] = perfCounters.reconnects.load();
    doc["disconnects"] = perfCounters.disconnects.load();
    doc["connectionDispatchRuns"] = perfCounters.connectionDispatchRuns.load();
    doc["connectionCadenceDisplayDue"] = perfCounters.connectionCadenceDisplayDue.load();
    doc["connectionCadenceHoldScanDwell"] = perfCounters.connectionCadenceHoldScanDwell.load();
    doc["connectionStateProcessRuns"] = perfCounters.connectionStateProcessRuns.load();
    doc["connectionStateWatchdogForces"] = perfCounters.connectionStateWatchdogForces.load();
    doc["connectionStateProcessGapMaxMs"] = perfCounters.connectionStateProcessGapMaxMs.load();
    doc["bleScanStateEntries"] = perfCounters.bleScanStateEntries.load();
    doc["bleScanStateExits"] = perfCounters.bleScanStateExits.load();
    doc["bleScanTargetFound"] = perfCounters.bleScanTargetFound.load();
    doc["bleScanNoTargetExits"] = perfCounters.bleScanNoTargetExits.load();
    doc["bleScanDwellMaxMs"] = perfCounters.bleScanDwellMaxMs.load();
    doc["uuid128FallbackHits"] = perfCounters.uuid128FallbackHits.load();
    doc["bleDiscTaskCreateFail"] = perfCounters.bleDiscTaskCreateFail.load();
    doc["wifiConnectDeferred"] = perfCounters.wifiConnectDeferred.load();
    doc["wifiStopGraceful"] = perfCounters.wifiStopGraceful.load();
    doc["wifiStopImmediate"] = perfCounters.wifiStopImmediate.load();
    doc["wifiStopManual"] = perfCounters.wifiStopManual.load();
    doc["wifiStopTimeout"] = perfCounters.wifiStopTimeout.load();
    doc["wifiStopNoClients"] = perfCounters.wifiStopNoClients.load();
    doc["wifiStopNoClientsAuto"] = perfCounters.wifiStopNoClientsAuto.load();
    doc["wifiStopLowDma"] = perfCounters.wifiStopLowDma.load();
    doc["wifiStopPoweroff"] = perfCounters.wifiStopPoweroff.load();
    doc["wifiStopOther"] = perfCounters.wifiStopOther.load();
    doc["wifiApDropLowDma"] = perfCounters.wifiApDropLowDma.load();
    doc["wifiApDropIdleSta"] = perfCounters.wifiApDropIdleSta.load();
    doc["wifiApUpTransitions"] = perfCounters.wifiApUpTransitions.load();
    doc["wifiApDownTransitions"] = perfCounters.wifiApDownTransitions.load();
    doc["wifiProcessMaxUs"] = perfCounters.wifiProcessMaxUs.load();
    doc["wifiHandleClientMaxUs"] = perfCounters.wifiHandleClientMaxUs.load();
    doc["wifiMaintenanceMaxUs"] = perfCounters.wifiMaintenanceMaxUs.load();
    doc["wifiStatusCheckMaxUs"] = perfCounters.wifiStatusCheckMaxUs.load();
    doc["wifiTimeoutCheckMaxUs"] = perfCounters.wifiTimeoutCheckMaxUs.load();
    doc["wifiHeapGuardMaxUs"] = perfCounters.wifiHeapGuardMaxUs.load();
    doc["wifiApStaPollMaxUs"] = perfCounters.wifiApStaPollMaxUs.load();
    doc["pushNowRetries"] = perfCounters.pushNowRetries.load();
    doc["pushNowFailures"] = perfCounters.pushNowFailures.load();
    doc["alertPersistStarts"] = perfCounters.alertPersistStarts.load();
    doc["alertPersistExpires"] = perfCounters.alertPersistExpires.load();
    doc["alertPersistClears"] = perfCounters.alertPersistClears.load();
    doc["autoPushStarts"] = perfCounters.autoPushStarts.load();
    doc["autoPushCompletes"] = perfCounters.autoPushCompletes.load();
    doc["autoPushNoProfile"] = perfCounters.autoPushNoProfile.load();
    doc["autoPushProfileLoadFail"] = perfCounters.autoPushProfileLoadFail.load();
    doc["autoPushProfileWriteFail"] = perfCounters.autoPushProfileWriteFail.load();
    doc["autoPushBusyRetries"] = perfCounters.autoPushBusyRetries.load();
    doc["autoPushModeFail"] = perfCounters.autoPushModeFail.load();
    doc["autoPushVolumeFail"] = perfCounters.autoPushVolumeFail.load();
    doc["autoPushDisconnectAbort"] = perfCounters.autoPushDisconnectAbort.load();
    doc["prioritySelectDisplayIndex"] = perfCounters.prioritySelectDisplayIndex.load();
    doc["prioritySelectRowFlag"] = perfCounters.prioritySelectRowFlag.load();
    doc["prioritySelectFirstUsable"] = perfCounters.prioritySelectFirstUsable.load();
    doc["prioritySelectFirstEntry"] = perfCounters.prioritySelectFirstEntry.load();
    doc["prioritySelectAmbiguousIndex"] = perfCounters.prioritySelectAmbiguousIndex.load();
    doc["prioritySelectUnusableIndex"] = perfCounters.prioritySelectUnusableIndex.load();
    doc["prioritySelectInvalidChosen"] = perfCounters.prioritySelectInvalidChosen.load();
    doc["alertTablePublishes"] = perfCounters.alertTablePublishes.load();
    doc["alertTablePublishes3Bogey"] = perfCounters.alertTablePublishes3Bogey.load();
    doc["alertTableRowReplacements"] = perfCounters.alertTableRowReplacements.load();
    doc["alertTableAssemblyTimeouts"] = perfCounters.alertTableAssemblyTimeouts.load();
    doc["parserRowsBandNone"] = perfCounters.parserRowsBandNone.load();
    doc["parserRowsKuRaw"] = perfCounters.parserRowsKuRaw.load();
    doc["displayLiveInvalidPrioritySkips"] = perfCounters.displayLiveInvalidPrioritySkips.load();
    doc["displayLiveFallbackToUsable"] = perfCounters.displayLiveFallbackToUsable.load();
    doc["voiceAnnouncePriority"] = perfCounters.voiceAnnouncePriority.load();
    doc["voiceAnnounceDirection"] = perfCounters.voiceAnnounceDirection.load();
    doc["voiceAnnounceSecondary"] = perfCounters.voiceAnnounceSecondary.load();
    doc["voiceAnnounceEscalation"] = perfCounters.voiceAnnounceEscalation.load();
    doc["voiceDirectionThrottled"] = perfCounters.voiceDirectionThrottled.load();
    doc["powerAutoPowerArmed"] = perfCounters.powerAutoPowerArmed.load();
    doc["powerAutoPowerTimerStart"] = perfCounters.powerAutoPowerTimerStart.load();
    doc["powerAutoPowerTimerCancel"] = perfCounters.powerAutoPowerTimerCancel.load();
    doc["powerAutoPowerTimerExpire"] = perfCounters.powerAutoPowerTimerExpire.load();
    doc["powerCriticalWarn"] = perfCounters.powerCriticalWarn.load();
    doc["powerCriticalShutdown"] = perfCounters.powerCriticalShutdown.load();
    doc["loopMaxUs"] = perfGetLoopMaxUs();
    doc["uptimeMs"] = millis();
    doc["wifiMaxUs"] = perfGetWifiMaxUs();
    doc["fsMaxUs"] = perfGetFsMaxUs();
    doc["sdMaxUs"] = perfGetSdMaxUs();
    doc["flushMaxUs"] = perfGetFlushMaxUs();
    doc["bleDrainMaxUs"] = perfGetBleDrainMaxUs();
    doc["bleProcessMaxUs"] = perfGetBleProcessMaxUs();
    doc["dispPipeMaxUs"] = perfGetDispPipeMaxUs();
    doc["loopMaxPrevWindowUs"] = perfGetPrevWindowLoopMaxUs();
    doc["wifiMaxPrevWindowUs"] = perfGetPrevWindowWifiMaxUs();
    doc["bleProcessMaxPrevWindowUs"] = perfGetPrevWindowBleProcessMaxUs();
    doc["dispPipeMaxPrevWindowUs"] = perfGetPrevWindowDispPipeMaxUs();
    const uint32_t wifiApLastReasonCode = perfGetWifiApLastTransitionReason();
    doc["wifiApActive"] = perfGetWifiApState();
    doc["wifiApLastTransitionMs"] = perfGetWifiApLastTransitionMs();
    doc["wifiApLastTransitionReasonCode"] = wifiApLastReasonCode;
    doc["wifiApLastTransitionReason"] = perfWifiApTransitionReasonName(wifiApLastReasonCode);
    doc["proxyAdvertisingOnTransitions"] = perfCounters.proxyAdvertisingOnTransitions.load();
    doc["proxyAdvertisingOffTransitions"] = perfCounters.proxyAdvertisingOffTransitions.load();
    const uint32_t proxyAdvertisingLastReasonCode = perfGetProxyAdvertisingLastTransitionReason();
    doc["proxyAdvertising"] = perfGetProxyAdvertisingState();
    doc["proxyAdvertisingLastTransitionMs"] = perfGetProxyAdvertisingLastTransitionMs();
    doc["proxyAdvertisingLastTransitionReasonCode"] = proxyAdvertisingLastReasonCode;
    doc["proxyAdvertisingLastTransitionReason"] =
        perfProxyAdvertisingTransitionReasonName(proxyAdvertisingLastReasonCode);

    const uint32_t nowMs = millis();
    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    JsonObject gpsObj = doc["gps"].to<JsonObject>();
    gpsObj["enabled"] = gpsStatus.enabled;
    gpsObj["mode"] = (gpsStatus.parserActive || gpsStatus.moduleDetected || gpsStatus.hardwareSamples > 0)
                         ? "runtime"
                         : "scaffold";
    gpsObj["sampleValid"] = gpsStatus.sampleValid;
    gpsObj["hasFix"] = gpsStatus.hasFix;
    gpsObj["satellites"] = gpsStatus.satellites;
    gpsObj["injectedSamples"] = gpsStatus.injectedSamples;
    gpsObj["moduleDetected"] = gpsStatus.moduleDetected;
    gpsObj["detectionTimedOut"] = gpsStatus.detectionTimedOut;
    gpsObj["parserActive"] = gpsStatus.parserActive;
    gpsObj["hardwareSamples"] = gpsStatus.hardwareSamples;
    gpsObj["bytesRead"] = gpsStatus.bytesRead;
    gpsObj["sentencesSeen"] = gpsStatus.sentencesSeen;
    gpsObj["sentencesParsed"] = gpsStatus.sentencesParsed;
    gpsObj["parseFailures"] = gpsStatus.parseFailures;
    gpsObj["checksumFailures"] = gpsStatus.checksumFailures;
    gpsObj["bufferOverruns"] = gpsStatus.bufferOverruns;
    if (std::isnan(gpsStatus.hdop)) {
        gpsObj["hdop"] = nullptr;
    } else {
        gpsObj["hdop"] = gpsStatus.hdop;
    }
    gpsObj["locationValid"] = gpsStatus.locationValid;
    if (gpsStatus.locationValid) {
        gpsObj["latitude"] = gpsStatus.latitudeDeg;
        gpsObj["longitude"] = gpsStatus.longitudeDeg;
    } else {
        gpsObj["latitude"] = nullptr;
        gpsObj["longitude"] = nullptr;
    }
    gpsObj["courseValid"] = gpsStatus.courseValid;
    if (gpsStatus.courseValid) {
        gpsObj["courseDeg"] = gpsStatus.courseDeg;
        gpsObj["courseSampleTsMs"] = gpsStatus.courseSampleTsMs;
    } else {
        gpsObj["courseDeg"] = nullptr;
        gpsObj["courseSampleTsMs"] = nullptr;
    }
    if (gpsStatus.sampleValid) {
        gpsObj["speedMph"] = gpsStatus.speedMph;
        gpsObj["sampleTsMs"] = gpsStatus.sampleTsMs;
    } else {
        gpsObj["speedMph"] = nullptr;
        gpsObj["sampleTsMs"] = nullptr;
    }
    if (gpsStatus.sampleAgeMs == UINT32_MAX) {
        gpsObj["sampleAgeMs"] = nullptr;
    } else {
        gpsObj["sampleAgeMs"] = gpsStatus.sampleAgeMs;
    }
    if (gpsStatus.courseAgeMs == UINT32_MAX) {
        gpsObj["courseAgeMs"] = nullptr;
    } else {
        gpsObj["courseAgeMs"] = gpsStatus.courseAgeMs;
    }
    if (gpsStatus.lastSentenceTsMs == 0) {
        gpsObj["lastSentenceTsMs"] = nullptr;
    } else {
        gpsObj["lastSentenceTsMs"] = gpsStatus.lastSentenceTsMs;
    }
    const GpsObservationLogStats gpsLogStats = gpsObservationLog.stats();
    JsonObject gpsLogObj = doc["gpsLog"].to<JsonObject>();
    gpsLogObj["published"] = gpsLogStats.published;
    gpsLogObj["drops"] = gpsLogStats.drops;
    gpsLogObj["size"] = static_cast<uint32_t>(gpsLogStats.size);
    gpsLogObj["capacity"] = static_cast<uint32_t>(GpsObservationLog::kCapacity);
    doc["gpsObsDrops"] = gpsLogStats.drops;

    const SpeedSelectorStatus speedStatus = speedSourceSelector.snapshot(nowMs);
    JsonObject speedObj = doc["speedSource"].to<JsonObject>();
    speedObj["gpsEnabled"] = speedStatus.gpsEnabled;
    speedObj["selected"] = SpeedSourceSelector::sourceName(speedStatus.selectedSource);
    if (speedStatus.selectedSource == SpeedSource::NONE) {
        speedObj["selectedMph"] = nullptr;
        speedObj["selectedAgeMs"] = nullptr;
    } else {
        speedObj["selectedMph"] = speedStatus.selectedSpeedMph;
        speedObj["selectedAgeMs"] = speedStatus.selectedAgeMs;
    }
    speedObj["gpsFresh"] = speedStatus.gpsFresh;
    speedObj["gpsMph"] = speedStatus.gpsSpeedMph;
    if (speedStatus.gpsAgeMs == UINT32_MAX) {
        speedObj["gpsAgeMs"] = nullptr;
    } else {
        speedObj["gpsAgeMs"] = speedStatus.gpsAgeMs;
    }
    speedObj["sourceSwitches"] = speedStatus.sourceSwitches;
    speedObj["gpsSelections"] = speedStatus.gpsSelections;
    speedObj["noSourceSelections"] = speedStatus.noSourceSelections;
    
    // Heap stats - both total and DMA-capable (for WiFi/SD contention diagnosis)
    doc["heapFree"] = ESP.getFreeHeap();
    doc["heapMinFree"] = perfGetMinFreeHeap();
    doc["heapLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    // DMA stats use cached values from StorageManager (no extra API calls)
    doc["heapDma"] = StorageManager::getCachedFreeDma();
    doc["heapDmaMin"] = perfGetMinFreeDma();
    doc["heapDmaLargest"] = StorageManager::getCachedLargestDma();
    doc["heapDmaLargestMin"] = perfGetMinLargestDma();
    
    // PSRAM stats
    doc["psramTotal"] = static_cast<uint32_t>(ESP.getPsramSize());
    doc["psramFree"] = static_cast<uint32_t>(ESP.getFreePsram());
    doc["psramLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    // SD access contention stats
    doc["sdTryLockFails"] = StorageManager::sdTryLockFailCount.load();
    doc["sdDmaStarvation"] = StorageManager::sdDmaStarvationCount.load();
    
#if PERF_METRICS
    doc["monitoringEnabled"] = (bool)PERF_MONITORING;
#if PERF_MONITORING
    uint32_t minUsVal = perfLatency.minUs.load();
    uint32_t minUs = (minUsVal == UINT32_MAX) ? 0 : minUsVal;
    doc["latencyMinUs"] = minUs;
    doc["latencyAvgUs"] = perfLatency.avgUs();
    doc["latencyMaxUs"] = perfLatency.maxUs.load();
    doc["latencySamples"] = perfLatency.sampleCount.load();
    doc["debugEnabled"] = perfDebugEnabled;
#else
    doc["latencyMinUs"] = 0;
    doc["latencyAvgUs"] = 0;
    doc["latencyMaxUs"] = 0;
    doc["latencySamples"] = 0;
    doc["debugEnabled"] = false;
#endif
#else
    doc["metricsEnabled"] = false;
#endif
    
    // Add proxy metrics from BLE client
    const ProxyMetrics& proxy = bleClient.getProxyMetrics();
    JsonObject proxyObj = doc["proxy"].to<JsonObject>();
    proxyObj["sendCount"] = proxy.sendCount;
    proxyObj["dropCount"] = proxy.dropCount;
    proxyObj["errorCount"] = proxy.errorCount;
    proxyObj["queueHighWater"] = proxy.queueHighWater;
    proxyObj["connected"] = bleClient.isProxyClientConnected();
    proxyObj["advertising"] = perfGetProxyAdvertisingState() != 0;
    proxyObj["advertisingOnTransitions"] = perfCounters.proxyAdvertisingOnTransitions.load();
    proxyObj["advertisingOffTransitions"] = perfCounters.proxyAdvertisingOffTransitions.load();
    proxyObj["advertisingLastTransitionMs"] = perfGetProxyAdvertisingLastTransitionMs();
    const uint32_t proxyLastReasonCode = perfGetProxyAdvertisingLastTransitionReason();
    proxyObj["advertisingLastTransitionReasonCode"] = proxyLastReasonCode;
    proxyObj["advertisingLastTransitionReason"] =
        perfProxyAdvertisingTransitionReasonName(proxyLastReasonCode);

    // Event-bus health metrics (used to verify no backlog/drop under load).
    JsonObject eventBusObj = doc["eventBus"].to<JsonObject>();
    eventBusObj["publishCount"] = systemEventBus.getPublishCount();
    eventBusObj["dropCount"] = systemEventBus.getDropCount();
    eventBusObj["size"] = static_cast<uint32_t>(systemEventBus.size());

    const V1Settings& settings = settingsManager.get();
    const GpsLockoutCoreGuardStatus lockoutGuard = gpsLockoutEvaluateCoreGuard(
        settings.gpsLockoutCoreGuardEnabled,
        settings.gpsLockoutMaxQueueDrops,
        settings.gpsLockoutMaxPerfDrops,
        settings.gpsLockoutMaxEventBusDrops,
        perfCounters.queueDrops.load(),
        perfCounters.perfDrop.load(),
        systemEventBus.getDropCount());
    JsonObject lockoutObj = doc["lockout"].to<JsonObject>();
    lockoutObj["mode"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    lockoutObj["modeRaw"] = static_cast<int>(settings.gpsLockoutMode);
    lockoutObj["coreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    lockoutObj["coreGuardTripped"] = lockoutGuard.tripped;
    lockoutObj["coreGuardReason"] = lockoutGuard.reason;
    lockoutObj["maxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    lockoutObj["maxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    lockoutObj["maxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    lockoutObj["learnerPromotionHits"] = static_cast<uint32_t>(lockoutLearner.promotionHits());
    lockoutObj["learnerRadiusE5"] = static_cast<uint32_t>(lockoutLearner.radiusE5());
    lockoutObj["learnerFreqToleranceMHz"] = static_cast<uint32_t>(lockoutLearner.freqToleranceMHz());
    lockoutObj["learnerLearnIntervalHours"] = static_cast<uint32_t>(lockoutLearner.learnIntervalHours());
    lockoutObj["learnerUnlearnIntervalHours"] = static_cast<uint32_t>(settings.gpsLockoutLearnerUnlearnIntervalHours);
    lockoutObj["learnerUnlearnCount"] = static_cast<uint32_t>(settings.gpsLockoutLearnerUnlearnCount);
    lockoutObj["manualDemotionMissCount"] = static_cast<uint32_t>(settings.gpsLockoutManualDemotionMissCount);
    lockoutObj["kaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    lockoutObj["enforceRequested"] = (settings.gpsLockoutMode == LOCKOUT_RUNTIME_ENFORCE);
    lockoutObj["enforceAllowed"] = (settings.gpsLockoutMode == LOCKOUT_RUNTIME_ENFORCE) &&
                                   !lockoutGuard.tripped;
    
    sendJsonStream(server, doc);
}

static void sendMetricsSoak(WebServer& server) {
    // Soak mode trims heavyweight diagnostic blocks (GPS/speed snapshots)
    // while preserving all fields consumed by soak_parse_metrics.py.
    JsonDocument doc;

    doc["rxPackets"] = perfCounters.rxPackets.load();
    doc["parseSuccesses"] = perfCounters.parseSuccesses.load();
    doc["parseFailures"] = perfCounters.parseFailures.load();
    doc["queueDrops"] = perfCounters.queueDrops.load();
    doc["perfDrop"] = perfCounters.perfDrop.load();
    doc["oversizeDrops"] = perfCounters.oversizeDrops.load();
    doc["queueHighWater"] = perfCounters.queueHighWater.load();
    doc["bleMutexTimeout"] = perfCounters.bleMutexTimeout.load();
    doc["displayUpdates"] = perfCounters.displayUpdates.load();
    doc["displaySkips"] = perfCounters.displaySkips.load();
    doc["audioPlayCount"] = perfCounters.audioPlayCount.load();
    doc["audioPlayBusy"] = perfCounters.audioPlayBusy.load();
    doc["audioTaskFail"] = perfCounters.audioTaskFail.load();
    doc["reconnects"] = perfCounters.reconnects.load();
    doc["disconnects"] = perfCounters.disconnects.load();
    doc["connectionDispatchRuns"] = perfCounters.connectionDispatchRuns.load();
    doc["connectionCadenceDisplayDue"] = perfCounters.connectionCadenceDisplayDue.load();
    doc["connectionCadenceHoldScanDwell"] = perfCounters.connectionCadenceHoldScanDwell.load();
    doc["connectionStateProcessRuns"] = perfCounters.connectionStateProcessRuns.load();
    doc["connectionStateWatchdogForces"] = perfCounters.connectionStateWatchdogForces.load();
    doc["connectionStateProcessGapMaxMs"] = perfCounters.connectionStateProcessGapMaxMs.load();
    doc["bleScanStateEntries"] = perfCounters.bleScanStateEntries.load();
    doc["bleScanStateExits"] = perfCounters.bleScanStateExits.load();
    doc["bleScanTargetFound"] = perfCounters.bleScanTargetFound.load();
    doc["bleScanNoTargetExits"] = perfCounters.bleScanNoTargetExits.load();
    doc["bleScanDwellMaxMs"] = perfCounters.bleScanDwellMaxMs.load();
    doc["wifiConnectDeferred"] = perfCounters.wifiConnectDeferred.load();
    doc["wifiStopGraceful"] = perfCounters.wifiStopGraceful.load();
    doc["wifiStopImmediate"] = perfCounters.wifiStopImmediate.load();
    doc["wifiStopManual"] = perfCounters.wifiStopManual.load();
    doc["wifiStopTimeout"] = perfCounters.wifiStopTimeout.load();
    doc["wifiStopNoClients"] = perfCounters.wifiStopNoClients.load();
    doc["wifiStopNoClientsAuto"] = perfCounters.wifiStopNoClientsAuto.load();
    doc["wifiStopLowDma"] = perfCounters.wifiStopLowDma.load();
    doc["wifiStopPoweroff"] = perfCounters.wifiStopPoweroff.load();
    doc["wifiStopOther"] = perfCounters.wifiStopOther.load();
    doc["wifiApDropLowDma"] = perfCounters.wifiApDropLowDma.load();
    doc["wifiApDropIdleSta"] = perfCounters.wifiApDropIdleSta.load();
    doc["wifiApUpTransitions"] = perfCounters.wifiApUpTransitions.load();
    doc["wifiApDownTransitions"] = perfCounters.wifiApDownTransitions.load();
    doc["wifiProcessMaxUs"] = perfCounters.wifiProcessMaxUs.load();
    doc["wifiHandleClientMaxUs"] = perfCounters.wifiHandleClientMaxUs.load();
    doc["wifiMaintenanceMaxUs"] = perfCounters.wifiMaintenanceMaxUs.load();
    doc["wifiStatusCheckMaxUs"] = perfCounters.wifiStatusCheckMaxUs.load();
    doc["wifiTimeoutCheckMaxUs"] = perfCounters.wifiTimeoutCheckMaxUs.load();
    doc["wifiHeapGuardMaxUs"] = perfCounters.wifiHeapGuardMaxUs.load();
    doc["wifiApStaPollMaxUs"] = perfCounters.wifiApStaPollMaxUs.load();

    doc["loopMaxUs"] = perfGetLoopMaxUs();
    doc["uptimeMs"] = millis();
    doc["wifiMaxUs"] = perfGetWifiMaxUs();
    doc["fsMaxUs"] = perfGetFsMaxUs();
    doc["sdMaxUs"] = perfGetSdMaxUs();
    doc["flushMaxUs"] = perfGetFlushMaxUs();
    doc["bleDrainMaxUs"] = perfGetBleDrainMaxUs();
    doc["bleProcessMaxUs"] = perfGetBleProcessMaxUs();
    doc["dispPipeMaxUs"] = perfGetDispPipeMaxUs();
    doc["loopMaxPrevWindowUs"] = perfGetPrevWindowLoopMaxUs();
    doc["wifiMaxPrevWindowUs"] = perfGetPrevWindowWifiMaxUs();
    doc["bleProcessMaxPrevWindowUs"] = perfGetPrevWindowBleProcessMaxUs();
    doc["dispPipeMaxPrevWindowUs"] = perfGetPrevWindowDispPipeMaxUs();
    const uint32_t wifiApLastReasonCode = perfGetWifiApLastTransitionReason();
    doc["wifiApActive"] = perfGetWifiApState();
    doc["wifiApLastTransitionMs"] = perfGetWifiApLastTransitionMs();
    doc["wifiApLastTransitionReasonCode"] = wifiApLastReasonCode;
    doc["wifiApLastTransitionReason"] = perfWifiApTransitionReasonName(wifiApLastReasonCode);
    doc["proxyAdvertisingOnTransitions"] = perfCounters.proxyAdvertisingOnTransitions.load();
    doc["proxyAdvertisingOffTransitions"] = perfCounters.proxyAdvertisingOffTransitions.load();
    const uint32_t proxyAdvertisingLastReasonCode = perfGetProxyAdvertisingLastTransitionReason();
    doc["proxyAdvertising"] = perfGetProxyAdvertisingState();
    doc["proxyAdvertisingLastTransitionMs"] = perfGetProxyAdvertisingLastTransitionMs();
    doc["proxyAdvertisingLastTransitionReasonCode"] = proxyAdvertisingLastReasonCode;
    doc["proxyAdvertisingLastTransitionReason"] =
        perfProxyAdvertisingTransitionReasonName(proxyAdvertisingLastReasonCode);

    const GpsObservationLogStats gpsLogStats = gpsObservationLog.stats();
    doc["gpsObsDrops"] = gpsLogStats.drops;

    doc["heapFree"] = ESP.getFreeHeap();
    doc["heapMinFree"] = perfGetMinFreeHeap();
    doc["heapDma"] = StorageManager::getCachedFreeDma();
    doc["heapDmaMin"] = perfGetMinFreeDma();
    doc["heapDmaLargest"] = StorageManager::getCachedLargestDma();
    doc["heapDmaLargestMin"] = perfGetMinLargestDma();

#if PERF_METRICS && PERF_MONITORING
    doc["latencyMaxUs"] = perfLatency.maxUs.load();
#else
    doc["latencyMaxUs"] = 0;
#endif

    const ProxyMetrics& proxy = bleClient.getProxyMetrics();
    JsonObject proxyObj = doc["proxy"].to<JsonObject>();
    proxyObj["dropCount"] = proxy.dropCount;
    proxyObj["advertising"] = perfGetProxyAdvertisingState() != 0;
    proxyObj["advertisingOnTransitions"] = perfCounters.proxyAdvertisingOnTransitions.load();
    proxyObj["advertisingOffTransitions"] = perfCounters.proxyAdvertisingOffTransitions.load();
    proxyObj["advertisingLastTransitionMs"] = perfGetProxyAdvertisingLastTransitionMs();
    const uint32_t proxyLastReasonCode = perfGetProxyAdvertisingLastTransitionReason();
    proxyObj["advertisingLastTransitionReasonCode"] = proxyLastReasonCode;
    proxyObj["advertisingLastTransitionReason"] =
        perfProxyAdvertisingTransitionReasonName(proxyLastReasonCode);

    JsonObject eventBusObj = doc["eventBus"].to<JsonObject>();
    eventBusObj["publishCount"] = systemEventBus.getPublishCount();
    eventBusObj["dropCount"] = systemEventBus.getDropCount();
    eventBusObj["size"] = static_cast<uint32_t>(systemEventBus.size());

    const V1Settings& settings = settingsManager.get();
    const GpsLockoutCoreGuardStatus lockoutGuard = gpsLockoutEvaluateCoreGuard(
        settings.gpsLockoutCoreGuardEnabled,
        settings.gpsLockoutMaxQueueDrops,
        settings.gpsLockoutMaxPerfDrops,
        settings.gpsLockoutMaxEventBusDrops,
        perfCounters.queueDrops.load(),
        perfCounters.perfDrop.load(),
        systemEventBus.getDropCount());
    JsonObject lockoutObj = doc["lockout"].to<JsonObject>();
    lockoutObj["coreGuardTripped"] = lockoutGuard.tripped;

    sendJsonStream(server, doc);
}

void handleApiMetrics(WebServer& server) {
    if (server.hasArg("soak") && isTruthyArgValue(server.arg("soak"))) {
        sendMetricsSoak(server);
        return;
    }
    sendMetrics(server);
}

void handleDebugEnable(WebServer& server) {
    bool enable = true;
    if (server.hasArg("enable")) {
        enable = (server.arg("enable") == "true" || server.arg("enable") == "1");
    }
    perfMetricsSetDebug(enable);
    server.send(200, "application/json", "{\"success\":true,\"debugEnabled\":" + String(enable ? "true" : "false") + "}");
}

void handleApiDebugEnable(WebServer& server,
                          const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleDebugEnable(server);
}

void handleMetricsReset(WebServer& server) {
    // Clear soak-facing counters without touching runtime state/queues.
    perfMetricsReset();
    systemEventBus.resetStats();
    bleClient.resetProxyMetrics();
    server.send(200, "application/json", "{\"success\":true,\"metricsReset\":true}");
}

void handleApiMetricsReset(WebServer& server,
                           const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleMetricsReset(server);
}

void handleCameraAlertRender(WebServer& server) {
#ifdef UNIT_TEST
    server.send(200, "application/json", "{\"success\":true,\"testStub\":true}");
    return;
#else
    static constexpr uint32_t kDebugCameraHoldMsDefault = 5000;
    static constexpr uint32_t kDebugCameraHoldMsMax = 15000;

    CameraType type = CameraType::SPEED;
    if (server.hasArg("type")) {
        type = parseCameraTypeArg(server.arg("type"));
        if (type == CameraType::INVALID) {
            server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid type\"}");
            return;
        }
    }

    uint32_t distanceCm = 16093;
    if (server.hasArg("distanceCm") && !parseUint32Arg(server.arg("distanceCm"), distanceCm)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid distanceCm\"}");
        return;
    }
    if (distanceCm == 0) {
        distanceCm = 1;
    }

    uint32_t holdMs = kDebugCameraHoldMsDefault;
    if (server.hasArg("holdMs") && !parseUint32Arg(server.arg("holdMs"), holdMs)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid holdMs\"}");
        return;
    }
    holdMs = std::min(holdMs, kDebugCameraHoldMsMax);

    DebugCameraVoiceStage voiceStage = DebugCameraVoiceStage::FAR;
    if (server.hasArg("voiceStage") &&
        !parseDebugCameraVoiceStageArg(server.arg("voiceStage"), voiceStage)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid voiceStage\"}");
        return;
    }

    CameraAlertDisplayPayload payload{};
    payload.type = type;
    payload.active = true;
    payload.distanceCm = distanceCm;
    if (!displayPipelineModule.debugRenderCameraPayload(millis(), payload, holdMs)) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"display unavailable\"}");
        return;
    }

    const bool voiceRequested = voiceStage != DebugCameraVoiceStage::NONE;
    const bool voiceStarted =
        voiceRequested ? play_camera_alert(type, voiceStage == DebugCameraVoiceStage::NEAR) : false;

    JsonDocument doc;
    doc["success"] = true;
    doc["type"] = cameraTypeApiName(type);
    doc["distanceCm"] = distanceCm;
    doc["holdMs"] = holdMs;
    doc["voiceStage"] = debugCameraVoiceStageName(voiceStage);
    doc["voiceRequested"] = voiceRequested;
    doc["voiceStarted"] = voiceStarted;
    sendJsonStream(server, doc);
#endif
}

void handleApiCameraAlertRender(WebServer& server,
                                const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleCameraAlertRender(server);
}

void handleCameraAlertClear(WebServer& server) {
#ifdef UNIT_TEST
    server.send(200, "application/json", "{\"success\":true,\"testStub\":true}");
#else
    displayPipelineModule.clearDebugCameraOverride();
    displayPipelineModule.restoreCurrentOwner(millis());

    JsonDocument doc;
    doc["success"] = true;
    doc["debugOverrideCleared"] = true;
    doc["restored"] = true;
    sendJsonStream(server, doc);
#endif
}

void handleApiCameraAlertClear(WebServer& server,
                               const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleCameraAlertClear(server);
}

void handleProxyAdvertisingControl(WebServer& server) {
    JsonDocument body;
    bool hasBody = false;
    if (!parseRequestBody(server, body, hasBody)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON body\"}");
        return;
    }
    const JsonDocument* bodyPtr = hasBody ? &body : nullptr;
    const bool enable = requestBoolArg(server, bodyPtr, "enabled", true);

    const bool ok = bleClient.forceProxyAdvertising(
        enable,
        static_cast<uint8_t>(enable ? PerfProxyAdvertisingTransitionReason::StartDirect
                                    : PerfProxyAdvertisingTransitionReason::StopOther));

    JsonDocument doc;
    doc["success"] = ok;
    doc["requestedEnabled"] = enable;
    doc["advertising"] = bleClient.isProxyAdvertising();
    doc["proxyEnabled"] = bleClient.isProxyEnabled();
    doc["v1Connected"] = bleClient.isConnected();
    doc["wifiPriority"] = bleClient.isWifiPriority();
    doc["proxyClientConnected"] = bleClient.isProxyClientConnected();
    const uint32_t reasonCode = perfGetProxyAdvertisingLastTransitionReason();
    doc["lastTransitionReasonCode"] = reasonCode;
    doc["lastTransitionReason"] = perfProxyAdvertisingTransitionReasonName(reasonCode);
    sendJsonStream(server, doc);
}

void handleApiProxyAdvertisingControl(WebServer& server,
                                      const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleProxyAdvertisingControl(server);
}

void sendPanic(WebServer& server, bool soakMode) {
    // Return last panic info from LittleFS (written by logPanicBreadcrumbs on crash recovery)
    // with a lightweight soak mode that avoids streaming large panic strings.
    JsonDocument doc;
    
    // Get last reset reason
    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "UNKNOWN";
    switch (reason) {
        case ESP_RST_POWERON: reasonStr = "POWERON"; break;
        case ESP_RST_SW: reasonStr = "SW"; break;
        case ESP_RST_PANIC: reasonStr = "PANIC"; break;
        case ESP_RST_INT_WDT: reasonStr = "WDT_INT"; break;
        case ESP_RST_TASK_WDT: reasonStr = "WDT_TASK"; break;
        case ESP_RST_WDT: reasonStr = "WDT"; break;
        case ESP_RST_DEEPSLEEP: reasonStr = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT: reasonStr = "BROWNOUT"; break;
        default: break;
    }
    doc["lastResetReason"] = reasonStr;
    doc["wasCrash"] = (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || 
                       reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT);

    const PanicFileSnapshot& panicSnapshot = getPanicFileSnapshot();
    doc["hasPanicFile"] = panicSnapshot.hasPanicFile;
    if (!soakMode) {
        doc["panicInfo"] = panicSnapshot.panicInfo;
        // Current heap stats for interactive debugging/comparison.
        doc["heapFree"] = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        doc["heapLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        doc["heapMinEver"] = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
        doc["heapDma"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        doc["heapDmaMin"] = perfGetMinFreeDma();
    }

    sendJsonStream(server, doc);
}

void handleApiPanic(WebServer& server) {
    const bool soakMode = server.hasArg("soak") && isTruthyArgValue(server.arg("soak"));
    sendPanic(server, soakMode);
}

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
    JsonDocument doc;
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
    JsonDocument doc;
    doc["success"] = true;
    appendScenarioStatus(doc, millis());
    sendJsonStream(server, doc);
}

void handleV1ScenarioLoad(WebServer& server) {
    JsonDocument body;
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

    JsonDocument doc;
    doc["success"] = true;
    doc["action"] = "load";
    appendScenarioStatus(doc, millis());
    sendJsonStream(server, doc);
}

void handleV1ScenarioStart(WebServer& server) {
    JsonDocument body;
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

    JsonDocument doc;
    doc["success"] = true;
    doc["action"] = "start";
    appendScenarioStatus(doc, millis());
    sendJsonStream(server, doc);
}

void handleV1ScenarioStop(WebServer& server) {
    stopScenarioPlayback();
    JsonDocument doc;
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
                             const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleV1ScenarioLoad(server);
}

void handleApiV1ScenarioStart(WebServer& server,
                              const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleV1ScenarioStart(server);
}

void handleApiV1ScenarioStop(WebServer& server,
                             const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleV1ScenarioStop(server);
}

void handleApiPerfFilesList(WebServer& server,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    DebugPerfFilesService::handleApiPerfFilesList(server, checkRateLimit, markUiActivity);
}

void handleApiPerfFilesDownload(WebServer& server,
                                const std::function<bool()>& checkRateLimit,
                                const std::function<void()>& markUiActivity) {
    DebugPerfFilesService::handleApiPerfFilesDownload(server, checkRateLimit, markUiActivity);
}

void handleApiPerfFilesDelete(WebServer& server,
                              const std::function<bool()>& checkRateLimit,
                              const std::function<void()>& markUiActivity) {
    DebugPerfFilesService::handleApiPerfFilesDelete(server, checkRateLimit, markUiActivity);
}

void process(uint32_t nowMs) {
    pumpScenarioPlayback(nowMs);
}

}  // namespace DebugApiService
