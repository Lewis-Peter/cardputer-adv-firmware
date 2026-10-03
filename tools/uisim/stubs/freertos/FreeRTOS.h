#pragma once
#include <cstdint>
#include <cstddef>

#ifndef pdTRUE
#define pdTRUE 1
#endif
#ifndef pdFALSE
#define pdFALSE 0
#endif
#ifndef pdPASS
#define pdPASS 1
#endif
#ifndef pdFAIL
#define pdFAIL 0
#endif
#ifndef portMAX_DELAY
#define portMAX_DELAY 0xffffffffUL
#endif
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (ms)
#endif

typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
typedef void* QueueHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef uint8_t StackType_t;
