#include "debug_api_service.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cmath>
#include <algorithm>
#include <vector>

#include "../../perf_metrics.h"
#include "../../settings.h"
#include "../../obd_handler.h"
#include "../../ble_client.h"
#include "../../storage_manager.h"
#include "../../perf_sd_logger.h"
#include "../gps/gps_runtime_module.h"
#include "../gps/gps_observation_log.h"
#include "../gps/gps_lockout_safety.h"
#include "../lockout/lockout_learner.h"
#include "../lockout/lockout_band_policy.h"
#include "../speed/speed_source_selector.h"
#include "../system/system_event_bus.h"

// Extern globals (defined in main.cpp / module .cpp files).
extern V1BLEClient bleClient;
extern SystemEventBus systemEventBus;

// Conditionally-compiled perf latency externs (must be at file scope, not inside namespace).
#if PERF_METRICS && PERF_MONITORING
extern PerfLatency perfLatency;
extern bool perfDebugEnabled;
#endif

namespace {

String fileNameFromPath(const String& path) {
    int slash = path.lastIndexOf('/');
    if (slash >= 0) {
        return path.substring(slash + 1);
    }
    return path;
}

bool isValidPerfFileName(const String& name) {
    if (name.length() == 0 || name.length() > 64) {
        return false;
    }
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0) {
        return false;
    }
    if (name == "perf.csv") {
        return true;  // Legacy fallback filename.
    }
    if (!name.startsWith("perf_boot_") || !name.endsWith(".csv")) {
        return false;
    }
    int digitStart = strlen("perf_boot_");
    int digitEnd = name.length() - 4;  // Exclude ".csv"
    if (digitEnd <= digitStart) {
        return false;
    }
    for (int i = digitStart; i < digitEnd; ++i) {
        char c = name.charAt(i);
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

bool perfFilePathFromName(const String& name, String& outPath) {
    if (!isValidPerfFileName(name)) {
        return false;
    }
    outPath = "/perf/" + name;
    return true;
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
    doc["displayUpdates"] = perfCounters.displayUpdates.load();
    doc["displaySkips"] = perfCounters.displaySkips.load();
    doc["reconnects"] = perfCounters.reconnects.load();
    doc["disconnects"] = perfCounters.disconnects.load();
    doc["uuid128FallbackHits"] = perfCounters.uuid128FallbackHits.load();
    doc["bleDiscTaskCreateFail"] = perfCounters.bleDiscTaskCreateFail.load();
    doc["wifiConnectDeferred"] = perfCounters.wifiConnectDeferred.load();
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
    doc["speedVolBoosts"] = perfCounters.speedVolBoosts.load();
    doc["speedVolRestores"] = perfCounters.speedVolRestores.load();
    doc["speedVolFadeTakeovers"] = perfCounters.speedVolFadeTakeovers.load();
    doc["speedVolNoHeadroom"] = perfCounters.speedVolNoHeadroom.load();
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
    doc["wifiMaxUs"] = perfGetWifiMaxUs();
    doc["fsMaxUs"] = perfGetFsMaxUs();
    doc["sdMaxUs"] = perfGetSdMaxUs();
    doc["flushMaxUs"] = perfGetFlushMaxUs();
    doc["bleDrainMaxUs"] = perfGetBleDrainMaxUs();

    // OBD health snapshot (non-blocking lock attempt in OBD handler).
    const OBDPerfSnapshot obdPerf = obdHandler.getPerfSnapshot();
    JsonObject obdObj = doc["obd"].to<JsonObject>();
    obdObj["state"] = obdPerf.state;
    obdObj["connected"] = (obdPerf.connected != 0);
    obdObj["scanActive"] = (obdPerf.scanActive != 0);
    obdObj["hasValidData"] = (obdPerf.hasValidData != 0);
    if (obdPerf.sampleAgeMs == UINT32_MAX) {
        obdObj["sampleAgeMs"] = nullptr;
    } else {
        obdObj["sampleAgeMs"] = obdPerf.sampleAgeMs;
    }
    if (obdPerf.speedMphX10 < 0) {
        obdObj["speedMphX10"] = nullptr;
    } else {
        obdObj["speedMphX10"] = obdPerf.speedMphX10;
    }
    obdObj["connFailures"] = obdPerf.connectionFailures;
    obdObj["pollFailStreak"] = obdPerf.consecutivePollFailures;
    obdObj["notifyDrops"] = obdPerf.notifyDrops;

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

    const SpeedSelectorStatus speedStatus = speedSourceSelector.snapshot(nowMs);
    JsonObject speedObj = doc["speedSource"].to<JsonObject>();
    speedObj["gpsEnabled"] = speedStatus.gpsEnabled;
    speedObj["obdConnected"] = speedStatus.obdConnected;
    speedObj["selected"] = SpeedSourceSelector::sourceName(speedStatus.selectedSource);
    if (speedStatus.selectedSource == SpeedSource::NONE) {
        speedObj["selectedMph"] = nullptr;
        speedObj["selectedAgeMs"] = nullptr;
    } else {
        speedObj["selectedMph"] = speedStatus.selectedSpeedMph;
        speedObj["selectedAgeMs"] = speedStatus.selectedAgeMs;
    }
    speedObj["obdFresh"] = speedStatus.obdFresh;
    speedObj["obdMph"] = speedStatus.obdSpeedMph;
    if (speedStatus.obdAgeMs == UINT32_MAX) {
        speedObj["obdAgeMs"] = nullptr;
    } else {
        speedObj["obdAgeMs"] = speedStatus.obdAgeMs;
    }
    speedObj["gpsFresh"] = speedStatus.gpsFresh;
    speedObj["gpsMph"] = speedStatus.gpsSpeedMph;
    if (speedStatus.gpsAgeMs == UINT32_MAX) {
        speedObj["gpsAgeMs"] = nullptr;
    } else {
        speedObj["gpsAgeMs"] = speedStatus.gpsAgeMs;
    }
    speedObj["sourceSwitches"] = speedStatus.sourceSwitches;
    speedObj["obdSelections"] = speedStatus.obdSelections;
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
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleApiMetrics(WebServer& server) {
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

void sendPanic(WebServer& server) {
    // Return last panic info from LittleFS (written by logPanicBreadcrumbs on crash recovery)
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
    
    // Try to read panic.txt from LittleFS
    String panicContent = "";
    if (LittleFS.exists("/panic.txt")) {
        File f = LittleFS.open("/panic.txt", "r");
        if (f) {
            panicContent = f.readString();
            f.close();
        }
        doc["hasPanicFile"] = true;
    } else {
        doc["hasPanicFile"] = false;
    }
    doc["panicInfo"] = panicContent;
    
    // Current heap stats for comparison
    doc["heapFree"] = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    doc["heapLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    doc["heapMinEver"] = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    doc["heapDma"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    doc["heapDmaMin"] = perfGetMinFreeDma();
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleApiPanic(WebServer& server) {
    sendPanic(server);
}

void sendPerfFilesList(WebServer& server) {
    JsonDocument doc;
    doc["success"] = true;
    doc["storageReady"] = storageManager.isReady();
    doc["onSdCard"] = storageManager.isSDCard();
    doc["path"] = "/perf";

    JsonArray filesArr = doc["files"].to<JsonArray>();

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json);
        return;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        doc["success"] = false;
        doc["error"] = lock.isDmaStarved() ? "Low DMA heap; perf file listing deferred" : "SD busy";
        String json;
        serializeJson(doc, json);
        server.send(503, "application/json", json);
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists("/perf")) {
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json);
        return;
    }

    File dir = fs->open("/perf");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        doc["success"] = false;
        doc["error"] = "Failed to open /perf directory";
        String json;
        serializeJson(doc, json);
        server.send(500, "application/json", json);
        return;
    }

    struct PerfFileInfo {
        String name;
        uint32_t sizeBytes;
        uint32_t bootId;
    };
    std::vector<PerfFileInfo> rows;
    rows.reserve(16);

    File entry;
    while ((entry = dir.openNextFile())) {
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }

        String name = fileNameFromPath(entry.name());
        if (!isValidPerfFileName(name)) {
            entry.close();
            continue;
        }

        uint32_t bootId = 0;
        if (name.startsWith("perf_boot_") && name.endsWith(".csv")) {
            String digits = name.substring(strlen("perf_boot_"), name.length() - 4);
            bootId = static_cast<uint32_t>(strtoul(digits.c_str(), nullptr, 10));
        }

        rows.push_back({name, static_cast<uint32_t>(entry.size()), bootId});
        entry.close();
    }
    dir.close();

    std::sort(rows.begin(), rows.end(), [](const PerfFileInfo& a, const PerfFileInfo& b) {
        if (a.bootId != b.bootId) {
            return a.bootId > b.bootId;
        }
        return a.name > b.name;
    });

    for (const PerfFileInfo& row : rows) {
        JsonObject f = filesArr.add<JsonObject>();
        f["name"] = row.name;
        f["sizeBytes"] = row.sizeBytes;
        f["bootId"] = row.bootId;
        f["active"] = (String("/perf/") + row.name) == String(perfSdLogger.csvPath());
    }
    doc["count"] = static_cast<uint32_t>(rows.size());

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleApiPerfFilesList(WebServer& server,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendPerfFilesList(server);
}

void handlePerfFileDownload(WebServer& server) {
    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing file name\"}");
        return;
    }

    String requestedName = server.arg("name");
    String path;
    if (!perfFilePathFromName(requestedName, path)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid file name\"}");
        return;
    }

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"SD storage unavailable\"}");
        return;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        if (lock.isDmaStarved()) {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"Low DMA heap; try again\"}");
        } else {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"SD busy\"}");
        }
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists(path)) {
        server.send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
        return;
    }

    File f = fs->open(path, FILE_READ);
    if (!f) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Failed to open file\"}");
        return;
    }

    size_t fileSize = f.size();
    server.sendHeader("Content-Type", "text/csv");
    String contentDisposition = String("attachment; filename=\"") + requestedName + "\"";
    server.sendHeader("Content-Disposition", contentDisposition);
    server.sendHeader("Cache-Control", "no-cache");
    server.setContentLength(fileSize);
    server.send(200, "text/csv", "");

    static constexpr size_t CHUNK_SIZE = 4096;
    static uint8_t buffer[CHUNK_SIZE];

    size_t totalSent = 0;
    while (f.available() && server.client().connected()) {
        size_t toRead = min(CHUNK_SIZE, static_cast<size_t>(f.available()));
        size_t bytesRead = f.read(buffer, toRead);
        if (bytesRead > 0) {
            server.client().write(buffer, bytesRead);
            totalSent += bytesRead;
        }
        yield();
        if ((totalSent % 32768) == 0) {
            delay(1);
        }
    }

    f.close();
}

void handleApiPerfFilesDownload(WebServer& server,
                                const std::function<bool()>& checkRateLimit,
                                const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handlePerfFileDownload(server);
}

void handlePerfFileDelete(WebServer& server) {
    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Missing file name\"}");
        return;
    }

    String requestedName = server.arg("name");
    String path;
    if (!perfFilePathFromName(requestedName, path)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid file name\"}");
        return;
    }

    if (!storageManager.isReady() || !storageManager.isSDCard()) {
        server.send(503, "application/json", "{\"success\":false,\"error\":\"SD storage unavailable\"}");
        return;
    }

    StorageManager::SDLockBlocking lock(storageManager.getSDMutex());
    if (!lock) {
        if (lock.isDmaStarved()) {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"Low DMA heap; try again\"}");
        } else {
            server.send(503, "application/json", "{\"success\":false,\"error\":\"SD busy\"}");
        }
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs || !fs->exists(path)) {
        server.send(404, "application/json", "{\"success\":false,\"error\":\"File not found\"}");
        return;
    }

    bool ok = fs->remove(path);

    JsonDocument doc;
    doc["success"] = ok;
    doc["name"] = requestedName;
    if (!ok) {
        doc["error"] = "Delete failed";
    }

    String json;
    serializeJson(doc, json);
    server.send(ok ? 200 : 500, "application/json", json);
}

void handleApiPerfFilesDelete(WebServer& server,
                              const std::function<bool()>& checkRateLimit,
                              const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handlePerfFileDelete(server);
}

}  // namespace DebugApiService
