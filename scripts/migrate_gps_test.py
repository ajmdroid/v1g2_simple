#!/usr/bin/env python3
"""Migrate GPS API test file from std::function to fn-ptr+ctx pattern."""

import sys

path = "test/test_gps_api_service/test_gps_api_service.cpp"

with open(path, "r") as f:
    src = f.read()

helpers = """\nvoid tearDown() {}\n\nnamespace {\n\nstruct RateLimitCtx {\n    int calls = 0;\n    bool allow = true;\n};\n\nstruct UiActivityCtx {\n    int calls = 0;\n};\n\nstatic bool doRateLimit(void* ctx) {\n    auto* c = static_cast<RateLimitCtx*>(ctx);\n    c->calls++;\n    return c->allow;\n}\n\nstatic void doUiActivity(void* ctx) {\n    static_cast<UiActivityCtx*>(ctx)->calls++;\n}\n\n}  // namespace\n"""

# Replace tearDown + inject helpers
src = src.replace("void tearDown() {}\n", helpers, 1)

changes = {
    # handleApiStatus: uiActivityCalls local var
    "    int uiActivityCalls = 0;\n\n    gpsRuntime.snapshotStatus.enabled = true;":
        "    UiActivityCtx uiCtx;\n\n    gpsRuntime.snapshotStatus.enabled = true;",

    # handleApiStatus: lambda at end of call
    "        [&uiActivityCalls]() { ++uiActivityCalls; });\n\n    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);\n    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);\n    TEST_ASSERT_EQUAL_INT(1, gpsRuntime.snapshotCalls);":
        "        doUiActivity, &uiCtx);\n\n    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);\n    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);\n    TEST_ASSERT_EQUAL_INT(1, gpsRuntime.snapshotCalls);",

    # handleApiObservations rate-limited (return false)
    "    int rateLimitCalls = 0;\n    int uiActivityCalls = 0;\n\n    GpsApiService::handleApiObservations(\n        server,\n        gpsLog,\n        [&rateLimitCalls]() {\n            ++rateLimitCalls;\n            return false;\n        },\n        [&uiActivityCalls]() { ++uiActivityCalls; });\n\n    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);\n    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);":
        "    RateLimitCtx rlCtx{ .allow = false };\n    UiActivityCtx uiCtx;\n\n    GpsApiService::handleApiObservations(\n        server,\n        gpsLog,\n        doRateLimit, &rlCtx,\n        doUiActivity, &uiCtx);\n\n    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);\n    TEST_ASSERT_EQUAL_INT(0, uiCtx.calls);",

    # handleApiObservations returns samples (return true)
    "    int rateLimitCalls = 0;\n    int uiActivityCalls = 0;\n\n    gpsLog.publish(makeObservation(800, true, 15.0f, 4, 1.5f, false, NAN, NAN));\n    gpsLog.publish(makeObservation(900, true, 35.5f, 6, 0.8f, true, 40.1f, -73.2f));\n    server.setArg(\"limit\", \"1\");\n\n    GpsApiService::handleApiObservations(\n        server,\n        gpsLog,\n        [&rateLimitCalls]() {\n            ++rateLimitCalls;\n            return true;\n        },\n        [&uiActivityCalls]() { ++uiActivityCalls; });\n\n    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);\n    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);":
        "    RateLimitCtx rlCtx;\n    UiActivityCtx uiCtx;\n\n    gpsLog.publish(makeObservation(800, true, 15.0f, 4, 1.5f, false, NAN, NAN));\n    gpsLog.publish(makeObservation(900, true, 35.5f, 6, 0.8f, true, 40.1f, -73.2f));\n    server.setArg(\"limit\", \"1\");\n\n    GpsApiService::handleApiObservations(\n        server,\n        gpsLog,\n        doRateLimit, &rlCtx,\n        doUiActivity, &uiCtx);\n\n    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);\n    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);",

    # handleApiConfigGet: uiActivityCalls local var (unique context: settingsManager.settings.gpsEnabled = false)
    "    int uiActivityCalls = 0;\n\n    settingsManager.settings.gpsEnabled = false;\n    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_ENFORCE;":
        "    UiActivityCtx uiCtx;\n\n    settingsManager.settings.gpsEnabled = false;\n    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_ENFORCE;",

    # handleApiConfigGet: lambda at end -> also check the assert below
    "        [&uiActivityCalls]() { ++uiActivityCalls; });\n\n    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);\n    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);\n\n    JsonDocument doc;\n    parseBody(server, doc);\n\n    TEST_ASSERT_TRUE(doc[\"success\"].as<bool>());\n    TEST_ASSERT_FALSE(doc[\"enabled\"].as<bool>());":
        "        doUiActivity, &uiCtx);\n\n    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);\n    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);\n\n    JsonDocument doc;\n    parseBody(server, doc);\n\n    TEST_ASSERT_TRUE(doc[\"success\"].as<bool>());\n    TEST_ASSERT_FALSE(doc[\"enabled\"].as<bool>());",

    # handleApiConfig rate-limited: both captures
    "    int rateLimitCalls = 0;\n    int uiActivityCalls = 0;\n\n    GpsApiService::handleApiConfig(\n        server,\n        settingsManager,\n        gpsRuntime,\n        speedSelector,\n        lockoutLearner,\n        gpsLog,\n        perfCounters,\n        eventBus,\n        [&rateLimitCalls]() {\n            ++rateLimitCalls;\n            return false;\n        },\n        [&uiActivityCalls]() { ++uiActivityCalls; });\n\n    TEST_ASSERT_EQUAL_INT(1, rateLimitCalls);\n    TEST_ASSERT_EQUAL_INT(0, uiActivityCalls);\n    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);":
        "    RateLimitCtx rlCtx{ .allow = false };\n    UiActivityCtx uiCtx;\n\n    GpsApiService::handleApiConfig(\n        server,\n        settingsManager,\n        gpsRuntime,\n        speedSelector,\n        lockoutLearner,\n        gpsLog,\n        perfCounters,\n        eventBus,\n        doRateLimit, &rlCtx,\n        doUiActivity, &uiCtx);\n\n    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);\n    TEST_ASSERT_EQUAL_INT(0, uiCtx.calls);\n    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);",

    # handleApiConfig rejects_invalid_json: stateless + capture
    "    int uiActivityCalls = 0;\n\n    server.setArg(\"plain\", \"{bad json\");\n\n    GpsApiService::handleApiConfig(\n        server,\n        settingsManager,\n        gpsRuntime,\n        speedSelector,\n        lockoutLearner,\n        gpsLog,\n        perfCounters,\n        eventBus,\n        []() { return true; },\n        [&uiActivityCalls]() { ++uiActivityCalls; });\n\n    TEST_ASSERT_EQUAL_INT(1, uiActivityCalls);":
        "    UiActivityCtx uiCtx;\n\n    server.setArg(\"plain\", \"{bad json\");\n\n    GpsApiService::handleApiConfig(\n        server,\n        settingsManager,\n        gpsRuntime,\n        speedSelector,\n        lockoutLearner,\n        gpsLog,\n        perfCounters,\n        eventBus,\n        [](void* /*ctx*/) { return true; }, nullptr,\n        doUiActivity, &uiCtx);\n\n    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);",

    # handleApiConfig stateless + nullptr: many occurrences — batch replace
    "        []() { return true; },\n        nullptr);":
        "        [](void* /*ctx*/) { return true; }, nullptr,\n        nullptr, nullptr);",
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
