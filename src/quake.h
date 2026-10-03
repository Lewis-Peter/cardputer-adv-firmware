// 地震速报。数据来自 USGS 的公开 GeoJSON 摘要源（免 key、全球覆盖）：
//   https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/{2.5_day,4.5_week}.geojson
//
// 跟 Typhoon 那一页刻意分工：JMA 只发西北太平洋的热带气旋，USGS 这份是**全球**的，
// 大西洋、印度洋、内陆断裂带都在里面。两页共用 worldmap.h 那张陆地掩码和 geoloc 的定位。
//
// ⚠️ 不走 fetchJsonHttp，**逐 feature 流式解析**——理由跟 github.cpp 顶上那段一模一样：
// 这一页同时背着 TLS 常驻缓冲 + 证书链校验的峰值，而 2.5_day 那个源一天里能有 100 多条、
// 整包 200KB 以上。整包进 ArduinoJson 必然 NoMemory（github 页 2026-08-26 就是这么炸的，
// minEver 掉到 584 字节）。逐元素解析的堆占用跟返回大小无关，只跟单条 feature 有关。
//
// ⚠️ USGS 的 properties.time 是**毫秒**级 epoch（不是秒），而且是 int64——用 int 接会溢出。
#pragma once
#include "globals.h"

void quakeEnter();
void quakeUpdate();
void quakeKey(char k);
void drawQuake();       // 第1页：最近地震列表 + 选中那条的详情
void drawQuakeMap();    // 第2页：世界地图打点（震级决定点的大小和颜色）
