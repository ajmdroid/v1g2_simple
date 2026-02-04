/**
 * WiFi Task Module - FreeRTOS task for non-blocking WiFi operations
 * 
 * This module moves WiFi operations to Core 0 to prevent blocking
 * the main loop (Core 1) which handles BLE and display updates.
 * 
 * Implementation Stages:
 * - Stage 1: Infrastructure only (mutex, queue, state structs) - NO behavior change
 * - Stage 2: Basic status monitoring on Core 0
 * - Stage 3: Connection management migration
 * - Stage 4: NTP sync migration
 * - Stage 5: Full WiFi operation migration
 * 
 * Thread Safety:
 * - Atomic variables for lock-free status checks (bool, simple types)
 * - Mutex-protected state struct for complex data (IP, SSID, etc.)
 * - Command queue for async operations from main loop
 * 
 * Author: V1 Gen2 Simple Display Project
 * Date: February 2026
 */

#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <atomic>

// ============================================================================
// Feature Flag - Set to 0 to disable WiFi task and use legacy behavior
// ============================================================================
#ifndef WIFI_TASK_ENABLED
#define WIFI_TASK_ENABLED 0  // DISABLED: Task on Core 0 interferes with BLE connection
#endif

// ============================================================================
// Configuration Constants
// ============================================================================
#define WIFI_TASK_STACK_SIZE    4096    // Stack size in bytes (4KB)
#define WIFI_TASK_PRIORITY      3       // Low priority (1-24 range, below BLE/display)
#define WIFI_TASK_CORE          0       // Run on Core 0 (PRO_CPU, where WiFi stack lives)
#define WIFI_TASK_INTERVAL_MS   100     // Task loop interval (10 Hz)
#define WIFI_CMD_QUEUE_SIZE     10      // Command queue depth
#define WIFI_MUTEX_TIMEOUT_MS   10      // Mutex wait timeout (non-blocking in main loop)

// ============================================================================
// Command Types (for main loop -> WiFi task communication)
// ============================================================================
enum class WiFiTaskCmd : uint8_t {
    NONE = 0,
    SYNC_NTP,           // Request NTP time sync
    UPDATE_MDNS,        // Update mDNS advertisement
    CHECK_CONNECTION,   // Force connection check
    RECONNECT,          // Force reconnection attempt
    DISCONNECT          // Disconnect from WiFi
};

// ============================================================================
// WiFi Task State (shared between cores, mutex protected)
// ============================================================================
struct WiFiTaskState {
    // Connection info
    IPAddress ipAddress;
    int8_t rssi;
    uint32_t connectTime;       // millis() when connected
    uint32_t lastNtpSync;       // millis() of last successful NTP sync
    char ssid[33];              // Current SSID (32 chars + null terminator)
    
    // Performance metrics
    uint32_t taskMaxTime_us;    // Max task execution time since last read
    uint32_t cmdProcessed;      // Total commands processed
    uint32_t reconnectCount;    // Number of reconnection attempts
};

// ============================================================================
// Atomic State (no mutex needed - lock-free access from any core)
// ============================================================================
extern std::atomic<bool> g_wifiTaskRunning;     // Is the task active?
extern std::atomic<bool> g_wifiConnected;       // Is WiFi connected? (fast check)
extern std::atomic<bool> g_ntpSynced;           // Has NTP synced at least once?
extern std::atomic<uint8_t> g_wifiTaskErrors;   // Error counter

// ============================================================================
// Shared State (mutex protected - use wifiTaskGetState() to access)
// ============================================================================
extern WiFiTaskState g_wifiTaskState;
extern SemaphoreHandle_t g_wifiTaskMutex;

// ============================================================================
// Command Queue
// ============================================================================
extern QueueHandle_t g_wifiTaskCmdQueue;

// ============================================================================
// Public API
// ============================================================================

/**
 * Initialize the WiFi task infrastructure.
 * Creates mutex, queue, and (when WIFI_TASK_ENABLED=1) starts the task on Core 0.
 * Safe to call even if WIFI_TASK_ENABLED is 0 (will just init structures).
 * 
 * Call this in setup() AFTER WiFi.begin() has been called.
 * 
 * @return true if initialization successful, false on error
 */
bool wifiTaskInit();

/**
 * Stop and cleanup the WiFi task.
 * Deletes task (if running). Safe to call multiple times.
 * Note: Mutex and queue are NOT deleted to avoid use-after-free issues.
 */
void wifiTaskStop();

/**
 * Check if WiFi task is currently running.
 * Thread-safe (uses atomic).
 * 
 * @return true if task is running on Core 0
 */
bool wifiTaskIsRunning();

/**
 * Send a command to the WiFi task (non-blocking).
 * Safe to call from main loop - will NEVER block.
 * 
 * @param cmd Command to send
 * @return true if command was queued, false if queue full or not initialized
 */
bool wifiTaskSendCmd(WiFiTaskCmd cmd);

/**
 * Get a copy of the current WiFi state (mutex protected).
 * Uses non-blocking mutex by default. May return false if mutex is held.
 * 
 * @param state Output structure to fill
 * @param timeout_ms Maximum time to wait for mutex (0 = don't wait, default)
 * @return true if state was read, false if mutex timeout
 */
bool wifiTaskGetState(WiFiTaskState& state, uint32_t timeout_ms = 0);

/**
 * Get WiFi task performance metrics for logging.
 * Resets max timing value after reading (for interval tracking).
 * 
 * @param maxTime_us Output: max task execution time since last call
 * @param queueDepth Output: current command queue depth
 * @return true if metrics read successfully
 */
bool wifiTaskGetMetrics(uint32_t& maxTime_us, uint8_t& queueDepth);

// ============================================================================
// Legacy Compatibility
// ============================================================================

/**
 * Check if we should use legacy WiFi handling in main loop.
 * Returns true when:
 * - WIFI_TASK_ENABLED is 0 (compile-time disabled), OR
 * - Task failed to start (runtime failure)
 * 
 * Use this to conditionally call wifiManager.process() in main loop.
 */
inline bool wifiTaskUseLegacy() {
    #if WIFI_TASK_ENABLED
    return !g_wifiTaskRunning.load();
    #else
    return true;  // Always use legacy when feature is disabled
    #endif
}

#endif // WIFI_TASK_H
