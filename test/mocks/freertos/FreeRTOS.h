// Mock FreeRTOS types for native testing
#pragma once

#include <cstdint>

// Semaphore/Mutex types
typedef void* SemaphoreHandle_t;
typedef void* QueueHandle_t;
typedef void* TaskHandle_t;

// Tick type
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFF

// Semaphore stubs
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)1; }
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return (void*)1; }
inline int xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}

// Queue stubs
inline QueueHandle_t xQueueCreate(uint32_t, uint32_t) { return (void*)1; }
inline int xQueueSend(QueueHandle_t, const void*, TickType_t) { return 1; }
inline int xQueueReceive(QueueHandle_t, void*, TickType_t) { return 0; }
inline int uxQueueMessagesWaiting(QueueHandle_t) { return 0; }

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
inline uint32_t heap_caps_get_free_size(int) { return 320000; }
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
