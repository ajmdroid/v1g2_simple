"""
Phase 3.11: Migrate TouchUiModule::Callbacks + DebugApiService::SoakMetricsBuildFn
to fn-ptr + void* ctx pattern.
"""
import re

def replace_exactly_once(src, old, new, label):
    count = src.count(old)
    if count == 0:
        print(f'  NOT FOUND: {label}')
        return src
    if count > 1:
        print(f'  MULTIPLE MATCHES ({count}): {label}')
        return src
    print(f'  Replaced: {label}')
    return src.replace(old, new, 1)

# ============================================================
# 1. touch_ui_module.h - Callbacks struct
# ============================================================
path = 'src/modules/touch/touch_ui_module.h'
src = open(path).read()

src = replace_exactly_once(src, '#include <functional>\n\n', '', 'remove <functional>')

old_callbacks = '''    struct Callbacks {
        std::function<bool()> isWifiSetupActive;
        std::function<void()> stopWifiSetup;   // stop AP/setup mode
        std::function<void()> startWifi;       // start WiFi/AP
        std::function<void()> drawWifiIndicator;
        std::function<void()> restoreDisplay;  // refresh display with current state
        std::function<ObdRuntimeStatus(uint32_t nowMs)> readObdStatus;
        std::function<bool(uint32_t nowMs)> requestObdManualPairScan;
        std::function<bool(uint32_t nowMs)> isObdPairGestureSafe;
    };'''

new_callbacks = '''    struct Callbacks {
        bool (*isWifiSetupActive)(void* ctx) = nullptr;
        void* isWifiSetupActiveCtx = nullptr;
        void (*stopWifiSetup)(void* ctx) = nullptr;       // stop AP/setup mode
        void* stopWifiSetupCtx = nullptr;
        void (*startWifi)(void* ctx) = nullptr;           // start WiFi/AP
        void* startWifiCtx = nullptr;
        void (*drawWifiIndicator)(void* ctx) = nullptr;
        void* drawWifiIndicatorCtx = nullptr;
        void (*restoreDisplay)(void* ctx) = nullptr;      // refresh display with current state
        void* restoreDisplayCtx = nullptr;
        ObdRuntimeStatus (*readObdStatus)(uint32_t nowMs, void* ctx) = nullptr;
        void* readObdStatusCtx = nullptr;
        bool (*requestObdManualPairScan)(uint32_t nowMs, void* ctx) = nullptr;
        void* requestObdManualPairScanCtx = nullptr;
        bool (*isObdPairGestureSafe)(uint32_t nowMs, void* ctx) = nullptr;
        void* isObdPairGestureSafeCtx = nullptr;
    };'''

src = replace_exactly_once(src, old_callbacks, new_callbacks, 'Callbacks struct')
open(path, 'w').write(src)
print(f'touch_ui_module.h std::function remaining: {src.count("std::function")}')

# ============================================================
# 2. touch_ui_module.cpp - all callback invocations
# ============================================================
path = 'src/modules/touch/touch_ui_module.cpp'
src = open(path).read()

# requestObdManualPairScan
src = replace_exactly_once(src,
    'if (callbacks.requestObdManualPairScan) {\n                (void)callbacks.requestObdManualPairScan(nowMs);\n            }',
    'if (callbacks.requestObdManualPairScan) {\n                (void)callbacks.requestObdManualPairScan(nowMs, callbacks.requestObdManualPairScanCtx);\n            }',
    'requestObdManualPairScan call')

# isWifiSetupActive + stopWifiSetup + startWifi + drawWifiIndicator
old_wifi_block = '''            if (callbacks.isWifiSetupActive && callbacks.isWifiSetupActive()) {
                if (callbacks.stopWifiSetup) callbacks.stopWifiSetup();
            } else {
                if (callbacks.startWifi) callbacks.startWifi();
            }
            if (callbacks.drawWifiIndicator) callbacks.drawWifiIndicator();'''
new_wifi_block = '''            if (callbacks.isWifiSetupActive && callbacks.isWifiSetupActive(callbacks.isWifiSetupActiveCtx)) {
                if (callbacks.stopWifiSetup) callbacks.stopWifiSetup(callbacks.stopWifiSetupCtx);
            } else {
                if (callbacks.startWifi) callbacks.startWifi(callbacks.startWifiCtx);
            }
            if (callbacks.drawWifiIndicator) callbacks.drawWifiIndicator(callbacks.drawWifiIndicatorCtx);'''
src = replace_exactly_once(src, old_wifi_block, new_wifi_block, 'wifi toggle block')

# restoreDisplay
src = replace_exactly_once(src,
    'if (callbacks.restoreDisplay) callbacks.restoreDisplay();',
    'if (callbacks.restoreDisplay) callbacks.restoreDisplay(callbacks.restoreDisplayCtx);',
    'restoreDisplay call')

# canArmObdPairGesture: readObdStatus + isObdPairGestureSafe
old_obd = '''    const ObdRuntimeStatus status = callbacks.readObdStatus(nowMs);
    return status.enabled &&
           !status.connected &&
           !status.scanInProgress &&
           !status.manualScanPending &&
           callbacks.isObdPairGestureSafe(nowMs);'''
new_obd = '''    const ObdRuntimeStatus status = callbacks.readObdStatus(nowMs, callbacks.readObdStatusCtx);
    return status.enabled &&
           !status.connected &&
           !status.scanInProgress &&
           !status.manualScanPending &&
           callbacks.isObdPairGestureSafe(nowMs, callbacks.isObdPairGestureSafeCtx);'''
src = replace_exactly_once(src, old_obd, new_obd, 'canArmObdPairGesture callbacks')

open(path, 'w').write(src)
print(f'touch_ui_module.cpp std::function remaining: {src.count("std::function")}')

# ============================================================
# 3. main.cpp - TouchUiModule::Callbacks initialization
# ============================================================
path = 'src/main.cpp'
src = open(path).read()

old_touch_cbs = '''    TouchUiModule::Callbacks touchCbs{
        .isWifiSetupActive = [] { return wifiManager.isWifiServiceActive(); },
        .stopWifiSetup = [] { wifiManager.stopSetupMode(true); },
        .startWifi = [] { (void)wifiManager.startSetupMode(false); },
        .drawWifiIndicator = [] { display.drawWiFiIndicator(); },
        .restoreDisplay = [] {
            if (mainRuntimeState.bootSplashHoldActive) {
                return;
            }
            displayPipelineModule.restoreCurrentOwner(millis());
        },
        .readObdStatus = [](uint32_t nowMs) { return obdRuntimeModule.snapshot(nowMs); },
        .requestObdManualPairScan = [](uint32_t nowMs) {
            return obdRuntimeModule.requestManualPairScan(nowMs);
        },
        .isObdPairGestureSafe = [](uint32_t nowMs) {
            return displayPipelineModule.allowsObdPairGesture(nowMs);
        }
    };'''

new_touch_cbs = '''    TouchUiModule::Callbacks touchCbs{
        .isWifiSetupActive = [](void* /*ctx*/) { return wifiManager.isWifiServiceActive(); },
        .stopWifiSetup = [](void* /*ctx*/) { wifiManager.stopSetupMode(true); },
        .startWifi = [](void* /*ctx*/) { (void)wifiManager.startSetupMode(false); },
        .drawWifiIndicator = [](void* /*ctx*/) { display.drawWiFiIndicator(); },
        .restoreDisplay = [](void* /*ctx*/) {
            if (mainRuntimeState.bootSplashHoldActive) {
                return;
            }
            displayPipelineModule.restoreCurrentOwner(millis());
        },
        .readObdStatus = [](uint32_t nowMs, void* /*ctx*/) { return obdRuntimeModule.snapshot(nowMs); },
        .requestObdManualPairScan = [](uint32_t nowMs, void* /*ctx*/) {
            return obdRuntimeModule.requestManualPairScan(nowMs);
        },
        .isObdPairGestureSafe = [](uint32_t nowMs, void* /*ctx*/) {
            return displayPipelineModule.allowsObdPairGesture(nowMs);
        }
    };'''

src = replace_exactly_once(src, old_touch_cbs, new_touch_cbs, 'TouchUiModule::Callbacks in main.cpp')
open(path, 'w').write(src)
print(f'main.cpp touch std::function remaining: {src.count("std::function")}')

# ============================================================
# 4. test_touch_ui_module.cpp
# ============================================================
path = 'test/test_touch_ui_module/test_touch_ui_module.cpp'
src = open(path).read()

old_test_cbs = '''TouchUiModule::Callbacks makeCallbacks() {
    return TouchUiModule::Callbacks{
        .isWifiSetupActive = [] { return wifiSetupActive; },
        .stopWifiSetup = [] { ++wifiStopCalls; wifiSetupActive = false; },
        .startWifi = [] { ++wifiStartCalls; wifiSetupActive = true; },
        .drawWifiIndicator = [] { display.drawWiFiIndicator(); },
        .restoreDisplay = [] {},
        .readObdStatus = [](uint32_t) { return obdStatus; },
        .requestObdManualPairScan = [](uint32_t) {
            ++manualPairRequests;
            obdStatus.manualScanPending = true;
            return true;
        },
        .isObdPairGestureSafe = [](uint32_t) { return obdPairSafe; },
    };
}'''

new_test_cbs = '''TouchUiModule::Callbacks makeCallbacks() {
    return TouchUiModule::Callbacks{
        .isWifiSetupActive = [](void* /*ctx*/) { return wifiSetupActive; },
        .stopWifiSetup = [](void* /*ctx*/) { ++wifiStopCalls; wifiSetupActive = false; },
        .startWifi = [](void* /*ctx*/) { ++wifiStartCalls; wifiSetupActive = true; },
        .drawWifiIndicator = [](void* /*ctx*/) { display.drawWiFiIndicator(); },
        .restoreDisplay = [](void* /*ctx*/) {},
        .readObdStatus = [](uint32_t, void* /*ctx*/) { return obdStatus; },
        .requestObdManualPairScan = [](uint32_t, void* /*ctx*/) {
            ++manualPairRequests;
            obdStatus.manualScanPending = true;
            return true;
        },
        .isObdPairGestureSafe = [](uint32_t, void* /*ctx*/) { return obdPairSafe; },
    };
}'''

src = replace_exactly_once(src, old_test_cbs, new_test_cbs, 'test makeCallbacks')
open(path, 'w').write(src)
print(f'test_touch_ui_module.cpp std::function remaining: {src.count("std::function")}')

# ============================================================
# 5. debug_soak_metrics_cache.h
# ============================================================
path = 'src/modules/debug/debug_soak_metrics_cache.h'
src = open(path).read()

src = replace_exactly_once(src, '#include <functional>\n\n', '', 'remove <functional>')

# Remove SoakMetricsBuildFn typedef and update signature
old_header_sig = '''using SoakMetricsBuildFn = std::function<void(JsonDocument&)>;

bool sendCachedSoakMetrics(WebServer& server,
                           SoakMetricsJsonCache& cache,
                           uint32_t cacheTtlMs,
                           const SoakMetricsBuildFn& buildMetrics,
                           const std::function<uint32_t()>& millisFn = nullptr);'''

new_header_sig = '''bool sendCachedSoakMetrics(WebServer& server,
                           SoakMetricsJsonCache& cache,
                           uint32_t cacheTtlMs,
                           void (*buildMetrics)(JsonDocument&, void*), void* buildMetricsCtx,
                           uint32_t (*millisFn)(void*) = nullptr, void* millisCtx = nullptr);'''

src = replace_exactly_once(src, old_header_sig, new_header_sig, 'sendCachedSoakMetrics signature + remove typedef')
open(path, 'w').write(src)
print(f'debug_soak_metrics_cache.h std::function remaining: {src.count("std::function")}')

# ============================================================
# 6. debug_soak_metrics_cache.cpp
# ============================================================
path = 'src/modules/debug/debug_soak_metrics_cache.cpp'
src = open(path).read()

old_cpp_sig = '''bool sendCachedSoakMetrics(WebServer& server,
                           SoakMetricsJsonCache& cache,
                           uint32_t cacheTtlMs,
                           const SoakMetricsBuildFn& buildMetrics,
                           const std::function<uint32_t()>& millisFn) {
    const uint32_t nowMs = millisFn ? millisFn() : static_cast<uint32_t>(millis());'''

new_cpp_sig = '''bool sendCachedSoakMetrics(WebServer& server,
                           SoakMetricsJsonCache& cache,
                           uint32_t cacheTtlMs,
                           void (*buildMetrics)(JsonDocument&, void*), void* buildMetricsCtx,
                           uint32_t (*millisFn)(void*), void* millisCtx) {
    const uint32_t nowMs = millisFn ? millisFn(millisCtx) : static_cast<uint32_t>(millis());'''

src = replace_exactly_once(src, old_cpp_sig, new_cpp_sig, 'sendCachedSoakMetrics signature in cpp')

# Update buildMetrics call inside the function
src = replace_exactly_once(src,
    '    if (buildMetrics) {\n        buildMetrics(doc);\n    }',
    '    if (buildMetrics) {\n        buildMetrics(doc, buildMetricsCtx);\n    }',
    'buildMetrics call')

open(path, 'w').write(src)
print(f'debug_soak_metrics_cache.cpp std::function remaining: {src.count("std::function")}')

# ============================================================
# 7. debug_api_service.cpp - sendMetricsSoak call
# ============================================================
path = 'src/modules/debug/debug_api_service.cpp'
src = open(path).read()

old_soak = '''    DebugApiService::sendCachedSoakMetrics(
        server,
        gSoakMetricsCache,
        kSoakMetricsCacheTtlMs,
        [](JsonDocument& doc) {
            buildMetricsSoakDoc(doc);
        },
        []() {
            return static_cast<uint32_t>(millis());
        });'''

new_soak = '''    DebugApiService::sendCachedSoakMetrics(
        server,
        gSoakMetricsCache,
        kSoakMetricsCacheTtlMs,
        [](JsonDocument& doc, void* /*ctx*/) {
            buildMetricsSoakDoc(doc);
        }, nullptr,
        [](void* /*ctx*/) -> uint32_t {
            return static_cast<uint32_t>(millis());
        }, nullptr);'''

src = replace_exactly_once(src, old_soak, new_soak, 'sendCachedSoakMetrics call in debug_api_service.cpp')
open(path, 'w').write(src)
print(f'debug_api_service.cpp std::function remaining: {src.count("std::function")}')

# ============================================================
# 8. test_debug_soak_metrics_cache.cpp
# ============================================================
path = 'test/test_debug_soak_metrics_cache/test_debug_soak_metrics_cache.cpp'
src = open(path).read()

# Add static buildFnWrapper and remove makeBuildFn
old_make_build_fn = '''DebugApiService::SoakMetricsBuildFn makeBuildFn(FakeMetricsSource& source) {
    return [&source](JsonDocument& doc) {
        source.buildCalls++;
        doc["mode"] = source.mode;
        doc["counter"] = source.counter;
        if (source.blob.length() > 0) {
            doc["blob"] = source.blob;
        }
    };
}'''

new_make_build_fn = '''static void buildFnWrapper(JsonDocument& doc, void* ctx) {
    auto* source = static_cast<FakeMetricsSource*>(ctx);
    source->buildCalls++;
    doc["mode"] = source->mode;
    doc["counter"] = source->counter;
    if (source->blob.length() > 0) {
        doc["blob"] = source->blob;
    }
}'''

src = replace_exactly_once(src, old_make_build_fn, new_make_build_fn, 'replace makeBuildFn with buildFnWrapper')

# Replace all sendCachedSoakMetrics call arg lines: makeBuildFn(source) -> buildFnWrapper, &source
# and [&now]() { return now; } -> fn-ptr + &now
src = re.sub(
    r'makeBuildFn\(source\),\s*\n\s*\[&now\]\(\) \{ return now; \}\)',
    'buildFnWrapper, &source,\n        [](void* ctx) -> uint32_t { return *static_cast<uint32_t*>(ctx); }, &now)',
    src
)

open(path, 'w').write(src)
remaining = src.count('DebugApiService::SoakMetricsBuildFn')
print(f'test_debug_soak_metrics_cache.cpp SoakMetricsBuildFn remaining: {remaining}')
old_style = src.count('[&now]() {')
print(f'test old [&now]() {{ remaining: {old_style}')
remaining_fn = src.count('makeBuildFn')
print(f'test makeBuildFn remaining: {remaining_fn}')
print('Phase 3.11 migration complete.')
