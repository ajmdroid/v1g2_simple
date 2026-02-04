/**
 * WiFi Task Implementation
 * 
 * Stage 1: Infrastructure only - task is created but NOT started yet.
 * This file creates the synchronization primitives and state structures
 * but does not change any runtime behavior until WIFI_TASK_ENABLED=1.
 * 
 * See wifi_task.h for full documentation.
 */

#include "wifi_task.h"

// ============================================================================
// Atomic State Definitions
// ============================================================================
std::atomic<bool> g_wifiTaskRunning{false};
std::atomic<bool> g_wifiConnected{false};
std::atomic<bool> g_ntpSynced{false};
std::atomic<uint8_t> g_wifiTaskErrors{0};

// ============================================================================
// Shared State Definitions
// ============================================================================
WiFiTaskState g_wifiTaskState = {
    .ipAddress = IPAddress(0, 0, 0, 0),
    .rssi = 0,
    .connectTime = 0,
    .lastNtpSync = 0,
    .ssid = {0},
    .taskMaxTime_us = 0,
    .cmdProcessed = 0,
    .reconnectCount = 0
};

SemaphoreHandle_t g_wifiTaskMutex = nullptr;
QueueHandle_t g_wifiTaskCmdQueue = nullptr;

// ============================================================================
// Private Variables
// ============================================================================
static TaskHandle_t s_wifiTaskHandle = nullptr;
static const char* TAG = "WiFiTask";

// ============================================================================
// Forward Declarations
// ============================================================================
#if WIFI_TASK_ENABLED
static void wifiTaskFunction(void* parameter);
#endif

// ============================================================================
// Public API Implementation
// ============================================================================

bool wifiTaskInit() {
    Serial.printf("[%s] Initializing (WIFI_TASK_ENABLED=%d)\n", TAG, WIFI_TASK_ENABLED);
    
    // Create mutex for shared state protection
    if (g_wifiTaskMutex == nullptr) {
        g_wifiTaskMutex = xSemaphoreCreateMutex();
        if (g_wifiTaskMutex == nullptr) {
            Serial.printf("[%s] ERROR: Failed to create mutex\n", TAG);
            g_wifiTaskErrors++;
            return false;
        }
        Serial.printf("[%s] Mutex created\n", TAG);
    }
    
    // Create command queue
    if (g_wifiTaskCmdQueue == nullptr) {
        g_wifiTaskCmdQueue = xQueueCreate(WIFI_CMD_QUEUE_SIZE, sizeof(WiFiTaskCmd));
        if (g_wifiTaskCmdQueue == nullptr) {
            Serial.printf("[%s] ERROR: Failed to create command queue\n", TAG);
            g_wifiTaskErrors++;
            return false;
        }
        Serial.printf("[%s] Command queue created (depth=%d)\n", TAG, WIFI_CMD_QUEUE_SIZE);
    }
    
    // Initialize shared state with current WiFi status (if already connected)
    if (xSemaphoreTake(g_wifiTaskMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (WiFi.status() == WL_CONNECTED) {
            g_wifiConnected.store(true);
            g_wifiTaskState.ipAddress = WiFi.localIP();
            g_wifiTaskState.rssi = WiFi.RSSI();
            g_wifiTaskState.connectTime = millis();
            String ssid = WiFi.SSID();
            strncpy(g_wifiTaskState.ssid, ssid.c_str(), sizeof(g_wifiTaskState.ssid) - 1);
            g_wifiTaskState.ssid[sizeof(g_wifiTaskState.ssid) - 1] = '\0';
            Serial.printf("[%s] WiFi already connected: %s (%s)\n", TAG, 
                         g_wifiTaskState.ssid, g_wifiTaskState.ipAddress.toString().c_str());
        } else {
            g_wifiConnected.store(false);
            Serial.printf("[%s] WiFi not connected (status=%d)\n", TAG, WiFi.status());
        }
        xSemaphoreGive(g_wifiTaskMutex);
    }
    
#if WIFI_TASK_ENABLED
    // Stage 2+: Create and start the WiFi task on Core 0
    if (s_wifiTaskHandle == nullptr) {
        BaseType_t result = xTaskCreatePinnedToCore(
            wifiTaskFunction,       // Task function
            "WiFiTask",             // Task name (for debugging)
            WIFI_TASK_STACK_SIZE,   // Stack size in bytes
            nullptr,                // Task parameters
            WIFI_TASK_PRIORITY,     // Priority (low)
            &s_wifiTaskHandle,      // Task handle output
            WIFI_TASK_CORE          // Core 0 (PRO_CPU)
        );
        
        if (result != pdPASS) {
            Serial.printf("[%s] ERROR: Failed to create task (result=%d)\n", TAG, result);
            g_wifiTaskErrors++;
            return false;
        }
        
        g_wifiTaskRunning.store(true);
        Serial.printf("[%s] Task started on Core %d (priority=%d, stack=%d bytes)\n", 
                      TAG, WIFI_TASK_CORE, WIFI_TASK_PRIORITY, WIFI_TASK_STACK_SIZE);
    }
#else
    Serial.printf("[%s] Task disabled (WIFI_TASK_ENABLED=0) - using legacy WiFi handling\n", TAG);
#endif
    
    Serial.printf("[%s] Initialization complete\n", TAG);
    return true;
}

void wifiTaskStop() {
    Serial.printf("[%s] Stopping...\n", TAG);
    
    // Signal task to stop
    g_wifiTaskRunning.store(false);
    
    // Delete task if running
    if (s_wifiTaskHandle != nullptr) {
        // Give task time to exit gracefully
        vTaskDelay(pdMS_TO_TICKS(WIFI_TASK_INTERVAL_MS * 2));
        
        // Force delete if still running
        if (eTaskGetState(s_wifiTaskHandle) != eDeleted) {
            vTaskDelete(s_wifiTaskHandle);
        }
        s_wifiTaskHandle = nullptr;
        Serial.printf("[%s] Task deleted\n", TAG);
    }
    
    // Note: We intentionally DON'T delete the mutex and queue here.
    // Other code might still reference them (race condition).
    // They'll be cleaned up on reboot. This is safe for embedded systems
    // where cleanup typically only happens at shutdown/restart.
    
    Serial.printf("[%s] Stopped\n", TAG);
}

bool wifiTaskIsRunning() {
    return g_wifiTaskRunning.load();
}

bool wifiTaskSendCmd(WiFiTaskCmd cmd) {
    if (g_wifiTaskCmdQueue == nullptr) {
        return false;
    }
    
    // Non-blocking send - timeout of 0 means never block main loop
    BaseType_t result = xQueueSend(g_wifiTaskCmdQueue, &cmd, 0);
    return (result == pdTRUE);
}

bool wifiTaskGetState(WiFiTaskState& state, uint32_t timeout_ms) {
    if (g_wifiTaskMutex == nullptr) {
        return false;
    }
    
    // Use 0 ticks for non-blocking, or convert ms to ticks
    TickType_t timeout = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    
    if (xSemaphoreTake(g_wifiTaskMutex, timeout) == pdTRUE) {
        // Copy entire state struct
        state = g_wifiTaskState;
        xSemaphoreGive(g_wifiTaskMutex);
        return true;
    }
    
    return false;  // Mutex timeout - don't block caller
}

bool wifiTaskGetMetrics(uint32_t& maxTime_us, uint8_t& queueDepth) {
    if (g_wifiTaskMutex == nullptr || g_wifiTaskCmdQueue == nullptr) {
        maxTime_us = 0;
        queueDepth = 0;
        return false;
    }
    
    // Get queue depth (thread-safe FreeRTOS call, no mutex needed)
    queueDepth = (uint8_t)uxQueueMessagesWaiting(g_wifiTaskCmdQueue);
    
    // Get and reset max time (needs mutex for atomic read-modify-write)
    if (xSemaphoreTake(g_wifiTaskMutex, pdMS_TO_TICKS(WIFI_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        maxTime_us = g_wifiTaskState.taskMaxTime_us;
        g_wifiTaskState.taskMaxTime_us = 0;  // Reset for next interval
        xSemaphoreGive(g_wifiTaskMutex);
        return true;
    }
    
    maxTime_us = 0;
    return false;  // Mutex timeout
}

// ============================================================================
// WiFi Task Function (Stage 2+ - only compiled when WIFI_TASK_ENABLED=1)
// ============================================================================
#if WIFI_TASK_ENABLED

static void wifiTaskFunction(void* parameter) {
    Serial.printf("[%s] Task running on Core %d\n", TAG, xPortGetCoreID());
    
    const TickType_t xDelay = pdMS_TO_TICKS(WIFI_TASK_INTERVAL_MS);
    uint32_t loopCount = 0;
    
    while (g_wifiTaskRunning.load()) {
        uint32_t startTime = micros();
        
        // ---------------------------------------------------------------------
        // 1. Update connection status (fast, non-blocking check)
        // ---------------------------------------------------------------------
        bool isConnected = (WiFi.status() == WL_CONNECTED);
        g_wifiConnected.store(isConnected);
        
        // ---------------------------------------------------------------------
        // 2. Update shared state (mutex protected)
        // ---------------------------------------------------------------------
        if (isConnected) {
            if (xSemaphoreTake(g_wifiTaskMutex, pdMS_TO_TICKS(WIFI_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                g_wifiTaskState.ipAddress = WiFi.localIP();
                g_wifiTaskState.rssi = WiFi.RSSI();
                
                // Update SSID if changed (rare)
                String currentSSID = WiFi.SSID();
                if (strncmp(g_wifiTaskState.ssid, currentSSID.c_str(), sizeof(g_wifiTaskState.ssid)) != 0) {
                    strncpy(g_wifiTaskState.ssid, currentSSID.c_str(), sizeof(g_wifiTaskState.ssid) - 1);
                    g_wifiTaskState.ssid[sizeof(g_wifiTaskState.ssid) - 1] = '\0';
                }
                
                xSemaphoreGive(g_wifiTaskMutex);
            }
        }
        
        // ---------------------------------------------------------------------
        // 3. Process command queue (non-blocking receive)
        // ---------------------------------------------------------------------
        WiFiTaskCmd cmd = WiFiTaskCmd::NONE;
        while (xQueueReceive(g_wifiTaskCmdQueue, &cmd, 0) == pdTRUE) {
            // Process command - Stage 3+ will implement actual handlers
            switch (cmd) {
                case WiFiTaskCmd::SYNC_NTP:
                    // TODO: Stage 4 - Implement async NTP sync here
                    Serial.printf("[%s] CMD: SYNC_NTP (not implemented - Stage 4)\n", TAG);
                    break;
                    
                case WiFiTaskCmd::UPDATE_MDNS:
                    // TODO: Stage 5 - Implement mDNS update here
                    Serial.printf("[%s] CMD: UPDATE_MDNS (not implemented - Stage 5)\n", TAG);
                    break;
                    
                case WiFiTaskCmd::CHECK_CONNECTION:
                    // Already done above (connection status check)
                    break;
                    
                case WiFiTaskCmd::RECONNECT:
                    // TODO: Stage 3 - Implement reconnection logic here
                    Serial.printf("[%s] CMD: RECONNECT (not implemented - Stage 3)\n", TAG);
                    if (xSemaphoreTake(g_wifiTaskMutex, pdMS_TO_TICKS(WIFI_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                        g_wifiTaskState.reconnectCount++;
                        xSemaphoreGive(g_wifiTaskMutex);
                    }
                    break;
                    
                case WiFiTaskCmd::DISCONNECT:
                    // TODO: Stage 3 - Implement disconnect logic here
                    Serial.printf("[%s] CMD: DISCONNECT (not implemented - Stage 3)\n", TAG);
                    break;
                    
                default:
                    break;
            }
            
            // Update command counter
            if (xSemaphoreTake(g_wifiTaskMutex, pdMS_TO_TICKS(WIFI_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                g_wifiTaskState.cmdProcessed++;
                xSemaphoreGive(g_wifiTaskMutex);
            }
        }
        
        // ---------------------------------------------------------------------
        // 4. Track performance metrics
        // ---------------------------------------------------------------------
        uint32_t elapsed = micros() - startTime;
        if (xSemaphoreTake(g_wifiTaskMutex, 0) == pdTRUE) {  // Non-blocking attempt
            if (elapsed > g_wifiTaskState.taskMaxTime_us) {
                g_wifiTaskState.taskMaxTime_us = elapsed;
            }
            xSemaphoreGive(g_wifiTaskMutex);
        }
        
        // ---------------------------------------------------------------------
        // 5. Yield to other tasks (100ms delay = 10 Hz loop rate)
        // ---------------------------------------------------------------------
        loopCount++;
        vTaskDelay(xDelay);
    }
    
    Serial.printf("[%s] Task exiting after %lu iterations\n", TAG, loopCount);
    
    // Task cleanup - delete self
    s_wifiTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

#endif // WIFI_TASK_ENABLED
