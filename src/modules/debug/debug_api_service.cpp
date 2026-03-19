#include "debug_api_service.h"
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

void appendFullMetricsDoc(JsonDocument& doc, const PerfRuntimeMetricsSnapshot& snapshot) {
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
    JsonDocument doc;
    PerfRuntimeMetricsSnapshot snapshot{};
    perfCaptureRuntimeMetricsSnapshot(snapshot);
    appendFullMetricsDoc(doc, snapshot);
    sendJsonStream(server, doc);
}

static void buildMetricsSoakDoc(JsonDocument& doc) {
    PerfRuntimeMetricsSnapshot snapshot{};
    perfCaptureRuntimeMetricsSnapshot(snapshot);
    appendSoakMetricsDoc(doc, snapshot);
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
