// Mock FreeRTOS types for native testing
#pragma once

#include <cstdint>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstring>
#include "../mock_heap_caps_state.h"

// Semaphore/Mutex types
typedef void* SemaphoreHandle_t;
typedef void* QueueHandle_t;
typedef void* TaskHandle_t;
typedef struct StaticQueue_t {
    uint8_t unused = 0;
} StaticQueue_t;

// Tick type
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFF

typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#ifndef pdTRUE
#define pdTRUE 1
#endif
#ifndef pdFALSE
#define pdFALSE 0
#endif

// Semaphore stubs
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)1; }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return (void*)1; }
inline int xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}

// Queue stubs
struct MockQueueState {
    uint32_t capacity = 0;
    uint32_t itemSize = 0;
    std::deque<std::vector<uint8_t>> items;
};

struct MockQueueCreateState {
    uint32_t dynamicCalls = 0;
    uint32_t staticCalls = 0;
    uint32_t lastLength = 0;
    uint32_t lastItemSize = 0;
    uint8_t* lastStorageBuffer = nullptr;
    bool failDynamic = false;
    bool failStatic = false;
};

inline MockQueueCreateState g_mock_queue_create_state{};

inline void mock_reset_queue_create_state() {
    g_mock_queue_create_state = MockQueueCreateState{};
}

inline QueueHandle_t xQueueCreate(uint32_t length, uint32_t itemSize) {
    g_mock_queue_create_state.dynamicCalls++;
    g_mock_queue_create_state.lastLength = length;
    g_mock_queue_create_state.lastItemSize = itemSize;
    g_mock_queue_create_state.lastStorageBuffer = nullptr;
    if (g_mock_queue_create_state.failDynamic) {
        return nullptr;
    }
    MockQueueState* q = new MockQueueState();
    q->capacity = length;
    q->itemSize = itemSize;
    return reinterpret_cast<QueueHandle_t>(q);
}

inline QueueHandle_t xQueueCreateStatic(uint32_t length,
                                        uint32_t itemSize,
                                        uint8_t* storageBuffer,
                                        StaticQueue_t*) {
    g_mock_queue_create_state.staticCalls++;
    g_mock_queue_create_state.lastLength = length;
    g_mock_queue_create_state.lastItemSize = itemSize;
    g_mock_queue_create_state.lastStorageBuffer = storageBuffer;
    if (g_mock_queue_create_state.failStatic || storageBuffer == nullptr) {
        return nullptr;
    }
    MockQueueState* q = new MockQueueState();
    q->capacity = length;
    q->itemSize = itemSize;
    return reinterpret_cast<QueueHandle_t>(q);
}

inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t) {
    if (!queue || !item) return pdFALSE;
    MockQueueState* q = reinterpret_cast<MockQueueState*>(queue);
    if (q->items.size() >= q->capacity) return pdFALSE;
    const uint8_t* bytes = static_cast<const uint8_t*>(item);
    q->items.emplace_back(bytes, bytes + q->itemSize);
    return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t queue, void* out, TickType_t) {
    if (!queue || !out) return pdFALSE;
    MockQueueState* q = reinterpret_cast<MockQueueState*>(queue);
    if (q->items.empty()) return pdFALSE;
    std::vector<uint8_t> item = std::move(q->items.front());
    q->items.pop_front();
    std::memcpy(out, item.data(), std::min<size_t>(q->itemSize, item.size()));
    return pdTRUE;
}

inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
    if (!queue) return 0;
    MockQueueState* q = reinterpret_cast<MockQueueState*>(queue);
    return static_cast<UBaseType_t>(q->items.size());
}

inline void vQueueDelete(QueueHandle_t queue) {
    MockQueueState* q = reinterpret_cast<MockQueueState*>(queue);
    delete q;
}

struct MockTaskCreateState {
    uint32_t standardCalls = 0;
    uint32_t capsCalls = 0;
    uint32_t lastStackSize = 0;
    UBaseType_t lastPriority = 0;
    BaseType_t lastCore = 0;
    uint32_t lastCaps = 0;
    bool failStandard = false;
    bool failCaps = false;
};

inline MockTaskCreateState g_mock_task_create_state{};

inline void mock_reset_task_create_state() {
    g_mock_task_create_state = MockTaskCreateState{};
}

// Task stubs
inline void vTaskDelay(TickType_t) {}
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return nullptr; }

// Critical section stubs
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
inline void taskENTER_CRITICAL(portMUX_TYPE*) {}
inline void taskEXIT_CRITICAL(portMUX_TYPE*) {}

// ESP-specific
inline uint32_t esp_get_free_heap_size() { return 320000; }
inline uint32_t heap_caps_get_free_size(int) { return g_mock_heap_caps_free_size; }
#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL 0
#endif
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT 0
#endif
