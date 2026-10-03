// 共享定位：GNSS 有定位就用实测坐标（最准），否则走 ip-api.com 的 IP 定位顺便拿城市名。
// Weather / ADS-B / Sats 三个 app 都要"我在哪"，所以抽出来一份，别各写各的。
#pragma once
#include "globals.h"

struct GeoFix {
  double lat = 0, lon = 0;
  String name = "";     // 城市名；GPS 定位时是 "GPS"
  bool fromGps = false;
  bool valid = false;
};

// 取当前位置。GNSS 有 fix 时每次都重新读（人是会动的）；IP 定位结果缓存住，
// 不然每个 app 每次刷新都去打一次 ip-api，纯属浪费（还会被限流）。
// err 非空时写入失败原因。
bool geoGet(GeoFix& out, String* err = nullptr);
// 只用不联网就能拿到的来源（GNSS fix、内存缓存、NVS 缓存）。拿不到返回 false，不发请求。
// 调用方拿它先试一下，就知道这次值不值得交给后台工人去跑（见 astro.cpp）。
bool geoGetOffline(GeoFix& out);
void geoInvalidate();   // 强制下次重新做 IP 定位（换网之后用）
