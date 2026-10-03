// 桌面侧的 esp_heap_caps 替身。只为让 src/ram_profile.cpp 能在这台机器上过一遍
// 编译器（-fsyntax-only），**不是**要在桌面上模拟堆——桌面上的"空闲内存"跟
// ESP32-S3 那 ~72KB 毫无可比性，报出来只会误导。
//
// 那为什么还要它？因为 ram_profile.cpp 全是 printf 格式串（%9lu / %+9ld / %-14s），
// 而格式串跟参数类型对不上是**静默**的：板子上照样烧得进去，只是打出来的数是垃圾。
// 桌面这一遍带 -Wformat，正好把这类错在没上手之前挡掉。
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_32BIT    (1 << 1)

inline size_t heap_caps_get_free_size(uint32_t) {
  static const size_t vals[] = {188000, 170000, 105200, 99000, 98000, 101200, 95000, 94000, 93600, 90000, 85000, 62000};
  static size_t i = 0;
  return vals[(i++) % (sizeof(vals)/sizeof(vals[0]))];
}
inline size_t heap_caps_get_largest_free_block(uint32_t) {
  static const size_t vals[] = {110000, 98000, 60000, 55000, 54000, 56000, 52000, 51500, 51000, 48000, 45000, 41000};
  static size_t i = 0;
  return vals[(i++) % (sizeof(vals)/sizeof(vals[0]))];
}

// globals.cpp 的画布按需释放/恢复（canvasRelease/canvasRestore）自己拿 heap_caps_malloc 申请显存。
// 桌面上没有 DMA 区之分，直接落到 malloc/free 上即可。
#include <stdlib.h>
inline void* heap_caps_malloc(size_t n, uint32_t) { return malloc(n); }
inline void  heap_caps_free(void* p) { free(p); }
