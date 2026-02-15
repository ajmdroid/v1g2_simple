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
    return pdPASS;
}

inline void vTaskDelete(void*) {}
inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
inline void xTaskNotifyGive(TaskHandle_t) {}
