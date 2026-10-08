#pragma once
#include <math.h>

// 雷达页的纯数学：把经纬度换成以"我"为中心的东/北向米数，再按量程缩到屏幕像素。
// 几十公里内用等距矩形近似足够（Remote ID 的有效距离远小于这个）。
struct RadarPt {
  float eastM, northM, distM;
};

inline RadarPt radarRel(double myLat, double myLon, double lat, double lon) {
  const double M_PER_DEG = 111320.0;
  RadarPt p;
  p.northM = (float)((lat - myLat) * M_PER_DEG);
  double dLon = lon - myLon;
  if (dLon > 180.0) dLon -= 360.0; else if (dLon < -180.0) dLon += 360.0;
  p.eastM = (float)(dLon * M_PER_DEG * cos(myLat * M_PI / 180.0));
  p.distM = sqrtf(p.eastM * p.eastM + p.northM * p.northM);
  return p;
}

// 量程档位（外圈对应的米数）。选能装下最远目标的最小一档。
inline float radarPickRange(float maxDistM) {
  static const float R[] = {100, 250, 500, 1000, 2000, 5000, 10000, 20000, 50000};
  const int n = sizeof(R) / sizeof(R[0]);
  for (int i = 0; i < n; ++i) if (maxDistM <= R[i] * 0.9f) return R[i];
  return R[n - 1];
}

// 北向上：屏幕 x 向右 = 东，y 向下 = 南。超出量程时压到圆周上，clipped = true。
inline void radarToPx(const RadarPt& p, float rangeM, int radiusPx, int& dx, int& dy, bool& clipped) {
  float k = (float)radiusPx / rangeM;
  float x = p.eastM * k, y = -p.northM * k;
  float r = sqrtf(x * x + y * y);
  clipped = r > radiusPx;
  if (clipped && r > 0) { x *= radiusPx / r; y *= radiusPx / r; }
  dx = (int)lroundf(x); dy = (int)lroundf(y);
}
