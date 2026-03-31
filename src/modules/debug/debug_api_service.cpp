#include "debug_api_service.h"
#include "debug_perf_files_service.h"
#include "debug_soak_metrics_cache.h"
#include <ArduinoJson.h>
#include "../wifi/wifi_json_document.h"
#include <LittleFS.h>
#include <cmath>
#include <new>
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
#include "debug_api_service_deps.inc"

// --- Dependency storage (shared with debug_api_scenario_service.cpp via deps header) ---
namespace DebugApiService {
namespace deps {
SystemEventBus* eventBus  = nullptr;
V1BLEClient*    bleClient = nullptr;
BleQueueModule* bleQueue  = nullptr;
}  // namespace deps
}  // namespace DebugApiService

void DebugApiService::begin(SystemEventBus* eventBus, V1BLEClient* ble, BleQueueModule* bleQueue) {
    deps::eventBus  = eventBus;
    deps::bleClient = ble;
    deps::bleQueue  = bleQueue;
}

#if defined(__GNUC__)
#define DEBUG_API_NOINLINE __attribute__((noinline))
#else
#define DEBUG_API_NOINLINE
#endif

namespace {
bool isTruthyArgValue(const String& value) {
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "on" || value == "ON";
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
constexpr uint32_t kSoakMetricsCacheTtlMs = 250;
DebugApiService::SoakMetricsJsonCache gSoakMetricsCache;

class MetricsSnapshotScratch {
public:
    MetricsSnapshotScratch() {
        void* storage = heap_caps_malloc(sizeof(PerfRuntimeMetricsSnapshot),
                                         WifiJson::kPsramCaps);
        if (!storage) {
            storage = heap_caps_malloc(sizeof(PerfRuntimeMetricsSnapshot),
                                       WifiJson::kInternalCaps);
        }
        if (storage) {
            snapshot_ = new (storage) PerfRuntimeMetricsSnapshot();
        }
    }

    ~MetricsSnapshotScratch() {
        if (!snapshot_) {
            return;
        }
        snapshot_->~PerfRuntimeMetricsSnapshot();
        heap_caps_free(snapshot_);
    }

    PerfRuntimeMetricsSnapshot* get() const {
        return snapshot_;
    }

    explicit operator bool() const {
        return snapshot_ != nullptr;
    }

private:
    PerfRuntimeMetricsSnapshot* snapshot_ = nullptr;
};

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

void appendSettingsPersistenceMetrics(JsonDocument& doc,
                                      const PerfRuntimeMetricsSnapshot& snapshot) {
    JsonObject persistenceObj = doc["settingsPersistence"].to<JsonObject>();
    persistenceObj["backupRevision"] = snapshot.settingsPersistence.backupRevision;
    persistenceObj["deferredBackupPending"] =
        snapshot.settingsPersistence.deferredBackupPending;
    persistenceObj["deferredBackupRetryScheduled"] =
        snapshot.settingsPersistence.deferredBackupRetryScheduled;
    if (!snapshot.settingsPersistence.deferredBackupHasNextAttempt) {
        persistenceObj["deferredBackupNextAttemptAtMs"] = nullptr;
    } else {
        persistenceObj["deferredBackupNextAttemptAtMs"] =
            snapshot.settingsPersistence.deferredBackupNextAttemptAtMs;
    }
    persistenceObj["deferredBackupDelayMs"] =
        snapshot.settingsPersistence.deferredBackupDelayMs;
    persistenceObj["perfLoggingEnabled"] =
        snapshot.settingsPersistence.perfLoggingEnabled;
    persistenceObj["perfLoggingPath"] = snapshot.settingsPersistence.perfLoggingPath;
}

void appendWifiAutoStartMetrics(JsonDocument& doc,
                                const PerfRuntimeMetricsSnapshot& snapshot) {
    JsonObject wifiAutoStart = doc["wifiAutoStart"].to<JsonObject>();
    wifiAutoStart["gate"] = snapshot.wifiAutoStart.gate;
    wifiAutoStart["gateCode"] = snapshot.wifiAutoStart.gateCode;
    wifiAutoStart["enableWifi"] = snapshot.wifiAutoStart.enableWifi;
    wifiAutoStart["enableWifiAtBoot"] = snapshot.wifiAutoStart.enableWifiAtBoot;
    wifiAutoStart["bleConnected"] = snapshot.wifiAutoStart.bleConnected;
    wifiAutoStart["v1ConnectedAtMs"] = snapshot.wifiAutoStart.v1ConnectedAtMs;
    wifiAutoStart["msSinceV1Connect"] = snapshot.wifiAutoStart.msSinceV1Connect;
    wifiAutoStart["settleMs"] = snapshot.wifiAutoStart.settleMs;
    wifiAutoStart["bootTimeoutMs"] = snapshot.wifiAutoStart.bootTimeoutMs;
    wifiAutoStart["canStartDma"] = snapshot.wifiAutoStart.canStartDma;
    wifiAutoStart["wifiAutoStartDone"] = snapshot.wifiAutoStart.wifiAutoStartDone;
    wifiAutoStart["bleSettled"] = snapshot.wifiAutoStart.bleSettled;
    wifiAutoStart["bootTimeoutReached"] = snapshot.wifiAutoStart.bootTimeoutReached;
    wifiAutoStart["shouldAutoStart"] = snapshot.wifiAutoStart.shouldAutoStart;
    wifiAutoStart["startTriggered"] = snapshot.wifiAutoStart.startTriggered;
    wifiAutoStart["startSucceeded"] = snapshot.wifiAutoStart.startSucceeded;
}

void appendDisplayAttributionMetrics(JsonDocument& doc, const PerfSdSnapshot& flat) {
    doc["displayFullRenderCount"] = flat.displayFullRenderCount;
    doc["displayIncrementalRenderCount"] = flat.displayIncrementalRenderCount;
    doc["displayCardsOnlyRenderCount"] = flat.displayCardsOnlyRenderCount;
    doc["displayRestingFullRenderCount"] = flat.displayRestingFullRenderCount;
    doc["displayRestingIncrementalRenderCount"] = flat.displayRestingIncrementalRenderCount;
    doc["displayPersistedRenderCount"] = flat.displayPersistedRenderCount;
    doc["displayPreviewRenderCount"] = flat.displayPreviewRenderCount;
    doc["displayRestoreRenderCount"] = flat.displayRestoreRenderCount;
    doc["displayLiveScenarioRenderCount"] = flat.displayLiveScenarioRenderCount;
    doc["displayRestingScenarioRenderCount"] = flat.displayRestingScenarioRenderCount;
    doc["displayPersistedScenarioRenderCount"] = flat.displayPersistedScenarioRenderCount;
    doc["displayPreviewScenarioRenderCount"] = flat.displayPreviewScenarioRenderCount;
    doc["displayRestoreScenarioRenderCount"] = flat.displayRestoreScenarioRenderCount;
    doc["displayRedrawReasonFirstRunCount"] = flat.displayRedrawReasonFirstRunCount;
    doc["displayRedrawReasonEnterLiveCount"] = flat.displayRedrawReasonEnterLiveCount;
    doc["displayRedrawReasonLeaveLiveCount"] = flat.displayRedrawReasonLeaveLiveCount;
    doc["displayRedrawReasonLeavePersistedCount"] = flat.displayRedrawReasonLeavePersistedCount;
    doc["displayRedrawReasonForceRedrawCount"] = flat.displayRedrawReasonForceRedrawCount;
    doc["displayRedrawReasonFrequencyChangeCount"] = flat.displayRedrawReasonFrequencyChangeCount;
    doc["displayRedrawReasonBandSetChangeCount"] = flat.displayRedrawReasonBandSetChangeCount;
    doc["displayRedrawReasonArrowChangeCount"] = flat.displayRedrawReasonArrowChangeCount;
    doc["displayRedrawReasonSignalBarChangeCount"] = flat.displayRedrawReasonSignalBarChangeCount;
    doc["displayRedrawReasonVolumeChangeCount"] = flat.displayRedrawReasonVolumeChangeCount;
    doc["displayRedrawReasonBogeyCounterChangeCount"] = flat.displayRedrawReasonBogeyCounterChangeCount;
    doc["displayRedrawReasonRssiRefreshCount"] = flat.displayRedrawReasonRssiRefreshCount;
    doc["displayRedrawReasonFlashTickCount"] = flat.displayRedrawReasonFlashTickCount;
    doc["displayFullFlushCount"] = flat.displayFullFlushCount;
    doc["displayPartialFlushCount"] = flat.displayPartialFlushCount;
    doc["displayPartialFlushAreaPeakPx"] = flat.displayPartialFlushAreaPeakPx;
    doc["displayPartialFlushAreaTotalPx"] = flat.displayPartialFlushAreaTotalPx;
    doc["displayFlushEquivalentAreaTotalPx"] = flat.displayFlushEquivalentAreaTotalPx;
    doc["displayFlushMaxAreaPx"] = flat.displayFlushMaxAreaPx;
    doc["displayBaseFrameMaxUs"] = flat.displayBaseFrameMaxUs;
    doc["displayStatusStripMaxUs"] = flat.displayStatusStripMaxUs;
    doc["displayFrequencyMaxUs"] = flat.displayFrequencyMaxUs;
    doc["displayBandsBarsMaxUs"] = flat.displayBandsBarsMaxUs;
    doc["displayArrowsIconsMaxUs"] = flat.displayArrowsIconsMaxUs;
    doc["displayCardsMaxUs"] = flat.displayCardsMaxUs;
    doc["displayFlushSubphaseMaxUs"] = flat.displayFlushSubphaseMaxUs;
    doc["displayLiveRenderMaxUs"] = flat.displayLiveRenderMaxUs;
    doc["displayRestingRenderMaxUs"] = flat.displayRestingRenderMaxUs;
    doc["displayPersistedRenderMaxUs"] = flat.displayPersistedRenderMaxUs;
    doc["displayPreviewRenderMaxUs"] = flat.displayPreviewRenderMaxUs;
    doc["displayRestoreRenderMaxUs"] = flat.displayRestoreRenderMaxUs;
    doc["displayPreviewFirstRenderMaxUs"] = flat.displayPreviewFirstRenderMaxUs;
    doc["displayPreviewSteadyRenderMaxUs"] = flat.displayPreviewSteadyRenderMaxUs;
}

void appendGpsMetrics(JsonDocument& doc, const PerfRuntimeMetricsSnapshot& snapshot) {
    JsonObject gpsObj = doc["gps"].to<JsonObject>();
    gpsObj["enabled"] = snapshot.gps.enabled;
    gpsObj["mode"] = snapshot.gps.mode;
    gpsObj["sampleValid"] = snapshot.gps.sampleValid;
    gpsObj["hasFix"] = snapshot.gps.hasFix;
    gpsObj["satellites"] = snapshot.gps.satellites;
    gpsObj["injectedSamples"] = snapshot.gps.injectedSamples;
    gpsObj["moduleDetected"] = snapshot.gps.moduleDetected;
    gpsObj["detectionTimedOut"] = snapshot.gps.detectionTimedOut;
    gpsObj["parserActive"] = snapshot.gps.parserActive;
    gpsObj["hardwareSamples"] = snapshot.gps.hardwareSamples;
    gpsObj["bytesRead"] = snapshot.gps.bytesRead;
    gpsObj["sentencesSeen"] = snapshot.gps.sentencesSeen;
    gpsObj["sentencesParsed"] = snapshot.gps.sentencesParsed;
    gpsObj["parseFailures"] = snapshot.gps.parseFailures;
    gpsObj["checksumFailures"] = snapshot.gps.checksumFailures;
    gpsObj["bufferOverruns"] = snapshot.gps.bufferOverruns;
    if (!snapshot.gps.hdopValid) {
        gpsObj["hdop"] = nullptr;
    } else {
        gpsObj["hdop"] = snapshot.gps.hdop;
    }
    gpsObj["locationValid"] = snapshot.gps.locationValid;
    if (snapshot.gps.locationValid) {
        gpsObj["latitude"] = snapshot.gps.latitudeDeg;
        gpsObj["longitude"] = snapshot.gps.longitudeDeg;
    } else {
        gpsObj["latitude"] = nullptr;
        gpsObj["longitude"] = nullptr;
    }
    gpsObj["courseValid"] = snapshot.gps.courseValid;
    if (snapshot.gps.courseValid) {
        gpsObj["courseDeg"] = snapshot.gps.courseDeg;
        gpsObj["courseSampleTsMs"] = snapshot.gps.courseSampleTsMs;
    } else {
        gpsObj["courseDeg"] = nullptr;
        gpsObj["courseSampleTsMs"] = nullptr;
    }
    if (snapshot.gps.sampleValid) {
        gpsObj["speedMph"] = snapshot.gps.speedMph;
        gpsObj["sampleTsMs"] = snapshot.gps.sampleTsMs;
    } else {
        gpsObj["speedMph"] = nullptr;
        gpsObj["sampleTsMs"] = nullptr;
    }
    if (!snapshot.gps.sampleAgeValid) {
        gpsObj["sampleAgeMs"] = nullptr;
    } else {
        gpsObj["sampleAgeMs"] = snapshot.gps.sampleAgeMs;
    }
    if (!snapshot.gps.courseAgeValid) {
        gpsObj["courseAgeMs"] = nullptr;
    } else {
        gpsObj["courseAgeMs"] = snapshot.gps.courseAgeMs;
    }
    if (!snapshot.gps.lastSentenceTsValid) {
        gpsObj["lastSentenceTsMs"] = nullptr;
    } else {
        gpsObj["lastSentenceTsMs"] = snapshot.gps.lastSentenceTsMs;
    }
}

void appendGpsLogMetrics(JsonDocument& doc, const PerfRuntimeMetricsSnapshot& snapshot) {
    JsonObject gpsLogObj = doc["gpsLog"].to<JsonObject>();
    gpsLogObj["published"] = snapshot.gpsLog.published;
    gpsLogObj["drops"] = snapshot.gpsLog.drops;
    gpsLogObj["size"] = snapshot.gpsLog.size;
    gpsLogObj["capacity"] = snapshot.gpsLog.capacity;
}

void appendSpeedSourceMetrics(JsonDocument& doc,
                              const PerfRuntimeMetricsSnapshot& snapshot) {
    JsonObject speedObj = doc["speedSource"].to<JsonObject>();
    speedObj["gpsEnabled"] = snapshot.speedSource.gpsEnabled;
    speedObj["selected"] = snapshot.speedSource.selected;
    if (!snapshot.speedSource.selectedValueValid) {
        speedObj["selectedMph"] = nullptr;
        speedObj["selectedAgeMs"] = nullptr;
    } else {
        speedObj["selectedMph"] = snapshot.speedSource.selectedMph;
        speedObj["selectedAgeMs"] = snapshot.speedSource.selectedAgeMs;
    }
    speedObj["gpsFresh"] = snapshot.speedSource.gpsFresh;
    speedObj["gpsMph"] = snapshot.speedSource.gpsMph;
    if (!snapshot.speedSource.gpsAgeValid) {
        speedObj["gpsAgeMs"] = nullptr;
    } else {
        speedObj["gpsAgeMs"] = snapshot.speedSource.gpsAgeMs;
    }
    speedObj["sourceSwitches"] = snapshot.speedSource.sourceSwitches;
    speedObj["gpsSelections"] = snapshot.speedSource.gpsSelections;
    speedObj["noSourceSelections"] = snapshot.speedSource.noSourceSelections;
}

void appendProxyMetrics(JsonDocument& doc, const PerfRuntimeMetricsSnapshot& snapshot) {
    JsonObject proxyObj = doc["proxy"].to<JsonObject>();
    proxyObj["sendCount"] = snapshot.proxy.sendCount;
    proxyObj["dropCount"] = snapshot.proxy.dropCount;
    proxyObj["errorCount"] = snapshot.proxy.errorCount;
    proxyObj["queueHighWater"] = snapshot.proxy.queueHighWater;
    proxyObj["connected"] = snapshot.proxy.connected;
    proxyObj["advertising"] = snapshot.proxy.advertising;
    proxyObj["advertisingOnTransitions"] = snapshot.proxy.advertisingOnTransitions;
    proxyObj["advertisingOffTransitions"] = snapshot.proxy.advertisingOffTransitions;
    proxyObj["advertisingLastTransitionMs"] = snapshot.proxy.advertisingLastTransitionMs;
    proxyObj["advertisingLastTransitionReasonCode"] =
        snapshot.proxy.advertisingLastTransitionReasonCode;
    proxyObj["advertisingLastTransitionReason"] =
        snapshot.proxy.advertisingLastTransitionReason;
}

void appendEventBusMetrics(JsonDocument& doc, const PerfRuntimeMetricsSnapshot& snapshot) {
    JsonObject eventBusObj = doc["eventBus"].to<JsonObject>();
    eventBusObj["publishCount"] = snapshot.eventBus.publishCount;
    eventBusObj["dropCount"] = snapshot.eventBus.dropCount;
    eventBusObj["size"] = snapshot.eventBus.size;
}

void appendLockoutMetrics(JsonDocument& doc, const PerfRuntimeMetricsSnapshot& snapshot) {
    JsonObject lockoutObj = doc["lockout"].to<JsonObject>();
    lockoutObj["mode"] = snapshot.lockout.mode;
    lockoutObj["modeRaw"] = snapshot.lockout.modeRaw;
    lockoutObj["coreGuardEnabled"] = snapshot.lockout.coreGuardEnabled;
    lockoutObj["coreGuardTripped"] = snapshot.lockout.coreGuardTripped;
    lockoutObj["coreGuardReason"] = snapshot.lockout.coreGuardReason;
    lockoutObj["maxQueueDrops"] = snapshot.lockout.maxQueueDrops;
    lockoutObj["maxPerfDrops"] = snapshot.lockout.maxPerfDrops;
    lockoutObj["maxEventBusDrops"] = snapshot.lockout.maxEventBusDrops;
    lockoutObj["learnerPromotionHits"] = snapshot.lockout.learnerPromotionHits;
    lockoutObj["learnerRadiusE5"] = snapshot.lockout.learnerRadiusE5;
    lockoutObj["learnerFreqToleranceMHz"] = snapshot.lockout.learnerFreqToleranceMHz;
    lockoutObj["learnerLearnIntervalHours"] = snapshot.lockout.learnerLearnIntervalHours;
    lockoutObj["learnerUnlearnIntervalHours"] = snapshot.lockout.learnerUnlearnIntervalHours;
    lockoutObj["learnerUnlearnCount"] = snapshot.lockout.learnerUnlearnCount;
    lockoutObj["manualDemotionMissCount"] = snapshot.lockout.manualDemotionMissCount;
    lockoutObj["kaLearningEnabled"] = snapshot.lockout.kaLearningEnabled;
    lockoutObj["enforceRequested"] = snapshot.lockout.enforceRequested;
    lockoutObj["enforceAllowed"] = snapshot.lockout.enforceAllowed;
}

DEBUG_API_NOINLINE void appendFullMetricsDoc(JsonDocument& doc,
                                             const PerfRuntimeMetricsSnapshot& snapshot) {
    const PerfSdSnapshot& flat = snapshot.flat;
    doc["rxPackets"] = flat.rx;
    doc["rxBytes"] = flat.rxBytes;
    doc["parseSuccesses"] = flat.parseOk;
    doc["parseFailures"] = flat.parseFail;
    doc["queueDrops"] = flat.qDrop;
    doc["perfDrop"] = flat.perfDrop;
    doc["perfSdLockFail"] = snapshot.perfSdLockFail;
    doc["perfSdDirFail"] = snapshot.perfSdDirFail;
    doc["perfSdOpenFail"] = snapshot.perfSdOpenFail;
    doc["perfSdHeaderFail"] = snapshot.perfSdHeaderFail;
    doc["perfSdMarkerFail"] = snapshot.perfSdMarkerFail;
    doc["perfSdWriteFail"] = snapshot.perfSdWriteFail;
    doc["oversizeDrops"] = flat.oversizeDrops;
    doc["queueHighWater"] = flat.queueHighWater;
    doc["phoneCmdDropsOverflow"] = snapshot.phoneCmdDrops.overflow;
    doc["phoneCmdDropsInvalid"] = snapshot.phoneCmdDrops.invalid;
    doc["phoneCmdDropsBleFail"] = snapshot.phoneCmdDrops.bleFail;
    doc["phoneCmdDropsLockBusy"] = snapshot.phoneCmdDrops.lockBusy;
    doc["cmdBleBusy"] = flat.cmdBleBusy;
    doc["bleMutexTimeout"] = flat.bleMutexTimeout;
    doc["displayUpdates"] = flat.displayUpdates;
    doc["displaySkips"] = flat.displaySkips;
    doc["audioPlayCount"] = flat.audioPlayCount;
    doc["audioPlayBusy"] = flat.audioPlayBusy;
    doc["audioTaskFail"] = flat.audioTaskFail;
    doc["bleState"] = snapshot.bleState;
    doc["bleStateCode"] = snapshot.bleStateCode;
    doc["subscribeStep"] = snapshot.subscribeStep;
    doc["subscribeStepCode"] = snapshot.subscribeStepCode;
    doc["connectInProgress"] = snapshot.connectInProgress;
    doc["asyncConnectPending"] = snapshot.asyncConnectPending;
    doc["pendingDisconnectCleanup"] = snapshot.pendingDisconnectCleanup;
    doc["proxyAdvertising"] = snapshot.proxyAdvertising;
    doc["proxyAdvertisingOnTransitions"] = snapshot.proxyAdvertisingOnTransitions;
    doc["proxyAdvertisingOffTransitions"] = snapshot.proxyAdvertisingOffTransitions;
    doc["proxyAdvertisingLastTransitionMs"] = snapshot.proxyAdvertisingLastTransitionMs;
    doc["proxyAdvertisingLastTransitionReasonCode"] = snapshot.proxyAdvertisingLastTransitionReasonCode;
    doc["proxyAdvertisingLastTransitionReason"] = snapshot.proxyAdvertisingLastTransitionReason;
    doc["wifiPriorityMode"] = snapshot.wifiPriorityMode;
    appendWifiAutoStartMetrics(doc, snapshot);
    doc["reconnects"] = flat.reconn;
    doc["disconnects"] = flat.disc;
    doc["connectionDispatchRuns"] = snapshot.connectionDispatchRuns;
    doc["connectionCadenceDisplayDue"] = snapshot.connectionCadenceDisplayDue;
    doc["connectionCadenceHoldScanDwell"] = snapshot.connectionCadenceHoldScanDwell;
    doc["connectionStateProcessRuns"] = snapshot.connectionStateProcessRuns;
    doc["connectionStateWatchdogForces"] = snapshot.connectionStateWatchdogForces;
    doc["connectionStateProcessGapMaxMs"] = snapshot.connectionStateProcessGapMaxMs;
    doc["bleScanStateEntries"] = snapshot.bleScanStateEntries;
    doc["bleScanStateExits"] = snapshot.bleScanStateExits;
    doc["bleScanTargetFound"] = snapshot.bleScanTargetFound;
    doc["bleScanNoTargetExits"] = snapshot.bleScanNoTargetExits;
    doc["bleScanDwellMaxMs"] = snapshot.bleScanDwellMaxMs;
    doc["uuid128FallbackHits"] = snapshot.uuid128FallbackHits;
    doc["bleDiscTaskCreateFail"] = flat.bleDiscTaskCreateFail;
    doc["wifiConnectDeferred"] = flat.wifiConnectDeferred;
    doc["wifiStopGraceful"] = snapshot.wifiStopGraceful;
    doc["wifiStopImmediate"] = snapshot.wifiStopImmediate;
    doc["wifiStopManual"] = snapshot.wifiStopManual;
    doc["wifiStopTimeout"] = snapshot.wifiStopTimeout;
    doc["wifiStopNoClients"] = snapshot.wifiStopNoClients;
    doc["wifiStopNoClientsAuto"] = snapshot.wifiStopNoClientsAuto;
    doc["wifiStopLowDma"] = snapshot.wifiStopLowDma;
    doc["wifiStopPoweroff"] = snapshot.wifiStopPoweroff;
    doc["wifiStopOther"] = snapshot.wifiStopOther;
    doc["wifiApDropLowDma"] = snapshot.wifiApDropLowDma;
    doc["wifiApDropIdleSta"] = snapshot.wifiApDropIdleSta;
    doc["wifiApUpTransitions"] = snapshot.wifiApUpTransitions;
    doc["wifiApDownTransitions"] = snapshot.wifiApDownTransitions;
    doc["wifiProcessMaxUs"] = snapshot.wifiProcessMaxUs;
    doc["wifiHandleClientMaxUs"] = flat.wifiHandleClientMaxUs;
    doc["wifiMaintenanceMaxUs"] = flat.wifiMaintenanceMaxUs;
    doc["wifiStatusCheckMaxUs"] = flat.wifiStatusCheckMaxUs;
    doc["wifiTimeoutCheckMaxUs"] = flat.wifiTimeoutCheckMaxUs;
    doc["wifiHeapGuardMaxUs"] = flat.wifiHeapGuardMaxUs;
    doc["wifiApStaPollMaxUs"] = flat.wifiApStaPollMaxUs;
    doc["wifiStopHttpServerMaxUs"] = flat.wifiStopHttpServerMaxUs;
    doc["wifiStopStaDisconnectMaxUs"] = flat.wifiStopStaDisconnectMaxUs;
    doc["wifiStopApDisableMaxUs"] = flat.wifiStopApDisableMaxUs;
    doc["wifiStopModeOffMaxUs"] = flat.wifiStopModeOffMaxUs;
    doc["wifiStartPreflightMaxUs"] = flat.wifiStartPreflightMaxUs;
    doc["wifiStartApBringupMaxUs"] = flat.wifiStartApBringupMaxUs;
    doc["loopMaxUs"] = flat.loopMaxUs;
    doc["uptimeMs"] = snapshot.uptimeMs;
    doc["wifiMaxUs"] = flat.wifiMaxUs;
    doc["fsMaxUs"] = flat.fsMaxUs;
    doc["sdMaxUs"] = flat.sdMaxUs;
    doc["flushMaxUs"] = flat.flushMaxUs;
    doc["dispMaxUs"] = flat.dispMaxUs;
    doc["bleDrainMaxUs"] = flat.bleDrainMaxUs;
    doc["bleFollowupRequestAlertMaxUs"] = flat.bleFollowupRequestAlertMaxUs;
    doc["bleFollowupRequestVersionMaxUs"] = flat.bleFollowupRequestVersionMaxUs;
    doc["bleConnectStableCallbackMaxUs"] = flat.bleConnectStableCallbackMaxUs;
    doc["bleProxyStartMaxUs"] = flat.bleProxyStartMaxUs;
    doc["bleProcessMaxUs"] = flat.bleProcessMaxUs;
    doc["dispPipeMaxUs"] = flat.dispPipeMaxUs;
    doc["displayVoiceMaxUs"] = flat.displayVoiceMaxUs;
    doc["displayGapRecoverMaxUs"] = flat.displayGapRecoverMaxUs;
    appendDisplayAttributionMetrics(doc, flat);
    doc["obdConnectCallMaxUs"] = flat.obdConnectCallMaxUs;
    doc["obdSecurityStartCallMaxUs"] = flat.obdSecurityStartCallMaxUs;
    doc["obdDiscoveryCallMaxUs"] = flat.obdDiscoveryCallMaxUs;
    doc["obdSubscribeCallMaxUs"] = flat.obdSubscribeCallMaxUs;
    doc["obdWriteCallMaxUs"] = flat.obdWriteCallMaxUs;
    doc["obdRssiCallMaxUs"] = flat.obdRssiCallMaxUs;
    doc["loopMaxPrevWindowUs"] = snapshot.loopMaxPrevWindowUs;
    doc["wifiMaxPrevWindowUs"] = snapshot.wifiMaxPrevWindowUs;
    doc["bleProcessMaxPrevWindowUs"] = snapshot.bleProcessMaxPrevWindowUs;
    doc["dispPipeMaxPrevWindowUs"] = snapshot.dispPipeMaxPrevWindowUs;
    doc["wifiApActive"] = snapshot.wifiApActive;
    doc["wifiApLastTransitionMs"] = snapshot.wifiApLastTransitionMs;
    doc["wifiApLastTransitionReasonCode"] = snapshot.wifiApLastTransitionReasonCode;
    doc["wifiApLastTransitionReason"] = snapshot.wifiApLastTransitionReason;
    appendGpsMetrics(doc, snapshot);
    appendGpsLogMetrics(doc, snapshot);
    doc["gpsObsDrops"] = snapshot.gpsLog.drops;
    appendSettingsPersistenceMetrics(doc, snapshot);
    appendSpeedSourceMetrics(doc, snapshot);
    doc["heapFree"] = snapshot.heap.heapFree;
    doc["heapMinFree"] = snapshot.heap.heapMinFree;
    doc["heapLargest"] = snapshot.heap.heapLargest;
    doc["heapDma"] = snapshot.heap.heapInternalFree;
    doc["heapDmaMin"] = snapshot.heap.heapInternalFreeMin;
    doc["heapDmaLargest"] = snapshot.heap.heapInternalLargest;
    doc["heapDmaLargestMin"] = snapshot.heap.heapInternalLargestMin;
    doc["psramTotal"] = snapshot.psram.total;
    doc["psramFree"] = snapshot.psram.free;
    doc["psramLargest"] = snapshot.psram.largest;
    doc["sdTryLockFails"] = snapshot.sdContention.tryLockFails;
    doc["sdDmaStarvation"] = snapshot.sdContention.dmaStarvation;
    if (!snapshot.metricsEnabled) {
        doc["metricsEnabled"] = false;
    } else {
        doc["monitoringEnabled"] = snapshot.monitoringEnabled;
        doc["latencyMinUs"] = snapshot.latencyMinUs;
        doc["latencyAvgUs"] = snapshot.latencyAvgUs;
        doc["latencyMaxUs"] = snapshot.latencyMaxUs;
        doc["latencySamples"] = snapshot.latencySamples;
        doc["debugEnabled"] = snapshot.debugEnabled;
    }
    appendProxyMetrics(doc, snapshot);
    appendEventBusMetrics(doc, snapshot);
    appendLockoutMetrics(doc, snapshot);
}

void appendSoakMetricsDoc(JsonDocument& doc, const PerfRuntimeMetricsSnapshot& snapshot) {
    const PerfSdSnapshot& flat = snapshot.flat;
    doc["rxPackets"] = flat.rx;
    doc["parseSuccesses"] = flat.parseOk;
    doc["parseFailures"] = flat.parseFail;
    doc["queueDrops"] = flat.qDrop;
    doc["perfDrop"] = flat.perfDrop;
    doc["oversizeDrops"] = flat.oversizeDrops;
    doc["queueHighWater"] = flat.queueHighWater;
    doc["bleMutexTimeout"] = flat.bleMutexTimeout;
    doc["displayUpdates"] = flat.displayUpdates;
    doc["displaySkips"] = flat.displaySkips;
    doc["audioPlayCount"] = flat.audioPlayCount;
    doc["audioPlayBusy"] = flat.audioPlayBusy;
    doc["audioTaskFail"] = flat.audioTaskFail;
    doc["bleState"] = snapshot.bleState;
    doc["bleStateCode"] = snapshot.bleStateCode;
    doc["subscribeStep"] = snapshot.subscribeStep;
    doc["subscribeStepCode"] = snapshot.subscribeStepCode;
    doc["connectInProgress"] = snapshot.connectInProgress;
    doc["asyncConnectPending"] = snapshot.asyncConnectPending;
    doc["pendingDisconnectCleanup"] = snapshot.pendingDisconnectCleanup;
    doc["proxyAdvertising"] = snapshot.proxyAdvertising;
    doc["proxyAdvertisingLastTransitionReason"] = snapshot.proxyAdvertisingLastTransitionReason;
    doc["proxyAdvertisingLastTransitionReasonCode"] = snapshot.proxyAdvertisingLastTransitionReasonCode;
    doc["wifiPriorityMode"] = snapshot.wifiPriorityMode;
    doc["reconnects"] = flat.reconn;
    doc["disconnects"] = flat.disc;
    doc["connectionDispatchRuns"] = snapshot.connectionDispatchRuns;
    doc["connectionCadenceDisplayDue"] = snapshot.connectionCadenceDisplayDue;
    doc["connectionCadenceHoldScanDwell"] = snapshot.connectionCadenceHoldScanDwell;
    doc["connectionStateProcessRuns"] = snapshot.connectionStateProcessRuns;
    doc["connectionStateWatchdogForces"] = snapshot.connectionStateWatchdogForces;
    doc["connectionStateProcessGapMaxMs"] = snapshot.connectionStateProcessGapMaxMs;
    doc["bleScanStateEntries"] = snapshot.bleScanStateEntries;
    doc["bleScanStateExits"] = snapshot.bleScanStateExits;
    doc["bleScanTargetFound"] = snapshot.bleScanTargetFound;
    doc["bleScanNoTargetExits"] = snapshot.bleScanNoTargetExits;
    doc["bleScanDwellMaxMs"] = snapshot.bleScanDwellMaxMs;
    doc["wifiConnectDeferred"] = flat.wifiConnectDeferred;
    doc["wifiStopGraceful"] = snapshot.wifiStopGraceful;
    doc["wifiStopImmediate"] = snapshot.wifiStopImmediate;
    doc["wifiStopManual"] = snapshot.wifiStopManual;
    doc["wifiStopTimeout"] = snapshot.wifiStopTimeout;
    doc["wifiStopNoClients"] = snapshot.wifiStopNoClients;
    doc["wifiStopNoClientsAuto"] = snapshot.wifiStopNoClientsAuto;
    doc["wifiStopLowDma"] = snapshot.wifiStopLowDma;
    doc["wifiStopPoweroff"] = snapshot.wifiStopPoweroff;
    doc["wifiStopOther"] = snapshot.wifiStopOther;
    doc["wifiApDropLowDma"] = snapshot.wifiApDropLowDma;
    doc["wifiApDropIdleSta"] = snapshot.wifiApDropIdleSta;
    doc["wifiApUpTransitions"] = snapshot.wifiApUpTransitions;
    doc["wifiApDownTransitions"] = snapshot.wifiApDownTransitions;
    doc["wifiProcessMaxUs"] = snapshot.wifiProcessMaxUs;
    doc["wifiHandleClientMaxUs"] = flat.wifiHandleClientMaxUs;
    doc["wifiMaintenanceMaxUs"] = flat.wifiMaintenanceMaxUs;
    doc["wifiStatusCheckMaxUs"] = flat.wifiStatusCheckMaxUs;
    doc["wifiTimeoutCheckMaxUs"] = flat.wifiTimeoutCheckMaxUs;
    doc["wifiHeapGuardMaxUs"] = flat.wifiHeapGuardMaxUs;
    doc["wifiApStaPollMaxUs"] = flat.wifiApStaPollMaxUs;
    doc["wifiStopHttpServerMaxUs"] = flat.wifiStopHttpServerMaxUs;
    doc["wifiStopStaDisconnectMaxUs"] = flat.wifiStopStaDisconnectMaxUs;
    doc["wifiStopApDisableMaxUs"] = flat.wifiStopApDisableMaxUs;
    doc["wifiStopModeOffMaxUs"] = flat.wifiStopModeOffMaxUs;
    doc["wifiStartPreflightMaxUs"] = flat.wifiStartPreflightMaxUs;
    doc["wifiStartApBringupMaxUs"] = flat.wifiStartApBringupMaxUs;
    doc["loopMaxUs"] = flat.loopMaxUs;
    doc["uptimeMs"] = snapshot.uptimeMs;
    doc["wifiMaxUs"] = flat.wifiMaxUs;
    doc["fsMaxUs"] = flat.fsMaxUs;
    doc["sdMaxUs"] = flat.sdMaxUs;
    doc["flushMaxUs"] = flat.flushMaxUs;
    doc["dispMaxUs"] = flat.dispMaxUs;
    doc["bleDrainMaxUs"] = flat.bleDrainMaxUs;
    doc["bleFollowupRequestAlertMaxUs"] = flat.bleFollowupRequestAlertMaxUs;
    doc["bleFollowupRequestVersionMaxUs"] = flat.bleFollowupRequestVersionMaxUs;
    doc["bleConnectStableCallbackMaxUs"] = flat.bleConnectStableCallbackMaxUs;
    doc["bleProxyStartMaxUs"] = flat.bleProxyStartMaxUs;
    doc["bleProcessMaxUs"] = flat.bleProcessMaxUs;
    doc["dispPipeMaxUs"] = flat.dispPipeMaxUs;
    doc["displayVoiceMaxUs"] = flat.displayVoiceMaxUs;
    doc["displayGapRecoverMaxUs"] = flat.displayGapRecoverMaxUs;
    appendDisplayAttributionMetrics(doc, flat);
    doc["obdConnectCallMaxUs"] = flat.obdConnectCallMaxUs;
    doc["obdSecurityStartCallMaxUs"] = flat.obdSecurityStartCallMaxUs;
    doc["obdDiscoveryCallMaxUs"] = flat.obdDiscoveryCallMaxUs;
    doc["obdSubscribeCallMaxUs"] = flat.obdSubscribeCallMaxUs;
    doc["obdWriteCallMaxUs"] = flat.obdWriteCallMaxUs;
    doc["obdRssiCallMaxUs"] = flat.obdRssiCallMaxUs;
    doc["loopMaxPrevWindowUs"] = snapshot.loopMaxPrevWindowUs;
    doc["wifiMaxPrevWindowUs"] = snapshot.wifiMaxPrevWindowUs;
    doc["bleProcessMaxPrevWindowUs"] = snapshot.bleProcessMaxPrevWindowUs;
    doc["dispPipeMaxPrevWindowUs"] = snapshot.dispPipeMaxPrevWindowUs;
    doc["wifiApActive"] = snapshot.wifiApActive;
    doc["wifiApLastTransitionMs"] = snapshot.wifiApLastTransitionMs;
    doc["wifiApLastTransitionReasonCode"] = snapshot.wifiApLastTransitionReasonCode;
    doc["wifiApLastTransitionReason"] = snapshot.wifiApLastTransitionReason;
    doc["proxyAdvertisingOnTransitions"] = snapshot.proxyAdvertisingOnTransitions;
    doc["proxyAdvertisingOffTransitions"] = snapshot.proxyAdvertisingOffTransitions;
    doc["proxyAdvertisingLastTransitionMs"] = snapshot.proxyAdvertisingLastTransitionMs;
    doc["gpsObsDrops"] = snapshot.gpsLog.drops;
    doc["heapFree"] = snapshot.heap.heapFree;
    doc["heapMinFree"] = snapshot.heap.heapMinFree;
    doc["heapDma"] = snapshot.heap.heapInternalFree;
    doc["heapDmaMin"] = snapshot.heap.heapInternalFreeMin;
    doc["heapDmaLargest"] = snapshot.heap.heapInternalLargest;
    doc["heapDmaLargestMin"] = snapshot.heap.heapInternalLargestMin;
    doc["latencyMaxUs"] = snapshot.latencyMaxUs;
    appendEventBusMetrics(doc, snapshot);
}
}  // anonymous namespace
namespace DebugApiService {
static void sendMetrics(WebServer& server) {
    // /api/debug/metrics runs on Arduino's loopTask (8 KB stack by default).
    // Keep the large runtime snapshot off-stack so Tier 4 observability work
    // cannot destabilize Tier 1/2 runtime paths during Wi-Fi + BLE coexistence.
    MetricsSnapshotScratch snapshotScratch;
    if (!snapshotScratch) {
        Serial.println("[DebugApi] Metrics snapshot allocation failed");
        server.send(503, "application/json",
                    "{\"error\":\"metrics snapshot alloc failed\"}");
        return;
    }

    perfCaptureRuntimeMetricsSnapshot(*snapshotScratch.get());

    WifiJson::Document doc;
    appendFullMetricsDoc(doc, *snapshotScratch.get());
    sendJsonStream(server, doc);
}

static void buildMetricsSoakDoc(JsonDocument& doc) {
    MetricsSnapshotScratch snapshotScratch;
    if (!snapshotScratch) {
        doc["error"] = "metrics snapshot alloc failed";
        return;
    }

    perfCaptureRuntimeMetricsSnapshot(*snapshotScratch.get());
    appendSoakMetricsDoc(doc, *snapshotScratch.get());
}

static void sendMetricsSoak(WebServer& server) {
    DebugApiService::sendCachedSoakMetrics(
        server,
        gSoakMetricsCache,
        kSoakMetricsCacheTtlMs,
        [](JsonDocument& doc, void* /*ctx*/) {
            buildMetricsSoakDoc(doc);
        }, nullptr,
        [](void* /*ctx*/) -> uint32_t {
            return static_cast<uint32_t>(millis());
        }, nullptr);
}
void handleApiMetrics(WebServer& server) {
    // Guard: refuse to build the large JSON doc if internal DMA heap is low.
    // WiFi STA + BLE dual-role can leave <20KB internal SRAM; the JsonDocument
    // + WiFi TX buffers would starve the display QSPI flush and cause a panic.
    const uint32_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (dmaFree < StorageManager::MIN_DMA_FREE_FOR_SD) {
        server.send(503, "application/json",
                    "{\"error\":\"internal heap too low for metrics\",\"dmaFree\":" + String(dmaFree) + "}");
        return;
    }

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
                          bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleDebugEnable(server);
}
void handleMetricsReset(WebServer& server) {
    // Clear soak-facing counters without touching runtime state/queues.
    perfMetricsReset();
    deps::eventBus->resetStats();
    deps::bleClient->resetProxyMetrics();
    DebugApiService::invalidateSoakMetricsCache(gSoakMetricsCache);
    server.send(200, "application/json", "{\"success\":true,\"metricsReset\":true}");
}
void handleApiMetricsReset(WebServer& server,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleMetricsReset(server);
}
void handleProxyAdvertisingControl(WebServer& server) {
    WifiJson::Document body;
    bool hasBody = false;
    if (!parseRequestBody(server, body, hasBody)) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON body\"}");
        return;
    }
    const JsonDocument* bodyPtr = hasBody ? &body : nullptr;
    const bool enable = requestBoolArg(server, bodyPtr, "enabled", true);
    const bool ok = deps::bleClient->forceProxyAdvertising(
        enable,
        static_cast<uint8_t>(enable ? PerfProxyAdvertisingTransitionReason::StartDirect
                                    : PerfProxyAdvertisingTransitionReason::StopOther));
    WifiJson::Document doc;
    doc["success"] = ok;
    doc["requestedEnabled"] = enable;
    doc["advertising"] = deps::bleClient->isProxyAdvertising();
    doc["proxyEnabled"] = deps::bleClient->isProxyEnabled();
    doc["v1Connected"] = deps::bleClient->isConnected();
    doc["wifiPriority"] = deps::bleClient->isWifiPriority();
    doc["proxyClientConnected"] = deps::bleClient->isProxyClientConnected();
    const uint32_t reasonCode = perfGetProxyAdvertisingLastTransitionReason();
    doc["lastTransitionReasonCode"] = reasonCode;
    doc["lastTransitionReason"] = perfProxyAdvertisingTransitionReasonName(reasonCode);
    sendJsonStream(server, doc);
}
void handleApiProxyAdvertisingControl(WebServer& server,
                                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    handleProxyAdvertisingControl(server);
}
void sendPanic(WebServer& server, bool soakMode) {
    // Return last panic info from LittleFS (written by logPanicBreadcrumbs on crash recovery)
    // with a lightweight soak mode that avoids streaming large panic strings.
    WifiJson::Document doc;
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
                            bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                            void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    DebugPerfFilesService::handleApiPerfFilesList(server, checkRateLimit, rateLimitCtx, markUiActivity, uiActivityCtx);
}
void handleApiPerfFilesDownload(WebServer& server,
                                bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                                void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    DebugPerfFilesService::handleApiPerfFilesDownload(server, checkRateLimit, rateLimitCtx, markUiActivity, uiActivityCtx);
}
void handleApiPerfFilesDelete(WebServer& server,
                              bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                              void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    DebugPerfFilesService::handleApiPerfFilesDelete(server, checkRateLimit, rateLimitCtx, markUiActivity, uiActivityCtx);
}
}  // namespace DebugApiService
