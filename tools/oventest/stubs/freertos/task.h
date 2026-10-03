#pragma once
#include <cstdint>
#include <cstddef>

typedef void* TaskHandle_t;
inline void vTaskDelete(TaskHandle_t) {}
inline int xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t, void*, int, TaskHandle_t*, int) { return 1; }
