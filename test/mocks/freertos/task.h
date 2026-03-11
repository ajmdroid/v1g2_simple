#pragma once

#include "FreeRTOS.h"

typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#ifndef pdPASS
#define pdPASS 1
#endif

#ifndef pdTRUE
#define pdTRUE 1
#endif

#ifndef pdFALSE
#define pdFALSE 0
#endif

#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (ms)
#endif

inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*),
                                          const char*,
                                          uint32_t,
                                          void*,
                                          UBaseType_t,
                                          TaskHandle_t*,
                                          BaseType_t) {
    g_mock_task_create_state.standardCalls++;
    g_mock_task_create_state.lastCaps = 0;
    if (g_mock_task_create_state.failStandard) {
        return pdFALSE;
    }
    return pdPASS;
}

inline BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*)(void*),
                                                  const char*,
                                                  uint32_t stackSize,
                                                  void*,
                                                  UBaseType_t priority,
                                                  TaskHandle_t*,
                                                  BaseType_t core,
                                                  uint32_t caps) {
    g_mock_task_create_state.capsCalls++;
    g_mock_task_create_state.lastStackSize = stackSize;
    g_mock_task_create_state.lastPriority = priority;
    g_mock_task_create_state.lastCore = core;
    g_mock_task_create_state.lastCaps = caps;
    if (g_mock_task_create_state.failCaps) {
        return pdFALSE;
    }
    return pdPASS;
}

inline void vTaskDelete(void*) {}
inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
inline void xTaskNotifyGive(TaskHandle_t) {}
inline void taskYIELD() {}
