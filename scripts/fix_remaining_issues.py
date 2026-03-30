#!/usr/bin/env python3
"""Fix remaining issues:
1. DisplayColors cpp: handleApiGet runtime.getSettings() and handleApiClear checkRateLimit()
2. V1Sync test: makeRuntime lambdas + handler call sites
"""

# --- Fix DisplayColors cpp ---
dc_path = 'src/modules/wifi/wifi_display_colors_api_service.cpp'
dc = open(dc_path).read()

# Fix handleApiClear body: checkRateLimit() → checkRateLimit(rateLimitCtx)
dc = dc.replace(
    'void handleApiClear(WebServer& server,\n                    const Runtime& runtime,\n                    bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {\n    if (checkRateLimit && !checkRateLimit()) return;',
    'void handleApiClear(WebServer& server,\n                    const Runtime& runtime,\n                    bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {\n    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;'
)

# Fix handleApiGet: runtime.getSettings() → runtime.getSettings(runtime.getSettingsCtx)
# There are two occurrences: one in handleApiSave (already patched) and one in handleApiGet.
# The handleApiSave one should already be patched. Let's check and fix the remaining one.
dc_patched = dc.replace(
    'void handleApiGet(WebServer& server, const Runtime& runtime) {\n    if (!runtime.getSettings) {\n        server.send(500, "application/json", "{\\\"error\\\":\\\"Settings unavailable\\\"}");\n        return;\n    }\n\n    const V1Settings& s = runtime.getSettings();',
    'void handleApiGet(WebServer& server, const Runtime& runtime) {\n    if (!runtime.getSettings) {\n        server.send(500, "application/json", "{\\\"error\\\":\\\"Settings unavailable\\\"}");\n        return;\n    }\n\n    const V1Settings& s = runtime.getSettings(runtime.getSettingsCtx);'
)
if dc_patched == dc:
    print('  WARNING: handleApiGet pattern not found, trying simpler match')
else:
    dc = dc_patched
    print('  Fixed handleApiGet runtime.getSettings()')

open(dc_path, 'w').write(dc)
print(f'DisplayColors cpp std::function remaining: {dc.count("std::function")}')
remaining_issues = []
if 'runtime.getSettings()' in dc:
    remaining_issues.append('runtime.getSettings()')
if '!checkRateLimit()' in dc:
    remaining_issues.append('checkRateLimit() without ctx')
if remaining_issues:
    print(f'  REMAINING ISSUES: {remaining_issues}')
else:
    print('  All calls migrated in DisplayColors cpp')

# --- Fix V1Sync test ---
sync_path = 'test/test_wifi_v1_sync_api_service/test_wifi_v1_sync_api_service.cpp'
sync = open(sync_path).read()

# Replace makeRuntime factory - old style (14 lambdas, mix of [&rt] and stateless)
sync = sync.replace(
    """\
static WifiV1ProfileApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1ProfileApiService::Runtime{
        []() { return std::vector<String>{}; },
        [](const String&, WifiV1ProfileApiService::ProfileSummary&) { return false; },
        [](const String&, String&) { return false; },
        [&rt](const String&, uint8_t outBytes[6], bool& displayOn) {
            rt.loadProfileSettingsCalls++;
            if (!rt.profileFound) {
                return false;
            }
            memcpy(outBytes, rt.profileBytes, 6);
            displayOn = rt.profileDisplayOn;
            return true;
        },
        [&rt](const JsonObject& settingsObj, uint8_t outBytes[6]) {
            rt.parseSettingsCalls++;
            if (!rt.parseSettingsOk) {
                return false;
            }
            memset(outBytes, 0, 6);
            if (settingsObj["byte0"].is<int>()) {
                outBytes[0] = static_cast<uint8_t>(settingsObj["byte0"].as<int>());
            }
            return true;
        },
        [](const String&, const String&, bool, const uint8_t[6], String&) { return false; },
        [](const String&) { return false; },
        [&rt]() {
            rt.requestCalls++;
            return rt.requestResult;
        },
        [&rt](const uint8_t inBytes[6]) {
            rt.writeCalls++;
            memcpy(rt.lastWriteBytes, inBytes, 6);
            return rt.writeResult;
        },
        [&rt](bool displayOn) {
            rt.setDisplayCalls++;
            rt.lastDisplayOn = displayOn;
        },
        []() { return false; },
        []() { return String("{}"); },
        [&rt]() { return rt.connected; },
        []() {},
    };
}""",
    """\
static WifiV1ProfileApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1ProfileApiService::Runtime{
        [](void* /*ctx*/) { return std::vector<String>{}; }, nullptr,
        [](const String&, WifiV1ProfileApiService::ProfileSummary&, void* /*ctx*/) { return false; }, nullptr,
        [](const String&, String&, void* /*ctx*/) { return false; }, nullptr,
        [](const String&, uint8_t outBytes[6], bool& displayOn, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->loadProfileSettingsCalls++;
            if (!rtp->profileFound) {
                return false;
            }
            memcpy(outBytes, rtp->profileBytes, 6);
            displayOn = rtp->profileDisplayOn;
            return true;
        }, &rt,
        [](const JsonObject& settingsObj, uint8_t outBytes[6], void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->parseSettingsCalls++;
            if (!rtp->parseSettingsOk) {
                return false;
            }
            memset(outBytes, 0, 6);
            if (settingsObj["byte0"].is<int>()) {
                outBytes[0] = static_cast<uint8_t>(settingsObj["byte0"].as<int>());
            }
            return true;
        }, &rt,
        [](const String&, const String&, bool, const uint8_t[6], String&, void* /*ctx*/) { return false; }, nullptr,
        [](const String&, void* /*ctx*/) { return false; }, nullptr,
        [](void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->requestCalls++;
            return rtp->requestResult;
        }, &rt,
        [](const uint8_t inBytes[6], void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->writeCalls++;
            memcpy(rtp->lastWriteBytes, inBytes, 6);
            return rtp->writeResult;
        }, &rt,
        [](bool displayOn, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setDisplayCalls++;
            rtp->lastDisplayOn = displayOn;
        }, &rt,
        [](void* /*ctx*/) { return false; }, nullptr,
        [](void* /*ctx*/) { return String("{}"); }, nullptr,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connected; }, &rt,
        [](void* /*ctx*/) {}, nullptr,
    };
}"""
)

# Handler call sites
sync = sync.replace('        []() { return false; });', '        [](void* /*ctx*/) { return false; }, nullptr);')
sync = sync.replace('        []() { return true; });', '        [](void* /*ctx*/) { return true; }, nullptr);')

open(sync_path, 'w').write(sync)
print(f'V1Sync test std::function remaining: {sync.count("std::function")}')
print(f'V1Sync test old []() {{ remaining: {sync.count("[]() {")}')
print('done')
