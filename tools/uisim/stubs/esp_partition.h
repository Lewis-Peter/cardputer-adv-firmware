#pragma once
#include <stdint.h>
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_ANY 0
struct esp_partition_t {
  uint32_t size;
};
static inline const esp_partition_t* esp_partition_find_first(int, int, const char*) { return nullptr; }
