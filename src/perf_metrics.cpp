/**
 * Low-Overhead Performance Metrics Implementation
 */

#include "perf_metrics.h"
#include "ble_client.h"
#include "perf_sd_logger.h"
#include "storage_manager.h"
#include "time_service.h"
#include "settings.h"
#include "../include/main_globals.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_observation_log.h"
#include "modules/gps/gps_lockout_safety.h"
#include "modules/lockout/lockout_band_policy.h"
#include "modules/lockout/lockout_learner.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/system/system_event_bus.h"
#include "modules/wifi/wifi_auto_start_module.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <cmath>

// Global instances
PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
extern SystemEventBus  systemEventBus;
extern GpsRuntimeModule  gpsRuntimeModule;
extern GpsObservationLog gpsObservationLog;
extern ObdRuntimeModule  obdRuntimeModule;
extern SpeedSourceSelector speedSourceSelector;
extern LockoutLearner    lockoutLearner;

#if PERF_METRICS
PerfLatency perfLatency;
#endif

#if PERF_METRICS && PERF_MONITORING
bool perfDebugEnabled = false;
uint32_t perfLastReportMs = 0;
#endif

// Session minima for true MALLOC_CAP_DMA heap (updated only in sampled snapshot path).
static uint32_t sDmaFreeCapMin = UINT32_MAX;
static uint32_t sDmaLargestCapMin = UINT32_MAX;
static std::atomic<uint32_t> sPrevWindowLoopMaxUs{0};
static std::atomic<uint32_t> sPrevWindowWifiMaxUs{0};
static std::atomic<uint32_t> sPrevWindowBleProcessMaxUs{0};
static std::atomic<uint32_t> sPrevWindowDispPipeMaxUs{0};
static std::atomic<uint8_t> sDisplayRenderScenario{
    static_cast<uint8_t>(PerfDisplayRenderScenario::None)};

void perfMetricsInit() {
    perfCounters.reset();
    perfExtended.reset();
    sDmaFreeCapMin = UINT32_MAX;
    sDmaLargestCapMin = UINT32_MAX;
    sPrevWindowLoopMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowWifiMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowBleProcessMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowDispPipeMaxUs.store(0, std::memory_order_relaxed);
    sDisplayRenderScenario.store(
        static_cast<uint8_t>(PerfDisplayRenderScenario::None), std::memory_order_relaxed);
#if PERF_METRICS
    perfLatency.reset();
#if PERF_MONITORING
    perfDebugEnabled = false;
    perfLastReportMs = millis();
#endif
#endif
}

void perfMetricsReset() {
    perfCounters.reset();
    perfExtended.reset();
    sDmaFreeCapMin = UINT32_MAX;
    sDmaLargestCapMin = UINT32_MAX;
    sPrevWindowLoopMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowWifiMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowBleProcessMaxUs.store(0, std::memory_order_relaxed);
    sPrevWindowDispPipeMaxUs.store(0, std::memory_order_relaxed);
    sDisplayRenderScenario.store(
        static_cast<uint8_t>(PerfDisplayRenderScenario::None), std::memory_order_relaxed);
#if PERF_METRICS
    perfLatency.reset();
#endif
}

namespace {
static constexpr uint32_t kLatencyBucketsMs[PerfHistogramMs::kBucketCount] = {
    1, 2, 5, 10, 20, 50, 100, 200, 500, 1000
};
// Keep aligned with UI scan dwell target so "fast exit" remains actionable.
static constexpr uint32_t kFastScanExitThresholdMs = 400;
static portMUX_TYPE sPerfSnapshotMux = portMUX_INITIALIZER_UNLOCKED;

static PerfDisplayRenderScenario currentDisplayRenderScenario() {
    return static_cast<PerfDisplayRenderScenario>(
        sDisplayRenderScenario.load(std::memory_order_relaxed));
}

static void recordDisplayScenarioRenderCount(PerfDisplayRenderScenario scenario) {
    switch (scenario) {
        case PerfDisplayRenderScenario::Live:
            perfExtended.displayLiveScenarioRenderCount++;
            break;
        case PerfDisplayRenderScenario::Resting:
            perfExtended.displayRestingScenarioRenderCount++;
            break;
        case PerfDisplayRenderScenario::Persisted:
            perfExtended.displayPersistedScenarioRenderCount++;
            break;
        case PerfDisplayRenderScenario::PreviewFirstFrame:
        case PerfDisplayRenderScenario::PreviewSteadyFrame:
            perfExtended.displayPreviewScenarioRenderCount++;
            break;
        case PerfDisplayRenderScenario::Restore:
            perfExtended.displayRestoreScenarioRenderCount++;
            break;
        case PerfDisplayRenderScenario::None:
        default:
            break;
    }
}

static void recordDisplayScenarioRenderMax(PerfDisplayRenderScenario scenario, uint32_t us) {
    switch (scenario) {
        case PerfDisplayRenderScenario::Live:
            if (us > perfExtended.displayLiveRenderMaxUs) {
                perfExtended.displayLiveRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::Resting:
            if (us > perfExtended.displayRestingRenderMaxUs) {
                perfExtended.displayRestingRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::Persisted:
            if (us > perfExtended.displayPersistedRenderMaxUs) {
                perfExtended.displayPersistedRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::PreviewFirstFrame:
            if (us > perfExtended.displayPreviewRenderMaxUs) {
                perfExtended.displayPreviewRenderMaxUs = us;
            }
            if (us > perfExtended.displayPreviewFirstRenderMaxUs) {
                perfExtended.displayPreviewFirstRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::PreviewSteadyFrame:
            if (us > perfExtended.displayPreviewRenderMaxUs) {
                perfExtended.displayPreviewRenderMaxUs = us;
            }
            if (us > perfExtended.displayPreviewSteadyRenderMaxUs) {
                perfExtended.displayPreviewSteadyRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::Restore:
            if (us > perfExtended.displayRestoreRenderMaxUs) {
                perfExtended.displayRestoreRenderMaxUs = us;
            }
            break;
        case PerfDisplayRenderScenario::None:
        default:
            break;
    }
}

static void addLatencySample(PerfHistogramMs& hist, uint32_t ms) {
    if (ms > hist.maxMs) {
        hist.maxMs = ms;
    }
    // Always increment total - values > max bucket go into overflow
    hist.total++;
    for (size_t i = 0; i < PerfHistogramMs::kBucketCount; ++i) {
        if (ms <= kLatencyBucketsMs[i]) {
            hist.buckets[i]++;
            return;
        }
    }
    // Value exceeds all buckets - counted in total but not in any bucket
    // calcP95 will return maxMs for these overflow cases
    hist.overflow++;
}

static uint32_t calcP95(const PerfHistogramMs& hist) {
    if (hist.total == 0) {
        return 0;
    }
    uint32_t target = (hist.total * 95 + 99) / 100;
    uint32_t cumulative = 0;
    for (size_t i = 0; i < PerfHistogramMs::kBucketCount; ++i) {
        cumulative += hist.buckets[i];
        if (cumulative >= target) {
            return kLatencyBucketsMs[i];
        }
    }
    return hist.maxMs;
}

static const char* wifiApTransitionReasonNameInternal(uint32_t reasonCode) {
    switch (static_cast<PerfWifiApTransitionReason>(reasonCode)) {
        case PerfWifiApTransitionReason::Startup:
            return "startup";
        case PerfWifiApTransitionReason::StopManual:
            return "stop_manual";
        case PerfWifiApTransitionReason::StopTimeout:
            return "stop_timeout";
        case PerfWifiApTransitionReason::StopNoClients:
            return "stop_no_clients";
        case PerfWifiApTransitionReason::StopNoClientsAuto:
            return "stop_no_clients_auto";
        case PerfWifiApTransitionReason::DropLowDma:
            return "drop_low_dma";
        case PerfWifiApTransitionReason::DropIdleSta:
            return "drop_idle_sta";
        case PerfWifiApTransitionReason::StopPoweroff:
            return "stop_poweroff";
        case PerfWifiApTransitionReason::StopOther:
            return "stop_other";
        case PerfWifiApTransitionReason::Unknown:
        default:
            return "unknown";
    }
}

static const char* proxyAdvertisingTransitionReasonNameInternal(uint32_t reasonCode) {
    switch (static_cast<PerfProxyAdvertisingTransitionReason>(reasonCode)) {
        case PerfProxyAdvertisingTransitionReason::StartConnected:
            return "start_connected";
        case PerfProxyAdvertisingTransitionReason::StartWifiPriorityResume:
            return "start_wifi_priority_resume";
        case PerfProxyAdvertisingTransitionReason::StartRetryWindow:
            return "start_retry_window";
        case PerfProxyAdvertisingTransitionReason::StartAppDisconnect:
            return "start_app_disconnect";
        case PerfProxyAdvertisingTransitionReason::StartDirect:
            return "start_direct";
        case PerfProxyAdvertisingTransitionReason::StopWifiPriority:
            return "stop_wifi_priority";
        case PerfProxyAdvertisingTransitionReason::StopNoClientTimeout:
            return "stop_no_client_timeout";
        case PerfProxyAdvertisingTransitionReason::StopIdleWindow:
            return "stop_idle_window";
        case PerfProxyAdvertisingTransitionReason::StopBeforeV1Connect:
            return "stop_before_v1_connect";
        case PerfProxyAdvertisingTransitionReason::StopV1Disconnect:
            return "stop_v1_disconnect";
        case PerfProxyAdvertisingTransitionReason::StopAppConnected:
            return "stop_app_connected";
        case PerfProxyAdvertisingTransitionReason::StopOther:
            return "stop_other";
        case PerfProxyAdvertisingTransitionReason::Unknown:
        default:
            return "unknown";
    }
}

struct RuntimeSnapshotCaptureContext {
    uint32_t nowMs = 0;
    uint32_t freeHeap = 0;
    uint32_t largestHeap = 0;
    uint32_t freeDma = 0;
    uint32_t largestDma = 0;
    uint32_t freeDmaCap = 0;
    uint32_t largestDmaCap = 0;
    uint32_t psramTotal = 0;
    uint32_t psramFree = 0;
    uint32_t psramLargest = 0;
    GpsRuntimeStatus gpsStatus = {};
    GpsObservationLogStats gpsLogStats = {};
    ObdRuntimeStatus obdStatus = {};
    SpeedSelectorStatus speedStatus = {};
    WifiAutoStartDecisionSnapshot wifiAutoStart = {};
    ProxyMetrics proxyMetrics = {};
    uint32_t eventBusPublishCount = 0;
    uint32_t eventBusDropCount = 0;
    uint32_t eventBusSize = 0;
    PhoneCmdDropMetricsSnapshot phoneCmdDropMetrics = {};
    const V1Settings* settings = nullptr;
    GpsLockoutCoreGuardStatus lockoutGuard = {};
    uint32_t backupRevision = 0;
    bool deferredBackupPending = false;
    bool deferredBackupRetryScheduled = false;
    uint32_t deferredBackupNextAttemptAtMs = 0;
    bool perfLoggingEnabled = false;
    const char* perfLoggingPath = "";
    uint32_t sdTryLockFails = 0;
    uint32_t sdDmaStarvation = 0;
};

static RuntimeSnapshotCaptureContext captureRuntimeSnapshotContext() {
    RuntimeSnapshotCaptureContext ctx{};
    ctx.nowMs = millis();
    ctx.freeHeap = ESP.getFreeHeap();
    ctx.largestHeap = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ctx.freeDma = StorageManager::getCachedFreeDma();
    ctx.largestDma = StorageManager::getCachedLargestDma();
    ctx.freeDmaCap = heap_caps_get_free_size(MALLOC_CAP_DMA);
    ctx.largestDmaCap = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    ctx.psramTotal = static_cast<uint32_t>(ESP.getPsramSize());
    ctx.psramFree = static_cast<uint32_t>(ESP.getFreePsram());
    ctx.psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ctx.gpsStatus = gpsRuntimeModule.snapshot(ctx.nowMs);
    ctx.gpsLogStats = gpsObservationLog.stats();
    ctx.obdStatus = obdRuntimeModule.snapshot(ctx.nowMs);
    ctx.speedStatus = speedSourceSelector.snapshot();
    ctx.wifiAutoStart = wifiAutoStartModule.getLastDecision();
    ctx.proxyMetrics = bleClient.getProxyMetrics();
    ctx.eventBusPublishCount = systemEventBus.getPublishCount();
    ctx.eventBusDropCount = systemEventBus.getDropCount();
    ctx.eventBusSize = static_cast<uint32_t>(systemEventBus.size());
    ctx.phoneCmdDropMetrics = perfPhoneCmdDropMetricsSnapshot();
    ctx.settings = &settingsManager.get();
    ctx.lockoutGuard = gpsLockoutEvaluateCoreGuard(
        ctx.settings->gpsLockoutCoreGuardEnabled,
        ctx.settings->gpsLockoutMaxQueueDrops,
        ctx.settings->gpsLockoutMaxPerfDrops,
        ctx.settings->gpsLockoutMaxEventBusDrops,
        perfCounters.queueDrops.load(std::memory_order_relaxed),
        perfCounters.perfDrop.load(std::memory_order_relaxed),
        ctx.eventBusDropCount);
    ctx.backupRevision = settingsManager.backupRevision();
    ctx.deferredBackupPending = settingsManager.deferredBackupPending();
    ctx.deferredBackupRetryScheduled = settingsManager.deferredBackupRetryScheduled();
    ctx.deferredBackupNextAttemptAtMs = settingsManager.deferredBackupNextAttemptAtMs();
    ctx.perfLoggingEnabled = perfSdLogger.isEnabled();
    ctx.perfLoggingPath = perfSdLogger.csvPath();
    ctx.sdTryLockFails = StorageManager::sdTryLockFailCount.load(std::memory_order_relaxed);
    ctx.sdDmaStarvation = StorageManager::sdDmaStarvationCount.load(std::memory_order_relaxed);
    return ctx;
}

static void populateFlatSnapshot(PerfSdSnapshot& flat,
                                 const RuntimeSnapshotCaptureContext& ctx,
                                 PerfRuntimeSnapshotMode mode) {
    flat = {};
    flat.millisTs = ctx.nowMs;
    flat.timeValid = timeService.timeValid() ? 1 : 0;
    flat.timeSource = timeService.timeSource();
    flat.freeHeap = ctx.freeHeap;
    flat.freeDma = ctx.freeDma;
    flat.largestDma = ctx.largestDma;
    flat.freeDmaCap = ctx.freeDmaCap;
    flat.largestDmaCap = ctx.largestDmaCap;

    flat.rx = perfCounters.rxPackets.load(std::memory_order_relaxed);
    flat.qDrop = perfCounters.queueDrops.load(std::memory_order_relaxed);
    flat.perfDrop = perfCounters.perfDrop.load(std::memory_order_relaxed);
    flat.eventBusDrops = ctx.eventBusDropCount;
    flat.parseOk = perfCounters.parseSuccesses.load(std::memory_order_relaxed);
    flat.parseFail = perfCounters.parseFailures.load(std::memory_order_relaxed);
    flat.disc = perfCounters.disconnects.load(std::memory_order_relaxed);
    flat.reconn = perfCounters.reconnects.load(std::memory_order_relaxed);

    flat.alertPersistStarts = perfCounters.alertPersistStarts.load(std::memory_order_relaxed);
    flat.alertPersistExpires = perfCounters.alertPersistExpires.load(std::memory_order_relaxed);
    flat.alertPersistClears = perfCounters.alertPersistClears.load(std::memory_order_relaxed);
    flat.autoPushStarts = perfCounters.autoPushStarts.load(std::memory_order_relaxed);
    flat.autoPushCompletes = perfCounters.autoPushCompletes.load(std::memory_order_relaxed);
    flat.autoPushNoProfile = perfCounters.autoPushNoProfile.load(std::memory_order_relaxed);
    flat.autoPushProfileLoadFail = perfCounters.autoPushProfileLoadFail.load(std::memory_order_relaxed);
    flat.autoPushProfileWriteFail = perfCounters.autoPushProfileWriteFail.load(std::memory_order_relaxed);
    flat.autoPushBusyRetries = perfCounters.autoPushBusyRetries.load(std::memory_order_relaxed);
    flat.autoPushModeFail = perfCounters.autoPushModeFail.load(std::memory_order_relaxed);
    flat.autoPushVolumeFail = perfCounters.autoPushVolumeFail.load(std::memory_order_relaxed);
    flat.autoPushDisconnectAbort = perfCounters.autoPushDisconnectAbort.load(std::memory_order_relaxed);
    flat.prioritySelectDisplayIndex = perfCounters.prioritySelectDisplayIndex.load(std::memory_order_relaxed);
    flat.prioritySelectRowFlag = perfCounters.prioritySelectRowFlag.load(std::memory_order_relaxed);
    flat.prioritySelectFirstUsable = perfCounters.prioritySelectFirstUsable.load(std::memory_order_relaxed);
    flat.prioritySelectFirstEntry = perfCounters.prioritySelectFirstEntry.load(std::memory_order_relaxed);
    flat.prioritySelectAmbiguousIndex = perfCounters.prioritySelectAmbiguousIndex.load(std::memory_order_relaxed);
    flat.prioritySelectUnusableIndex = perfCounters.prioritySelectUnusableIndex.load(std::memory_order_relaxed);
    flat.prioritySelectInvalidChosen = perfCounters.prioritySelectInvalidChosen.load(std::memory_order_relaxed);
    flat.alertTablePublishes = perfCounters.alertTablePublishes.load(std::memory_order_relaxed);
    flat.alertTablePublishes3Bogey = perfCounters.alertTablePublishes3Bogey.load(std::memory_order_relaxed);
    flat.alertTableRowReplacements = perfCounters.alertTableRowReplacements.load(std::memory_order_relaxed);
    flat.alertTableAssemblyTimeouts = perfCounters.alertTableAssemblyTimeouts.load(std::memory_order_relaxed);
    flat.parserRowsBandNone = perfCounters.parserRowsBandNone.load(std::memory_order_relaxed);
    flat.parserRowsKuRaw = perfCounters.parserRowsKuRaw.load(std::memory_order_relaxed);
    flat.displayLiveInvalidPrioritySkips = perfCounters.displayLiveInvalidPrioritySkips.load(std::memory_order_relaxed);
    flat.displayLiveFallbackToUsable = perfCounters.displayLiveFallbackToUsable.load(std::memory_order_relaxed);
    flat.voiceAnnouncePriority = perfCounters.voiceAnnouncePriority.load(std::memory_order_relaxed);
    flat.voiceAnnounceDirection = perfCounters.voiceAnnounceDirection.load(std::memory_order_relaxed);
    flat.voiceAnnounceSecondary = perfCounters.voiceAnnounceSecondary.load(std::memory_order_relaxed);
    flat.voiceAnnounceEscalation = perfCounters.voiceAnnounceEscalation.load(std::memory_order_relaxed);
    flat.voiceDirectionThrottled = perfCounters.voiceDirectionThrottled.load(std::memory_order_relaxed);
    flat.powerAutoPowerArmed = perfCounters.powerAutoPowerArmed.load(std::memory_order_relaxed);
    flat.powerAutoPowerTimerStart = perfCounters.powerAutoPowerTimerStart.load(std::memory_order_relaxed);
    flat.powerAutoPowerTimerCancel = perfCounters.powerAutoPowerTimerCancel.load(std::memory_order_relaxed);
    flat.powerAutoPowerTimerExpire = perfCounters.powerAutoPowerTimerExpire.load(std::memory_order_relaxed);
    flat.powerCriticalWarn = perfCounters.powerCriticalWarn.load(std::memory_order_relaxed);
    flat.powerCriticalShutdown = perfCounters.powerCriticalShutdown.load(std::memory_order_relaxed);
    flat.cmdBleBusy = perfCounters.cmdBleBusy.load(std::memory_order_relaxed);
    flat.gpsEnabled = ctx.gpsStatus.enabled ? 1 : 0;
    flat.gpsHasFix = ctx.gpsStatus.hasFix ? 1 : 0;
    flat.gpsLocationValid = ctx.gpsStatus.locationValid ? 1 : 0;
    flat.gpsSatellites = ctx.gpsStatus.satellites;
    flat.gpsParserActive = ctx.gpsStatus.parserActive ? 1 : 0;
    flat.gpsModuleDetected = ctx.gpsStatus.moduleDetected ? 1 : 0;
    flat.gpsDetectionTimedOut = ctx.gpsStatus.detectionTimedOut ? 1 : 0;
    flat.gpsSpeedMphX10 =
        (ctx.gpsStatus.sampleValid && std::isfinite(ctx.gpsStatus.speedMph))
            ? static_cast<int32_t>(std::lround(ctx.gpsStatus.speedMph * 10.0f))
            : -1;
    flat.gpsHdopX10 =
        std::isfinite(ctx.gpsStatus.hdop)
            ? static_cast<uint16_t>(std::lround(((ctx.gpsStatus.hdop < 0.0f) ? 0.0f : ctx.gpsStatus.hdop) * 10.0f))
            : UINT16_MAX;
    flat.gpsSampleAgeMs = ctx.gpsStatus.sampleAgeMs;
    flat.gpsObsDrops = ctx.gpsLogStats.drops;
    flat.gpsObsSize = static_cast<uint32_t>(ctx.gpsLogStats.size);
    flat.gpsObsPublished = ctx.gpsLogStats.published;

    flat.rxBytes = perfCounters.rxBytes.load(std::memory_order_relaxed);
    flat.oversizeDrops = perfCounters.oversizeDrops.load(std::memory_order_relaxed);
    flat.queueHighWater = perfCounters.queueHighWater.load(std::memory_order_relaxed);
    flat.bleMutexSkip = perfCounters.bleMutexSkip.load(std::memory_order_relaxed);
    flat.bleMutexTimeout = perfCounters.bleMutexTimeout.load(std::memory_order_relaxed);
    flat.cmdPaceNotYet = perfCounters.cmdPaceNotYet.load(std::memory_order_relaxed);
    flat.bleDiscTaskCreateFail = perfCounters.bleDiscTaskCreateFail.load(std::memory_order_relaxed);
    flat.displayUpdates = perfCounters.displayUpdates.load(std::memory_order_relaxed);
    flat.displaySkips = perfCounters.displaySkips.load(std::memory_order_relaxed);
    flat.wifiConnectDeferred = perfCounters.wifiConnectDeferred.load(std::memory_order_relaxed);
    flat.pushNowRetries = perfCounters.pushNowRetries.load(std::memory_order_relaxed);
    flat.pushNowFailures = perfCounters.pushNowFailures.load(std::memory_order_relaxed);
    flat.audioPlayCount = perfCounters.audioPlayCount.load(std::memory_order_relaxed);
    flat.audioPlayBusy = perfCounters.audioPlayBusy.load(std::memory_order_relaxed);
    flat.audioTaskFail = perfCounters.audioTaskFail.load(std::memory_order_relaxed);
    flat.sigObsQueueDrops = perfCounters.sigObsQueueDrops.load(std::memory_order_relaxed);
    flat.sigObsWriteFail = perfCounters.sigObsWriteFail.load(std::memory_order_relaxed);
    flat.freeDmaMin = (perfExtended.minFreeDma == UINT32_MAX) ? ctx.freeDma : perfExtended.minFreeDma;
    flat.largestDmaMin =
        (perfExtended.minLargestDma == UINT32_MAX) ? ctx.largestDma : perfExtended.minLargestDma;
    flat.bleState = bleClient.getBLEStateCode();
    flat.subscribeStep = bleClient.getSubscribeStepCode();
    flat.connectInProgress = bleClient.isConnectInProgress() ? 1 : 0;
    flat.asyncConnectPending = bleClient.isAsyncConnectPending() ? 1 : 0;
    flat.pendingDisconnectCleanup = bleClient.hasPendingDisconnectCleanup() ? 1 : 0;
    flat.proxyAdvertising = perfGetProxyAdvertisingState() != 0 ? 1 : 0;
    flat.proxyAdvertisingLastTransitionReason =
        static_cast<uint8_t>(perfGetProxyAdvertisingLastTransitionReason());
    flat.wifiPriorityMode = bleClient.isWifiPriority() ? 1 : 0;

    portENTER_CRITICAL(&sPerfSnapshotMux);
    const uint32_t dmaFreeMin = [&]() {
        if (mode == PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks &&
            ctx.freeDmaCap < sDmaFreeCapMin) {
            sDmaFreeCapMin = ctx.freeDmaCap;
        }
        return (sDmaFreeCapMin == UINT32_MAX) ? ctx.freeDmaCap : sDmaFreeCapMin;
    }();
    const uint32_t dmaLargestMin = [&]() {
        if (mode == PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks &&
            ctx.largestDmaCap < sDmaLargestCapMin) {
            sDmaLargestCapMin = ctx.largestDmaCap;
        }
        return (sDmaLargestCapMin == UINT32_MAX) ? ctx.largestDmaCap : sDmaLargestCapMin;
    }();
    flat.dmaFreeMin = dmaFreeMin;
    flat.dmaLargestMin = dmaLargestMin;

    flat.loopMaxUs = perfExtended.loopMaxUs;
    flat.bleDrainMaxUs = perfExtended.bleDrainMaxUs;
    flat.dispMaxUs = perfExtended.displayRenderMaxUs;
    flat.bleProcessMaxUs = perfExtended.bleProcessMaxUs;
    flat.touchMaxUs = perfExtended.touchMaxUs;
    flat.obdMaxUs = perfExtended.obdMaxUs;
    flat.obdConnectCallMaxUs = perfExtended.obdConnectCallMaxUs;
    flat.obdSecurityStartCallMaxUs = perfExtended.obdSecurityStartCallMaxUs;
    flat.obdDiscoveryCallMaxUs = perfExtended.obdDiscoveryCallMaxUs;
    flat.obdSubscribeCallMaxUs = perfExtended.obdSubscribeCallMaxUs;
    flat.obdWriteCallMaxUs = perfExtended.obdWriteCallMaxUs;
    flat.obdRssiCallMaxUs = perfExtended.obdRssiCallMaxUs;
    flat.obdPollErrors = ctx.obdStatus.pollErrors;
    flat.obdStaleCount = ctx.obdStatus.staleSpeedCount;
    flat.obdVinDetected = ctx.obdStatus.vinDetected ? 1 : 0;
    flat.obdVehicleFamily = static_cast<uint8_t>(ctx.obdStatus.vehicleFamily);
    flat.obdEotValid = ctx.obdStatus.eotValid ? 1 : 0;
    flat.obdEotC_x10 = ctx.obdStatus.eotValid ? ctx.obdStatus.eotC_x10 : 0;
    flat.obdEotAgeMs = ctx.obdStatus.eotValid ? ctx.obdStatus.eotAgeMs : UINT32_MAX;
    flat.obdEotProfileId = static_cast<uint8_t>(ctx.obdStatus.eotProfileId);
    flat.obdEotProbeFailures = ctx.obdStatus.eotProbeFailures;
    flat.gpsMaxUs = perfExtended.gpsMaxUs;
    flat.lockoutMaxUs = perfExtended.lockoutMaxUs;
    flat.wifiMaxUs = perfExtended.wifiMaxUs;
    flat.wifiHandleClientMaxUs = perfExtended.wifiHandleClientMaxUs;
    flat.wifiMaintenanceMaxUs = perfExtended.wifiMaintenanceMaxUs;
    flat.wifiStatusCheckMaxUs = perfExtended.wifiStatusCheckMaxUs;
    flat.wifiTimeoutCheckMaxUs = perfExtended.wifiTimeoutCheckMaxUs;
    flat.wifiHeapGuardMaxUs = perfExtended.wifiHeapGuardMaxUs;
    flat.wifiApStaPollMaxUs = perfExtended.wifiApStaPollMaxUs;
    flat.wifiStopHttpServerMaxUs = perfExtended.wifiStopHttpServerMaxUs;
    flat.wifiStopStaDisconnectMaxUs = perfExtended.wifiStopStaDisconnectMaxUs;
    flat.wifiStopApDisableMaxUs = perfExtended.wifiStopApDisableMaxUs;
    flat.wifiStopModeOffMaxUs = perfExtended.wifiStopModeOffMaxUs;
    flat.wifiStartPreflightMaxUs = perfExtended.wifiStartPreflightMaxUs;
    flat.wifiStartApBringupMaxUs = perfExtended.wifiStartApBringupMaxUs;
    flat.fsMaxUs = perfExtended.fsMaxUs;
    flat.sdMaxUs = perfExtended.sdMaxUs;
    flat.sdWriteCount = perfExtended.sdWriteCount;
    flat.sdWriteLt1msCount = perfExtended.sdWriteLt1msCount;
    flat.sdWrite1to5msCount = perfExtended.sdWrite1to5msCount;
    flat.sdWrite5to10msCount = perfExtended.sdWrite5to10msCount;
    flat.sdWriteGe10msCount = perfExtended.sdWriteGe10msCount;
    flat.flushMaxUs = perfExtended.flushMaxUs;
    flat.bleConnectMaxUs = perfExtended.bleConnectMaxUs;
    flat.bleDiscoveryMaxUs = perfExtended.bleDiscoveryMaxUs;
    flat.bleSubscribeMaxUs = perfExtended.bleSubscribeMaxUs;
    flat.dispPipeMaxUs = perfExtended.dispPipeMaxUs;
    flat.lockoutSaveMaxUs = perfExtended.lockoutSaveMaxUs;
    flat.learnerSaveMaxUs = perfExtended.learnerSaveMaxUs;
    flat.timeSaveMaxUs = perfExtended.timeSaveMaxUs;
    flat.perfReportMaxUs = perfExtended.perfReportMaxUs;
    flat.minLargestBlock =
        (perfExtended.minLargestBlock == UINT32_MAX) ? 0 : perfExtended.minLargestBlock;

    flat.uiToScanCount = perfExtended.uiToScanCount;
    flat.uiToRestCount = perfExtended.uiToRestCount;
    flat.uiScanToRestCount = perfExtended.uiScanToRestCount;
    flat.uiFastScanExitCount = perfExtended.uiFastScanExitCount;
    flat.uiLastScanDwellMs = perfExtended.uiLastScanDwellMs;
    flat.uiMinScanDwellMs =
        (perfExtended.uiMinScanDwellMs == UINT32_MAX) ? 0 : perfExtended.uiMinScanDwellMs;
    flat.fadeDownCount = perfExtended.fadeDownCount;
    flat.fadeRestoreCount = perfExtended.fadeRestoreCount;
    flat.fadeSkipEqualCount = perfExtended.fadeSkipEqualCount;
    flat.fadeSkipNoBaselineCount = perfExtended.fadeSkipNoBaselineCount;
    flat.fadeSkipNotFadedCount = perfExtended.fadeSkipNotFadedCount;
    flat.fadeLastDecision = perfExtended.fadeLastDecision;
    flat.fadeLastCurrentVol = perfExtended.fadeLastCurrentVol;
    flat.fadeLastOriginalVol = perfExtended.fadeLastOriginalVol;
    flat.fadeLastDecisionMs = perfExtended.fadeLastDecisionMs;
    flat.preQuietDropCount = perfExtended.preQuietDropCount;
    flat.preQuietRestoreCount = perfExtended.preQuietRestoreCount;
    flat.preQuietRestoreRetryCount = perfExtended.preQuietRestoreRetryCount;
    flat.speedVolDropCount = perfExtended.speedVolDropCount;
    flat.speedVolRestoreCount = perfExtended.speedVolRestoreCount;
    flat.speedVolRetryCount = perfExtended.speedVolRetryCount;
    flat.bleScanStartMs = perfExtended.bleScanStartMs;
    flat.bleTargetFoundMs = perfExtended.bleTargetFoundMs;
    flat.bleConnectStartMs = perfExtended.bleConnectStartMs;
    flat.bleConnectedMs = perfExtended.bleConnectedMs;
    flat.bleFirstRxMs = perfExtended.bleFirstRxMs;
    flat.bleFollowupRequestAlertMaxUs = perfExtended.bleFollowupRequestAlertMaxUs;
    flat.bleFollowupRequestVersionMaxUs = perfExtended.bleFollowupRequestVersionMaxUs;
    flat.bleConnectStableCallbackMaxUs = perfExtended.bleConnectStableCallbackMaxUs;
    flat.bleProxyStartMaxUs = perfExtended.bleProxyStartMaxUs;
    flat.displayVoiceMaxUs = perfExtended.displayVoiceMaxUs;
    flat.displayGapRecoverMaxUs = perfExtended.displayGapRecoverMaxUs;
    flat.displayFullRenderCount = perfExtended.displayFullRenderCount;
    flat.displayIncrementalRenderCount = perfExtended.displayIncrementalRenderCount;
    flat.displayCardsOnlyRenderCount = perfExtended.displayCardsOnlyRenderCount;
    flat.displayRestingFullRenderCount = perfExtended.displayRestingFullRenderCount;
    flat.displayRestingIncrementalRenderCount = perfExtended.displayRestingIncrementalRenderCount;
    flat.displayPersistedRenderCount = perfExtended.displayPersistedRenderCount;
    flat.displayPreviewRenderCount = perfExtended.displayPreviewRenderCount;
    flat.displayRestoreRenderCount = perfExtended.displayRestoreRenderCount;
    flat.displayLiveScenarioRenderCount = perfExtended.displayLiveScenarioRenderCount;
    flat.displayRestingScenarioRenderCount = perfExtended.displayRestingScenarioRenderCount;
    flat.displayPersistedScenarioRenderCount = perfExtended.displayPersistedScenarioRenderCount;
    flat.displayPreviewScenarioRenderCount = perfExtended.displayPreviewScenarioRenderCount;
    flat.displayRestoreScenarioRenderCount = perfExtended.displayRestoreScenarioRenderCount;
    flat.displayRedrawReasonFirstRunCount = perfExtended.displayRedrawReasonFirstRunCount;
    flat.displayRedrawReasonEnterLiveCount = perfExtended.displayRedrawReasonEnterLiveCount;
    flat.displayRedrawReasonLeaveLiveCount = perfExtended.displayRedrawReasonLeaveLiveCount;
    flat.displayRedrawReasonLeavePersistedCount = perfExtended.displayRedrawReasonLeavePersistedCount;
    flat.displayRedrawReasonForceRedrawCount = perfExtended.displayRedrawReasonForceRedrawCount;
    flat.displayRedrawReasonFrequencyChangeCount = perfExtended.displayRedrawReasonFrequencyChangeCount;
    flat.displayRedrawReasonBandSetChangeCount = perfExtended.displayRedrawReasonBandSetChangeCount;
    flat.displayRedrawReasonArrowChangeCount = perfExtended.displayRedrawReasonArrowChangeCount;
    flat.displayRedrawReasonSignalBarChangeCount = perfExtended.displayRedrawReasonSignalBarChangeCount;
    flat.displayRedrawReasonVolumeChangeCount = perfExtended.displayRedrawReasonVolumeChangeCount;
    flat.displayRedrawReasonBogeyCounterChangeCount = perfExtended.displayRedrawReasonBogeyCounterChangeCount;
    flat.displayRedrawReasonRssiRefreshCount = perfExtended.displayRedrawReasonRssiRefreshCount;
    flat.displayRedrawReasonFlashTickCount = perfExtended.displayRedrawReasonFlashTickCount;
    flat.displayFullFlushCount = perfExtended.displayFullFlushCount;
    flat.displayPartialFlushCount = perfExtended.displayPartialFlushCount;
    flat.displayFlushBatchCount = perfExtended.displayFlushBatchCount;
    flat.displayPartialFlushAreaPeakPx = perfExtended.displayPartialFlushAreaPeakPx;
    flat.displayPartialFlushAreaTotalPx = perfExtended.displayPartialFlushAreaTotalPx;
    flat.displayFlushEquivalentAreaTotalPx = perfExtended.displayFlushEquivalentAreaTotalPx;
    flat.displayFlushMaxAreaPx = perfExtended.displayFlushMaxAreaPx;
    flat.displayBaseFrameMaxUs = perfExtended.displayBaseFrameMaxUs;
    flat.displayStatusStripMaxUs = perfExtended.displayStatusStripMaxUs;
    flat.displayFrequencyMaxUs = perfExtended.displayFrequencyMaxUs;
    flat.displayBandsBarsMaxUs = perfExtended.displayBandsBarsMaxUs;
    flat.displayArrowsIconsMaxUs = perfExtended.displayArrowsIconsMaxUs;
    flat.displayCardsMaxUs = perfExtended.displayCardsMaxUs;
    flat.displayFlushSubphaseMaxUs = perfExtended.displayFlushSubphaseMaxUs;
    flat.displayLiveRenderMaxUs = perfExtended.displayLiveRenderMaxUs;
    flat.displayRestingRenderMaxUs = perfExtended.displayRestingRenderMaxUs;
    flat.displayPersistedRenderMaxUs = perfExtended.displayPersistedRenderMaxUs;
    flat.displayPreviewRenderMaxUs = perfExtended.displayPreviewRenderMaxUs;
    flat.displayRestoreRenderMaxUs = perfExtended.displayRestoreRenderMaxUs;
    flat.displayPreviewFirstRenderMaxUs = perfExtended.displayPreviewFirstRenderMaxUs;
    flat.displayPreviewSteadyRenderMaxUs = perfExtended.displayPreviewSteadyRenderMaxUs;

    if (mode == PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks) {
        sPrevWindowLoopMaxUs.store(flat.loopMaxUs, std::memory_order_relaxed);
        sPrevWindowWifiMaxUs.store(flat.wifiMaxUs, std::memory_order_relaxed);
        sPrevWindowBleProcessMaxUs.store(flat.bleProcessMaxUs, std::memory_order_relaxed);
        sPrevWindowDispPipeMaxUs.store(flat.dispPipeMaxUs, std::memory_order_relaxed);

        perfExtended.loopMaxUs = 0;
        perfExtended.bleDrainMaxUs = 0;
        perfExtended.displayRenderMaxUs = 0;
        perfExtended.dispPipeMaxUs = 0;
        perfExtended.bleProcessMaxUs = 0;
        perfExtended.bleFollowupRequestAlertMaxUs = 0;
        perfExtended.bleFollowupRequestVersionMaxUs = 0;
        perfExtended.bleConnectStableCallbackMaxUs = 0;
        perfExtended.bleProxyStartMaxUs = 0;
        perfExtended.displayVoiceMaxUs = 0;
        perfExtended.displayGapRecoverMaxUs = 0;
        perfExtended.displayPartialFlushAreaPeakPx = 0;
        perfExtended.displayFlushMaxAreaPx = 0;
        perfExtended.displayBaseFrameMaxUs = 0;
        perfExtended.displayStatusStripMaxUs = 0;
        perfExtended.displayFrequencyMaxUs = 0;
        perfExtended.displayBandsBarsMaxUs = 0;
        perfExtended.displayArrowsIconsMaxUs = 0;
        perfExtended.displayCardsMaxUs = 0;
        perfExtended.displayFlushSubphaseMaxUs = 0;
        perfExtended.displayLiveRenderMaxUs = 0;
        perfExtended.displayRestingRenderMaxUs = 0;
        perfExtended.displayPersistedRenderMaxUs = 0;
        perfExtended.displayPreviewRenderMaxUs = 0;
        perfExtended.displayRestoreRenderMaxUs = 0;
        perfExtended.displayPreviewFirstRenderMaxUs = 0;
        perfExtended.displayPreviewSteadyRenderMaxUs = 0;
        perfExtended.touchMaxUs = 0;
        perfExtended.obdMaxUs = 0;
        perfExtended.obdConnectCallMaxUs = 0;
        perfExtended.obdSecurityStartCallMaxUs = 0;
        perfExtended.obdDiscoveryCallMaxUs = 0;
        perfExtended.obdSubscribeCallMaxUs = 0;
        perfExtended.obdWriteCallMaxUs = 0;
        perfExtended.obdRssiCallMaxUs = 0;
        perfExtended.gpsMaxUs = 0;
        perfExtended.lockoutMaxUs = 0;
        perfExtended.wifiMaxUs = 0;
        perfExtended.wifiHandleClientMaxUs = 0;
        perfExtended.wifiMaintenanceMaxUs = 0;
        perfExtended.wifiStatusCheckMaxUs = 0;
        perfExtended.wifiTimeoutCheckMaxUs = 0;
        perfExtended.wifiHeapGuardMaxUs = 0;
        perfExtended.wifiApStaPollMaxUs = 0;
        perfExtended.wifiStopHttpServerMaxUs = 0;
        perfExtended.wifiStopStaDisconnectMaxUs = 0;
        perfExtended.wifiStopApDisableMaxUs = 0;
        perfExtended.wifiStopModeOffMaxUs = 0;
        perfExtended.wifiStartPreflightMaxUs = 0;
        perfExtended.wifiStartApBringupMaxUs = 0;
        perfExtended.fsMaxUs = 0;
        perfExtended.sdMaxUs = 0;
        perfExtended.flushMaxUs = 0;
        perfExtended.bleConnectMaxUs = 0;
        perfExtended.bleDiscoveryMaxUs = 0;
        perfExtended.bleSubscribeMaxUs = 0;
        perfExtended.lockoutSaveMaxUs = 0;
        perfExtended.learnerSaveMaxUs = 0;
        perfExtended.timeSaveMaxUs = 0;
        perfExtended.perfReportMaxUs = 0;
        perfExtended.minLargestBlock = UINT32_MAX;
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

static void populateRuntimeSnapshot(PerfRuntimeMetricsSnapshot& snapshot,
                                    const RuntimeSnapshotCaptureContext& ctx,
                                    PerfRuntimeSnapshotMode mode) {
    snapshot = {};
    populateFlatSnapshot(snapshot.flat, ctx, mode);

    snapshot.phoneCmdDrops = ctx.phoneCmdDropMetrics;
    snapshot.uptimeMs = ctx.nowMs;
    snapshot.connectionDispatchRuns = perfCounters.connectionDispatchRuns.load(std::memory_order_relaxed);
    snapshot.connectionCadenceDisplayDue =
        perfCounters.connectionCadenceDisplayDue.load(std::memory_order_relaxed);
    snapshot.connectionCadenceHoldScanDwell =
        perfCounters.connectionCadenceHoldScanDwell.load(std::memory_order_relaxed);
    snapshot.connectionStateProcessRuns =
        perfCounters.connectionStateProcessRuns.load(std::memory_order_relaxed);
    snapshot.connectionStateWatchdogForces =
        perfCounters.connectionStateWatchdogForces.load(std::memory_order_relaxed);
    snapshot.connectionStateProcessGapMaxMs =
        perfCounters.connectionStateProcessGapMaxMs.load(std::memory_order_relaxed);
    snapshot.bleScanStateEntries = perfCounters.bleScanStateEntries.load(std::memory_order_relaxed);
    snapshot.bleScanStateExits = perfCounters.bleScanStateExits.load(std::memory_order_relaxed);
    snapshot.bleScanTargetFound = perfCounters.bleScanTargetFound.load(std::memory_order_relaxed);
    snapshot.bleScanNoTargetExits =
        perfCounters.bleScanNoTargetExits.load(std::memory_order_relaxed);
    snapshot.bleScanDwellMaxMs = perfCounters.bleScanDwellMaxMs.load(std::memory_order_relaxed);
    snapshot.uuid128FallbackHits = perfCounters.uuid128FallbackHits.load(std::memory_order_relaxed);
    snapshot.wifiStopGraceful = perfCounters.wifiStopGraceful.load(std::memory_order_relaxed);
    snapshot.wifiStopImmediate = perfCounters.wifiStopImmediate.load(std::memory_order_relaxed);
    snapshot.wifiStopManual = perfCounters.wifiStopManual.load(std::memory_order_relaxed);
    snapshot.wifiStopTimeout = perfCounters.wifiStopTimeout.load(std::memory_order_relaxed);
    snapshot.wifiStopNoClients = perfCounters.wifiStopNoClients.load(std::memory_order_relaxed);
    snapshot.wifiStopNoClientsAuto =
        perfCounters.wifiStopNoClientsAuto.load(std::memory_order_relaxed);
    snapshot.wifiStopLowDma = perfCounters.wifiStopLowDma.load(std::memory_order_relaxed);
    snapshot.wifiStopPoweroff = perfCounters.wifiStopPoweroff.load(std::memory_order_relaxed);
    snapshot.wifiStopOther = perfCounters.wifiStopOther.load(std::memory_order_relaxed);
    snapshot.wifiApDropLowDma = perfCounters.wifiApDropLowDma.load(std::memory_order_relaxed);
    snapshot.wifiApDropIdleSta = perfCounters.wifiApDropIdleSta.load(std::memory_order_relaxed);
    snapshot.wifiApUpTransitions = perfCounters.wifiApUpTransitions.load(std::memory_order_relaxed);
    snapshot.wifiApDownTransitions =
        perfCounters.wifiApDownTransitions.load(std::memory_order_relaxed);
    snapshot.wifiProcessMaxUs = perfCounters.wifiProcessMaxUs.load(std::memory_order_relaxed);
    const BLEState bleState = bleClient.getBLEState();
    snapshot.bleState = bleStateToString(bleState);
    snapshot.bleStateCode = snapshot.flat.bleState;
    snapshot.subscribeStep = bleClient.getSubscribeStepName();
    snapshot.subscribeStepCode = snapshot.flat.subscribeStep;
    snapshot.connectInProgress = snapshot.flat.connectInProgress != 0;
    snapshot.asyncConnectPending = snapshot.flat.asyncConnectPending != 0;
    snapshot.pendingDisconnectCleanup = snapshot.flat.pendingDisconnectCleanup != 0;
    snapshot.proxyAdvertising = snapshot.flat.proxyAdvertising != 0;
    snapshot.proxyAdvertisingOnTransitions = perfCounters.proxyAdvertisingOnTransitions.load(std::memory_order_relaxed);
    snapshot.proxyAdvertisingOffTransitions = perfCounters.proxyAdvertisingOffTransitions.load(std::memory_order_relaxed);
    snapshot.proxyAdvertisingLastTransitionMs = perfGetProxyAdvertisingLastTransitionMs();
    snapshot.proxyAdvertisingLastTransitionReasonCode = perfGetProxyAdvertisingLastTransitionReason();
    snapshot.proxyAdvertisingLastTransitionReason =
        perfProxyAdvertisingTransitionReasonName(snapshot.proxyAdvertisingLastTransitionReasonCode);
    snapshot.wifiPriorityMode = snapshot.flat.wifiPriorityMode != 0;
    snapshot.loopMaxPrevWindowUs = perfGetPrevWindowLoopMaxUs();
    snapshot.wifiMaxPrevWindowUs = perfGetPrevWindowWifiMaxUs();
    snapshot.bleProcessMaxPrevWindowUs = perfGetPrevWindowBleProcessMaxUs();
    snapshot.dispPipeMaxPrevWindowUs = perfGetPrevWindowDispPipeMaxUs();
    snapshot.wifiApActive = perfGetWifiApState();
    snapshot.wifiApLastTransitionMs = perfGetWifiApLastTransitionMs();
    snapshot.wifiApLastTransitionReasonCode = perfGetWifiApLastTransitionReason();
    snapshot.wifiApLastTransitionReason =
        perfWifiApTransitionReasonName(snapshot.wifiApLastTransitionReasonCode);
    snapshot.perfSdLockFail = perfCounters.perfSdLockFail.load(std::memory_order_relaxed);
    snapshot.perfSdDirFail = perfCounters.perfSdDirFail.load(std::memory_order_relaxed);
    snapshot.perfSdOpenFail = perfCounters.perfSdOpenFail.load(std::memory_order_relaxed);
    snapshot.perfSdHeaderFail = perfCounters.perfSdHeaderFail.load(std::memory_order_relaxed);
    snapshot.perfSdMarkerFail = perfCounters.perfSdMarkerFail.load(std::memory_order_relaxed);
    snapshot.perfSdWriteFail = perfCounters.perfSdWriteFail.load(std::memory_order_relaxed);
#if PERF_METRICS
    snapshot.monitoringEnabled = static_cast<bool>(PERF_MONITORING);
#if PERF_MONITORING
    const uint32_t minUsVal = perfLatency.minUs.load(std::memory_order_relaxed);
    snapshot.latencyMinUs = (minUsVal == UINT32_MAX) ? 0 : minUsVal;
    snapshot.latencyAvgUs = perfLatency.avgUs();
    snapshot.latencyMaxUs = perfLatency.maxUs.load(std::memory_order_relaxed);
    snapshot.latencySamples = perfLatency.sampleCount.load(std::memory_order_relaxed);
    snapshot.debugEnabled = perfDebugEnabled;
#endif
#else
    snapshot.metricsEnabled = false;
#endif

    snapshot.wifiAutoStart.gate = wifiAutoStartGateName(ctx.wifiAutoStart.gate);
    snapshot.wifiAutoStart.gateCode = static_cast<uint8_t>(ctx.wifiAutoStart.gate);
    snapshot.wifiAutoStart.enableWifi = ctx.wifiAutoStart.enableWifi;
    snapshot.wifiAutoStart.enableWifiAtBoot = ctx.wifiAutoStart.enableWifiAtBoot;
    snapshot.wifiAutoStart.bleConnected = ctx.wifiAutoStart.bleConnected;
    snapshot.wifiAutoStart.v1ConnectedAtMs = ctx.wifiAutoStart.v1ConnectedAtMs;
    snapshot.wifiAutoStart.msSinceV1Connect = ctx.wifiAutoStart.msSinceV1Connect;
    snapshot.wifiAutoStart.settleMs = ctx.wifiAutoStart.settleMs;
    snapshot.wifiAutoStart.bootTimeoutMs = ctx.wifiAutoStart.bootTimeoutMs;
    snapshot.wifiAutoStart.canStartDma = ctx.wifiAutoStart.canStartDma;
    snapshot.wifiAutoStart.wifiAutoStartDone = ctx.wifiAutoStart.wifiAutoStartDone;
    snapshot.wifiAutoStart.bleSettled = ctx.wifiAutoStart.bleSettled;
    snapshot.wifiAutoStart.bootTimeoutReached = ctx.wifiAutoStart.bootTimeoutReached;
    snapshot.wifiAutoStart.shouldAutoStart = ctx.wifiAutoStart.shouldAutoStart;
    snapshot.wifiAutoStart.startTriggered = ctx.wifiAutoStart.startTriggered;
    snapshot.wifiAutoStart.startSucceeded = ctx.wifiAutoStart.startSucceeded;

    snapshot.settingsPersistence.backupRevision = ctx.backupRevision;
    snapshot.settingsPersistence.deferredBackupPending = ctx.deferredBackupPending;
    snapshot.settingsPersistence.deferredBackupRetryScheduled = ctx.deferredBackupRetryScheduled;
    snapshot.settingsPersistence.deferredBackupHasNextAttempt = ctx.deferredBackupNextAttemptAtMs != 0;
    snapshot.settingsPersistence.deferredBackupNextAttemptAtMs = ctx.deferredBackupNextAttemptAtMs;
    snapshot.settingsPersistence.deferredBackupDelayMs =
        (ctx.deferredBackupNextAttemptAtMs != 0 &&
         static_cast<int32_t>(ctx.deferredBackupNextAttemptAtMs - ctx.nowMs) > 0)
            ? (ctx.deferredBackupNextAttemptAtMs - ctx.nowMs)
            : 0;
    snapshot.settingsPersistence.perfLoggingEnabled = ctx.perfLoggingEnabled;
    snapshot.settingsPersistence.perfLoggingPath = ctx.perfLoggingPath;

    snapshot.gps.enabled = ctx.gpsStatus.enabled;
    snapshot.gps.mode =
        (ctx.gpsStatus.parserActive || ctx.gpsStatus.moduleDetected || ctx.gpsStatus.hardwareSamples > 0)
            ? "runtime"
            : "scaffold";
    snapshot.gps.sampleValid = ctx.gpsStatus.sampleValid;
    snapshot.gps.hasFix = ctx.gpsStatus.hasFix;
    snapshot.gps.satellites = ctx.gpsStatus.satellites;
    snapshot.gps.injectedSamples = ctx.gpsStatus.injectedSamples > 0;
    snapshot.gps.moduleDetected = ctx.gpsStatus.moduleDetected;
    snapshot.gps.detectionTimedOut = ctx.gpsStatus.detectionTimedOut;
    snapshot.gps.parserActive = ctx.gpsStatus.parserActive;
    snapshot.gps.hardwareSamples = ctx.gpsStatus.hardwareSamples;
    snapshot.gps.bytesRead = ctx.gpsStatus.bytesRead;
    snapshot.gps.sentencesSeen = ctx.gpsStatus.sentencesSeen;
    snapshot.gps.sentencesParsed = ctx.gpsStatus.sentencesParsed;
    snapshot.gps.parseFailures = ctx.gpsStatus.parseFailures;
    snapshot.gps.checksumFailures = ctx.gpsStatus.checksumFailures;
    snapshot.gps.bufferOverruns = ctx.gpsStatus.bufferOverruns;
    snapshot.gps.hdopValid = std::isfinite(ctx.gpsStatus.hdop);
    snapshot.gps.hdop = snapshot.gps.hdopValid ? ctx.gpsStatus.hdop : 0.0f;
    snapshot.gps.locationValid = ctx.gpsStatus.locationValid;
    snapshot.gps.latitudeDeg = ctx.gpsStatus.locationValid ? ctx.gpsStatus.latitudeDeg : 0.0;
    snapshot.gps.longitudeDeg = ctx.gpsStatus.locationValid ? ctx.gpsStatus.longitudeDeg : 0.0;
    snapshot.gps.courseValid = ctx.gpsStatus.courseValid;
    snapshot.gps.courseDeg = ctx.gpsStatus.courseValid ? ctx.gpsStatus.courseDeg : 0.0f;
    snapshot.gps.courseSampleTsMs = ctx.gpsStatus.courseValid ? ctx.gpsStatus.courseSampleTsMs : 0;
    snapshot.gps.speedMph = ctx.gpsStatus.sampleValid ? ctx.gpsStatus.speedMph : 0.0f;
    snapshot.gps.sampleTsMs = ctx.gpsStatus.sampleValid ? ctx.gpsStatus.sampleTsMs : 0;
    snapshot.gps.sampleAgeValid = ctx.gpsStatus.sampleAgeMs != UINT32_MAX;
    snapshot.gps.sampleAgeMs = snapshot.gps.sampleAgeValid ? ctx.gpsStatus.sampleAgeMs : 0;
    snapshot.gps.courseAgeValid = ctx.gpsStatus.courseAgeMs != UINT32_MAX;
    snapshot.gps.courseAgeMs = snapshot.gps.courseAgeValid ? ctx.gpsStatus.courseAgeMs : 0;
    snapshot.gps.lastSentenceTsValid = ctx.gpsStatus.lastSentenceTsMs != 0;
    snapshot.gps.lastSentenceTsMs = snapshot.gps.lastSentenceTsValid ? ctx.gpsStatus.lastSentenceTsMs : 0;

    snapshot.gpsLog.published = ctx.gpsLogStats.published;
    snapshot.gpsLog.drops = ctx.gpsLogStats.drops;
    snapshot.gpsLog.size = static_cast<uint32_t>(ctx.gpsLogStats.size);
    snapshot.gpsLog.capacity = static_cast<uint32_t>(GpsObservationLog::kCapacity);

    snapshot.speedSource.gpsEnabled = ctx.speedStatus.gpsEnabled;
    snapshot.speedSource.selected = SpeedSourceSelector::sourceName(ctx.speedStatus.selectedSource);
    snapshot.speedSource.selectedValueValid = ctx.speedStatus.selectedSource != SpeedSource::NONE;
    snapshot.speedSource.selectedMph = snapshot.speedSource.selectedValueValid ? ctx.speedStatus.selectedSpeedMph : 0.0f;
    snapshot.speedSource.selectedAgeMs = snapshot.speedSource.selectedValueValid ? ctx.speedStatus.selectedAgeMs : 0;
    snapshot.speedSource.gpsFresh = ctx.speedStatus.gpsFresh;
    snapshot.speedSource.gpsMph = ctx.speedStatus.gpsSpeedMph;
    snapshot.speedSource.gpsAgeValid = ctx.speedStatus.gpsAgeMs != UINT32_MAX;
    snapshot.speedSource.gpsAgeMs = snapshot.speedSource.gpsAgeValid ? ctx.speedStatus.gpsAgeMs : 0;
    snapshot.speedSource.sourceSwitches = ctx.speedStatus.sourceSwitches;
    snapshot.speedSource.gpsSelections = ctx.speedStatus.gpsSelections;
    snapshot.speedSource.noSourceSelections = ctx.speedStatus.noSourceSelections;

    snapshot.heap.heapFree = ctx.freeHeap;
    snapshot.heap.heapMinFree = perfGetMinFreeHeap();
    snapshot.heap.heapLargest = ctx.largestHeap;
    snapshot.heap.heapInternalFree = ctx.freeDma;
    snapshot.heap.heapInternalFreeMin = snapshot.flat.freeDmaMin;
    snapshot.heap.heapInternalLargest = ctx.largestDma;
    snapshot.heap.heapInternalLargestMin = snapshot.flat.largestDmaMin;
    snapshot.heap.heapDmaFree = ctx.freeDmaCap;
    snapshot.heap.heapDmaFreeMin = snapshot.flat.dmaFreeMin;
    snapshot.heap.heapDmaLargest = ctx.largestDmaCap;
    snapshot.heap.heapDmaLargestMin = snapshot.flat.dmaLargestMin;

    snapshot.psram.total = ctx.psramTotal;
    snapshot.psram.free = ctx.psramFree;
    snapshot.psram.largest = ctx.psramLargest;

    snapshot.sdContention.tryLockFails = ctx.sdTryLockFails;
    snapshot.sdContention.dmaStarvation = ctx.sdDmaStarvation;

    snapshot.proxy.sendCount = ctx.proxyMetrics.sendCount;
    snapshot.proxy.dropCount = ctx.proxyMetrics.dropCount;
    snapshot.proxy.errorCount = ctx.proxyMetrics.errorCount;
    snapshot.proxy.queueHighWater = ctx.proxyMetrics.queueHighWater;
    snapshot.proxy.connected = bleClient.isProxyClientConnected();
    snapshot.proxy.advertising = snapshot.proxyAdvertising;
    snapshot.proxy.advertisingOnTransitions = snapshot.proxyAdvertisingOnTransitions;
    snapshot.proxy.advertisingOffTransitions = snapshot.proxyAdvertisingOffTransitions;
    snapshot.proxy.advertisingLastTransitionMs = snapshot.proxyAdvertisingLastTransitionMs;
    snapshot.proxy.advertisingLastTransitionReasonCode = snapshot.proxyAdvertisingLastTransitionReasonCode;
    snapshot.proxy.advertisingLastTransitionReason = snapshot.proxyAdvertisingLastTransitionReason;

    snapshot.eventBus.publishCount = ctx.eventBusPublishCount;
    snapshot.eventBus.dropCount = ctx.eventBusDropCount;
    snapshot.eventBus.size = ctx.eventBusSize;

    snapshot.lockout.mode = lockoutRuntimeModeName(ctx.settings->gpsLockoutMode);
    snapshot.lockout.modeRaw = static_cast<int>(ctx.settings->gpsLockoutMode);
    snapshot.lockout.coreGuardEnabled = ctx.settings->gpsLockoutCoreGuardEnabled;
    snapshot.lockout.coreGuardTripped = ctx.lockoutGuard.tripped;
    snapshot.lockout.coreGuardReason = ctx.lockoutGuard.reason;
    snapshot.lockout.maxQueueDrops = ctx.settings->gpsLockoutMaxQueueDrops;
    snapshot.lockout.maxPerfDrops = ctx.settings->gpsLockoutMaxPerfDrops;
    snapshot.lockout.maxEventBusDrops = ctx.settings->gpsLockoutMaxEventBusDrops;
    snapshot.lockout.learnerPromotionHits = static_cast<uint32_t>(lockoutLearner.promotionHits());
    snapshot.lockout.learnerRadiusE5 = static_cast<uint32_t>(lockoutLearner.radiusE5());
    snapshot.lockout.learnerFreqToleranceMHz = static_cast<uint32_t>(lockoutLearner.freqToleranceMHz());
    snapshot.lockout.learnerLearnIntervalHours = static_cast<uint32_t>(lockoutLearner.learnIntervalHours());
    snapshot.lockout.learnerUnlearnIntervalHours = static_cast<uint32_t>(ctx.settings->gpsLockoutLearnerUnlearnIntervalHours);
    snapshot.lockout.learnerUnlearnCount = static_cast<uint32_t>(ctx.settings->gpsLockoutLearnerUnlearnCount);
    snapshot.lockout.manualDemotionMissCount = static_cast<uint32_t>(ctx.settings->gpsLockoutManualDemotionMissCount);
    snapshot.lockout.kaLearningEnabled = ctx.settings->gpsLockoutKaLearningEnabled;
    snapshot.lockout.enforceRequested = (ctx.settings->gpsLockoutMode == LOCKOUT_RUNTIME_ENFORCE);
    snapshot.lockout.enforceAllowed = snapshot.lockout.enforceRequested && !ctx.lockoutGuard.tripped;
}

static void captureSdSnapshot(PerfSdSnapshot& snapshot) {
    // loopTask has an 8 KB stack budget. Keep the periodic SD snapshot on the
    // flat-only path so Tier 4 observability work cannot pay for the larger
    // runtime wrapper when only the CSV payload is needed.
    const RuntimeSnapshotCaptureContext ctx = captureRuntimeSnapshotContext();
    populateFlatSnapshot(snapshot, ctx, PerfRuntimeSnapshotMode::CaptureAndResetWindowPeaks);
}

} // namespace

void perfCaptureRuntimeMetricsSnapshot(PerfRuntimeMetricsSnapshot& snapshot,
                                       PerfRuntimeSnapshotMode mode) {
    const RuntimeSnapshotCaptureContext ctx = captureRuntimeSnapshotContext();
    populateRuntimeSnapshot(snapshot, ctx, mode);
}

void perfRecordNotifyToDisplayMs(uint32_t ms) {
    addLatencySample(perfExtended.notifyToDisplayMs, ms);
}

void perfRecordNotifyToProxyMs(uint32_t ms) {
    addLatencySample(perfExtended.notifyToProxyMs, ms);
}

void perfRecordLoopJitterUs(uint32_t us) {
    if (us > perfExtended.loopMaxUs) {
        perfExtended.loopMaxUs = us;
    }
}

void perfRecordHeapStats(uint32_t freeHeap, uint32_t largestBlock, uint32_t freeDma, uint32_t largestDma) {
    if (freeHeap < perfExtended.minFreeHeap) {
        perfExtended.minFreeHeap = freeHeap;
    }
    if (largestBlock < perfExtended.minLargestBlock) {
        perfExtended.minLargestBlock = largestBlock;
    }
    if (freeDma < perfExtended.minFreeDma) {
        perfExtended.minFreeDma = freeDma;
    }
    if (largestDma < perfExtended.minLargestDma) {
        perfExtended.minLargestDma = largestDma;
    }
}

void perfRecordWifiProcessUs(uint32_t us) {
    if (us > perfExtended.wifiMaxUs) {
        perfExtended.wifiMaxUs = us;
    }
}

void perfRecordWifiHandleClientUs(uint32_t us) {
    if (us > perfExtended.wifiHandleClientMaxUs) {
        perfExtended.wifiHandleClientMaxUs = us;
    }
}

void perfRecordWifiMaintenanceUs(uint32_t us) {
    if (us > perfExtended.wifiMaintenanceMaxUs) {
        perfExtended.wifiMaintenanceMaxUs = us;
    }
}

void perfRecordWifiStatusCheckUs(uint32_t us) {
    if (us > perfExtended.wifiStatusCheckMaxUs) {
        perfExtended.wifiStatusCheckMaxUs = us;
    }
}

void perfRecordWifiTimeoutCheckUs(uint32_t us) {
    if (us > perfExtended.wifiTimeoutCheckMaxUs) {
        perfExtended.wifiTimeoutCheckMaxUs = us;
    }
}

void perfRecordWifiHeapGuardUs(uint32_t us) {
    if (us > perfExtended.wifiHeapGuardMaxUs) {
        perfExtended.wifiHeapGuardMaxUs = us;
    }
}

void perfRecordWifiApStaPollUs(uint32_t us) {
    if (us > perfExtended.wifiApStaPollMaxUs) {
        perfExtended.wifiApStaPollMaxUs = us;
    }
}

void perfRecordWifiStopHttpServerUs(uint32_t us) {
    if (us > perfExtended.wifiStopHttpServerMaxUs) {
        perfExtended.wifiStopHttpServerMaxUs = us;
    }
}

void perfRecordWifiStopStaDisconnectUs(uint32_t us) {
    if (us > perfExtended.wifiStopStaDisconnectMaxUs) {
        perfExtended.wifiStopStaDisconnectMaxUs = us;
    }
}

void perfRecordWifiStopApDisableUs(uint32_t us) {
    if (us > perfExtended.wifiStopApDisableMaxUs) {
        perfExtended.wifiStopApDisableMaxUs = us;
    }
}

void perfRecordWifiStopModeOffUs(uint32_t us) {
    if (us > perfExtended.wifiStopModeOffMaxUs) {
        perfExtended.wifiStopModeOffMaxUs = us;
    }
}

void perfRecordWifiStartPreflightUs(uint32_t us) {
    if (us > perfExtended.wifiStartPreflightMaxUs) {
        perfExtended.wifiStartPreflightMaxUs = us;
    }
}

void perfRecordWifiStartApBringupUs(uint32_t us) {
    if (us > perfExtended.wifiStartApBringupMaxUs) {
        perfExtended.wifiStartApBringupMaxUs = us;
    }
}

void perfRecordFsServeUs(uint32_t us) {
    if (us > perfExtended.fsMaxUs) {
        perfExtended.fsMaxUs = us;
    }
}

void perfRecordSdFlushUs(uint32_t us) {
    if (us > perfExtended.sdMaxUs) {
        perfExtended.sdMaxUs = us;
    }
    perfExtended.sdWriteCount++;
    if (us < 1000) {
        perfExtended.sdWriteLt1msCount++;
    } else if (us < 5000) {
        perfExtended.sdWrite1to5msCount++;
    } else if (us < 10000) {
        perfExtended.sdWrite5to10msCount++;
    } else {
        perfExtended.sdWriteGe10msCount++;
    }
}

void perfRecordFlushUs(uint32_t us, uint32_t areaPx, bool fullFlush) {
    if (us > perfExtended.flushMaxUs) {
        perfExtended.flushMaxUs = us;
        perfExtended.displayFlushMaxAreaPx = areaPx;
    }
    perfExtended.displayFlushEquivalentAreaTotalPx += areaPx;
    if (fullFlush) {
        perfExtended.displayFullFlushCount++;
    } else {
        perfExtended.displayPartialFlushCount++;
        perfExtended.displayPartialFlushAreaTotalPx += areaPx;
        if (areaPx > perfExtended.displayPartialFlushAreaPeakPx) {
            perfExtended.displayPartialFlushAreaPeakPx = areaPx;
        }
    }
}

void perfRecordDisplayRenderUs(uint32_t us) {
    if (us > perfExtended.displayRenderMaxUs) {
        perfExtended.displayRenderMaxUs = us;
    }
}

void perfRecordDisplayScenarioRenderUs(uint32_t us) {
    const PerfDisplayRenderScenario scenario = currentDisplayRenderScenario();
    recordDisplayScenarioRenderCount(scenario);
    recordDisplayScenarioRenderMax(scenario, us);
}

void perfRecordDisplayRenderPath(PerfDisplayRenderPath path) {
    switch (path) {
        case PerfDisplayRenderPath::Full:
            perfExtended.displayFullRenderCount++;
            break;
        case PerfDisplayRenderPath::Incremental:
            perfExtended.displayIncrementalRenderCount++;
            break;
        case PerfDisplayRenderPath::CardsOnly:
            perfExtended.displayCardsOnlyRenderCount++;
            break;
        case PerfDisplayRenderPath::RestingFull:
            perfExtended.displayRestingFullRenderCount++;
            break;
        case PerfDisplayRenderPath::RestingIncremental:
            perfExtended.displayRestingIncrementalRenderCount++;
            break;
        case PerfDisplayRenderPath::Persisted:
            perfExtended.displayPersistedRenderCount++;
            break;
        case PerfDisplayRenderPath::Preview:
            perfExtended.displayPreviewRenderCount++;
            break;
        case PerfDisplayRenderPath::Restore:
            perfExtended.displayRestoreRenderCount++;
            break;
        default:
            break;
    }
}

void perfRecordDisplayRedrawReason(PerfDisplayRedrawReason reason) {
    switch (reason) {
        case PerfDisplayRedrawReason::FirstRun:
            perfExtended.displayRedrawReasonFirstRunCount++;
            break;
        case PerfDisplayRedrawReason::EnterLive:
            perfExtended.displayRedrawReasonEnterLiveCount++;
            break;
        case PerfDisplayRedrawReason::LeaveLive:
            perfExtended.displayRedrawReasonLeaveLiveCount++;
            break;
        case PerfDisplayRedrawReason::LeavePersisted:
            perfExtended.displayRedrawReasonLeavePersistedCount++;
            break;
        case PerfDisplayRedrawReason::ForceRedraw:
            perfExtended.displayRedrawReasonForceRedrawCount++;
            break;
        case PerfDisplayRedrawReason::FrequencyChange:
            perfExtended.displayRedrawReasonFrequencyChangeCount++;
            break;
        case PerfDisplayRedrawReason::BandSetChange:
            perfExtended.displayRedrawReasonBandSetChangeCount++;
            break;
        case PerfDisplayRedrawReason::ArrowChange:
            perfExtended.displayRedrawReasonArrowChangeCount++;
            break;
        case PerfDisplayRedrawReason::SignalBarChange:
            perfExtended.displayRedrawReasonSignalBarChangeCount++;
            break;
        case PerfDisplayRedrawReason::VolumeChange:
            perfExtended.displayRedrawReasonVolumeChangeCount++;
            break;
        case PerfDisplayRedrawReason::BogeyCounterChange:
            perfExtended.displayRedrawReasonBogeyCounterChangeCount++;
            break;
        case PerfDisplayRedrawReason::RssiRefresh:
            perfExtended.displayRedrawReasonRssiRefreshCount++;
            break;
        case PerfDisplayRedrawReason::FlashTick:
            perfExtended.displayRedrawReasonFlashTickCount++;
            break;
        default:
            break;
    }
}

void perfRecordDisplayRenderSubphaseUs(PerfDisplayRenderSubphase subphase, uint32_t us) {
    switch (subphase) {
        case PerfDisplayRenderSubphase::BaseFrame:
            if (us > perfExtended.displayBaseFrameMaxUs) {
                perfExtended.displayBaseFrameMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::StatusStrip:
            if (us > perfExtended.displayStatusStripMaxUs) {
                perfExtended.displayStatusStripMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::Frequency:
            if (us > perfExtended.displayFrequencyMaxUs) {
                perfExtended.displayFrequencyMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::BandsBars:
            if (us > perfExtended.displayBandsBarsMaxUs) {
                perfExtended.displayBandsBarsMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::ArrowsIcons:
            if (us > perfExtended.displayArrowsIconsMaxUs) {
                perfExtended.displayArrowsIconsMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::Cards:
            if (us > perfExtended.displayCardsMaxUs) {
                perfExtended.displayCardsMaxUs = us;
            }
            break;
        case PerfDisplayRenderSubphase::Flush:
            if (us > perfExtended.displayFlushSubphaseMaxUs) {
                perfExtended.displayFlushSubphaseMaxUs = us;
            }
            break;
        default:
            break;
    }
}

void perfSetDisplayRenderScenario(PerfDisplayRenderScenario scenario) {
    sDisplayRenderScenario.store(static_cast<uint8_t>(scenario), std::memory_order_relaxed);
}

PerfDisplayRenderScenario perfGetDisplayRenderScenario() {
    return currentDisplayRenderScenario();
}

void perfClearDisplayRenderScenario() {
    perfSetDisplayRenderScenario(PerfDisplayRenderScenario::None);
}

void perfRecordBleDrainUs(uint32_t us) {
    if (us > perfExtended.bleDrainMaxUs) {
        perfExtended.bleDrainMaxUs = us;
    }
}

void perfRecordBleConnectUs(uint32_t us) {
    if (us > perfExtended.bleConnectMaxUs) {
        perfExtended.bleConnectMaxUs = us;
    }
}

void perfRecordBleDiscoveryUs(uint32_t us) {
    if (us > perfExtended.bleDiscoveryMaxUs) {
        perfExtended.bleDiscoveryMaxUs = us;
    }
}

void perfRecordBleSubscribeUs(uint32_t us) {
    if (us > perfExtended.bleSubscribeMaxUs) {
        perfExtended.bleSubscribeMaxUs = us;
    }
}

void perfRecordBleFollowupRequestAlertUs(uint32_t us) {
    if (us > perfExtended.bleFollowupRequestAlertMaxUs) {
        perfExtended.bleFollowupRequestAlertMaxUs = us;
    }
}

void perfRecordBleFollowupRequestVersionUs(uint32_t us) {
    if (us > perfExtended.bleFollowupRequestVersionMaxUs) {
        perfExtended.bleFollowupRequestVersionMaxUs = us;
    }
}

void perfRecordBleConnectStableCallbackUs(uint32_t us) {
    if (us > perfExtended.bleConnectStableCallbackMaxUs) {
        perfExtended.bleConnectStableCallbackMaxUs = us;
    }
}

void perfRecordBleProxyStartUs(uint32_t us) {
    if (us > perfExtended.bleProxyStartMaxUs) {
        perfExtended.bleProxyStartMaxUs = us;
    }
}

void perfRecordBleProcessUs(uint32_t us) {
    if (us > perfExtended.bleProcessMaxUs) {
        perfExtended.bleProcessMaxUs = us;
    }
}

void perfRecordDispPipeUs(uint32_t us) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    if (us > perfExtended.dispPipeMaxUs) {
        perfExtended.dispPipeMaxUs = us;
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordDisplayVoiceUs(uint32_t us) {
    if (us > perfExtended.displayVoiceMaxUs) {
        perfExtended.displayVoiceMaxUs = us;
    }
}

void perfRecordDisplayGapRecoverUs(uint32_t us) {
    if (us > perfExtended.displayGapRecoverMaxUs) {
        perfExtended.displayGapRecoverMaxUs = us;
    }
}

void perfRecordTouchUs(uint32_t us) {
    if (us > perfExtended.touchMaxUs) {
        perfExtended.touchMaxUs = us;
    }
}

void perfRecordGpsUs(uint32_t us) {
    if (us > perfExtended.gpsMaxUs) {
        perfExtended.gpsMaxUs = us;
    }
}

void perfRecordObdUs(uint32_t us) {
    if (us > perfExtended.obdMaxUs) {
        perfExtended.obdMaxUs = us;
    }
}

void perfRecordObdConnectCallUs(uint32_t us) {
    if (us > perfExtended.obdConnectCallMaxUs) {
        perfExtended.obdConnectCallMaxUs = us;
    }
}

void perfRecordObdSecurityStartCallUs(uint32_t us) {
    if (us > perfExtended.obdSecurityStartCallMaxUs) {
        perfExtended.obdSecurityStartCallMaxUs = us;
    }
}

void perfRecordObdDiscoveryCallUs(uint32_t us) {
    if (us > perfExtended.obdDiscoveryCallMaxUs) {
        perfExtended.obdDiscoveryCallMaxUs = us;
    }
}

void perfRecordObdSubscribeCallUs(uint32_t us) {
    if (us > perfExtended.obdSubscribeCallMaxUs) {
        perfExtended.obdSubscribeCallMaxUs = us;
    }
}

void perfRecordObdWriteCallUs(uint32_t us) {
    if (us > perfExtended.obdWriteCallMaxUs) {
        perfExtended.obdWriteCallMaxUs = us;
    }
}

void perfRecordObdRssiCallUs(uint32_t us) {
    if (us > perfExtended.obdRssiCallMaxUs) {
        perfExtended.obdRssiCallMaxUs = us;
    }
}

void perfRecordLockoutUs(uint32_t us) {
    if (us > perfExtended.lockoutMaxUs) {
        perfExtended.lockoutMaxUs = us;
    }
}

void perfRecordLockoutSaveUs(uint32_t us) {
    if (us > perfExtended.lockoutSaveMaxUs) {
        perfExtended.lockoutSaveMaxUs = us;
    }
}

void perfRecordLearnerSaveUs(uint32_t us) {
    if (us > perfExtended.learnerSaveMaxUs) {
        perfExtended.learnerSaveMaxUs = us;
    }
}

void perfRecordTimeSaveUs(uint32_t us) {
    if (us > perfExtended.timeSaveMaxUs) {
        perfExtended.timeSaveMaxUs = us;
    }
}

void perfRecordPerfReportUs(uint32_t us) {
    if (us > perfExtended.perfReportMaxUs) {
        perfExtended.perfReportMaxUs = us;
    }
}

void perfRecordDisplayScreenTransition(PerfDisplayScreen from, PerfDisplayScreen to, uint32_t nowMs) {
    if (from == to) {
        return;
    }

    portENTER_CRITICAL(&sPerfSnapshotMux);

    if (to == PerfDisplayScreen::Scanning) {
        perfExtended.uiToScanCount++;
        perfExtended.uiLastScanEnteredMs = nowMs;
    } else if (to == PerfDisplayScreen::Resting) {
        perfExtended.uiToRestCount++;
    }

    if (from == PerfDisplayScreen::Scanning && to == PerfDisplayScreen::Resting) {
        perfExtended.uiScanToRestCount++;
    }

    if (from == PerfDisplayScreen::Scanning && to != PerfDisplayScreen::Scanning) {
        if (to != PerfDisplayScreen::Unknown && perfExtended.uiLastScanEnteredMs > 0) {
            uint32_t dwellMs = nowMs - perfExtended.uiLastScanEnteredMs;
            perfExtended.uiLastScanDwellMs = dwellMs;
            if (dwellMs < perfExtended.uiMinScanDwellMs) {
                perfExtended.uiMinScanDwellMs = dwellMs;
            }
            if (dwellMs < kFastScanExitThresholdMs) {
                perfExtended.uiFastScanExitCount++;
            }
        }
        perfExtended.uiLastScanEnteredMs = 0;
    }

    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordVolumeFadeDecision(PerfFadeDecision decision, uint8_t currentVolume, uint8_t originalVolume, uint32_t nowMs) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    switch (decision) {
        case PerfFadeDecision::FadeDown:
            perfExtended.fadeDownCount++;
            break;
        case PerfFadeDecision::RestoreApplied:
            perfExtended.fadeRestoreCount++;
            break;
        case PerfFadeDecision::RestoreSkippedEqual:
            perfExtended.fadeSkipEqualCount++;
            break;
        case PerfFadeDecision::RestoreSkippedNoBaseline:
            perfExtended.fadeSkipNoBaselineCount++;
            break;
        case PerfFadeDecision::RestoreSkippedNotFaded:
            perfExtended.fadeSkipNotFadedCount++;
            break;
        case PerfFadeDecision::None:
        default:
            break;
    }
    perfExtended.fadeLastDecision = static_cast<uint8_t>(decision);
    perfExtended.fadeLastCurrentVol = currentVolume;
    perfExtended.fadeLastOriginalVol = originalVolume;
    perfExtended.fadeLastDecisionMs = nowMs;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordPreQuietDrop() {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    perfExtended.preQuietDropCount++;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordPreQuietRestore() {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    perfExtended.preQuietRestoreCount++;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordPreQuietRestoreRetry() {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    perfExtended.preQuietRestoreRetryCount++;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordSpeedVolDrop() {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    perfExtended.speedVolDropCount++;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordSpeedVolRestore() {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    perfExtended.speedVolRestoreCount++;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordSpeedVolRetry() {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    perfExtended.speedVolRetryCount++;
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordBleTimelineEvent(PerfBleTimelineEvent event, uint32_t nowMs) {
    portENTER_CRITICAL(&sPerfSnapshotMux);
    uint32_t* target = nullptr;
    switch (event) {
        case PerfBleTimelineEvent::ScanStart:
            target = &perfExtended.bleScanStartMs;
            break;
        case PerfBleTimelineEvent::TargetFound:
            target = &perfExtended.bleTargetFoundMs;
            break;
        case PerfBleTimelineEvent::ConnectStart:
            target = &perfExtended.bleConnectStartMs;
            break;
        case PerfBleTimelineEvent::Connected:
            target = &perfExtended.bleConnectedMs;
            break;
        case PerfBleTimelineEvent::FirstRx:
            target = &perfExtended.bleFirstRxMs;
            break;
        default:
            break;
    }
    if (target && *target == 0) {
        *target = nowMs;
    }
    portEXIT_CRITICAL(&sPerfSnapshotMux);
}

void perfRecordWifiApTransition(bool apActive, uint8_t reasonCode, uint32_t nowMs) {
    const uint32_t newState = apActive ? 1u : 0u;
    const uint32_t previousState = perfCounters.wifiApState.exchange(newState, std::memory_order_relaxed);
    if (previousState == newState) {
        return;
    }
    if (newState != 0u) {
        PERF_INC(wifiApUpTransitions);
    } else {
        PERF_INC(wifiApDownTransitions);
    }
    perfCounters.wifiApLastTransitionMs.store(nowMs, std::memory_order_relaxed);
    perfCounters.wifiApLastTransitionReason.store(reasonCode, std::memory_order_relaxed);
}

void perfRecordProxyAdvertisingTransition(bool advertising, uint8_t reasonCode, uint32_t nowMs) {
    const uint32_t newState = advertising ? 1u : 0u;
    const uint32_t previousState =
        perfCounters.proxyAdvertisingState.exchange(newState, std::memory_order_relaxed);
    if (previousState == newState) {
        return;
    }
    if (newState != 0u) {
        PERF_INC(proxyAdvertisingOnTransitions);
    } else {
        PERF_INC(proxyAdvertisingOffTransitions);
    }
    perfCounters.proxyAdvertisingLastTransitionMs.store(nowMs, std::memory_order_relaxed);
    perfCounters.proxyAdvertisingLastTransitionReason.store(reasonCode, std::memory_order_relaxed);
}

uint32_t perfGetMinFreeHeap() { return perfExtended.minFreeHeap == UINT32_MAX ? 0 : perfExtended.minFreeHeap; }
uint32_t perfGetMinFreeDma() { return perfExtended.minFreeDma == UINT32_MAX ? 0 : perfExtended.minFreeDma; }
uint32_t perfGetPrevWindowLoopMaxUs() {
    return sPrevWindowLoopMaxUs.load(std::memory_order_relaxed);
}
uint32_t perfGetPrevWindowWifiMaxUs() {
    return sPrevWindowWifiMaxUs.load(std::memory_order_relaxed);
}
uint32_t perfGetPrevWindowBleProcessMaxUs() {
    return sPrevWindowBleProcessMaxUs.load(std::memory_order_relaxed);
}
uint32_t perfGetPrevWindowDispPipeMaxUs() {
    return sPrevWindowDispPipeMaxUs.load(std::memory_order_relaxed);
}
uint32_t perfGetWifiApState() {
    return perfCounters.wifiApState.load(std::memory_order_relaxed);
}
uint32_t perfGetWifiApLastTransitionMs() {
    return perfCounters.wifiApLastTransitionMs.load(std::memory_order_relaxed);
}
uint32_t perfGetWifiApLastTransitionReason() {
    return perfCounters.wifiApLastTransitionReason.load(std::memory_order_relaxed);
}
const char* perfWifiApTransitionReasonName(uint32_t reasonCode) {
    return wifiApTransitionReasonNameInternal(reasonCode);
}
uint32_t perfGetProxyAdvertisingState() {
    return perfCounters.proxyAdvertisingState.load(std::memory_order_relaxed);
}
uint32_t perfGetProxyAdvertisingLastTransitionMs() {
    return perfCounters.proxyAdvertisingLastTransitionMs.load(std::memory_order_relaxed);
}
uint32_t perfGetProxyAdvertisingLastTransitionReason() {
    return perfCounters.proxyAdvertisingLastTransitionReason.load(std::memory_order_relaxed);
}
const char* perfProxyAdvertisingTransitionReasonName(uint32_t reasonCode) {
    return proxyAdvertisingTransitionReasonNameInternal(reasonCode);
}

#if PERF_METRICS && PERF_MONITORING
bool perfMetricsCheckReport() {
    uint32_t now = millis();
    constexpr uint32_t STABILITY_REPORT_INTERVAL_MS = 5000;
    if (perfLastReportMs == 0) {
        perfLastReportMs = now;
        return false;
    }
    if (now - perfLastReportMs < STABILITY_REPORT_INTERVAL_MS) {
        return false;
    }
    perfLastReportMs = now;

    // Always capture the snapshot to cycle windowed maxima (prev-window
    // store + reset).  Without this, API-polled metrics like wifiMaxUs
    // accumulate as max-ever instead of per-window when SD is absent.
    PerfSdSnapshot snapshot{};
    captureSdSnapshot(snapshot);
    if (perfSdLogger.isEnabled()) {
        perfSdLogger.enqueue(snapshot);
    }
    return true;
}
#else
bool perfMetricsCheckReport() {
    return false;
}
#endif

bool perfMetricsEnqueueSnapshotNow() {
    if (!perfSdLogger.isEnabled()) {
        return false;
    }

    PerfSdSnapshot snapshot{};
    captureSdSnapshot(snapshot);
    return perfSdLogger.enqueue(snapshot);
}

void perfMetricsSetDebug(bool enabled) {
#if PERF_METRICS && PERF_MONITORING
    perfDebugEnabled = enabled;
    if (enabled) {
        perfLastReportMs = millis();  // Reset report timer
    }
#else
    (void)enabled;
#endif
}
