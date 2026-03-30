#!/usr/bin/env python3
"""Migrate OBD API test file from std::function to fn-ptr+ctx pattern.
All OBD test lambdas are stateless so no Ctx structs needed.
"""

path = "test/test_obd_api_service/test_obd_api_service.cpp"

with open(path, "r") as f:
    src = f.read()

changes = {
    # handleApiConfigGet: []() {} -> [](void* /*ctx*/) {}, nullptr
    "ObdApiService::handleApiConfigGet(server, settingsManager, []() {});":
        "ObdApiService::handleApiConfigGet(server, settingsManager, [](void* /*ctx*/) {}, nullptr);",

    # handleApiDevicesList: []() {} -> [](void* /*ctx*/) {}, nullptr
    "ObdApiService::handleApiDevicesList(server, obdRuntimeModule, settingsManager, []() {});":
        "ObdApiService::handleApiDevicesList(server, obdRuntimeModule, settingsManager, [](void* /*ctx*/) {}, nullptr);",

    # handleApiDeviceNameSave (2 occurrences): []() { return true; }, []() {}
    # -> [](void* /*ctx*/) { return true; }, nullptr, [](void* /*ctx*/) {}, nullptr
    "ObdApiService::handleApiDeviceNameSave(server,\n                                           settingsManager,\n                                           []() { return true; },\n                                           []() {});":
        "ObdApiService::handleApiDeviceNameSave(server,\n                                           settingsManager,\n                                           [](void* /*ctx*/) { return true; }, nullptr,\n                                           [](void* /*ctx*/) {}, nullptr);",

    # handleApiConfig (2 occurrences): both stateless
    "ObdApiService::handleApiConfig(server,\n                                   obdRuntimeModule,\n                                   settingsManager,\n                                   speedSourceSelector,\n                                   []() { return true; },\n                                   []() {});":
        "ObdApiService::handleApiConfig(server,\n                                   obdRuntimeModule,\n                                   settingsManager,\n                                   speedSourceSelector,\n                                   [](void* /*ctx*/) { return true; }, nullptr,\n                                   [](void* /*ctx*/) {}, nullptr);",

    # handleApiForget
    "ObdApiService::handleApiForget(server,\n                                   obdRuntimeModule,\n                                   settingsManager,\n                                   []() { return true; },\n                                   []() {});":
        "ObdApiService::handleApiForget(server,\n                                   obdRuntimeModule,\n                                   settingsManager,\n                                   [](void* /*ctx*/) { return true; }, nullptr,\n                                   [](void* /*ctx*/) {}, nullptr);",

    # handleApiScan (3 occurrences)
    "ObdApiService::handleApiScan(server,\n                                 obdRuntimeModule,\n                                 []() { return true; },\n                                 []() {});":
        "ObdApiService::handleApiScan(server,\n                                 obdRuntimeModule,\n                                 [](void* /*ctx*/) { return true; }, nullptr,\n                                 [](void* /*ctx*/) {}, nullptr);",

    # handleApiStatus: []() {} -> fn-ptr
    "ObdApiService::handleApiStatus(server, obdRuntimeModule, []() {});":
        "ObdApiService::handleApiStatus(server, obdRuntimeModule, [](void* /*ctx*/) {}, nullptr);",
}

for old, new in changes.items():
    count = src.count(old)
    if count == 0:
        print(f"WARNING: pattern not found:\n  {old[:80]!r}...")
    else:
        src = src.replace(old, new)
        print(f"Replaced {count}x: {old[:60]!r}...")

with open(path, "w") as f:
    f.write(src)

print("Done")
