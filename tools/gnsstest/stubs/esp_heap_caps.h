#pragma once
#include <cstddef>
#include <cstdint>

#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_DMA  8
#define MALLOC_CAP_SPIRAM 16

inline size_t heap_caps_get_free_size(uint32_t) { return 120 * 1024; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return 60 * 1024; }
