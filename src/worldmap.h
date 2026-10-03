// 世界陆地掩码：Natural Earth 110m land polygons（公有领域数据，简化到 110m 精度），
// 用 Python/Pillow 离线栅格化成 1bit/像素位图，MSB-first、每行 30 字节（240 恰好整除 8，不用补齐）。
// 覆盖整个地球 -180..180 经度 / +90..-90 纬度，等距圆柱投影。时钟第2页（晨昏线底图）和 GNSS
// 第3页（定位点在世界地图上）共用同一份数据，不追求精度，110m 已经是这个屏幕分辨率下能看出
// 效果的上限了。
#pragma once
#include <stdint.h>

static const int WORLD_MASK_W = 240;
static const int WORLD_MASK_H = 109;
static const int WORLD_MASK_STRIDE = 30;   // 每行字节数

// 位图数据本体在 worldmap.cpp（唯一一处定义）。这里只声明——写成 static 摆在头文件里的话，
// 每个 include 它的 .cpp 都会各存一份副本，3270 字节 × include 次数全是白占的 Flash。
extern const uint8_t WORLD_MASK[];

// x:0..239  y:0..108，超界调用方自己保证不越界
static inline bool worldIsLand(int x, int y) {
  return (WORLD_MASK[y * WORLD_MASK_STRIDE + (x >> 3)] & (0x80 >> (x & 7))) != 0;
}

// 经纬度直接查（自动裁到位图范围），给不是按 240x109 逐像素铺满整块位图的调用方用
// （比如 GNSS 地图那页，可用高度跟位图行数对不上，没法直接用上面那个按行列查的版本）。
static inline bool worldIsLandAtLatLon(double latDeg, double lonDeg) {
  int col = (int)((lonDeg + 180.0) / 360.0 * WORLD_MASK_W);
  int row = (int)((90.0 - latDeg) / 180.0 * WORLD_MASK_H);
  if (col < 0) col = 0; else if (col >= WORLD_MASK_W) col = WORLD_MASK_W - 1;
  if (row < 0) row = 0; else if (row >= WORLD_MASK_H) row = WORLD_MASK_H - 1;
  return worldIsLand(col, row);
}
