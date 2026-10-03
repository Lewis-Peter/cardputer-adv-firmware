#pragma once
#include "FreeRTOS.h"

inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char*, uint32_t, void*, BaseType_t, TaskHandle_t*, BaseType_t) {
  return pdPASS;
}
inline BaseType_t xTaskCreate(TaskFunction_t, const char*, uint32_t, void*, BaseType_t, TaskHandle_t*) {
  return pdPASS;
}
inline void vTaskDelete(TaskHandle_t) {}
inline void vTaskSuspend(TaskHandle_t) {}
