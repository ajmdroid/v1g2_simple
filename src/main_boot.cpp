/**
 * main_boot.cpp — Boot-time helper functions extracted from main.cpp.
 *
 * These are self-contained utilities called during setup() that have
 * no dependency on main.cpp's mutable state variables.  Extracting them
 * keeps the core setup()/loop() orchestration file focused.
 */

#include "main_internals.h"
#include "display.h"
#include "settings.h"               // clampLockoutLearnerRadiusE5Value
#include "modules/perf/debug_macros.h"  // SerialLog
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>

// Display is defined in main.cpp; needed by fatalBootError().
extern V1Display display;

// --- resetReasonToString ---

const char* resetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "WDT_INT";
        case ESP_RST_TASK_WDT: return "WDT_TASK";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNKNOWN";
    }
}

// --- normalizeLegacyLockoutRadiusScale ---

uint32_t normalizeLegacyLockoutRadiusScale(JsonDocument& doc) {
    JsonArray zones = doc["zones"].as<JsonArray>();
    if (zones.isNull()) {
        return 0;
    }

    // Legacy lockout files stored radiusE5 in a 10x scale.
    // Heuristic is safe: legacy valid range started at 450.
    static constexpr uint16_t LEGACY_RADIUS_MIN_E5 = 450;
    uint32_t migrated = 0;
    for (JsonObject zone : zones) {
        if (zone["rad"].isNull()) {
            continue;
        }
        const uint16_t raw = zone["rad"].as<uint16_t>();
        if (raw < LEGACY_RADIUS_MIN_E5) {
            continue;
        }
        const uint16_t normalized = clampLockoutLearnerRadiusE5Value(
            static_cast<int>((raw + 5u) / 10u));
        if (normalized != raw) {
            zone["rad"] = normalized;
            ++migrated;
        }
    }
    return migrated;
}

// --- logPanicBreadcrumbs ---

// PANIC BREADCRUMBS: Log heap stats + coredump info on crash recovery
void logPanicBreadcrumbs() {
    esp_reset_reason_t reason = esp_reset_reason();
    bool isCrash = (reason == ESP_RST_PANIC ||
                    reason == ESP_RST_INT_WDT ||
                    reason == ESP_RST_TASK_WDT ||
                    reason == ESP_RST_WDT);

    if (!isCrash) return;

    Serial.println("\n!!! CRASH RECOVERY DETECTED !!!");
    Serial.printf("Reset reason: %s\n", resetReasonToString(reason));

    // Log current heap stats (post-crash, but helpful for baseline)
    uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    uint32_t minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    Serial.printf("Heap now: free=%lu, largest=%lu, minEver=%lu\n",
                  (unsigned long)freeHeap, (unsigned long)largestBlock, (unsigned long)minFreeHeap);

    // Check for coredump
    esp_core_dump_summary_t summary;
    esp_err_t err = esp_core_dump_get_summary(&summary);
    if (err == ESP_OK) {
        Serial.println("Coredump found:");
        Serial.printf("  Crashed task: %s\n", summary.exc_task);
        Serial.printf("  Exception cause: %lu\n", (unsigned long)summary.ex_info.exc_cause);
        Serial.printf("  Exception PC: 0x%08lx\n", (unsigned long)summary.exc_pc);

        // Print backtrace if available
        if (summary.exc_bt_info.depth > 0) {
            Serial.print("  Backtrace: ");
            for (int i = 0; i < summary.exc_bt_info.depth && i < 16; i++) {
                Serial.printf("0x%08lx ", (unsigned long)summary.exc_bt_info.bt[i]);
            }
            Serial.println();
        }
    } else {
        Serial.printf("No coredump available (err=%d) - check serial log for backtrace\n", err);
    }

    Serial.println("!!! END CRASH RECOVERY INFO !!!\n");

    // Best-effort: Try to write panic.txt to LittleFS (SD not mounted yet)
    // This runs BEFORE storage init, so we use LittleFS directly
    if (LittleFS.begin(false, "/littlefs", 10, "storage")) {  // false = never auto-format during panic logging
        File f = LittleFS.open("/panic.txt", FILE_WRITE);
        if (f) {
            f.printf("CRASH at boot (millis=%lu)\n", millis());
            f.printf("Reset reason: %s\n", resetReasonToString(reason));
            f.printf("Heap: free=%lu, largest=%lu, minEver=%lu\n",
                     (unsigned long)freeHeap, (unsigned long)largestBlock, (unsigned long)minFreeHeap);

            if (err == ESP_OK) {
                f.printf("Task: %s\n", summary.exc_task);
                f.printf("PC: 0x%08lx\n", (unsigned long)summary.exc_pc);
                if (summary.exc_bt_info.depth > 0) {
                    f.print("BT: ");
                    for (int i = 0; i < summary.exc_bt_info.depth && i < 16; i++) {
                        f.printf("0x%08lx ", (unsigned long)summary.exc_bt_info.bt[i]);
                    }
                    f.println();
                }
            }
            f.close();
            Serial.println("[PANIC] Wrote /panic.txt to LittleFS");
        }
        LittleFS.end();  // Release mount before storage manager takes ownership
    }
}

// --- nvsHealthCheck ---

// Log NVS statistics without mutating settings namespaces during early boot.
void nvsHealthCheck() {
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        uint32_t usedPct = (stats.used_entries * 100) / stats.total_entries;
        Serial.printf("[NVS] Entries: %lu/%lu used (%lu%%), namespaces: %lu, free: %lu\n",
                      (unsigned long)stats.used_entries,
                      (unsigned long)stats.total_entries,
                      (unsigned long)usedPct,
                      (unsigned long)stats.namespace_count,
                      (unsigned long)stats.free_entries);

        if (usedPct > 80) {
            Serial.println("[NVS] WARN: NVS >80% full; deferring namespace cleanup until settings load resolves the active namespace");
        }
    } else {
        Serial.println("[NVS] WARN: Could not get NVS stats");
    }
}

// --- nextBootId ---

uint32_t nextBootId() {
    Preferences prefs;
    if (!prefs.begin("v1boot", false)) {
        return 0;
    }
    uint32_t bootId = prefs.getUInt("bootId", 0) + 1;
    prefs.putUInt("bootId", bootId);
    prefs.end();
    return bootId;
}

// --- fatalBootError ---

// Helper for fatal boot errors - shows message, waits, then restarts
// displayAvailable: true if display.begin() succeeded and we can show on-screen error
void fatalBootError(const char* message, bool displayAvailable) {
    SerialLog.printf("FATAL: %s\n", message);

    if (displayAvailable) {
        // Show error on screen with countdown
        display.showDisconnected();  // Clear screen with base frame
        // Draw error message (red text, centered)
        // Note: Using drawStatusText-like approach
        SerialLog.println("Showing error on display, will restart in 10 seconds...");

        // Simple countdown - show message and wait
        for (int i = 10; i > 0; i--) {
            SerialLog.printf("Restarting in %d...\n", i);
            delay(1000);
        }
    } else {
        // No display - just wait and restart
        SerialLog.println("Display unavailable. Restarting in 10 seconds...");
        delay(10000);
    }

    SerialLog.println("Restarting...");
    ESP.restart();
}
