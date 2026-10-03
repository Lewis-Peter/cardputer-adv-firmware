#pragma once
#include "FreeRTOS.h"

// 模拟器单线程：锁全是空操作
typedef void* SemaphoreHandle_t;
struct StaticSemaphore_t { int dummy; };
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* b) { return b; }
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t) { return pdTRUE; }
