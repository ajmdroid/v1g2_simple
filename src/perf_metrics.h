/**
 * Low-Overhead Performance Metrics (Channel A: flight recorder)
 * 
 * TWO-CHANNEL LOGGING ARCHITECTURE:
 * - Channel A (this file): Always-on numeric counters. RED ZONE SAFE.
 *   Counters only, no strings, no heap, no locks, no I/O.
 *   Emitted periodically from safe zone (once per second max).
 * 
 * RED ZONE SAFE MACROS (use these everywhere):
 *   PERF_INC(counter)        - Atomic increment, zero overhead
 *   PERF_MAX(counter, value) - Atomic max update, zero overhead
 * 
 * Design principles:
 * - No heap allocations
 * - No logging in hot paths
 * - Counters/timestamps stored in RAM (std::atomic)
 * - Sampled timing (1/N packets) to reduce overhead
 * - Compile-time gating via PERF_METRICS for extended stats
 * 
 * Usage:
 * - PERF_METRICS=0: Release builds, only essential counters
 * - PERF_METRICS=1: Debug builds, sampled timing + periodic reports
 */

#ifndef PERF_METRICS_H
#define PERF_METRICS_H

#include <Arduino.h>
#include <atomic>

// ============================================================================
// Compile-time gating
// Set PERF_METRICS=0 for release builds (minimal overhead)
// Set PERF_METRICS=1 for debug builds (sampled timing + reports)
// ============================================================================
#ifndef PERF_METRICS
#define PERF_METRICS 0  // Disabled for release
#endif

// Compile-time toggles for monitoring and verbose alerts
#ifndef PERF_MONITORING
#define PERF_MONITORING 1  // Disable to keep counters only (no sampling/prints)
#endif

#ifndef PERF_VERBOSE
#define PERF_VERBOSE 0  // Enable to allow immediate alerts and stage timings
#endif

// Sampling rate: measure 1 in N packets to reduce overhead
#ifndef PERF_SAMPLE_RATE
#define PERF_SAMPLE_RATE 8  // Measure every 8th packet
#endif

// Report interval (only in DEBUG mode)
#ifndef PERF_REPORT_INTERVAL_MS
#define PERF_REPORT_INTERVAL_MS 10000  // 10 seconds
#endif

// Threshold for alert (print immediately if exceeded)
#ifndef PERF_LATENCY_ALERT_MS
#define PERF_LATENCY_ALERT_MS 100  // Alert if latency > 100ms
#endif

// ============================================================================
// Always-on counters (zero overhead when not accessed)
// Uses std::atomic for thread-safe access from main loop and web handlers
// ============================================================================
struct PerfCounters {
    // Packet flow
    std::atomic<uint32_t> rxPackets{0};        // Total BLE notifications received
    std::atomic<uint32_t> rxBytes{0};          // Total bytes received
    std::atomic<uint32_t> queueDrops{0};       // Packets dropped (queue full)
    std::atomic<uint32_t> oversizeDrops{0};    // Packets dropped (too large for buffer)
    std::atomic<uint32_t> queueHighWater{0};   // Max queue depth seen
    std::atomic<uint32_t> proxyQueueHighWater{0}; // Max proxy queue depth
    std::atomic<uint32_t> phoneCmdQueueHighWater{0}; // Max phone→V1 cmd queue depth
    std::atomic<uint32_t> phoneCmdDropsOverflow{0}; // Phone→V1 queue overflow drops
    std::atomic<uint32_t> phoneCmdDropsInvalid{0}; // Phone→V1 malformed packet drops
    std::atomic<uint32_t> phoneCmdDropsBleFail{0}; // Phone→V1 hard BLE send failures
    std::atomic<uint32_t> phoneCmdDropsLockBusy{0}; // Phone→V1 queue lock-busy drops
    std::atomic<uint32_t> parseSuccesses{0};   // Successfully parsed packets
    std::atomic<uint32_t> parseFailures{0};    // Parse failures (resync)
    std::atomic<uint32_t> perfDrop{0};         // Perf SD snapshot drops (queue full)
    std::atomic<uint32_t> perfSdLockFail{0};   // Perf SD writer lock failures
    std::atomic<uint32_t> perfSdDirFail{0};    // Perf SD dir ensure failures
    std::atomic<uint32_t> perfSdOpenFail{0};   // Perf SD file open failures
    std::atomic<uint32_t> perfSdHeaderFail{0}; // Perf SD CSV header write failures
    std::atomic<uint32_t> perfSdMarkerFail{0}; // Perf SD session marker write failures
    std::atomic<uint32_t> perfSdWriteFail{0};  // Perf SD data line write failures
    
    // Connection
    std::atomic<uint32_t> reconnects{0};       // BLE reconnection count
    std::atomic<uint32_t> disconnects{0};      // BLE disconnection count
    std::atomic<uint32_t> connectionDispatchRuns{0}; // connection-state dispatch passes
    std::atomic<uint32_t> connectionCadenceDisplayDue{0}; // cadence allowed display/update tick
    std::atomic<uint32_t> connectionCadenceHoldScanDwell{0}; // scan dwell hold suppressed process
    std::atomic<uint32_t> connectionStateProcessRuns{0}; // connectionStateModule.process() calls
    std::atomic<uint32_t> connectionStateWatchdogForces{0}; // watchdog-forced process calls
    std::atomic<uint32_t> connectionStateProcessGapMaxMs{0}; // max observed gap between process runs
    std::atomic<uint32_t> bleScanStateEntries{0}; // transitions into SCANNING
    std::atomic<uint32_t> bleScanStateExits{0}; // transitions out of SCANNING
    std::atomic<uint32_t> bleScanTargetFound{0}; // SCANNING->SCAN_STOPPING due to target found
    std::atomic<uint32_t> bleScanNoTargetExits{0}; // SCANNING->DISCONNECTED without target
    std::atomic<uint32_t> bleScanDwellMaxMs{0}; // max SCANNING state dwell duration
    
    // Display
    std::atomic<uint32_t> displayUpdates{0};   // Frames drawn
    std::atomic<uint32_t> displaySkips{0};     // Updates skipped (throttled)
    std::atomic<uint32_t> cameraDisplayActive{0}; // Real camera display path active marker (1/0)
    std::atomic<uint32_t> cameraDebugOverrideActive{0}; // Debug camera override active marker (1/0)
    std::atomic<uint32_t> cameraDisplayFrames{0}; // Frames drawn by real camera display path
    std::atomic<uint32_t> cameraDebugDisplayFrames{0}; // Frames drawn by debug camera display override

    // Mutex contention monitoring (should stay low/zero in normal operation)
    std::atomic<uint32_t> bleMutexSkip{0};        // HOT path try-lock skips
    std::atomic<uint32_t> bleMutexTimeout{0};     // COLD path timeout failures
    std::atomic<uint32_t> cmdPaceNotYet{0};       // sendCommand pacing deferrals
    std::atomic<uint32_t> cmdBleBusy{0};          // sendCommand BLE write failed (transient)
    std::atomic<uint32_t> uuid128FallbackHits{0}; // 128-bit custom UUID fast extraction path hits
    std::atomic<uint32_t> bleDiscTaskCreateFail{0}; // Discovery task spawn failures
    std::atomic<uint32_t> wifiConnectDeferred{0}; // WiFi connects staged via non-blocking phase machine
    std::atomic<uint32_t> wifiStopGraceful{0}; // graceful staged WiFi stop requests
    std::atomic<uint32_t> wifiStopImmediate{0}; // immediate WiFi stop requests
    std::atomic<uint32_t> wifiStopManual{0}; // stop requests flagged manual
    std::atomic<uint32_t> wifiStopTimeout{0}; // stop reason timeout
    std::atomic<uint32_t> wifiStopNoClients{0}; // stop reason no_clients
    std::atomic<uint32_t> wifiStopNoClientsAuto{0}; // stop reason no_clients_auto
    std::atomic<uint32_t> wifiStopLowDma{0}; // stop reason low_dma
    std::atomic<uint32_t> wifiStopPoweroff{0}; // stop reason poweroff
    std::atomic<uint32_t> wifiStopOther{0}; // stop reasons not covered above
    std::atomic<uint32_t> wifiApDropLowDma{0}; // AP retired due to sustained low SRAM in AP+STA
    std::atomic<uint32_t> wifiApDropIdleSta{0}; // AP retired while STA remained connected
    std::atomic<uint32_t> wifiApUpTransitions{0}; // AP transition marker: down->up
    std::atomic<uint32_t> wifiApDownTransitions{0}; // AP transition marker: up->down
    std::atomic<uint32_t> wifiApState{0}; // AP state marker (1=up, 0=down)
    std::atomic<uint32_t> wifiApLastTransitionMs{0}; // millis() when AP state last changed
    std::atomic<uint32_t> wifiApLastTransitionReason{0}; // PerfWifiApTransitionReason
    std::atomic<uint32_t> wifiProcessMaxUs{0}; // max WiFiManager::process() duration
    std::atomic<uint32_t> wifiHandleClientMaxUs{0}; // max server.handleClient() duration inside WiFi process
    std::atomic<uint32_t> wifiMaintenanceMaxUs{0}; // max maintenance block duration inside WiFi process
    std::atomic<uint32_t> wifiStatusCheckMaxUs{0}; // max checkWifiClientStatus() duration
    std::atomic<uint32_t> wifiTimeoutCheckMaxUs{0}; // max checkAutoTimeout() duration
    std::atomic<uint32_t> wifiHeapGuardMaxUs{0}; // max heap guard sampling/evaluation duration
    std::atomic<uint32_t> wifiApStaPollMaxUs{0}; // max softAP station poll duration
    std::atomic<uint32_t> proxyAdvertisingOnTransitions{0}; // proxy advertising transition marker: off->on
    std::atomic<uint32_t> proxyAdvertisingOffTransitions{0}; // proxy advertising transition marker: on->off
    std::atomic<uint32_t> proxyAdvertisingState{0}; // proxy advertising state marker (1=on, 0=off)
    std::atomic<uint32_t> proxyAdvertisingLastTransitionMs{0}; // millis() when proxy advertising state last changed
    std::atomic<uint32_t> proxyAdvertisingLastTransitionReason{0}; // PerfProxyAdvertisingTransitionReason
    std::atomic<uint32_t> pushNowRetries{0};      // Non-blocking Push Now retry attempts
    std::atomic<uint32_t> pushNowFailures{0};     // Non-blocking Push Now exhausted retries
    std::atomic<uint32_t> alertPersistStarts{0};  // Persisted-alert sessions started
    std::atomic<uint32_t> alertPersistExpires{0}; // Persisted-alert windows expired naturally
    std::atomic<uint32_t> alertPersistClears{0};  // Persisted-alert state cleared explicitly
    std::atomic<uint32_t> autoPushStarts{0};      // Auto-push runs initiated
    std::atomic<uint32_t> autoPushCompletes{0};   // Auto-push runs completed
    std::atomic<uint32_t> autoPushNoProfile{0};   // Auto-push slot had no configured profile
    std::atomic<uint32_t> autoPushProfileLoadFail{0}; // Auto-push profile load failures
    std::atomic<uint32_t> autoPushProfileWriteFail{0}; // Auto-push profile write exhausted retries
    std::atomic<uint32_t> autoPushBusyRetries{0}; // Auto-push write-busy retries
    std::atomic<uint32_t> autoPushModeFail{0};    // Auto-push mode set failures
    std::atomic<uint32_t> autoPushVolumeFail{0};  // Auto-push volume set failures
    std::atomic<uint32_t> autoPushDisconnectAbort{0}; // Auto-push aborted due to disconnect
    std::atomic<uint32_t> prioritySelectDisplayIndex{0}; // Legacy display-aux0 priority path (kept for CSV/API compatibility)
    std::atomic<uint32_t> prioritySelectRowFlag{0};      // Priority chosen from alert-row isPriority bit
    std::atomic<uint32_t> prioritySelectFirstUsable{0};  // Priority chosen from first usable alert fallback
    std::atomic<uint32_t> prioritySelectFirstEntry{0};   // Priority fell back to entry 0 (last resort)
    std::atomic<uint32_t> prioritySelectAmbiguousIndex{0}; // Alert table completed as both 0-based and 1-based
    std::atomic<uint32_t> prioritySelectUnusableIndex{0};  // Row-priority candidate existed but was unusable
    std::atomic<uint32_t> prioritySelectInvalidChosen{0};  // Chosen alert invalid/zero-freq non-laser
    std::atomic<uint32_t> alertTablePublishes{0};          // Complete alert tables published
    std::atomic<uint32_t> alertTablePublishes3Bogey{0};    // Complete tables published with count=3
    std::atomic<uint32_t> alertTableRowReplacements{0};    // Duplicate row index replacements
    std::atomic<uint32_t> alertTableAssemblyTimeouts{0};   // Partial table assemblies dropped on timeout
    std::atomic<uint32_t> parserRowsBandNone{0};           // Alert rows decoded with BAND_NONE
    std::atomic<uint32_t> parserRowsKuRaw{0};              // Alert rows with Ku raw band bit (0x10)
    std::atomic<uint32_t> displayLiveInvalidPrioritySkips{0}; // Live display update early-returned invalid priority
    std::atomic<uint32_t> displayLiveFallbackToUsable{0};  // Live display used fallback usable alert
    std::atomic<uint32_t> voiceAnnouncePriority{0}; // Voice priority announcements emitted
    std::atomic<uint32_t> voiceAnnounceDirection{0}; // Voice direction/bogey announcements emitted
    std::atomic<uint32_t> voiceAnnounceSecondary{0}; // Voice secondary announcements emitted
    std::atomic<uint32_t> voiceAnnounceEscalation{0}; // Voice escalation announcements emitted
    std::atomic<uint32_t> voiceDirectionThrottled{0}; // Voice direction announcements suppressed by throttle
    std::atomic<uint32_t> powerAutoPowerArmed{0};    // Auto power-off armed on first V1 data
    std::atomic<uint32_t> powerAutoPowerTimerStart{0}; // Auto power-off timer started
    std::atomic<uint32_t> powerAutoPowerTimerCancel{0}; // Auto power-off timer cancelled on reconnect
    std::atomic<uint32_t> powerAutoPowerTimerExpire{0}; // Auto power-off timer expired
    std::atomic<uint32_t> powerCriticalWarn{0};      // Critical-battery warning shown
    std::atomic<uint32_t> powerCriticalShutdown{0};  // Critical-battery shutdown triggered

    // Audio
    std::atomic<uint32_t> audioPlayCount{0};         // Audio play tasks successfully started
    std::atomic<uint32_t> audioPlayBusy{0};          // Audio plays rejected (already playing)
    std::atomic<uint32_t> audioTaskFail{0};          // Audio task creation failures
    std::atomic<uint32_t> cameraVoiceQueued{0};      // Camera voice events queued
    std::atomic<uint32_t> cameraVoiceStarted{0};     // Camera voice playback starts confirmed

    // Lockout signal observation SD logger
    std::atomic<uint32_t> sigObsQueueDrops{0};      // Signal observation SD queue full drops
    std::atomic<uint32_t> sigObsWriteFail{0};       // Signal observation SD write failures
    
    // Timing (microseconds for precision)
    std::atomic<uint32_t> lastNotifyUs{0};     // Timestamp of last notify
    std::atomic<uint32_t> lastFlushUs{0};      // Timestamp of last flush
    
    void reset() {
        rxPackets.store(0, std::memory_order_relaxed);
        rxBytes.store(0, std::memory_order_relaxed);
        queueDrops.store(0, std::memory_order_relaxed);
        oversizeDrops.store(0, std::memory_order_relaxed);
        queueHighWater.store(0, std::memory_order_relaxed);
        proxyQueueHighWater.store(0, std::memory_order_relaxed);
        phoneCmdQueueHighWater.store(0, std::memory_order_relaxed);
        phoneCmdDropsOverflow.store(0, std::memory_order_relaxed);
        phoneCmdDropsInvalid.store(0, std::memory_order_relaxed);
        phoneCmdDropsBleFail.store(0, std::memory_order_relaxed);
        phoneCmdDropsLockBusy.store(0, std::memory_order_relaxed);
        parseSuccesses.store(0, std::memory_order_relaxed);
        parseFailures.store(0, std::memory_order_relaxed);
        perfDrop.store(0, std::memory_order_relaxed);
        perfSdLockFail.store(0, std::memory_order_relaxed);
        perfSdDirFail.store(0, std::memory_order_relaxed);
        perfSdOpenFail.store(0, std::memory_order_relaxed);
        perfSdHeaderFail.store(0, std::memory_order_relaxed);
        perfSdMarkerFail.store(0, std::memory_order_relaxed);
        perfSdWriteFail.store(0, std::memory_order_relaxed);
        reconnects.store(0, std::memory_order_relaxed);
        disconnects.store(0, std::memory_order_relaxed);
        connectionDispatchRuns.store(0, std::memory_order_relaxed);
        connectionCadenceDisplayDue.store(0, std::memory_order_relaxed);
        connectionCadenceHoldScanDwell.store(0, std::memory_order_relaxed);
        connectionStateProcessRuns.store(0, std::memory_order_relaxed);
        connectionStateWatchdogForces.store(0, std::memory_order_relaxed);
        connectionStateProcessGapMaxMs.store(0, std::memory_order_relaxed);
        bleScanStateEntries.store(0, std::memory_order_relaxed);
        bleScanStateExits.store(0, std::memory_order_relaxed);
        bleScanTargetFound.store(0, std::memory_order_relaxed);
        bleScanNoTargetExits.store(0, std::memory_order_relaxed);
        bleScanDwellMaxMs.store(0, std::memory_order_relaxed);
        displayUpdates.store(0, std::memory_order_relaxed);
        displaySkips.store(0, std::memory_order_relaxed);
        cameraDisplayActive.store(0, std::memory_order_relaxed);
        cameraDebugOverrideActive.store(0, std::memory_order_relaxed);
        cameraDisplayFrames.store(0, std::memory_order_relaxed);
        cameraDebugDisplayFrames.store(0, std::memory_order_relaxed);
        bleMutexSkip.store(0, std::memory_order_relaxed);
        bleMutexTimeout.store(0, std::memory_order_relaxed);
        cmdPaceNotYet.store(0, std::memory_order_relaxed);
        cmdBleBusy.store(0, std::memory_order_relaxed);
        uuid128FallbackHits.store(0, std::memory_order_relaxed);
        bleDiscTaskCreateFail.store(0, std::memory_order_relaxed);
        wifiConnectDeferred.store(0, std::memory_order_relaxed);
        wifiStopGraceful.store(0, std::memory_order_relaxed);
        wifiStopImmediate.store(0, std::memory_order_relaxed);
        wifiStopManual.store(0, std::memory_order_relaxed);
        wifiStopTimeout.store(0, std::memory_order_relaxed);
        wifiStopNoClients.store(0, std::memory_order_relaxed);
        wifiStopNoClientsAuto.store(0, std::memory_order_relaxed);
        wifiStopLowDma.store(0, std::memory_order_relaxed);
        wifiStopPoweroff.store(0, std::memory_order_relaxed);
        wifiStopOther.store(0, std::memory_order_relaxed);
        wifiApDropLowDma.store(0, std::memory_order_relaxed);
        wifiApDropIdleSta.store(0, std::memory_order_relaxed);
        wifiApUpTransitions.store(0, std::memory_order_relaxed);
        wifiApDownTransitions.store(0, std::memory_order_relaxed);
        wifiApState.store(0, std::memory_order_relaxed);
        wifiApLastTransitionMs.store(0, std::memory_order_relaxed);
        wifiApLastTransitionReason.store(0, std::memory_order_relaxed);
        wifiProcessMaxUs.store(0, std::memory_order_relaxed);
        wifiHandleClientMaxUs.store(0, std::memory_order_relaxed);
        wifiMaintenanceMaxUs.store(0, std::memory_order_relaxed);
        wifiStatusCheckMaxUs.store(0, std::memory_order_relaxed);
        wifiTimeoutCheckMaxUs.store(0, std::memory_order_relaxed);
        wifiHeapGuardMaxUs.store(0, std::memory_order_relaxed);
        wifiApStaPollMaxUs.store(0, std::memory_order_relaxed);
        proxyAdvertisingOnTransitions.store(0, std::memory_order_relaxed);
        proxyAdvertisingOffTransitions.store(0, std::memory_order_relaxed);
        proxyAdvertisingState.store(0, std::memory_order_relaxed);
        proxyAdvertisingLastTransitionMs.store(0, std::memory_order_relaxed);
        proxyAdvertisingLastTransitionReason.store(0, std::memory_order_relaxed);
        pushNowRetries.store(0, std::memory_order_relaxed);
        pushNowFailures.store(0, std::memory_order_relaxed);
        alertPersistStarts.store(0, std::memory_order_relaxed);
        alertPersistExpires.store(0, std::memory_order_relaxed);
        alertPersistClears.store(0, std::memory_order_relaxed);
        autoPushStarts.store(0, std::memory_order_relaxed);
        autoPushCompletes.store(0, std::memory_order_relaxed);
        autoPushNoProfile.store(0, std::memory_order_relaxed);
        autoPushProfileLoadFail.store(0, std::memory_order_relaxed);
        autoPushProfileWriteFail.store(0, std::memory_order_relaxed);
        autoPushBusyRetries.store(0, std::memory_order_relaxed);
        autoPushModeFail.store(0, std::memory_order_relaxed);
        autoPushVolumeFail.store(0, std::memory_order_relaxed);
        autoPushDisconnectAbort.store(0, std::memory_order_relaxed);
        prioritySelectDisplayIndex.store(0, std::memory_order_relaxed);
        prioritySelectRowFlag.store(0, std::memory_order_relaxed);
        prioritySelectFirstUsable.store(0, std::memory_order_relaxed);
        prioritySelectFirstEntry.store(0, std::memory_order_relaxed);
        prioritySelectAmbiguousIndex.store(0, std::memory_order_relaxed);
        prioritySelectUnusableIndex.store(0, std::memory_order_relaxed);
        prioritySelectInvalidChosen.store(0, std::memory_order_relaxed);
        alertTablePublishes.store(0, std::memory_order_relaxed);
        alertTablePublishes3Bogey.store(0, std::memory_order_relaxed);
        alertTableRowReplacements.store(0, std::memory_order_relaxed);
        alertTableAssemblyTimeouts.store(0, std::memory_order_relaxed);
        parserRowsBandNone.store(0, std::memory_order_relaxed);
        parserRowsKuRaw.store(0, std::memory_order_relaxed);
        displayLiveInvalidPrioritySkips.store(0, std::memory_order_relaxed);
        displayLiveFallbackToUsable.store(0, std::memory_order_relaxed);
        voiceAnnouncePriority.store(0, std::memory_order_relaxed);
        voiceAnnounceDirection.store(0, std::memory_order_relaxed);
        voiceAnnounceSecondary.store(0, std::memory_order_relaxed);
        voiceAnnounceEscalation.store(0, std::memory_order_relaxed);
        voiceDirectionThrottled.store(0, std::memory_order_relaxed);
        powerAutoPowerArmed.store(0, std::memory_order_relaxed);
        powerAutoPowerTimerStart.store(0, std::memory_order_relaxed);
        powerAutoPowerTimerCancel.store(0, std::memory_order_relaxed);
        powerAutoPowerTimerExpire.store(0, std::memory_order_relaxed);
        powerCriticalWarn.store(0, std::memory_order_relaxed);
        powerCriticalShutdown.store(0, std::memory_order_relaxed);
        audioPlayCount.store(0, std::memory_order_relaxed);
        audioPlayBusy.store(0, std::memory_order_relaxed);
        audioTaskFail.store(0, std::memory_order_relaxed);
        cameraVoiceQueued.store(0, std::memory_order_relaxed);
        cameraVoiceStarted.store(0, std::memory_order_relaxed);
        sigObsQueueDrops.store(0, std::memory_order_relaxed);
        sigObsWriteFail.store(0, std::memory_order_relaxed);
    }
};

// ============================================================================
// Extended metrics for p95/max latency, loop jitter, heap stats
// ==========================================================================
struct PerfHistogramMs {
    static constexpr size_t kBucketCount = 10;
    uint32_t buckets[kBucketCount] = {0};
    uint32_t total = 0;
    uint32_t maxMs = 0;
    uint32_t overflow = 0;  // Samples exceeding max bucket (>1000ms)

    void reset() {
        for (size_t i = 0; i < kBucketCount; ++i) {
            buckets[i] = 0;
        }
        total = 0;
        maxMs = 0;
        overflow = 0;
    }
};

enum class PerfDisplayScreen : uint8_t {
    Unknown = 0,
    Resting = 1,
    Scanning = 2,
    Disconnected = 3,
    Live = 4,
    Persisted = 5,
    Camera = 6
};

enum class PerfFadeDecision : uint8_t {
    None = 0,
    FadeDown = 1,
    RestoreApplied = 2,
    RestoreSkippedEqual = 3,
    RestoreSkippedNoBaseline = 4,
    RestoreSkippedNotFaded = 5
};

enum class PerfBleTimelineEvent : uint8_t {
    ScanStart = 1,
    TargetFound = 2,
    ConnectStart = 3,
    Connected = 4,
    FirstRx = 5
};

enum class PerfWifiApTransitionReason : uint8_t {
    Unknown = 0,
    Startup = 1,
    StopManual = 2,
    StopTimeout = 3,
    StopNoClients = 4,
    StopNoClientsAuto = 5,
    DropLowDma = 6,
    DropIdleSta = 7,
    StopPoweroff = 8,
    StopOther = 9
};

enum class PerfProxyAdvertisingTransitionReason : uint8_t {
    Unknown = 0,
    StartConnected = 1,
    StartWifiPriorityResume = 2,
    StartRetryWindow = 3,
    StartAppDisconnect = 4,
    StartDirect = 5,
    StopWifiPriority = 6,
    StopNoClientTimeout = 7,
    StopIdleWindow = 8,
    StopBeforeV1Connect = 9,
    StopV1Disconnect = 10,
    StopAppConnected = 11,
    StopOther = 12
};

struct PerfExtendedMetrics {
    PerfHistogramMs notifyToDisplayMs;
    PerfHistogramMs notifyToProxyMs;
    uint32_t loopMaxUs = 0;
    uint32_t minFreeHeap = UINT32_MAX;
    uint32_t minLargestBlock = UINT32_MAX;
    uint32_t minFreeDma = UINT32_MAX;         // DMA-capable internal SRAM (WiFi/SD contention)
    uint32_t minLargestDma = UINT32_MAX;      // Largest DMA block (fragmentation detection)
    uint32_t wifiMaxUs = 0;
    uint32_t fsMaxUs = 0;
    uint32_t sdMaxUs = 0;
    uint32_t flushMaxUs = 0;
    uint32_t displayRenderMaxUs = 0;  // Full display render time (draw + flush)
    uint32_t bleDrainMaxUs = 0;
    // BLE connection path timing (for diagnosing reconnect stalls)
    uint32_t bleConnectMaxUs = 0;     // pClient->connect() duration
    uint32_t bleDiscoveryMaxUs = 0;   // discoverAttributes() duration
    uint32_t bleSubscribeMaxUs = 0;   // setupCharacteristics() duration
    uint32_t bleProcessMaxUs = 0;     // bleClient.process() total duration
    uint32_t dispPipeMaxUs = 0;       // displayPipelineModule.handleParsed() duration
    uint32_t touchMaxUs = 0;          // touchUiModule.process() duration
    uint32_t cameraDisplayMaxUs = 0;  // Real camera display update duration
    uint32_t cameraDebugDisplayMaxUs = 0; // Debug camera display update duration
    uint32_t cameraProcessMaxUs = 0;  // CameraAlertModule::process() duration
    uint32_t gpsMaxUs = 0;             // gpsRuntimeModule.update() duration
    uint32_t lockoutMaxUs = 0;         // lockoutEnforcer.process() + signalCapture duration
    uint32_t lockoutSaveMaxUs = 0;     // lockout zone JSON serialize + SD write
    uint32_t learnerSaveMaxUs = 0;     // learner pending JSON serialize + SD write
    uint32_t timeSaveMaxUs = 0;        // timeService.periodicSave NVS write
    uint32_t perfReportMaxUs = 0;      // perfMetricsCheckReport snapshot + enqueue
    uint32_t uiToScanCount = 0;       // Screen transitions -> Scanning
    uint32_t uiToRestCount = 0;       // Screen transitions -> Resting
    uint32_t uiScanToRestCount = 0;   // Scanning -> Resting transitions
    uint32_t uiFastScanExitCount = 0; // Scanning dwell < threshold before leaving
    uint32_t uiLastScanDwellMs = 0;   // Last measured scanning dwell
    uint32_t uiMinScanDwellMs = UINT32_MAX; // Session minimum scanning dwell
    uint32_t uiLastScanEnteredMs = 0; // Internal marker for scanning dwell
    uint32_t fadeDownCount = 0;       // Fade-down commands generated
    uint32_t fadeRestoreCount = 0;    // Restore commands generated
    uint32_t fadeSkipEqualCount = 0;  // Restore skipped because current == original
    uint32_t fadeSkipNoBaselineCount = 0; // Restore skipped (baseline missing)
    uint32_t fadeSkipNotFadedCount = 0;   // Restore skipped (session not faded)
    uint8_t fadeLastDecision = 0;     // PerfFadeDecision
    uint8_t fadeLastCurrentVol = 0xFF;
    uint8_t fadeLastOriginalVol = 0xFF;
    uint32_t fadeLastDecisionMs = 0;
    uint32_t bleScanStartMs = 0;      // First transition to SCANNING
    uint32_t bleTargetFoundMs = 0;    // First "V1 found" scan-stop transition
    uint32_t bleConnectStartMs = 0;   // First transition to CONNECTING
    uint32_t bleConnectedMs = 0;      // First transition to CONNECTED
    uint32_t bleFirstRxMs = 0;        // First packet observed in BLE drain path

    void reset() {
        notifyToDisplayMs.reset();
        notifyToProxyMs.reset();
        loopMaxUs = 0;
        minFreeHeap = UINT32_MAX;
        minLargestBlock = UINT32_MAX;
        minFreeDma = UINT32_MAX;
        minLargestDma = UINT32_MAX;
        wifiMaxUs = 0;
        fsMaxUs = 0;
        sdMaxUs = 0;
        flushMaxUs = 0;
        displayRenderMaxUs = 0;
        bleDrainMaxUs = 0;
        bleConnectMaxUs = 0;
        bleDiscoveryMaxUs = 0;
        bleSubscribeMaxUs = 0;
        bleProcessMaxUs = 0;
        dispPipeMaxUs = 0;
        touchMaxUs = 0;
        cameraDisplayMaxUs = 0;
        cameraDebugDisplayMaxUs = 0;
        cameraProcessMaxUs = 0;
        gpsMaxUs = 0;
        lockoutMaxUs = 0;
        lockoutSaveMaxUs = 0;
        learnerSaveMaxUs = 0;
        timeSaveMaxUs = 0;
        perfReportMaxUs = 0;
        uiToScanCount = 0;
        uiToRestCount = 0;
        uiScanToRestCount = 0;
        uiFastScanExitCount = 0;
        uiLastScanDwellMs = 0;
        uiMinScanDwellMs = UINT32_MAX;
        uiLastScanEnteredMs = 0;
        fadeDownCount = 0;
        fadeRestoreCount = 0;
        fadeSkipEqualCount = 0;
        fadeSkipNoBaselineCount = 0;
        fadeSkipNotFadedCount = 0;
        fadeLastDecision = static_cast<uint8_t>(PerfFadeDecision::None);
        fadeLastCurrentVol = 0xFF;
        fadeLastOriginalVol = 0xFF;
        fadeLastDecisionMs = 0;
        bleScanStartMs = 0;
        bleTargetFoundMs = 0;
        bleConnectStartMs = 0;
        bleConnectedMs = 0;
        bleFirstRxMs = 0;
    }
};

extern PerfExtendedMetrics perfExtended;

void perfRecordNotifyToDisplayMs(uint32_t ms);
void perfRecordNotifyToProxyMs(uint32_t ms);
void perfRecordLoopJitterUs(uint32_t us);
void perfRecordHeapStats(uint32_t freeHeap, uint32_t largestBlock, uint32_t freeDma, uint32_t largestDma);
void perfRecordWifiProcessUs(uint32_t us);
void perfRecordFsServeUs(uint32_t us);
void perfRecordSdFlushUs(uint32_t us);
void perfRecordFlushUs(uint32_t us);
void perfRecordDisplayRenderUs(uint32_t us);
void perfRecordBleDrainUs(uint32_t us);
void perfRecordBleConnectUs(uint32_t us);
void perfRecordBleDiscoveryUs(uint32_t us);
void perfRecordBleSubscribeUs(uint32_t us);
void perfRecordBleProcessUs(uint32_t us);
void perfRecordDispPipeUs(uint32_t us);
void perfRecordTouchUs(uint32_t us);
void perfRecordCameraDisplayUs(uint32_t us);
void perfRecordCameraDebugDisplayUs(uint32_t us);
void perfRecordCameraProcessUs(uint32_t us);
void perfRecordGpsUs(uint32_t us);
void perfRecordLockoutUs(uint32_t us);
void perfRecordLockoutSaveUs(uint32_t us);
void perfRecordLearnerSaveUs(uint32_t us);
void perfRecordTimeSaveUs(uint32_t us);
void perfRecordPerfReportUs(uint32_t us);
void perfRecordDisplayScreenTransition(PerfDisplayScreen from, PerfDisplayScreen to, uint32_t nowMs);
void perfRecordVolumeFadeDecision(PerfFadeDecision decision, uint8_t currentVolume, uint8_t originalVolume, uint32_t nowMs);
void perfRecordBleTimelineEvent(PerfBleTimelineEvent event, uint32_t nowMs);
void perfRecordWifiApTransition(bool apActive, uint8_t reasonCode, uint32_t nowMs);
void perfRecordProxyAdvertisingTransition(bool advertising, uint8_t reasonCode, uint32_t nowMs);

uint32_t perfGetLoopMaxUs();
uint32_t perfGetMinFreeHeap();
uint32_t perfGetMinFreeDma();
uint32_t perfGetMinLargestDma();
uint32_t perfGetWifiMaxUs();
uint32_t perfGetFsMaxUs();
uint32_t perfGetSdMaxUs();
uint32_t perfGetFlushMaxUs();
uint32_t perfGetBleDrainMaxUs();
uint32_t perfGetBleProcessMaxUs();
uint32_t perfGetDispPipeMaxUs();
uint32_t perfGetCameraDisplayMaxUs();
uint32_t perfGetCameraDebugDisplayMaxUs();
uint32_t perfGetCameraProcessMaxUs();
uint32_t perfGetPrevWindowLoopMaxUs();
uint32_t perfGetPrevWindowWifiMaxUs();
uint32_t perfGetPrevWindowBleProcessMaxUs();
uint32_t perfGetPrevWindowDispPipeMaxUs();
uint32_t perfGetWifiApState();
uint32_t perfGetWifiApLastTransitionMs();
uint32_t perfGetWifiApLastTransitionReason();
const char* perfWifiApTransitionReasonName(uint32_t reasonCode);
uint32_t perfGetProxyAdvertisingState();
uint32_t perfGetProxyAdvertisingLastTransitionMs();
uint32_t perfGetProxyAdvertisingLastTransitionReason();
const char* perfProxyAdvertisingTransitionReasonName(uint32_t reasonCode);

// ============================================================================
// Sampled latency tracking (only when PERF_METRICS=1)
// Uses std::atomic for thread-safe access
// ============================================================================
struct PerfLatency {
    // BLE→Flush latency (microseconds)
    std::atomic<uint32_t> minUs{UINT32_MAX};
    std::atomic<uint32_t> maxUs{0};
    std::atomic<uint64_t> totalUs{0};
    std::atomic<uint32_t> sampleCount{0};
    
    // Per-stage breakdown (for debugging bottlenecks)
    std::atomic<uint32_t> notifyToQueueUs{0};    // notify callback → queue send
    std::atomic<uint32_t> queueToParseUs{0};     // queue receive → parse done
    std::atomic<uint32_t> parseToFlushUs{0};     // parse done → display flush
    
    void reset() {
        minUs.store(UINT32_MAX, std::memory_order_relaxed);
        maxUs.store(0, std::memory_order_relaxed);
        totalUs.store(0, std::memory_order_relaxed);
        sampleCount.store(0, std::memory_order_relaxed);
        notifyToQueueUs.store(0, std::memory_order_relaxed);
        queueToParseUs.store(0, std::memory_order_relaxed);
        parseToFlushUs.store(0, std::memory_order_relaxed);
    }
    
    uint32_t avgUs() const {
        uint32_t count = sampleCount.load(std::memory_order_relaxed);
        return count > 0 ? static_cast<uint32_t>(totalUs.load(std::memory_order_relaxed) / count) : 0;
    }
};

// ============================================================================
// Global instances
// ============================================================================
extern PerfCounters perfCounters;

#if PERF_METRICS
extern PerfLatency perfLatency;
#endif

#if PERF_METRICS && PERF_MONITORING
extern bool perfDebugEnabled;        // Runtime debug print enable
extern uint32_t perfLastReportMs;    // Last report timestamp
#endif

// ============================================================================
// Inline instrumentation macros (zero cost when disabled)
// ============================================================================

// Always-on counter increments
#define PERF_INC(counter) (perfCounters.counter++)
#define PERF_ADD(counter, value) (perfCounters.counter += (value))
#define PERF_SET(counter, value) (perfCounters.counter = (value))
#define PERF_MAX(counter, value) do { \
    const uint32_t _perfMaxValue = static_cast<uint32_t>(value); \
    uint32_t _perfMaxCurrent = perfCounters.counter.load(std::memory_order_relaxed); \
    while (_perfMaxValue > _perfMaxCurrent && \
           !perfCounters.counter.compare_exchange_weak(_perfMaxCurrent, _perfMaxValue, \
                                                       std::memory_order_relaxed, \
                                                       std::memory_order_relaxed)) {} \
} while(0)

// Timestamp capture (always on, but cheap)
#define PERF_TIMESTAMP_US() ((uint32_t)esp_timer_get_time())

#if PERF_METRICS && PERF_MONITORING

// Sampled latency recording
#define PERF_SAMPLE_LATENCY(startUs, endUs) do { \
    static uint32_t _sampleCounter = 0; \
    if ((++_sampleCounter & (PERF_SAMPLE_RATE - 1)) == 0) { \
        uint32_t _lat = (endUs) - (startUs); \
        if (_lat < perfLatency.minUs) perfLatency.minUs = _lat; \
        if (_lat > perfLatency.maxUs) perfLatency.maxUs = _lat; \
        perfLatency.totalUs += _lat; \
        perfLatency.sampleCount++; \
    } \
} while(0)

// Stage timing (for debugging)
#if PERF_VERBOSE
#define PERF_STAGE_TIME(stage, value) (perfLatency.stage = (value))
#else
#define PERF_STAGE_TIME(stage, value) ((void)0)
#endif

// Threshold alert (immediate print if exceeded)
#if PERF_VERBOSE
#define PERF_ALERT_IF_SLOW(latencyUs) do { \
    if (perfDebugEnabled && (latencyUs) > (PERF_LATENCY_ALERT_MS * 1000)) { \
        Serial.printf("[PERF ALERT] latency=%luus\n", (unsigned long)(latencyUs)); \
    } \
} while(0)
#else
#define PERF_ALERT_IF_SLOW(latencyUs) ((void)0)
#endif

#else  // PERF_METRICS == 0 or PERF_MONITORING == 0

#define PERF_SAMPLE_LATENCY(startUs, endUs) ((void)0)
#define PERF_STAGE_TIME(stage, value) ((void)0)
#define PERF_ALERT_IF_SLOW(latencyUs) ((void)0)

#endif  // PERF_METRICS && PERF_MONITORING

// ============================================================================
// API functions
// ============================================================================

// Initialize metrics system
void perfMetricsInit();

// Reset all metrics
void perfMetricsReset();

// Check if periodic report is due (call from loop)
// Returns true if report was printed
bool perfMetricsCheckReport();

// Best-effort immediate SD snapshot enqueue (non-blocking); returns false on skip/drop.
bool perfMetricsEnqueueSnapshotNow();

// Get JSON summary for web API
String perfMetricsToJson();

// Enable/disable debug prints at runtime
void perfMetricsSetDebug(bool enabled);

#endif // PERF_METRICS_H
