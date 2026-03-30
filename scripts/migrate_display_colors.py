#!/usr/bin/env python3
"""
Comprehensive migration script for DisplayColors, AutoPush, and V1Profile
services from std::function to fn-ptr+ctx pattern.

For each service:
1. Header: std::function fields -> fn-ptr+ctx pairs, handler sigs
2. Cpp: runtime.field(args) -> runtime.field(args, runtime.fieldCtx), handler sigs
3. Test: makeRuntime lambdas -> stateless with &rt ctx, handler calls
4. wifi_runtimes.cpp: factory lambdas -> stateless
5. wifi_routes.cpp: route calls -> inline fn-ptr + this
"""

import re
import sys

def replace_all(src, changes, label=""):
    for old, new in changes.items():
        count = src.count(old)
        if count == 0:
            print(f"  WARNING [{label}]: pattern not found:\n    {old[:80]!r}...")
        else:
            src = src.replace(old, new)
            print(f"  [{label}] Replaced {count}x: {old[:60]!r}...")
    return src


# ========================================================
# DISPLAY COLORS SERVICE
# ========================================================

print("\n=== DISPLAY COLORS ===")

# --- Header ---
hdr = open("src/modules/wifi/wifi_display_colors_api_service.h").read()

hdr = hdr.replace(
    "#include <cstdint>\n#include <functional>\n\n#include \"../../settings.h\"",
    "#include <cstdint>\n\n#include \"../../settings.h\""
)

hdr = hdr.replace(
    """\
struct Runtime {
    std::function<const V1Settings&()> getSettings;
    std::function<void(const DisplaySettingsUpdate&)> applySettingsUpdate;
    std::function<void()> resetDisplaySettings;
    std::function<void(uint8_t)> setDisplayBrightness;
    std::function<void()> forceDisplayRedraw;
    std::function<void(uint32_t)> requestColorPreviewHoldMs;
    std::function<bool()> isColorPreviewRunning;
    std::function<void()> cancelColorPreview;
};""",
    """\
struct Runtime {
    const V1Settings& (*getSettings)(void* ctx);
    void* getSettingsCtx;
    void (*applySettingsUpdate)(const DisplaySettingsUpdate& update, void* ctx);
    void* applySettingsUpdateCtx;
    void (*resetDisplaySettings)(void* ctx);
    void* resetDisplaySettingsCtx;
    void (*setDisplayBrightness)(uint8_t brightness, void* ctx);
    void* setDisplayBrightnessCtx;
    void (*forceDisplayRedraw)(void* ctx);
    void* forceDisplayRedrawCtx;
    void (*requestColorPreviewHoldMs)(uint32_t durationMs, void* ctx);
    void* requestColorPreviewHoldMsCtx;
    bool (*isColorPreviewRunning)(void* ctx);
    void* isColorPreviewRunningCtx;
    void (*cancelColorPreview)(void* ctx);
    void* cancelColorPreviewCtx;
};"""
)

for fn, args in [
    ("handleApiSave", "WebServer& server,\n                   const Runtime& runtime,"),
    ("handleApiReset", "WebServer& server,\n                    const Runtime& runtime,"),
    ("handleApiPreview", "WebServer& server,\n                      const Runtime& runtime,"),
    ("handleApiClear", "WebServer& server,\n                    const Runtime& runtime,"),
]:
    hdr = hdr.replace(
        f"void {fn}({args}\n                      const std::function<bool()>& checkRateLimit);",
        f"void {fn}({args}\n                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);"
    )

open("src/modules/wifi/wifi_display_colors_api_service.h", "w").write(hdr)
print("  Updated header")

# --- Cpp ---
cpp = open("src/modules/wifi/wifi_display_colors_api_service.cpp").read()

# Update handler signatures + checkRateLimit calls
for fn_prefix in [
    "void handleApiSave(",
    "void handleApiReset(",
    "void handleApiPreview(",
    "void handleApiClear(",
]:
    cpp = re.sub(
        r'(void handle\w+\([^)]*const Runtime& runtime,\s*)const std::function<bool\(\)>& checkRateLimit\)',
        r'\1bool (*checkRateLimit)(void* ctx), void* rateLimitCtx)',
        cpp
    )

cpp = cpp.replace("if (checkRateLimit && !checkRateLimit()) return;", "if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;")

# Update runtime field calls - add Ctx arg
runtime_fields_dc = [
    "getSettings", "applySettingsUpdate", "resetDisplaySettings",
    "setDisplayBrightness", "forceDisplayRedraw", "requestColorPreviewHoldMs",
    "isColorPreviewRunning", "cancelColorPreview",
]

for field in runtime_fields_dc:
    # Match runtime.field(args) - needs to add ctx as last argument
    # Pattern: runtime.field( ... ) where ... may be empty or have args
    # We use a targeted approach: find exactly how each field is called
    pass

# Manual replacements for specific calls in display colors cpp
dc_cpp_changes = {
    "const V1Settings& s = runtime.getSettings();":
        "const V1Settings& s = runtime.getSettings(runtime.getSettingsCtx);",
    "    runtime.applySettingsUpdate(update);":
        "    runtime.applySettingsUpdate(update, runtime.applySettingsUpdateCtx);",
    "    if (hasBrightness && runtime.setDisplayBrightness) {\n        runtime.setDisplayBrightness(nextBrightness);\n    }":
        "    if (hasBrightness && runtime.setDisplayBrightness) {\n        runtime.setDisplayBrightness(nextBrightness, runtime.setDisplayBrightnessCtx);\n    }",
    "    if (hasDisplayStyle && runtime.forceDisplayRedraw) {\n        runtime.forceDisplayRedraw();\n    }":
        "    if (hasDisplayStyle && runtime.forceDisplayRedraw) {\n        runtime.forceDisplayRedraw(runtime.forceDisplayRedrawCtx);\n    }",
    "        if (runtime.requestColorPreviewHoldMs) {\n            runtime.requestColorPreviewHoldMs(5500);  // Hold ~5.5s and cycle bands during preview.\n        }":
        "        if (runtime.requestColorPreviewHoldMs) {\n            runtime.requestColorPreviewHoldMs(5500, runtime.requestColorPreviewHoldMsCtx);  // Hold ~5.5s and cycle bands during preview.\n        }",
    "    runtime.resetDisplaySettings();":
        "    runtime.resetDisplaySettings(runtime.resetDisplaySettingsCtx);",
    "    if (runtime.requestColorPreviewHoldMs) {\n        runtime.requestColorPreviewHoldMs(5500);\n    }":
        "    if (runtime.requestColorPreviewHoldMs) {\n        runtime.requestColorPreviewHoldMs(5500, runtime.requestColorPreviewHoldMsCtx);\n    }",
    "    const bool previewRunning =\n        runtime.isColorPreviewRunning && runtime.isColorPreviewRunning();":
        "    const bool previewRunning =\n        runtime.isColorPreviewRunning && runtime.isColorPreviewRunning(runtime.isColorPreviewRunningCtx);",
    "    if (runtime.cancelColorPreview) {\n            runtime.cancelColorPreview();\n        }":
        "    if (runtime.cancelColorPreview) {\n            runtime.cancelColorPreview(runtime.cancelColorPreviewCtx);\n        }",
    "    if (runtime.requestColorPreviewHoldMs) {\n        runtime.requestColorPreviewHoldMs(5500);\n    }\n    server.send(200, \"application/json\", \"{\\\"success\\\":true,\\\"active\\\":true}\");":
        "    if (runtime.requestColorPreviewHoldMs) {\n        runtime.requestColorPreviewHoldMs(5500, runtime.requestColorPreviewHoldMsCtx);\n    }\n    server.send(200, \"application/json\", \"{\\\"success\\\":true,\\\"active\\\":true}\");",
    "    if (!runtime.getSettings || !runtime.applySettingsUpdate) {":
        "    if (!runtime.getSettings || !runtime.applySettingsUpdate) {",  # no change needed
}

cpp = replace_all(cpp, dc_cpp_changes, "display_colors cpp")
open("src/modules/wifi/wifi_display_colors_api_service.cpp", "w").write(cpp)
print("  Updated cpp")

# --- Test ---
test = open("test/test_wifi_display_colors_api_service/test_wifi_display_colors_api_service.cpp").read()

test = test.replace(
    """\
static WifiDisplayColorsApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiDisplayColorsApiService::Runtime{
        [&rt]() -> const V1Settings& {
            return rt.settings;
        },
        [&rt](const DisplaySettingsUpdate& update) {
            applyDisplaySettingsUpdateForTest(rt, update);
        },
        [&rt]() {
            resetDisplaySettingsForTest(rt);
        },
        [&rt](uint8_t brightness) {
            rt.setDisplayBrightnessCalls++;
            rt.lastDisplayBrightness = brightness;
        },
        [&rt]() {
            rt.forceDisplayRedrawCalls++;
        },
        [&rt](uint32_t holdMs) {
            rt.requestColorPreviewHoldCalls++;
            rt.lastPreviewHoldMs = holdMs;
        },
        [&rt]() {
            return rt.isColorPreviewRunning;
        },
        [&rt]() {
            rt.cancelColorPreviewCalls++;
        },
    };
}""",
    """\
static WifiDisplayColorsApiService::Runtime makeRuntime(FakeRuntime& rt) {
    return WifiDisplayColorsApiService::Runtime{
        [](void* ctx) -> const V1Settings& {
            return static_cast<FakeRuntime*>(ctx)->settings;
        }, &rt,
        [](const DisplaySettingsUpdate& update, void* ctx) {
            applyDisplaySettingsUpdateForTest(*static_cast<FakeRuntime*>(ctx), update);
        }, &rt,
        [](void* ctx) {
            resetDisplaySettingsForTest(*static_cast<FakeRuntime*>(ctx));
        }, &rt,
        [](uint8_t brightness, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->setDisplayBrightnessCalls++;
            rtp->lastDisplayBrightness = brightness;
        }, &rt,
        [](void* ctx) {
            static_cast<FakeRuntime*>(ctx)->forceDisplayRedrawCalls++;
        }, &rt,
        [](uint32_t holdMs, void* ctx) {
            auto* rtp = static_cast<FakeRuntime*>(ctx);
            rtp->requestColorPreviewHoldCalls++;
            rtp->lastPreviewHoldMs = holdMs;
        }, &rt,
        [](void* ctx) {
            return static_cast<FakeRuntime*>(ctx)->isColorPreviewRunning;
        }, &rt,
        [](void* ctx) {
            static_cast<FakeRuntime*>(ctx)->cancelColorPreviewCalls++;
        }, &rt,
    };
}"""
)

# Handler calls with lambdas
test = test.replace("        []() { return false; });", "        [](void* /*ctx*/) { return false; }, nullptr);")
test = test.replace("        []() { return true; });", "        [](void* /*ctx*/) { return true; }, nullptr);")
# nullptr -> nullptr, nullptr
test = test.replace(
    "WifiDisplayColorsApiService::handleApiPreview(server, makeRuntime(rt), nullptr);",
    "WifiDisplayColorsApiService::handleApiPreview(server, makeRuntime(rt), nullptr, nullptr);"
)
test = test.replace(
    "WifiDisplayColorsApiService::handleApiClear(server, makeRuntime(rt), nullptr);",
    "WifiDisplayColorsApiService::handleApiClear(server, makeRuntime(rt), nullptr, nullptr);"
)

open("test/test_wifi_display_colors_api_service/test_wifi_display_colors_api_service.cpp", "w").write(test)
print("  Updated test")

# --- wifi_runtimes.cpp: makeDisplayColorsRuntime ---
runtimes = open("src/wifi_runtimes.cpp").read()

runtimes = runtimes.replace(
    """\
WifiDisplayColorsApiService::Runtime WiFiManager::makeDisplayColorsRuntime() {
    return WifiDisplayColorsApiService::Runtime{
        [this]() -> const V1Settings& {
            return settingsManager.get();
        },
        [this](const DisplaySettingsUpdate& update) {
            settingsManager.applyDisplaySettingsUpdate(update, SettingsPersistMode::Deferred);
        },
        [this]() {
            settingsManager.resetDisplaySettings(SettingsPersistMode::Deferred);
        },
        [this](uint8_t brightness) {
            display.setBrightness(brightness);
        },
        [this]() {
            display.forceNextRedraw();
        },
        [](uint32_t durationMs) {
            requestColorPreviewHold(durationMs);
        },
        []() {
            return isColorPreviewRunning();
        },
        []() {
            cancelColorPreview();
        },
    };
}""",
    """\
WifiDisplayColorsApiService::Runtime WiFiManager::makeDisplayColorsRuntime() {
    return WifiDisplayColorsApiService::Runtime{
        [](void* /*ctx*/) -> const V1Settings& {
            return settingsManager.get();
        }, nullptr,
        [](const DisplaySettingsUpdate& update, void* /*ctx*/) {
            settingsManager.applyDisplaySettingsUpdate(update, SettingsPersistMode::Deferred);
        }, nullptr,
        [](void* /*ctx*/) {
            settingsManager.resetDisplaySettings(SettingsPersistMode::Deferred);
        }, nullptr,
        [](uint8_t brightness, void* ctx) {
            static_cast<WiFiManager*>(ctx)->display.setBrightness(brightness);
        }, this,
        [](void* ctx) {
            static_cast<WiFiManager*>(ctx)->display.forceNextRedraw();
        }, this,
        [](uint32_t durationMs, void* /*ctx*/) {
            requestColorPreviewHold(durationMs);
        }, nullptr,
        [](void* /*ctx*/) {
            return isColorPreviewRunning();
        }, nullptr,
        [](void* /*ctx*/) {
            cancelColorPreview();
        }, nullptr,
    };
}"""
)

open("src/wifi_runtimes.cpp", "w").write(runtimes)
print("  Updated wifi_runtimes.cpp")

# --- wifi_routes.cpp: display routes ---
routes = open("src/wifi_routes.cpp").read()

routes = routes.replace(
    """\
    server.on("/api/display/settings", HTTP_POST, [this, rateLimitCallback]() {
        WifiDisplayColorsApiService::handleApiSave(
            server,
            makeDisplayColorsRuntime(),
            rateLimitCallback);
    });""",
    """\
    server.on("/api/display/settings", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiSave(
            server,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });"""
)

routes = routes.replace(
    """\
    server.on("/api/display/settings/reset", HTTP_POST, [this, rateLimitCallback]() {
        WifiDisplayColorsApiService::handleApiReset(
            server,
            makeDisplayColorsRuntime(),
            rateLimitCallback);
    });""",
    """\
    server.on("/api/display/settings/reset", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiReset(
            server,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });"""
)

routes = routes.replace(
    """\
    server.on("/api/display/preview", HTTP_POST, [this, rateLimitCallback]() {
        WifiDisplayColorsApiService::handleApiPreview(
            server,
            makeDisplayColorsRuntime(),
            rateLimitCallback);
    });""",
    """\
    server.on("/api/display/preview", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiPreview(
            server,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });"""
)

routes = routes.replace(
    """\
    server.on("/api/display/preview/clear", HTTP_POST, [this, rateLimitCallback]() {
        WifiDisplayColorsApiService::handleApiClear(
            server,
            makeDisplayColorsRuntime(),
            rateLimitCallback);
    });""",
    """\
    server.on("/api/display/preview/clear", HTTP_POST, [this]() {
        WifiDisplayColorsApiService::handleApiClear(
            server,
            makeDisplayColorsRuntime(),
            [](void* ctx) { return static_cast<WiFiManager*>(ctx)->checkRateLimit(); }, this);
    });"""
)

open("src/wifi_routes.cpp", "w").write(routes)
print("  Updated wifi_routes.cpp")

print("\nDisplayColors migration complete.")
