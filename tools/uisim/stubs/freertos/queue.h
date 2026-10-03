#pragma once
#include "FreeRTOS.h"

inline QueueHandle_t xQueueCreate(uint32_t, uint32_t) {
  static int dummy;
  return (QueueHandle_t)&dummy;
}
inline void vQueueDelete(QueueHandle_t) {}
inline void xQueueReset(QueueHandle_t) {}
inline BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t) {
  return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t) {
  return pdFALSE;
}
