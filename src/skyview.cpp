// 「抬头天际」背景：ADS-B 飞机雷达和卫星过顶两页共用的坐标系底图。
// 单独成文件而不是塞在 ui_common.cpp 里，是为了让 tools/uisim 的桌面模拟器能直接编译这份真代码
// ——ui_common.cpp 拖着 M5.Display / icons / worldmap 一堆板级依赖，在 Mac 上编不动，
// 一旦模拟器那边"自己抄一份"，改了这里就不会反映到渲染出来的图上（踩过一次）。
#include "ui_common.h"

// ---- 抬头天际背景（ADS-B / Sats 共用）----
const char* CARD8[8] = {"N","NE","E","SE","S","SW","W","NW"};

void drawSkyBg(int ytop, int yhor, int maxEl) {
  const int hh = yhor - ytop;
  // 天空渐变：越靠地平线越亮一点点，给个纵深
  for (int y = ytop; y < yhor; y++) {
    float t = (float)(y - ytop) / hh;
    int v = 6 + (int)(t * 10);
    cv.drawFastHLine(0, y, SW, cv.color565(v, v + 2, (int)(v * 1.4f)));
  }
  // 仰角网格线（右端标度数）。间隔跟着 maxEl 走——飞机雷达的上限只有 30 度，
  // 要是写死 30 度一条就一条都画不出来，天空会是一整片空白。
  const int step = maxEl >= 60 ? 30 : 10;
  cv.setTextDatum(middle_right); cv.setTextSize(1);
  for (int e = step; e < maxEl; e += step) {
    int y = yhor - e * hh / maxEl;
    for (int x = 0; x < SW; x += 8) cv.drawPixel(x, y, DIM_BORDER);
    char s[6]; snprintf(s, sizeof(s), "%d", e);
    cv.setTextColor(ICON_DIM);
    cv.drawString(s, SW - 2, y);
  }
  // 地平线 + 方位刻度
  cv.drawFastHLine(0, yhor, SW, ICON_DIM);
  cv.setTextDatum(top_center); cv.setTextSize(1);
  cv.setTextColor(ICON_DIM);
  const char* cp[5] = {"N", "E", "S", "W", "N"};
  for (int i = 0; i < 5; i++) {
    int x = i * SW / 4;
    int tx = x < 6 ? 6 : (x > SW - 6 ? SW - 6 : x);
    cv.drawFastVLine(x < 1 ? 0 : (x > SW - 1 ? SW - 1 : x), yhor - 3, 6, DIM_BORDER);
    cv.drawString(cp[i], tx, yhor + 3);
  }
}

// 将 (方位角, 仰角) 映射为天际图坐标 (x, y)。
// Sats 卫星过顶与 GNSS 卫星分布共用此投影换算。
// az: 0~360 度，el: 0~maxEl 度。
// 返回 false 表示在地平线以下 (el < 0)，调用方跳过绘制；返回 true 表示有效坐标。
// 贴顶的点纵坐标收拢至少 ytop + 4，保证点及外圈辉光不穿过顶部分隔线。
bool skyCoord(float az, float el, int ytop, int yhor, int maxEl, int& x, int& y) {
  if (el < 0) return false;
  if (el > maxEl) el = maxEl;
  const int hh = yhor - ytop;
  x = (int)(az / 360.0f * SW);
  y = yhor - (int)(el * hh / maxEl);
  if (y < ytop + 4) y = ytop + 4;
  return true;
}
