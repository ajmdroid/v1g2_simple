#!/usr/bin/env python3
"""Migrate V1Profile test and production factories."""

path = 'test/test_wifi_v1_profile_api_service/test_wifi_v1_profile_api_service.cpp'
src = open(path).read()

# makeRuntime factory - convert all 14 [&rt] lambdas to stateless with ctx
src = src.replace(
    """static WifiV1ProfileApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1ProfileApiService::Runtime{
        [&rt]() {
            rt.listCalls++;
            return rt.listNames;
        },
        [&rt](const String& name, WifiV1ProfileApiService::ProfileSummary& summary) {
            return fakeLoadSummary(rt, name, summary);
        },
        [&rt](const String& name, String& profileJson) {
            return fakeLoadJson(rt, name, profileJson);
        },
        [&rt](const String&, uint8_t outBytes[6], bool& displayOn) {
            rt.loadProfileSettingsCalls++;
            if (!rt.loadProfileSettingsResult) {
                return false;
            }
            memcpy(outBytes, rt.storedProfileBytes, sizeof(rt.storedProfileBytes));
            displayOn = rt.storedProfileDisplayOn;
            return true;
        },
        [&rt](const JsonObject& settingsObj, uint8_t outBytes[6]) {
            rt.parseSettingsCalls++;
            if (!rt.parseSettingsOk) {
                return false;
            }
            memset(outBytes, 0xFF, 6);
            if (settingsObj["byte0"].is<int>()) {
                outBytes[0] = static_cast<uint8_t>(settingsObj["byte0"].as<int>());
            }
            return true;
        },
        [&rt](const String& name,
              const String& description,
              bool displayOn,
              const uint8_t inBytes[6],
              String& error) {
            rt.saveCalls++;
            rt.lastSaveName = name;
            rt.lastSaveDescription = description;
            rt.lastSaveDisplayOn = displayOn;
            memcpy(rt.lastSaveBytes, inBytes, 6);
            if (!rt.saveOk) {
                error = rt.saveError;
                return false;
            }
            return true;
        },
        [&rt](const String& /*name*/) {
            rt.deleteCalls++;
            return rt.deleteResult;
        },
        [&rt]() {
            rt.requestUserBytesCalls++;
            return rt.requestUserBytesResult;
        },
        [&rt](const uint8_t inBytes[6]) {
            rt.writeUserBytesCalls++;
            memcpy(rt.lastWrittenBytes, inBytes, sizeof(rt.lastWrittenBytes));
            return rt.writeUserBytesResult;
        },
        [&rt](bool displayOn) {
            rt.setDisplayOnCalls++;
            rt.lastSetDisplayOn = displayOn;
        },
        [&rt]() { return rt.hasCurrent; },
        [&rt]() { return rt.currentSettingsJson; },
        [&rt]() { return rt.connected; },
        [&rt]() { rt.requestDeferredBackupCalls++; },
    };
}""",
    """static WifiV1ProfileApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1ProfileApiService::Runtime{
        [](void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->listCalls++;
            return rtp->listNames;
        }, &rt,
        [](const String& name, WifiV1ProfileApiService::ProfileSummary& summary, void* ctx) {
            return fakeLoadSummary(*static_cast<FakeRuntime*>(ctx), name, summary);
        }, &rt,
        [](const String& name, String& profileJson, void* ctx) {
            return fakeLoadJson(*static_cast<FakeRuntime*>(ctx), name, profileJson);
        }, &rt,
        [](const String&, uint8_t outBytes[6], bool& displayOn, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->loadProfileSettingsCalls++;
            if (!rtp->loadProfileSettingsResult) {
                return false;
            }
            memcpy(outBytes, rtp->storedProfileBytes, sizeof(rtp->storedProfileBytes));
            displayOn = rtp->storedProfileDisplayOn;
            return true;
        }, &rt,
        [](const JsonObject& settingsObj, uint8_t outBytes[6], void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->parseSettingsCalls++;
            if (!rtp->parseSettingsOk) {
                return false;
            }
            memset(outBytes, 0xFF, 6);
            if (settingsObj["byte0"].is<int>()) {
                outBytes[0] = static_cast<uint8_t>(settingsObj["byte0"].as<int>());
            }
            return true;
        }, &rt,
        [](const String& name,
           const String& description,
           bool displayOn,
           const uint8_t inBytes[6],
           String& error,
           void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->saveCalls++;
            rtp->lastSaveName = name;
            rtp->lastSaveDescription = description;
            rtp->lastSaveDisplayOn = displayOn;
            memcpy(rtp->lastSaveBytes, inBytes, 6);
            if (!rtp->saveOk) {
                error = rtp->saveError;
                return false;
            }
            return true;
        }, &rt,
        [](const String& /*name*/, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->deleteCalls++;
            return rtp->deleteResult;
        }, &rt,
        [](void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->requestUserBytesCalls++;
            return rtp->requestUserBytesResult;
        }, &rt,
        [](const uint8_t inBytes[6], void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->writeUserBytesCalls++;
            memcpy(rtp->lastWrittenBytes, inBytes, sizeof(rtp->lastWrittenBytes));
            return rtp->writeUserBytesResult;
        }, &rt,
        [](bool displayOn, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setDisplayOnCalls++;
            rtp->lastSetDisplayOn = displayOn;
        }, &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->hasCurrent; }, &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->currentSettingsJson; }, &rt,
        [](void* ctx) { return static_cast<FakeRuntime*>(ctx)->connected; }, &rt,
        [](void* ctx) { static_cast<FakeRuntime*>(ctx)->requestDeferredBackupCalls++; }, &rt,
    };
}"""
)

# Handler call sites: stateless lambdas -> fn-ptr + nullptr
src = src.replace('        []() { return false; });', '        [](void* /*ctx*/) { return false; }, nullptr);')
src = src.replace('        []() { return true; });', '        [](void* /*ctx*/) { return true; }, nullptr);')

open(path, 'w').write(src)
print(f'std::function remaining: {src.count("std::function")}')
print(f'old []() {{ remaining: {src.count("[]() {")}')
print('done')
