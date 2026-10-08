#include "../../src/rid_radar.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>

int main() {
  // 正北 1km：east≈0, north≈1000
  RadarPt n = radarRel(22.0, 114.0, 22.0 + 1000.0 / 111320.0, 114.0);
  assert(fabsf(n.northM - 1000) < 1 && fabsf(n.eastM) < 1 && fabsf(n.distM - 1000) < 1);
  // 正东：高纬度经度压缩
  RadarPt e = radarRel(60.0, 10.0, 60.0, 10.0 + 0.01);
  assert(e.eastM > 540 && e.eastM < 580 && fabsf(e.northM) < 1);
  // 跨 180° 经线取短弧
  RadarPt w = radarRel(0.0, 179.995, 0.0, -179.995);
  assert(w.eastM > 0 && w.eastM < 1200);
  // 量程档位
  assert(radarPickRange(0) == 100 && radarPickRange(80) == 100 && radarPickRange(95) == 250);
  assert(radarPickRange(900) == 1000 && radarPickRange(1000) == 2000);
  assert(radarPickRange(1e9f) == 50000);
  // 像素映射：北→屏幕上方（dy<0），东→右
  int dx, dy; bool c;
  radarToPx(RadarPt{0, 500, 500}, 1000, 50, dx, dy, c); assert(dx == 0 && dy == -25 && !c);
  radarToPx(RadarPt{500, 0, 500}, 1000, 50, dx, dy, c); assert(dx == 25 && dy == 0 && !c);
  // 超量程：压到圆周
  radarToPx(RadarPt{3000, 4000, 5000}, 1000, 50, dx, dy, c);
  assert(c && abs(dx * dx + dy * dy - 2500) <= 60);
  radarToPx(RadarPt{0, 0, 0}, 1000, 50, dx, dy, c); assert(dx == 0 && dy == 0 && !c);
  std::puts("PASS: relative position, antimeridian, range picking, pixel mapping/clipping");
}
