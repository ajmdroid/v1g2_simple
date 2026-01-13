/**
 * Fixed-Size Event Ring Buffer
 * 
 * Heap-free, string-free event logging for embedded debugging.
 * 
 * Design:
 * - Fixed 64-entry ring buffer in static RAM
 * - No heap allocations, no String objects
 * - Constant-time insert (no blocking)
 * - Events are small structs with enum type + uint32 payload
 * - Dump via web API or serial command
 * 
 * Usage:
 * - EVENT_LOG(type, data) to record an event
 * - eventRingDump() to print all events
 * - /api/debug/events to get JSON
 */

#ifndef EVENT_RING_H
#define EVENT_RING_H

#include <Arduino.h>

// ============================================================================
// Event types (keep small - fits in uint8_t)
// ============================================================================
enum EventType : uint8_t {
    EVT_NONE = 0,
    
    // BLE events
    EVT_BLE_NOTIFY,          // BLE notification received (data = length)
    EVT_BLE_QUEUE_FULL,      // Queue full, dropped packet (data = queue depth)
    EVT_BLE_CONNECT,         // Connected to V1 (data = 0)
    EVT_BLE_DISCONNECT,      // Disconnected (data = reason code)
    EVT_BLE_RECONNECT,       // Reconnection attempt (data = attempt #)
    
    // Parse events
    EVT_PARSE_OK,            // Packet parsed (data = packet ID)
    EVT_PARSE_FAIL,          // Parse failure (data = error code)
    EVT_PARSE_RESYNC,        // Buffer resync (data = bytes skipped)
    
    // Display events
    EVT_DISPLAY_UPDATE,      // Display updated (data = latency_us / 100)
    EVT_DISPLAY_SKIP,        // Update skipped (data = reason)
    EVT_DISPLAY_FLUSH,       // Flush completed (data = duration_us / 100)
    
    // Alert events
    EVT_ALERT_NEW,           // New alert (data = band << 8 | strength)
    EVT_ALERT_CLEAR,         // Alerts cleared (data = 0)
    EVT_MUTE_ON,             // Mute activated (data = 0)
    EVT_MUTE_OFF,            // Mute deactivated (data = 0)
    
    // Push events
    EVT_PUSH_START,          // Auto-push started (data = slot)
    EVT_PUSH_CMD,            // Command sent (data = cmd type)
    EVT_PUSH_OK,             // Push succeeded (data = duration_ms)
    EVT_PUSH_FAIL,           // Push failed (data = reason code)
    
    // System events
    EVT_WIFI_CONNECT,        // WiFi connected (data = 0)
    EVT_WIFI_DISCONNECT,     // WiFi disconnected (data = 0)
    EVT_WIFI_AP_START,       // WiFi AP started (data = 0)
    EVT_WIFI_AP_STOP,        // WiFi AP stopped (data = 0)
    EVT_HEAP_LOW,            // Heap below threshold (data = free KB)
    EVT_LATENCY_SPIKE,       // Latency exceeded threshold (data = latency_us / 100)
    EVT_SLOW_LOOP,           // Main loop exceeded threshold (data = duration_ms)
    EVT_SLOW_DRAW,           // Display draw exceeded threshold (data = duration_ms)
    EVT_SLOW_PROXY,          // Proxy processing exceeded threshold (data = duration_ms)
    EVT_SLOW_PARSE,          // Parse exceeded threshold (data = duration_ms)
    EVT_SETUP_MODE_ENTER,    // Setup mode entered (data = 0)
    EVT_SETUP_MODE_EXIT,     // Setup mode exited (data = reason: 0=timeout, 1=manual)
    
    EVT_TYPE_COUNT           // Must be last
};

// ============================================================================
// Event structure (8 bytes for cache-friendly alignment)
// ============================================================================
struct Event {
    uint32_t timestampMs;    // millis() when event occurred
    uint16_t data;           // Event-specific payload
    EventType type;          // Event type enum
    uint8_t _pad;            // Padding for alignment
};

static_assert(sizeof(Event) == 8, "Event struct should be 8 bytes");

// ============================================================================
// Ring buffer configuration
// ============================================================================
#ifndef EVENT_RING_SIZE
#define EVENT_RING_SIZE 256   // Must be power of 2 (increased for diagnostics)
#endif

static_assert((EVENT_RING_SIZE & (EVENT_RING_SIZE - 1)) == 0, 
              "EVENT_RING_SIZE must be power of 2");

// Threshold constants for slow path detection
constexpr uint32_t SLOW_LOOP_THRESHOLD_MS = 25;   // Loop iteration > 25ms
constexpr uint32_t SLOW_DRAW_THRESHOLD_MS = 15;   // Display draw > 15ms
constexpr uint32_t SLOW_PARSE_THRESHOLD_MS = 5;   // Parse > 5ms
constexpr uint32_t SLOW_PROXY_THRESHOLD_MS = 10;  // Proxy > 10ms

// ============================================================================
// Global ring buffer
// ============================================================================
extern Event eventRing[EVENT_RING_SIZE];
extern volatile uint32_t eventRingHead;  // Next write position
extern volatile uint32_t eventRingCount; // Total events logged (for overflow detection)
extern portMUX_TYPE eventRingMux;        // Spinlock for thread safety

// ============================================================================
// API
// ============================================================================

// Initialize ring buffer
void eventRingInit();

// Log an event (constant-time, thread-safe with spinlock)
inline void eventRingLog(EventType type, uint16_t data = 0) {
    portENTER_CRITICAL(&eventRingMux);
    uint32_t idx = eventRingHead & (EVENT_RING_SIZE - 1);
    eventRing[idx].timestampMs = millis();
    eventRing[idx].data = data;
    eventRing[idx].type = type;
    eventRingHead++;
    eventRingCount++;
    portEXIT_CRITICAL(&eventRingMux);
}

// Convenience macro
#define EVENT_LOG(type, data) eventRingLog(type, data)

// Get event count (total logged, may exceed ring size)
inline uint32_t eventRingGetCount() {
    return eventRingCount;
}

// Check if ring has overflowed (lost events)
inline bool eventRingHasOverflow() {
    return eventRingCount > EVENT_RING_SIZE;
}

// Dump ring buffer to serial (for debugging)
void eventRingDump();

// Dump last N events to serial (compact format)
void eventRingDumpLast(uint32_t count);

// Get ring buffer as JSON (for web API)
String eventRingToJson();

// Clear ring buffer
void eventRingClear();

// Get human-readable event type name
const char* eventTypeName(EventType type);

// Process serial command for event ring (returns true if command was handled)
// Commands: "events", "events clear", "events last N"
bool eventRingProcessCommand(const String& cmd);

#endif // EVENT_RING_H
