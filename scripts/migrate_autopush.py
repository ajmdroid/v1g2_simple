#!/usr/bin/env python3
"""Comprehensive AutoPush migration: header, cpp, test, runtimes, routes."""

# ─── Header ────────────────────────────────────────────────────────────────
hdr_path = 'src/modules/wifi/wifi_autopush_api_service.h'
hdr = open(hdr_path).read()

hdr = hdr.replace(
    '#include <cstdint>\n#include <functional>\n',
    '#include <cstdint>\n'
)

hdr = hdr.replace(
    """\
struct Runtime {
    std::function<void(SlotsSnapshot&)> loadSlotsSnapshot;
    std::function<bool(String&)> loadPushStatusJson;
    std::function<bool(const SlotUpdateRequest&)> applySlotUpdate;
    std::function<void(int, const String&)> setSlotName;
    std::function<void(int, uint16_t)> setSlotColor;
    std::function<uint8_t(int)> getSlotVolume;
    std::function<uint8_t(int)> getSlotMuteVolume;
    std::function<void(int, uint8_t, uint8_t)> setSlotVolumes;
    std::function<void(int, bool)> setSlotDarkMode;
    std::function<void(int, bool)> setSlotMuteToZero;
    std::function<void(int, uint8_t)> setSlotAlertPersistSec;
    std::function<void(int, bool)> setSlotPriorityArrowOnly;
    std::function<void(int, const String&, int)> setSlotProfileAndMode;
    std::function<int()> getActiveSlot;
    std::function<void(int)> drawProfileIndicator;
    std::function<bool(const ActivationRequest&)> applyActivation;
    std::function<void(int)> setActiveSlot;
    std::function<void(bool)> setAutoPushEnabled;
    std::function<PushNowQueueResult(const PushNowRequest&)> queuePushNow;
};""",
    """\
struct Runtime {
    void (*loadSlotsSnapshot)(SlotsSnapshot& snapshot, void* ctx);
    void* loadSlotsSnapshotCtx;
    bool (*loadPushStatusJson)(String& json, void* ctx);
    void* loadPushStatusJsonCtx;
    bool (*applySlotUpdate)(const SlotUpdateRequest& request, void* ctx);
    void* applySlotUpdateCtx;
    void (*setSlotName)(int slot, const String& name, void* ctx);
    void* setSlotNameCtx;
    void (*setSlotColor)(int slot, uint16_t color, void* ctx);
    void* setSlotColorCtx;
    uint8_t (*getSlotVolume)(int slot, void* ctx);
    void* getSlotVolumeCtx;
    uint8_t (*getSlotMuteVolume)(int slot, void* ctx);
    void* getSlotMuteVolumeCtx;
    void (*setSlotVolumes)(int slot, uint8_t volume, uint8_t muteVolume, void* ctx);
    void* setSlotVolumesCtx;
    void (*setSlotDarkMode)(int slot, bool darkMode, void* ctx);
    void* setSlotDarkModeCtx;
    void (*setSlotMuteToZero)(int slot, bool muteToZero, void* ctx);
    void* setSlotMuteToZeroCtx;
    void (*setSlotAlertPersistSec)(int slot, uint8_t alertPersistSec, void* ctx);
    void* setSlotAlertPersistSecCtx;
    void (*setSlotPriorityArrowOnly)(int slot, bool priorityArrowOnly, void* ctx);
    void* setSlotPriorityArrowOnlyCtx;
    void (*setSlotProfileAndMode)(int slot, const String& profile, int mode, void* ctx);
    void* setSlotProfileAndModeCtx;
    int (*getActiveSlot)(void* ctx);
    void* getActiveSlotCtx;
    void (*drawProfileIndicator)(int slot, void* ctx);
    void* drawProfileIndicatorCtx;
    bool (*applyActivation)(const ActivationRequest& request, void* ctx);
    void* applyActivationCtx;
    void (*setActiveSlot)(int slot, void* ctx);
    void* setActiveSlotCtx;
    void (*setAutoPushEnabled)(bool enabled, void* ctx);
    void* setAutoPushEnabledCtx;
    PushNowQueueResult (*queuePushNow)(const PushNowRequest& request, void* ctx);
    void* queuePushNowCtx;
};"""
)

# Handler signatures
hdr = hdr.replace(
    'void handleApiSlotSave(WebServer& server,\n                       const Runtime& runtime,\n                       const std::function<bool()>& checkRateLimit);',
    'void handleApiSlotSave(WebServer& server,\n                       const Runtime& runtime,\n                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);'
)
hdr = hdr.replace(
    'void handleApiActivate(WebServer& server,\n                       const Runtime& runtime,\n                       const std::function<bool()>& checkRateLimit);',
    'void handleApiActivate(WebServer& server,\n                       const Runtime& runtime,\n                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);'
)
hdr = hdr.replace(
    'void handleApiPushNow(WebServer& server,\n                      const Runtime& runtime,\n                      const std::function<bool()>& checkRateLimit);',
    'void handleApiPushNow(WebServer& server,\n                      const Runtime& runtime,\n                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);'
)

open(hdr_path, 'w').write(hdr)
print(f'header std::function remaining: {hdr.count("std::function")}')

# ─── Cpp ───────────────────────────────────────────────────────────────────
cpp_path = 'src/modules/wifi/wifi_autopush_api_service.cpp'
cpp = open(cpp_path).read()

# Handler signature + checkRateLimit
for fn_name, indent in [('handleApiSlotSave', '                       '),
                         ('handleApiActivate', '                       '),
                         ('handleApiPushNow',  '                      ')]:
    cpp = cpp.replace(
        f'void {fn_name}(WebServer& server,\n{indent}const Runtime& runtime,\n{indent}const std::function<bool()>& checkRateLimit) {{\n    if (checkRateLimit && !checkRateLimit()) return;',
        f'void {fn_name}(WebServer& server,\n{indent}const Runtime& runtime,\n{indent}bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {{\n    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;'
    )

# Runtime field calls
replacements = [
    ('runtime.loadSlotsSnapshot(snapshot)',
     'runtime.loadSlotsSnapshot(snapshot, runtime.loadSlotsSnapshotCtx)'),
    ('runtime.loadPushStatusJson(json)',
     'runtime.loadPushStatusJson(json, runtime.loadPushStatusJsonCtx)'),
    ('runtime.applySlotUpdate(request)',
     'runtime.applySlotUpdate(request, runtime.applySlotUpdateCtx)'),
    ('runtime.setSlotName(slot, name)',
     'runtime.setSlotName(slot, name, runtime.setSlotNameCtx)'),
    ('runtime.setSlotColor(slot, static_cast<uint16_t>(color))',
     'runtime.setSlotColor(slot, static_cast<uint16_t>(color), runtime.setSlotColorCtx)'),
    ('runtime.getSlotVolume(slot)',
     'runtime.getSlotVolume(slot, runtime.getSlotVolumeCtx)'),
    ('runtime.getSlotMuteVolume(slot)',
     'runtime.getSlotMuteVolume(slot, runtime.getSlotMuteVolumeCtx)'),
    ('runtime.setSlotVolumes(slot, vol, mute)',
     'runtime.setSlotVolumes(slot, vol, mute, runtime.setSlotVolumesCtx)'),
    ('runtime.setSlotDarkMode(slot, darkMode)',
     'runtime.setSlotDarkMode(slot, darkMode, runtime.setSlotDarkModeCtx)'),
    ('runtime.setSlotMuteToZero(slot, muteToZero)',
     'runtime.setSlotMuteToZero(slot, muteToZero, runtime.setSlotMuteToZeroCtx)'),
    ('runtime.setSlotAlertPersistSec(slot, static_cast<uint8_t>(clamped))',
     'runtime.setSlotAlertPersistSec(slot, static_cast<uint8_t>(clamped), runtime.setSlotAlertPersistSecCtx)'),
    ('runtime.setSlotPriorityArrowOnly(slot, prioArrow)',
     'runtime.setSlotPriorityArrowOnly(slot, prioArrow, runtime.setSlotPriorityArrowOnlyCtx)'),
    ('runtime.setSlotProfileAndMode(slot, profile, mode)',
     'runtime.setSlotProfileAndMode(slot, profile, mode, runtime.setSlotProfileAndModeCtx)'),
    ('runtime.getActiveSlot()',
     'runtime.getActiveSlot(runtime.getActiveSlotCtx)'),
    ('runtime.drawProfileIndicator(slot)',
     'runtime.drawProfileIndicator(slot, runtime.drawProfileIndicatorCtx)'),
    ('runtime.applyActivation(request)',
     'runtime.applyActivation(request, runtime.applyActivationCtx)'),
    ('runtime.setActiveSlot(slot)',
     'runtime.setActiveSlot(slot, runtime.setActiveSlotCtx)'),
    ('runtime.setAutoPushEnabled(enable)',
     'runtime.setAutoPushEnabled(enable, runtime.setAutoPushEnabledCtx)'),
    ('runtime.queuePushNow(request)',
     'runtime.queuePushNow(request, runtime.queuePushNowCtx)'),
]

for old, new in replacements:
    count = cpp.count(old)
    if count > 0:
        cpp = cpp.replace(old, new)
        print(f'  cpp [{count}x] {old[:55]}')
    else:
        print(f'  cpp [0x] NOT FOUND: {old[:55]}')

open(cpp_path, 'w').write(cpp)
print(f'cpp std::function remaining: {cpp.count("std::function")}')

# ─── Test ──────────────────────────────────────────────────────────────────
test_path = 'test/test_wifi_autopush_api_service/test_wifi_autopush_api_service.cpp'
test = open(test_path).read()

test = test.replace(
    """\
static WifiAutoPushApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiAutoPushApiService::Runtime{
        [&rt](WifiAutoPushApiService::SlotsSnapshot& out) {
            rt.loadSlotsCalls++;
            out = rt.snapshot;
        },
        [&rt](String& outJson) {
            rt.loadStatusCalls++;
            if (!rt.statusAvailable) {
                return false;
            }
            outJson = rt.statusJson;
            return true;
        },
        [&rt](const WifiAutoPushApiService::SlotUpdateRequest& request) {
            return applySlotUpdateForTest(rt, request);
        },
        [&rt](int slot, const String& name) {
            rt.setSlotNameCalls++;
            rt.lastSlotIndex = slot;
            rt.lastSlotName = name;
        },
        [&rt](int slot, uint16_t color) {
            rt.setSlotColorCalls++;
            rt.lastSlotIndex = slot;
            rt.lastSlotColor = color;
        },
        [&rt](int slot) {
            return rt.slotVolumes[slot];
        },
        [&rt](int slot) {
            return rt.slotMuteVolumes[slot];
        },
        [&rt](int slot, uint8_t volume, uint8_t muteVolume) {
            rt.setSlotVolumesCalls++;
            rt.lastSlotIndex = slot;
            rt.lastSlotVolume = volume;
            rt.lastSlotMuteVolume = muteVolume;
            rt.slotVolumes[slot] = volume;
            rt.slotMuteVolumes[slot] = muteVolume;
        },
        [&rt](int slot, bool darkMode) {
            rt.setSlotDarkModeCalls++;
            rt.lastSlotIndex = slot;
            rt.lastDarkMode = darkMode;
        },
        [&rt](int slot, bool muteToZero) {
            rt.setSlotMuteToZeroCalls++;
            rt.lastSlotIndex = slot;
            rt.lastMuteToZero = muteToZero;
        },
        [&rt](int slot, uint8_t alertPersist) {
            rt.setSlotAlertPersistCalls++;
            rt.lastSlotIndex = slot;
            rt.lastAlertPersist = alertPersist;
        },
        [&rt](int slot, bool priorityArrowOnly) {
            rt.setSlotPriorityArrowOnlyCalls++;
            rt.lastSlotIndex = slot;
            rt.lastPriorityArrowOnly = priorityArrowOnly;
        },
        [&rt](int slot, const String& profile, int mode) {
            rt.setSlotProfileAndModeCalls++;
            rt.lastSlotIndex = slot;
            rt.lastProfile = profile;
            rt.lastMode = mode;
        },
        [&rt]() {
            return rt.activeSlot;
        },
        [&rt](int slot) {
            rt.drawProfileIndicatorCalls++;
            rt.lastSlotIndex = slot;
        },
        [&rt](const WifiAutoPushApiService::ActivationRequest& request) {
            return applyActivationForTest(rt, request);
        },
        [&rt](int slot) {
            rt.setActiveSlotCalls++;
            rt.activeSlot = slot;
        },
        [&rt](bool enabled) {
            rt.setAutoPushEnabledCalls++;
            rt.autoPushEnabled = enabled;
        },
        [&rt](const WifiAutoPushApiService::PushNowRequest& request) {
            rt.queuePushNowCalls++;
            rt.lastPushRequest = request;
            return rt.queueResult;
        },
    };
}""",
    """\
static WifiAutoPushApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiAutoPushApiService::Runtime{
        [](WifiAutoPushApiService::SlotsSnapshot& out, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->loadSlotsCalls++;
            out = rtp->snapshot;
        }, &rt,
        [](String& outJson, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->loadStatusCalls++;
            if (!rtp->statusAvailable) {
                return false;
            }
            outJson = rtp->statusJson;
            return true;
        }, &rt,
        [](const WifiAutoPushApiService::SlotUpdateRequest& request, void* ctx) {
            return applySlotUpdateForTest(*static_cast<FakeRuntime*>(ctx), request);
        }, &rt,
        [](int slot, const String& name, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setSlotNameCalls++;
            rtp->lastSlotIndex = slot;
            rtp->lastSlotName = name;
        }, &rt,
        [](int slot, uint16_t color, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setSlotColorCalls++;
            rtp->lastSlotIndex = slot;
            rtp->lastSlotColor = color;
        }, &rt,
        [](int slot, void* ctx) {
            return static_cast<FakeRuntime*>(ctx)->slotVolumes[slot];
        }, &rt,
        [](int slot, void* ctx) {
            return static_cast<FakeRuntime*>(ctx)->slotMuteVolumes[slot];
        }, &rt,
        [](int slot, uint8_t volume, uint8_t muteVolume, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setSlotVolumesCalls++;
            rtp->lastSlotIndex = slot;
            rtp->lastSlotVolume = volume;
            rtp->lastSlotMuteVolume = muteVolume;
            rtp->slotVolumes[slot] = volume;
            rtp->slotMuteVolumes[slot] = muteVolume;
        }, &rt,
        [](int slot, bool darkMode, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setSlotDarkModeCalls++;
            rtp->lastSlotIndex = slot;
            rtp->lastDarkMode = darkMode;
        }, &rt,
        [](int slot, bool muteToZero, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setSlotMuteToZeroCalls++;
            rtp->lastSlotIndex = slot;
            rtp->lastMuteToZero = muteToZero;
        }, &rt,
        [](int slot, uint8_t alertPersist, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setSlotAlertPersistCalls++;
            rtp->lastSlotIndex = slot;
            rtp->lastAlertPersist = alertPersist;
        }, &rt,
        [](int slot, bool priorityArrowOnly, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setSlotPriorityArrowOnlyCalls++;
            rtp->lastSlotIndex = slot;
            rtp->lastPriorityArrowOnly = priorityArrowOnly;
        }, &rt,
        [](int slot, const String& profile, int mode, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setSlotProfileAndModeCalls++;
            rtp->lastSlotIndex = slot;
            rtp->lastProfile = profile;
            rtp->lastMode = mode;
        }, &rt,
        [](void* ctx) {
            return static_cast<FakeRuntime*>(ctx)->activeSlot;
        }, &rt,
        [](int slot, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->drawProfileIndicatorCalls++;
            rtp->lastSlotIndex = slot;
        }, &rt,
        [](const WifiAutoPushApiService::ActivationRequest& request, void* ctx) {
            return applyActivationForTest(*static_cast<FakeRuntime*>(ctx), request);
        }, &rt,
        [](int slot, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setActiveSlotCalls++;
            rtp->activeSlot = slot;
        }, &rt,
        [](bool enabled, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setAutoPushEnabledCalls++;
            rtp->autoPushEnabled = enabled;
        }, &rt,
        [](const WifiAutoPushApiService::PushNowRequest& request, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->queuePushNowCalls++;
            rtp->lastPushRequest = request;
            return rtp->queueResult;
        }, &rt,
    };
}"""
)

# Handler call sites
test = test.replace('        []() { return false; });', '        [](void* /*ctx*/) { return false; }, nullptr);')
test = test.replace('        []() { return true; });', '        [](void* /*ctx*/) { return true; }, nullptr);')
# Inline calls: handleApiPushNow(server, makeRuntime(rt), []() { return true; });
test = test.replace(
    ', makeRuntime(rt), []() { return true; });',
    ', makeRuntime(rt), [](void* /*ctx*/) { return true; }, nullptr);'
)

open(test_path, 'w').write(test)
print(f'test std::function remaining: {test.count("std::function")}')
print(f'test old []() {{ remaining: {test.count("[]() {")}')

# ─── wifi_runtimes.cpp ────────────────────────────────────────────────────
rt_path = 'src/wifi_runtimes.cpp'
rt = open(rt_path).read()

rt = rt.replace(
    """\
WifiAutoPushApiService::Runtime WiFiManager::makeAutoPushRuntime() {
    auto applyDeferredSlotUpdate = [this](const AutoPushSlotUpdate& update) {
        return settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
    };
    auto applyDeferredStateUpdate = [this](const AutoPushStateUpdate& update) {
        return settingsManager.applyAutoPushStateUpdate(update, SettingsPersistMode::Deferred);
    };

    return WifiAutoPushApiService::Runtime{
        [this](WifiAutoPushApiService::SlotsSnapshot& snapshot) {
            const V1Settings& s = settingsManager.get();
            snapshot.enabled = s.autoPushEnabled;
            snapshot.activeSlot = s.activeSlot;

            for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
                const V1Settings::ConstAutoPushSlotView slot = s.autoPushSlotView(slotIndex);
                snapshot.slots[slotIndex].name = slot.name;
                snapshot.slots[slotIndex].profile = slot.config.profileName;
                snapshot.slots[slotIndex].mode = slot.config.mode;
                snapshot.slots[slotIndex].color = slot.color;
                snapshot.slots[slotIndex].volume = slot.volume;
                snapshot.slots[slotIndex].muteVolume = slot.muteVolume;
                snapshot.slots[slotIndex].darkMode = slot.darkMode;
                snapshot.slots[slotIndex].muteToZero = slot.muteToZero;
                snapshot.slots[slotIndex].alertPersist = slot.alertPersist;
                snapshot.slots[slotIndex].priorityArrowOnly = slot.priorityArrow;
            }
        },
        [this](String& json) {
            if (!getPushStatusJson) {
                return false;
            }
            json = getPushStatusJson();
            return true;
        },
        [this, applyDeferredSlotUpdate](const WifiAutoPushApiService::SlotUpdateRequest& request) {
            AutoPushSlotUpdate update;
            update.slot = request.slot;
            update.hasName = request.hasName;
            update.name = request.name;
            update.hasColor = request.hasColor;
            update.color = request.color;
            update.hasVolume = request.hasVolume;
            update.volume = request.volume;
            update.hasMuteVolume = request.hasMuteVolume;
            update.muteVolume = request.muteVolume;
            update.hasDarkMode = request.hasDarkMode;
            update.darkMode = request.darkMode;
            update.hasMuteToZero = request.hasMuteToZero;
            update.muteToZero = request.muteToZero;
            update.hasAlertPersist = request.hasAlertPersist;
            update.alertPersist = request.alertPersist;
            update.hasPriorityArrowOnly = request.hasPriorityArrowOnly;
            update.priorityArrowOnly = request.priorityArrowOnly;
            update.hasProfileName = true;
            update.profileName = request.profile;
            update.hasMode = true;
            update.mode = normalizeV1ModeValue(request.mode);
            return applyDeferredSlotUpdate(update);
        },
        [applyDeferredSlotUpdate](int slot, const String& name) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasName = true;
            update.name = name;
            (void)applyDeferredSlotUpdate(update);
        },
        [applyDeferredSlotUpdate](int slot, uint16_t color) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasColor = true;
            update.color = color;
            (void)applyDeferredSlotUpdate(update);
        },
        [this](int slot) {
            return settingsManager.getSlotVolume(slot);
        },
        [this](int slot) {
            return settingsManager.getSlotMuteVolume(slot);
        },
        [applyDeferredSlotUpdate](int slot, uint8_t volume, uint8_t muteVolume) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasVolume = true;
            update.volume = volume;
            update.hasMuteVolume = true;
            update.muteVolume = muteVolume;
            (void)applyDeferredSlotUpdate(update);
        },
        [applyDeferredSlotUpdate](int slot, bool darkMode) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasDarkMode = true;
            update.darkMode = darkMode;
            (void)applyDeferredSlotUpdate(update);
        },
        [applyDeferredSlotUpdate](int slot, bool muteToZero) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasMuteToZero = true;
            update.muteToZero = muteToZero;
            (void)applyDeferredSlotUpdate(update);
        },
        [applyDeferredSlotUpdate](int slot, uint8_t alertPersistSec) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasAlertPersist = true;
            update.alertPersist = alertPersistSec;
            (void)applyDeferredSlotUpdate(update);
        },
        [applyDeferredSlotUpdate](int slot, bool priorityArrowOnly) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasPriorityArrowOnly = true;
            update.priorityArrowOnly = priorityArrowOnly;
            (void)applyDeferredSlotUpdate(update);
        },
        [applyDeferredSlotUpdate](int slot, const String& profile, int mode) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasProfileName = true;
            update.profileName = profile;
            update.hasMode = true;
            update.mode = normalizeV1ModeValue(mode);
            (void)applyDeferredSlotUpdate(update);
        },
        [this]() {
            return static_cast<int>(settingsManager.get().activeSlot);
        },
        [this](int slot) {
            display.drawProfileIndicator(slot);
        },
        [this, applyDeferredStateUpdate](const WifiAutoPushApiService::ActivationRequest& request) {
            AutoPushStateUpdate update;
            update.hasActiveSlot = true;
            update.activeSlot = request.slot;
            update.hasEnabled = true;
            update.enabled = request.enable;
            return applyDeferredStateUpdate(update);
        },
        [applyDeferredStateUpdate](int slot) {
            AutoPushStateUpdate update;
            update.hasActiveSlot = true;
            update.activeSlot = slot;
            (void)applyDeferredStateUpdate(update);
        },
        [applyDeferredStateUpdate](bool enabled) {
            AutoPushStateUpdate update;
            update.hasEnabled = true;
            update.enabled = enabled;
            (void)applyDeferredStateUpdate(update);
        },
        [this](const WifiAutoPushApiService::PushNowRequest& request) {
            if (!queuePushNow) {
                return WifiAutoPushApiService::PushNowQueueResult::PROFILE_LOAD_FAILED;
            }
            return queuePushNow(request);
        },
    };
}""",
    """\
WifiAutoPushApiService::Runtime WiFiManager::makeAutoPushRuntime() {
    return WifiAutoPushApiService::Runtime{
        [](WifiAutoPushApiService::SlotsSnapshot& snapshot, void* /*ctx*/) {
            const V1Settings& s = settingsManager.get();
            snapshot.enabled = s.autoPushEnabled;
            snapshot.activeSlot = s.activeSlot;

            for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
                const V1Settings::ConstAutoPushSlotView slot = s.autoPushSlotView(slotIndex);
                snapshot.slots[slotIndex].name = slot.name;
                snapshot.slots[slotIndex].profile = slot.config.profileName;
                snapshot.slots[slotIndex].mode = slot.config.mode;
                snapshot.slots[slotIndex].color = slot.color;
                snapshot.slots[slotIndex].volume = slot.volume;
                snapshot.slots[slotIndex].muteVolume = slot.muteVolume;
                snapshot.slots[slotIndex].darkMode = slot.darkMode;
                snapshot.slots[slotIndex].muteToZero = slot.muteToZero;
                snapshot.slots[slotIndex].alertPersist = slot.alertPersist;
                snapshot.slots[slotIndex].priorityArrowOnly = slot.priorityArrow;
            }
        }, nullptr,
        [](String& json, void* ctx) {
            auto* mgr = static_cast<WiFiManager*>(ctx);
            if (!mgr->getPushStatusJson) {
                return false;
            }
            json = mgr->getPushStatusJson();
            return true;
        }, this,
        [](const WifiAutoPushApiService::SlotUpdateRequest& request, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = request.slot;
            update.hasName = request.hasName;
            update.name = request.name;
            update.hasColor = request.hasColor;
            update.color = request.color;
            update.hasVolume = request.hasVolume;
            update.volume = request.volume;
            update.hasMuteVolume = request.hasMuteVolume;
            update.muteVolume = request.muteVolume;
            update.hasDarkMode = request.hasDarkMode;
            update.darkMode = request.darkMode;
            update.hasMuteToZero = request.hasMuteToZero;
            update.muteToZero = request.muteToZero;
            update.hasAlertPersist = request.hasAlertPersist;
            update.alertPersist = request.alertPersist;
            update.hasPriorityArrowOnly = request.hasPriorityArrowOnly;
            update.priorityArrowOnly = request.priorityArrowOnly;
            update.hasProfileName = true;
            update.profileName = request.profile;
            update.hasMode = true;
            update.mode = normalizeV1ModeValue(request.mode);
            return settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, const String& name, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasName = true;
            update.name = name;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, uint16_t color, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasColor = true;
            update.color = color;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, void* /*ctx*/) {
            return settingsManager.getSlotVolume(slot);
        }, nullptr,
        [](int slot, void* /*ctx*/) {
            return settingsManager.getSlotMuteVolume(slot);
        }, nullptr,
        [](int slot, uint8_t volume, uint8_t muteVolume, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasVolume = true;
            update.volume = volume;
            update.hasMuteVolume = true;
            update.muteVolume = muteVolume;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, bool darkMode, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasDarkMode = true;
            update.darkMode = darkMode;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, bool muteToZero, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasMuteToZero = true;
            update.muteToZero = muteToZero;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, uint8_t alertPersistSec, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasAlertPersist = true;
            update.alertPersist = alertPersistSec;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, bool priorityArrowOnly, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasPriorityArrowOnly = true;
            update.priorityArrowOnly = priorityArrowOnly;
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, const String& profile, int mode, void* /*ctx*/) {
            AutoPushSlotUpdate update;
            update.slot = slot;
            update.hasProfileName = true;
            update.profileName = profile;
            update.hasMode = true;
            update.mode = normalizeV1ModeValue(mode);
            (void)settingsManager.applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](void* /*ctx*/) {
            return static_cast<int>(settingsManager.get().activeSlot);
        }, nullptr,
        [](int slot, void* ctx) {
            static_cast<WiFiManager*>(ctx)->display.drawProfileIndicator(slot);
        }, this,
        [](const WifiAutoPushApiService::ActivationRequest& request, void* /*ctx*/) {
            AutoPushStateUpdate update;
            update.hasActiveSlot = true;
            update.activeSlot = request.slot;
            update.hasEnabled = true;
            update.enabled = request.enable;
            return settingsManager.applyAutoPushStateUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](int slot, void* /*ctx*/) {
            AutoPushStateUpdate update;
            update.hasActiveSlot = true;
            update.activeSlot = slot;
            (void)settingsManager.applyAutoPushStateUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](bool enabled, void* /*ctx*/) {
            AutoPushStateUpdate update;
            update.hasEnabled = true;
            update.enabled = enabled;
            (void)settingsManager.applyAutoPushStateUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](const WifiAutoPushApiService::PushNowRequest& request, void* ctx) {
            auto* mgr = static_cast<WiFiManager*>(ctx);
            if (!mgr->queuePushNow) {
                return WifiAutoPushApiService::PushNowQueueResult::PROFILE_LOAD_FAILED;
            }
            return mgr->queuePushNow(request);
        }, this,
    };
}"""
)

open(rt_path, 'w').write(rt)
print(f'runtimes std::function in makeAutoPushRuntime: {rt[rt.find("makeAutoPushRuntime"):rt.find("makeDisplayColorsRuntime")].count("std::function")}')

# ─── wifi_routes.cpp ──────────────────────────────────────────────────────
routes_path = 'src/wifi_routes.cpp'
routes = open(routes_path).read()

routes = routes.replace(
    '    server.on("/api/autopush/slot", HTTP_POST, [this, rateLimitCallback]() {\n        WifiAutoPushApiService::handleApiSlotSave(\n            server,\n            makeAutoPushRuntime(),\n            rateLimitCallback);\n    });',
    '    server.on("/api/autopush/slot", HTTP_POST, [this]() {\n        WifiAutoPushApiService::handleApiSlotSave(\n            server,\n            makeAutoPushRuntime(),\n            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);\n    });'
)
routes = routes.replace(
    '    server.on("/api/autopush/activate", HTTP_POST, [this, rateLimitCallback]() {\n        WifiAutoPushApiService::handleApiActivate(\n            server,\n            makeAutoPushRuntime(),\n            rateLimitCallback);\n    });',
    '    server.on("/api/autopush/activate", HTTP_POST, [this]() {\n        WifiAutoPushApiService::handleApiActivate(\n            server,\n            makeAutoPushRuntime(),\n            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);\n    });'
)
routes = routes.replace(
    '    server.on("/api/autopush/push", HTTP_POST, [this, rateLimitCallback]() {\n        WifiAutoPushApiService::handleApiPushNow(\n            server,\n            makeAutoPushRuntime(),\n            rateLimitCallback);\n    });',
    '    server.on("/api/autopush/push", HTTP_POST, [this]() {\n        WifiAutoPushApiService::handleApiPushNow(\n            server,\n            makeAutoPushRuntime(),\n            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);\n    });'
)

open(routes_path, 'w').write(routes)
print('routes updated')
print('AutoPush migration complete.')
