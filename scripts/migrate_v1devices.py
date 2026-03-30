#!/usr/bin/env python3
"""Migrate WifiV1DevicesApiService from std::function to fn-ptr+ctx."""

# =========================================
# 1. Header: wifi_v1_devices_api_service.h
# =========================================
hdr_path = "src/modules/wifi/wifi_v1_devices_api_service.h"
with open(hdr_path) as f:
    src = f.read()

src = src.replace(
    "#include <cstdint>\n#include <functional>\n#include <vector>",
    "#include <cstdint>\n#include <vector>"
)

src = src.replace(
    """\
struct Runtime {
    std::function<std::vector<DeviceInfo>()> listDevices;
    std::function<bool(const String&, const String&)> setDeviceName;
    std::function<bool(const String&, uint8_t)> setDeviceDefaultProfile;
    std::function<bool(const String&)> deleteDevice;
};""",
    """\
struct Runtime {
    std::vector<DeviceInfo> (*listDevices)(void* ctx);
    void* listDevicesCtx;
    bool (*setDeviceName)(const String& address, const String& name, void* ctx);
    void* setDeviceNameCtx;
    bool (*setDeviceDefaultProfile)(const String& address, uint8_t profile, void* ctx);
    void* setDeviceDefaultProfileCtx;
    bool (*deleteDevice)(const String& address, void* ctx);
    void* deleteDeviceCtx;
};"""
)

for fn in ["handleApiDeviceNameSave", "handleApiDeviceProfileSave", "handleApiDeviceDelete"]:
    src = src.replace(
        f"                             const Runtime& runtime,\n"
        f"                             const std::function<bool()>& checkRateLimit);",
        f"                             const Runtime& runtime,\n"
        f"                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);"
    )

with open(hdr_path, "w") as f:
    f.write(src)
print(f"Updated {hdr_path}")

# =========================================
# 2. Cpp: wifi_v1_devices_api_service.cpp
# =========================================
cpp_path = "src/modules/wifi/wifi_v1_devices_api_service.cpp"
with open(cpp_path) as f:
    src = f.read()

# handleApiDevicesList: runtime.listDevices() -> runtime.listDevices(runtime.listDevicesCtx)
src = src.replace(
    "        devices = runtime.listDevices();\n",
    "        devices = runtime.listDevices(runtime.listDevicesCtx);\n"
)

# handleApiDeviceNameSave signature + checkRateLimit call
src = src.replace(
    "                             const std::function<bool()>& checkRateLimit) {\n    if (checkRateLimit && !checkRateLimit()) return;\n\n    if (!server.hasArg(\"address\")) {\n        server.send(400, \"application/json\", \"{\\\"error\\\":\\\"Missing address\\\"}\");\n        return;\n    }\n\n    if (!runtime.setDeviceName) {",
    "                             bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {\n    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;\n\n    if (!server.hasArg(\"address\")) {\n        server.send(400, \"application/json\", \"{\\\"error\\\":\\\"Missing address\\\"}\");\n        return;\n    }\n\n    if (!runtime.setDeviceName) {"
)
src = src.replace(
    "    if (!runtime.setDeviceName(address, name)) {",
    "    if (!runtime.setDeviceName(address, name, runtime.setDeviceNameCtx)) {"
)

# handleApiDeviceProfileSave
src = src.replace(
    "                                const std::function<bool()>& checkRateLimit) {\n    if (checkRateLimit && !checkRateLimit()) return;\n\n    if (!server.hasArg(\"address\") || !server.hasArg(\"profile\")) {\n        server.send(400, \"application/json\", \"{\\\"error\\\":\\\"Missing address or profile\\\"}\");\n        return;\n    }\n\n    if (!runtime.setDeviceDefaultProfile) {",
    "                                bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {\n    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;\n\n    if (!server.hasArg(\"address\") || !server.hasArg(\"profile\")) {\n        server.send(400, \"application/json\", \"{\\\"error\\\":\\\"Missing address or profile\\\"}\");\n        return;\n    }\n\n    if (!runtime.setDeviceDefaultProfile) {"
)
src = src.replace(
    "    if (!runtime.setDeviceDefaultProfile(address, static_cast<uint8_t>(profile))) {",
    "    if (!runtime.setDeviceDefaultProfile(address, static_cast<uint8_t>(profile), runtime.setDeviceDefaultProfileCtx)) {"
)

# handleApiDeviceDelete
src = src.replace(
    "                           const std::function<bool()>& checkRateLimit) {\n    if (checkRateLimit && !checkRateLimit()) return;\n\n    if (!server.hasArg(\"address\")) {\n        server.send(400, \"application/json\", \"{\\\"error\\\":\\\"Missing address\\\"}\");\n        return;\n    }\n\n    if (!runtime.deleteDevice) {",
    "                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx) {\n    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;\n\n    if (!server.hasArg(\"address\")) {\n        server.send(400, \"application/json\", \"{\\\"error\\\":\\\"Missing address\\\"}\");\n        return;\n    }\n\n    if (!runtime.deleteDevice) {"
)
src = src.replace(
    "    if (!runtime.deleteDevice(address)) {",
    "    if (!runtime.deleteDevice(address, runtime.deleteDeviceCtx)) {"
)

with open(cpp_path, "w") as f:
    f.write(src)
print(f"Updated {cpp_path}")

# =========================================
# 3. Test
# =========================================
test_path = "test/test_wifi_v1_devices_api_service/test_wifi_v1_devices_api_service.cpp"
with open(test_path) as f:
    src = f.read()

# Replace makeRuntime body - aggregate init with capturing lambdas -> fn-ptr+ctx pairs
src = src.replace(
    """\
static WifiV1DevicesApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1DevicesApiService::Runtime{
        [&rt]() {
            rt.listCalls++;
            return rt.devices;
        },
        [&rt](const String& address, const String& name) {
            rt.setNameCalls++;
            rt.lastAddress = address;
            rt.lastName = name;
            return rt.setNameResult;
        },
        [&rt](const String& address, uint8_t profile) {
            rt.setProfileCalls++;
            rt.lastAddress = address;
            rt.lastProfile = profile;
            return rt.setProfileResult;
        },
        [&rt](const String& address) {
            rt.deleteCalls++;
            rt.lastAddress = address;
            return rt.deleteResult;
        },
    };
}""",
    """\
static WifiV1DevicesApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiV1DevicesApiService::Runtime{
        [](void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->listCalls++;
            return rtp->devices;
        }, &rt,
        [](const String& address, const String& name, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setNameCalls++;
            rtp->lastAddress = address;
            rtp->lastName = name;
            return rtp->setNameResult;
        }, &rt,
        [](const String& address, uint8_t profile, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setProfileCalls++;
            rtp->lastAddress = address;
            rtp->lastProfile = profile;
            return rtp->setProfileResult;
        }, &rt,
        [](const String& address, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->deleteCalls++;
            rtp->lastAddress = address;
            return rtp->deleteResult;
        }, &rt,
    };
}"""
)

# Handler calls: []() { return true; } -> fn-ptr
src = src.replace(
    "        []() { return true; });",
    "        [](void* /*ctx*/) { return true; }, nullptr);"
)
src = src.replace(
    "        []() { return false; });",
    "        [](void* /*ctx*/) { return false; }, nullptr);"
)

with open(test_path, "w") as f:
    f.write(src)
print(f"Updated {test_path}")

# =========================================
# 4. wifi_runtimes.cpp - makeV1DevicesRuntime
# =========================================
runtimes_path = "src/wifi_runtimes.cpp"
with open(runtimes_path) as f:
    src = f.read()

src = src.replace(
    """\
WifiV1DevicesApiService::Runtime WiFiManager::makeV1DevicesRuntime() {
    return WifiV1DevicesApiService::Runtime{
        [this]() {
            std::vector<WifiV1DevicesApiService::DeviceInfo> payload;
            if (!v1DeviceStore.isReady()) {
                return payload;
            }

            auto devices = v1DeviceStore.listDevices();
            auto hasAddress = [&](const String& address) {
                if (address.length() == 0) {
                    return true;
                }
                for (const auto& device : devices) {
                    if (device.address.equalsIgnoreCase(address)) {
                        return true;
                    }
                }
                return false;
            };

            const String lastV1Address = normalizeV1DeviceAddress(settingsManager.get().lastV1Address);
            if (!hasAddress(lastV1Address)) {
                v1DeviceStore.touchDeviceInMemory(lastV1Address);
                devices = v1DeviceStore.listDevices();
            }

            String connectedAddress;
            NimBLEAddress connected = bleClient.getConnectedAddress();
            if (!connected.isNull()) {
                connectedAddress = normalizeV1DeviceAddress(String(connected.toString().c_str()));
                if (!hasAddress(connectedAddress)) {
                    v1DeviceStore.touchDeviceInMemory(connectedAddress);
                    devices = v1DeviceStore.listDevices();
                }
            }

            payload.reserve(devices.size());
            for (const auto& device : devices) {
                WifiV1DevicesApiService::DeviceInfo info;
                info.address = device.address;
                info.name = device.name;
                info.defaultProfile = device.defaultProfile;
                info.connected = connectedAddress.length() > 0 &&
                                 connectedAddress.equalsIgnoreCase(device.address);
                payload.push_back(info);
            }
            return payload;
        },
        [](const String& address, const String& name) {
            return v1DeviceStore.setDeviceName(address, name);
        },
        [](const String& address, uint8_t defaultProfile) {
            return v1DeviceStore.setDeviceDefaultProfile(address, defaultProfile);
        },
        [](const String& address) {
            return v1DeviceStore.removeDevice(address);
        },
    };
}""",
    """\
WifiV1DevicesApiService::Runtime WiFiManager::makeV1DevicesRuntime() {
    return WifiV1DevicesApiService::Runtime{
        [](void* /*ctx*/) {
            std::vector<WifiV1DevicesApiService::DeviceInfo> payload;
            if (!v1DeviceStore.isReady()) {
                return payload;
            }

            auto devices = v1DeviceStore.listDevices();
            auto hasAddress = [&](const String& address) {
                if (address.length() == 0) {
                    return true;
                }
                for (const auto& device : devices) {
                    if (device.address.equalsIgnoreCase(address)) {
                        return true;
                    }
                }
                return false;
            };

            const String lastV1Address = normalizeV1DeviceAddress(settingsManager.get().lastV1Address);
            if (!hasAddress(lastV1Address)) {
                v1DeviceStore.touchDeviceInMemory(lastV1Address);
                devices = v1DeviceStore.listDevices();
            }

            String connectedAddress;
            NimBLEAddress connected = bleClient.getConnectedAddress();
            if (!connected.isNull()) {
                connectedAddress = normalizeV1DeviceAddress(String(connected.toString().c_str()));
                if (!hasAddress(connectedAddress)) {
                    v1DeviceStore.touchDeviceInMemory(connectedAddress);
                    devices = v1DeviceStore.listDevices();
                }
            }

            payload.reserve(devices.size());
            for (const auto& device : devices) {
                WifiV1DevicesApiService::DeviceInfo info;
                info.address = device.address;
                info.name = device.name;
                info.defaultProfile = device.defaultProfile;
                info.connected = connectedAddress.length() > 0 &&
                                 connectedAddress.equalsIgnoreCase(device.address);
                payload.push_back(info);
            }
            return payload;
        }, nullptr,
        [](const String& address, const String& name, void* /*ctx*/) {
            return v1DeviceStore.setDeviceName(address, name);
        }, nullptr,
        [](const String& address, uint8_t defaultProfile, void* /*ctx*/) {
            return v1DeviceStore.setDeviceDefaultProfile(address, defaultProfile);
        }, nullptr,
        [](const String& address, void* /*ctx*/) {
            return v1DeviceStore.removeDevice(address);
        }, nullptr,
    };
}"""
)

with open(runtimes_path, "w") as f:
    f.write(src)
print(f"Updated {runtimes_path}")

# =========================================
# 5. wifi_routes.cpp - V1 devices routes
# =========================================
routes_path = "src/wifi_routes.cpp"
with open(routes_path) as f:
    src = f.read()

# handleApiDeviceNameSave
src = src.replace(
    """\
    server.on("/api/v1/devices/name", HTTP_POST, [this, rateLimitCallback]() {
        WifiV1DevicesApiService::handleApiDeviceNameSave(
            server,
            makeV1DevicesRuntime(),
            rateLimitCallback);
    });""",
    """\
    server.on("/api/v1/devices/name", HTTP_POST, [this]() {
        WifiV1DevicesApiService::handleApiDeviceNameSave(
            server,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });"""
)

# handleApiDeviceProfileSave
src = src.replace(
    """\
    server.on("/api/v1/devices/profile", HTTP_POST, [this, rateLimitCallback]() {
        WifiV1DevicesApiService::handleApiDeviceProfileSave(
            server,
            makeV1DevicesRuntime(),
            rateLimitCallback);
    });""",
    """\
    server.on("/api/v1/devices/profile", HTTP_POST, [this]() {
        WifiV1DevicesApiService::handleApiDeviceProfileSave(
            server,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });"""
)

# handleApiDeviceDelete
src = src.replace(
    """\
    server.on("/api/v1/devices/delete", HTTP_POST, [this, rateLimitCallback]() {
        WifiV1DevicesApiService::handleApiDeviceDelete(
            server,
            makeV1DevicesRuntime(),
            rateLimitCallback);
    });""",
    """\
    server.on("/api/v1/devices/delete", HTTP_POST, [this]() {
        WifiV1DevicesApiService::handleApiDeviceDelete(
            server,
            makeV1DevicesRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });"""
)

with open(routes_path, "w") as f:
    f.write(src)
print(f"Updated {routes_path}")

print("\nV1Devices migration complete.")
