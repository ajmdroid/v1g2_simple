#include "debug_api_service.h"
#include "debug_metrics_payload.h"
#include "debug_perf_files_service.h"
#include "debug_soak_metrics_cache.h"
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
#include "../../settings.h"
#include "../../ble_client.h"
#include "../../storage_manager.h"
#include "../../perf_sd_logger.h"
#include "../ble/ble_queue_module.h"
#include "../gps/gps_runtime_module.h"
#include "../gps/gps_observation_log.h"
#include "../gps/gps_lockout_safety.h"
#include "../lockout/lockout_learner.h"
#include "../lockout/lockout_band_policy.h"
#include "../speed/speed_source_selector.h"
#include "../system/system_event_bus.h"
#include "../../../include/main_globals.h"
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
constexpr uint32_t kSoakMetricsCacheTtlMs = 250;
DebugApiService::SoakMetricsJsonCache gSoakMetricsCache;
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

void appendSettingsPersistenceMetrics(JsonDocument& doc, uint32_t nowMs) {
    JsonObject persistenceObj = doc["settingsPersistence"].to<JsonObject>();
    persistenceObj["backupRevision"] = settingsManager.backupRevision();
    persistenceObj["deferredBackupPending"] = settingsManager.deferredBackupPending();
    persistenceObj["deferredBackupRetryScheduled"] =
        settingsManager.deferredBackupRetryScheduled();
    const uint32_t nextAttemptAtMs = settingsManager.deferredBackupNextAttemptAtMs();
    if (nextAttemptAtMs == 0) {
        persistenceObj["deferredBackupNextAttemptAtMs"] = nullptr;
        persistenceObj["deferredBackupDelayMs"] = 0;
    } else {
        persistenceObj["deferredBackupNextAttemptAtMs"] = nextAttemptAtMs;
        persistenceObj["deferredBackupDelayMs"] =
            (static_cast<int32_t>(nextAttemptAtMs - nowMs) > 0)
                ? (nextAttemptAtMs - nowMs)
                : 0;
    }
    persistenceObj["perfLoggingEnabled"] = perfSdLogger.isEnabled();
    persistenceObj["perfLoggingPath"] = perfSdLogger.csvPath();
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
    appendBleRuntimeMetricsPayload(doc);
    appendWifiAutoStartMetricsPayload(doc);
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
    doc["dispMaxUs"] = perfGetDisplayRenderMaxUs();
    doc["bleDrainMaxUs"] = perfGetBleDrainMaxUs();
    doc["bleFollowupRequestAlertMaxUs"] = perfGetBleFollowupRequestAlertMaxUs();
    doc["bleFollowupRequestVersionMaxUs"] = perfGetBleFollowupRequestVersionMaxUs();
    doc["bleConnectStableCallbackMaxUs"] = perfGetBleConnectStableCallbackMaxUs();
    doc["bleProxyStartMaxUs"] = perfGetBleProxyStartMaxUs();
    doc["bleProcessMaxUs"] = perfGetBleProcessMaxUs();
    doc["dispPipeMaxUs"] = perfGetDispPipeMaxUs();
    doc["displayVoiceMaxUs"] = perfGetDisplayVoiceMaxUs();
    doc["displayGapRecoverMaxUs"] = perfGetDisplayGapRecoverMaxUs();
    doc["obdConnectCallMaxUs"] = perfGetObdConnectCallMaxUs();
    doc["obdSecurityStartCallMaxUs"] = perfGetObdSecurityStartCallMaxUs();
    doc["obdDiscoveryCallMaxUs"] = perfGetObdDiscoveryCallMaxUs();
    doc["obdSubscribeCallMaxUs"] = perfGetObdSubscribeCallMaxUs();
    doc["obdWriteCallMaxUs"] = perfGetObdWriteCallMaxUs();
    doc["obdRssiCallMaxUs"] = perfGetObdRssiCallMaxUs();
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
    appendSettingsPersistenceMetrics(doc, nowMs);
    const SpeedSelectorStatus speedStatus = speedSourceSelector.snapshot();
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
static void buildMetricsSoakDoc(JsonDocument& doc) {
    // Soak mode trims heavyweight diagnostic blocks (GPS/speed snapshots)
    // while preserving all fields consumed by soak_parse_metrics.py.
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
    appendBleRuntimeMetricsPayload(doc);
    appendWifiAutoStartMetricsPayload(doc);
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
    doc["dispMaxUs"] = perfGetDisplayRenderMaxUs();
    doc["bleDrainMaxUs"] = perfGetBleDrainMaxUs();
    doc["bleFollowupRequestAlertMaxUs"] = perfGetBleFollowupRequestAlertMaxUs();
    doc["bleFollowupRequestVersionMaxUs"] = perfGetBleFollowupRequestVersionMaxUs();
    doc["bleConnectStableCallbackMaxUs"] = perfGetBleConnectStableCallbackMaxUs();
    doc["bleProxyStartMaxUs"] = perfGetBleProxyStartMaxUs();
    doc["bleProcessMaxUs"] = perfGetBleProcessMaxUs();
    doc["dispPipeMaxUs"] = perfGetDispPipeMaxUs();
    doc["displayVoiceMaxUs"] = perfGetDisplayVoiceMaxUs();
    doc["displayGapRecoverMaxUs"] = perfGetDisplayGapRecoverMaxUs();
    doc["obdConnectCallMaxUs"] = perfGetObdConnectCallMaxUs();
    doc["obdSecurityStartCallMaxUs"] = perfGetObdSecurityStartCallMaxUs();
    doc["obdDiscoveryCallMaxUs"] = perfGetObdDiscoveryCallMaxUs();
    doc["obdSubscribeCallMaxUs"] = perfGetObdSubscribeCallMaxUs();
    doc["obdWriteCallMaxUs"] = perfGetObdWriteCallMaxUs();
    doc["obdRssiCallMaxUs"] = perfGetObdRssiCallMaxUs();
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
    const uint32_t nowMs = millis();
    appendSettingsPersistenceMetrics(doc, nowMs);
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
    lockoutObj["coreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    lockoutObj["coreGuardTripped"] = lockoutGuard.tripped;
    lockoutObj["coreGuardReason"] = lockoutGuard.reason;
    lockoutObj["maxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    lockoutObj["maxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    lockoutObj["maxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
}
static void sendMetricsSoak(WebServer& server) {
    DebugApiService::sendCachedSoakMetrics(
        server,
        gSoakMetricsCache,
        kSoakMetricsCacheTtlMs,
        [](JsonDocument& doc) {
            buildMetricsSoakDoc(doc);
        },
        []() {
            return static_cast<uint32_t>(millis());
        });
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
    DebugApiService::invalidateSoakMetricsCache(gSoakMetricsCache);
    server.send(200, "application/json", "{\"success\":true,\"metricsReset\":true}");
}
void handleApiMetricsReset(WebServer& server,
                           const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleMetricsReset(server);
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
}  // namespace DebugApiService
