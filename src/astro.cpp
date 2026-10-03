#include "astro.h"
#include "bg_fetch.h"

#include <cmath>
#include <ctime>

#include "ui_common.h"
#include "icons.h"
#include "geoloc.h"
#include "worldmap.h"
#include "net_job.h"

// ---------------------------------------------------------------------------
// 位置。三页里只有日照需要它；晨昏线不需要，月相也不需要。
// geoGet() 在没有 GNSS fix 时会退回 IP 定位（那一步要联网），所以走 DeferredFetch
// ——按键回调和 render 都在 loop() 上，在里面同步拉网络会把主循环整个卡住。
// 拿到之后就一直用，页面本身不再有任何网络动作。
// ---------------------------------------------------------------------------
static DeferredFetch job;
static bool   havePos  = false;
static double aLat = 0, aLon = 0;
static String posName, posErr;

void astroEnter() { job.request(); }

static void astroApply(const GeoFix& fix) {
  aLat = fix.lat; aLon = fix.lon; posName = fix.name; havePos = true; posErr = "";
  dirty = true;
}

// 在后台工人上跑（bg_fetch.h）：没有 GNSS fix 也没缓存，要走一趟 IP 定位
static void astroLocate() {
  GeoFix fix; String e;
  if (geoGet(fix, &e)) astroApply(fix);
  else { havePos = false; posErr = e.length() ? e : "no position"; dirty = true; }
}

void astroUpdate() {
  if (!job.due()) return;
  // 大多数时候位置是现成的（GNSS 或缓存），当场用，不值得为它起一个 12KB 栈的任务、闪一下指示
  GeoFix fix;
  if (geoGetOffline(fix)) astroApply(fix);
  else bgFetchRun(astroLocate);
}

void astroKey(char k) {
  if (k == 'r' || k == 'R' || k == '\n') { job.request(); dirty = true; }
}

// ---------------------------------------------------------------------------
// 天文计算。这两个函数原来一个在 weather.cpp、一个在 ui_common.cpp，
// 是同一类东西（给 UTC 时刻推太阳在哪），放在一起。
// ---------------------------------------------------------------------------

// 太阳高度角（度），NOAA 那套低精度公式，误差 ~0.01°，画曲线绰绰有余。
// 输入是 UTC epoch，所以完全不碰时区——time(nullptr) 本来就是 UTC。
static float solarElev(time_t t, double latDeg, double lonDeg) {
  double n   = ((double)t - 946728000.0) / 86400.0;              // 距 J2000.0 的天数
  double L   = fmod(280.460 + 0.9856474 * n, 360.0);             // 平黄经
  double g   = fmod(357.528 + 0.9856003 * n, 360.0) * DEG_TO_RAD;// 平近点角
  double lam = (L + 1.915 * sin(g) + 0.020 * sin(2 * g)) * DEG_TO_RAD;
  double eps = 23.439 * DEG_TO_RAD;                              // 黄赤交角
  double dec = asin(sin(eps) * sin(lam));                        // 赤纬
  double ra  = atan2(cos(eps) * sin(lam), cos(lam));             // 赤经
  double gmst = fmod(18.697374558 + 24.06570982441908 * n, 24.0);
  if (gmst < 0) gmst += 24.0;
  double ha  = (gmst * 15.0 + lonDeg) * DEG_TO_RAD - ra;         // 时角
  double phi = latDeg * DEG_TO_RAD;
  return asin(sin(phi) * sin(dec) + cos(phi) * cos(dec) * cos(ha)) / DEG_TO_RAD;
}

// 日出/日落的判定高度角。**不是 0°**：-0.833° = 大气折射 34' + 太阳视半径 16'，
// 这是天文学和各家 API 通用的定义（"上边缘刚触地平线"）。用 0° 会把日出算晚、
// 日落算早各 3~5 分钟，纬度越高差得越多。
// 原来这两个值是从 open-meteo 的 daily.sunrise/sunset 抄来的，自己算之后这一页
// 才真的不需要网络。
static const float SUN_H0 = -0.833f;

// 在本地当天 [0, 86400) 秒里找高度角穿过 SUN_H0 的时刻。
// dir=+1 找上升沿（日出），-1 找下降沿（日落）。
// 先按 5 分钟粗扫定位变号区间，再二分 20 次收敛到秒级。
// 极昼/极夜时不存在穿越，返回 -1。
static int sunCross(time_t midnight, double lat, double lon, int dir) {
  const int STEP = 300;
  float prev = solarElev(midnight, lat, lon) - SUN_H0;
  for (int t = STEP; t <= 86400; t += STEP) {
    float cur = solarElev(midnight + t, lat, lon) - SUN_H0;
    bool up = (prev < 0 && cur >= 0), down = (prev >= 0 && cur < 0);
    if ((dir > 0 && up) || (dir < 0 && down)) {
      int lo = t - STEP, hi = t;
      for (int i = 0; i < 20; i++) {
        int mid = (lo + hi) / 2;
        float e = solarElev(midnight + mid, lat, lon) - SUN_H0;
        bool past = (dir > 0) ? (e >= 0) : (e < 0);
        if (past) hi = mid; else lo = mid;
      }
      return hi;
    }
    prev = cur;
  }
  return -1;
}

// 太阳直射点：赤纬 lat0（弧度）+ 直射经度 lon0Deg（东正，度）。NOAA 简化公式，只用当前 UTC
// 时刻（年内日期 + 一天中的时刻）推，不需要设备知道自己的经纬度——晨昏线形状只取决于太阳在
// 哪，不取决于设备在哪。注意用 gmtime_r 取 UTC，不能用本地时间（那个是加了时区偏移的）。
// 拆成带时刻参数的版本是为了 tools/astrotest 能喂指定时刻去比对参考星历。
static void subsolarPointAt(time_t now, double& lat0, double& lon0Deg) {
  struct tm utc; gmtime_r(&now, &utc);
  double utcHours = utc.tm_hour + utc.tm_min / 60.0 + utc.tm_sec / 3600.0;
  double gamma = 2.0 * PI / 365.0 * (utc.tm_yday + (utcHours - 12.0) / 24.0);
  lat0 = 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma)
         - 0.006758 * cos(2 * gamma) + 0.000907 * sin(2 * gamma)
         - 0.002697 * cos(3 * gamma) + 0.00148 * sin(3 * gamma);
  double eqtime = 229.18 * (0.000075 + 0.001868 * cos(gamma) - 0.032077 * sin(gamma)
                  - 0.014615 * cos(2 * gamma) - 0.040849 * sin(2 * gamma));
  lon0Deg = 15.0 * (12.0 - utcHours) - eqtime / 4.0;
  while (lon0Deg > 180)  lon0Deg -= 360;
  while (lon0Deg < -180) lon0Deg += 360;
}

static void subsolarPoint(double& lat0, double& lon0Deg) {
  subsolarPointAt(time(nullptr), lat0, lon0Deg);
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
static void hm(char* out, size_t n, int sec) {
  if (sec < 0) { snprintf(out, n, "--:--"); return; }
  snprintf(out, n, "%02d:%02d", (sec / 3600) % 24, (sec / 60) % 60);
}

static void rPod(int cx, int y, int cw, int ch, const char* lab, const char* val, uint16_t vc, uint16_t lc = 0x7BEF) {
  cv.fillRoundRect(cx, y, cw, ch, 3, 0x0821);
  cv.drawRoundRect(cx, y, cw, ch, 3, 0x18C3);
  cv.setTextDatum(top_center); cv.setTextSize(1);
  cv.setTextColor(lc, 0x0821);
  cv.drawString(lab, cx + cw / 2, y + 3);
  cv.setTextColor(vc, 0x0821);
  cv.drawString(val, cx + cw / 2, y + 14);
}

void drawAstroSun() {
  job.markShown();                      // ⚠️ 必须第一行，见 net_job.h
  cv.fillScreen(TFT_BLACK);

  char rightBuf[24] = "";
  if (job.pending) {
    snprintf(rightBuf, sizeof(rightBuf), "* LOCATING...");
  } else if (havePos && posName.length()) {
    snprintf(rightBuf, sizeof(rightBuf), "%s", trunc(posName, 13).c_str());
  }
  drawPageHeader("Daylight", rightBuf, 0xFDA0);

  time_t now = time(nullptr);
  struct tm lt{};
  if (!timeSynced || !localtime_r(&now, &lt)) {
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("waiting for clock sync...", SW / 2, SH / 2);
    drawPageDots();
    return;
  }
  if (!havePos) {
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(job.pending ? TFT_DARKGREY : TFT_RED, TFT_BLACK);
    cv.drawString(job.pending ? "locating position..." : trunc(posErr, 30).c_str(), SW / 2, SH / 2 - 6);
    if (!job.pending) {
      cv.setTextColor(ICON_DIM, TFT_BLACK);
      cv.drawString("Press 'r' to retry (GPS / WiFi)", SW / 2, SH / 2 + 8);
    }
    drawPageDots();
    return;
  }

  // 1. 太阳高度角主曲线卡片 (y = 15..93, h = 79)
  const int cardX = 4, cardY = 15, cardW = SW - 8, cardH = 79;
  cv.fillRoundRect(cardX, cardY, cardW, cardH, 3, 0x0821);
  cv.drawRoundRect(cardX, cardY, cardW, cardH, 3, 0x18C3);

  const int PX = 8, PW = 224, PY = 18, PH = 73;
  const int STEP = 2, NS = PW / STEP + 1;         // 每 2px 采一个点，113 个

  // 本地今天零点对应的 epoch
  int nowSec = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
  time_t midnight = now - nowSec;

  float el[NS];
  float lo = 1e9f, hi = -1e9f;
  for (int i = 0; i < NS; i++) {
    el[i] = solarElev(midnight + (time_t)((double)i / (NS - 1) * 86400.0), aLat, aLon);
    if (el[i] < lo) lo = el[i];
    if (el[i] > hi) hi = el[i];
  }
  if (hi < 8) hi = 8;
  if (lo > -8) lo = -8;
  float pad = (hi - lo) * 0.08f;
  hi += pad; lo -= pad;
  auto ey = [&](float e) { return PY + (int)((hi - e) / (hi - lo) * (PH - 1) + 0.5f); };
  const int hy = ey(0);   // 地平线

  const uint16_t DAY_FILL   = cv.color565(75, 48, 12);
  const uint16_t NIGHT_FILL = cv.color565(12, 16, 32);
  const uint16_t CURVE_COL  = 0xFDE0;

  // 渲染日夜区域填充
  for (int i = 0; i < NS; i++) {
    int x = PX + i * STEP, y = ey(el[i]);
    if (el[i] >= 0) cv.fillRect(x, y, STEP, hy - y + 1, DAY_FILL);
    else            cv.fillRect(x, hy, STEP, y - hy + 1, NIGHT_FILL);
  }

  // 6 / 12 / 18 点竖向标尺虚线
  for (int hh = 6; hh <= 18; hh += 6) {
    int x = PX + PW * hh / 24;
    for (int y = cardY + 2; y < cardY + cardH - 2; y += 3) cv.drawPixel(x, y, 0x18C3);
  }

  // 地平线参考线 (0°)
  if (hy >= cardY + 1 && hy <= cardY + cardH - 2) {
    cv.drawFastHLine(cardX + 1, hy, cardW - 2, 0x2945);
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setTextColor(0x4208, 0x0821);
    cv.drawString("0", cardX + 3, hy - 4);
  }

  // 描绘高度角平滑金线
  for (int i = 1; i < NS; i++)
    cv.drawLine(PX + (i - 1) * STEP, ey(el[i - 1]), PX + i * STEP, ey(el[i]), CURVE_COL);

  // 日出 / 日落时间计算
  int sr = sunCross(midnight, aLat, aLon, +1);
  int ss = sunCross(midnight, aLat, aLon, -1);
  char riseStr[8], setStr[8], noonStr[8];
  hm(riseStr, sizeof(riseStr), sr);
  hm(setStr,  sizeof(setStr),  ss);

  // 正午时刻与最大高度角（峰值三分法求极大）
  int mi = 0;
  for (int i = 1; i < NS; i++) if (el[i] > el[mi]) mi = i;
  double tLo = (double)(mi > 0 ? mi - 1 : 0)           / (NS - 1) * 86400.0;
  double tHi = (double)(mi < NS - 1 ? mi + 1 : NS - 1) / (NS - 1) * 86400.0;
  for (int i = 0; i < 30 && tHi - tLo > 0.5; i++) {
    double a = tLo + (tHi - tLo) / 3, c = tHi - (tHi - tLo) / 3;
    if (solarElev(midnight + (time_t)llround(a), aLat, aLon) <
        solarElev(midnight + (time_t)llround(c), aLat, aLon)) tLo = a; else tHi = c;
  }
  int noonSec = (int)llround((tLo + tHi) / 2);
  hm(noonStr, sizeof(noonStr), noonSec);
  float elMax = solarElev(midnight + noonSec, aLat, aLon);

  // 左上角峰值胶囊徽章
  char b[24];
  snprintf(b, sizeof(b), "MAX %.0f*", elMax);
  cv.fillRoundRect(cardX + 4, cardY + 4, 46, 9, 2, 0x2A00);
  cv.drawRoundRect(cardX + 4, cardY + 4, 46, 9, 2, 0x6BE0);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  cv.setTextColor(0xFDA0, 0x2A00);
  cv.drawString(b, cardX + 27, cardY + 8);

  // 当前太阳时刻：指示光柱 + 太阳核 + 实时读数徽章
  float elNow = solarElev(now, aLat, aLon);
  int nx = PX + (int)((float)nowSec / 86400.0f * PW);
  int ny = ey(elNow);
  for (int y = cardY + 2; y < cardY + cardH - 2; y += 2) cv.drawPixel(nx, y, 0x4208);
  cv.drawCircle(nx, ny, 4, elNow >= 0 ? 0xFDA0 : 0x4A69);
  cv.fillCircle(nx, ny, 2, TFT_WHITE);

  snprintf(b, sizeof(b), "%+.0f*", elNow);
  const int bw = 24, bh = 9;
  int bx = (nx > SW / 2) ? (nx - 6 - bw) : (nx + 6);
  int by = ny - 4;
  if (by < cardY + 2) by = cardY + 2;
  if (by + bh > cardY + cardH - 2) by = cardY + cardH - 2 - bh;
  cv.fillRoundRect(bx, by, bw, bh, 2, elNow >= 0 ? 0x3200 : 0x10A4);
  cv.drawRoundRect(bx, by, bw, bh, 2, elNow >= 0 ? 0x7BE0 : 0x2948);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  cv.setTextColor(elNow >= 0 ? 0xFDA0 : 0x8CD2, elNow >= 0 ? 0x3200 : 0x10A4);
  cv.drawString(b, bx + bw / 2, by + bh / 2);

  // 2. 底部四联赛博遥测指标舱 (y = 96..122, h = 26)
  const int podY = 96, podH = 26;
  rPod(4,   podY, 56, podH, "RISE", riseStr, 0xFDA0);
  rPod(63,  podY, 56, podH, "NOON", noonStr, TFT_WHITE);
  rPod(122, podY, 56, podH, "SET",  setStr,  0xFB80);

  // 昼长精准计算（修复跨午夜时 ss < sr 变 -- 的缺陷）
  char bDaylight[16];
  if (sr >= 0 && ss >= 0) {
    int dur = (ss >= sr) ? (ss - sr) : (86400 - sr + ss);
    snprintf(bDaylight, sizeof(bDaylight), "%dh%02dm", dur / 3600, (dur / 60) % 60);
  } else if (sr < 0 && ss < 0) {
    snprintf(bDaylight, sizeof(bDaylight), elMax > 0 ? "24h00m" : "0h00m");
  } else {
    snprintf(bDaylight, sizeof(bDaylight), "--");
  }
  rPod(181, podY, 55, podH, "DAYLIGHT", bDaylight, 0x07E0);

  drawPageDots();
}

// 晨昏线+世界地图。按列（经度）算太阳高度角=0 的纬度作为白天/黑夜分界，陆地/海洋
// 来自 worldmap.h 里离线栅格化好的 Natural Earth 110m 陆地掩码。
void drawAstroTerm() {
  job.markShown();
  cv.fillScreen(TFT_BLACK);

  time_t now = time(nullptr);
  struct tm utc{}; gmtime_r(&now, &utc);
  char rightBuf[24];
  snprintf(rightBuf, sizeof(rightBuf), "UTC %02d:%02d", utc.tm_hour, utc.tm_min);
  drawPageHeader("Terminator", rightBuf, ACCENT);

  if (!timeSynced) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString("no time sync yet", SW / 2, SH / 2 - 6);
    cv.drawString("can't place the terminator", SW / 2, SH / 2 + 6);
    drawPageDots();
    return;
  }

  double lat0, lon0Deg;
  subsolarPoint(lat0, lon0Deg);
  bool northLit = lat0 > 0;
  const uint16_t daySea    = cv.color565(25, 48, 75);
  const uint16_t dayLand   = cv.color565(60, 95, 48);
  const uint16_t nightSea  = cv.color565(8, 12, 22);
  const uint16_t nightLand = cv.color565(18, 32, 20);

  const int mapTop = 14, mapBot = 123;
  const int mapH = mapBot - mapTop;
  const int rows = mapH < WORLD_MASK_H ? mapH : WORLD_MASK_H;
  const int cols = SW < WORLD_MASK_W ? SW : WORLD_MASK_W;

  for (int x = 0; x < cols; x++) {
    double lonDeg = (x / (double)SW) * 360.0 - 180.0;
    double dlon = (lonDeg - lon0Deg) * PI / 180.0;
    double latRad = -atan2(cos(lat0) * cos(dlon), sin(lat0));
    int yBoundary = mapTop + (int)((90.0 - latRad * 180.0 / PI) / 180.0 * mapH);
    if (yBoundary < mapTop) yBoundary = mapTop;
    if (yBoundary > mapBot) yBoundary = mapBot;
    int rBoundary = yBoundary - mapTop;
    uint16_t runColor = 0;
    int runStartR = 0;
    for (int r = 0; r < rows; r++) {
      bool topHalf = r < rBoundary;
      bool day = topHalf ? northLit : !northLit;
      bool land = worldIsLand(x, r);
      uint16_t col = day ? (land ? dayLand : daySea) : (land ? nightLand : nightSea);
      if (r == 0) {
        runColor = col;
      } else if (col != runColor) {
        cv.drawFastVLine(x, mapTop + runStartR, r - runStartR, runColor);
        runStartR = r;
        runColor = col;
      }
      if (r == rows - 1) {
        cv.drawFastVLine(x, mapTop + runStartR, rows - runStartR, runColor);
      }
    }
  }

  // 赤道与本初子午线微弱标尺
  for (int x = 0; x < SW; x += 4) cv.drawPixel(x, mapTop + mapH / 2, 0x2945);
  for (int y = mapTop; y < mapBot; y += 4) cv.drawPixel(SW / 2, y, 0x2104);

  // 太阳直射点（天顶高光金星）
  int sx = (int)((lon0Deg + 180.0) / 360.0 * cols);
  int sy = mapTop + (int)((90.0 - lat0 * 180.0 / PI) / 180.0 * mapH);
  if (sx >= 0 && sx < cols && sy >= mapTop && sy < mapBot) {
    cv.drawCircle(sx, sy, 4, 0x8BE0);
    cv.fillCircle(sx, sy, 2, 0xFDE0);
    cv.drawPixel(sx, sy, TFT_WHITE);
  }

  // 用户当前位置高亮标点（YOU 目标准星）
  if (havePos) {
    int ux = (int)((aLon + 180.0) / 360.0 * cols);
    int uy = mapTop + (int)((90.0 - aLat) / 180.0 * mapH);
    if (ux >= 0 && ux < cols && uy >= mapTop && uy < mapBot) {
      cv.drawCircle(ux, uy, 4, 0x07FF);
      cv.fillCircle(ux, uy, 2, TFT_RED);
      cv.drawPixel(ux, uy, TFT_WHITE);

      int lx = (ux > cols - 32) ? (ux - 24) : (ux + 6);
      int ly = uy - 4;
      if (ly < mapTop + 2) ly = mapTop + 2;
      if (ly > mapBot - 10) ly = mapBot - 10;
      cv.fillRoundRect(lx, ly, 18, 8, 2, 0x0821);
      cv.drawRoundRect(lx, ly, 18, 8, 2, 0x07FF);
      cv.setTextDatum(middle_center); cv.setTextSize(1);
      cv.setTextColor(0x07FF, 0x0821);
      cv.drawString("YOU", lx + 9, ly + 4);
    }
  }

  // 底部两翼数据标尺与中心页码点 (y = 124..134)
  char buf[32];
  cv.setTextSize(1);

  // 左翼：太阳赤纬
  cv.setTextDatum(bottom_left);
  snprintf(buf, sizeof(buf), "DECL %+.1f*", lat0 * 180.0 / PI);
  cv.setTextColor(0x7BEF, TFT_BLACK);
  cv.drawString(buf, 4, SH - 2);

  // 右翼：天顶经纬度
  cv.setTextDatum(bottom_right);
  snprintf(buf, sizeof(buf), "ZENITH %.0f*%c,%.0f*%c",
           fabs(lat0 * 180.0 / PI), lat0 >= 0 ? 'N' : 'S',
           fabs(lon0Deg), lon0Deg >= 0 ? 'E' : 'W');
  cv.setTextColor(0xFDA0, TFT_BLACK);
  cv.drawString(buf, SW - 4, SH - 2);

  drawPageDots();
}

// tools/astrotest 的出口：static 函数在本翻译单元里转一手，固件编译时不存在。
#ifdef HOST_TEST
float hostSolarElev(time_t t, double lat, double lon) { return solarElev(t, lat, lon); }
int   hostSunCross(time_t midnight, double lat, double lon, int dir) { return sunCross(midnight, lat, lon, dir); }
void  hostSubsolarPointAt(time_t t, double& lat0, double& lon0Deg) { subsolarPointAt(t, lat0, lon0Deg); }
#endif
