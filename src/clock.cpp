#include "clock.h"
#include "ui_common.h"
#include "icons.h"
#include "power_util.h"
#include <ctime>
#include <cstdio>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923f
#endif

ClockStyle currentClockStyle = CLOCK_STYLE_HIERARCHIC;

void clockInit() {
  int s = loadInt("clock", "style", CLOCK_STYLE_HIERARCHIC);
  if (s < 0 || s >= CLOCK_STYLE_COUNT) s = CLOCK_STYLE_HIERARCHIC;
  currentClockStyle = (ClockStyle)s;
}

void clockCycleStyle() {
  currentClockStyle = (ClockStyle)(((int)currentClockStyle + 1) % CLOCK_STYLE_COUNT);
  saveInt("clock", "style", (int)currentClockStyle);
  dirty = true;
}

void clockKey(char k) {
  if (k == 'f' || k == 'F' || k == 's' || k == 'S') {
    clockCycleStyle();
  }
}

bool clockNeedsFastUpdate() {
  if (screenOff) return false;
  // 指针表盘秒针平滑扫秒：~12fps 就够顺，别每帧推 64KB 画布（时钟是开机默认页，待机常驻）
  if (currentClockStyle == CLOCK_STYLE_ANALOG) {
    static uint32_t lastFastMs = 0;
    if (millis() - lastFastMs >= 80) { lastFastMs = millis(); return true; }
    return false;
  }
  // 大字数码表盘冒号每 500ms 翻转闪烁
  if (currentClockStyle == CLOCK_STYLE_DIGITAL) {
    static uint32_t lastHalf = 0;
    uint32_t cur = millis() / 500;
    if (cur != lastHalf) {
      lastHalf = cur;
      return true;
    }
  }
  return false;
}

static const char* WEEKDAYS[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static int daysInMonth(int y, int m) {   // m: 1-12
  static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return d[m - 1];
}

// 一行「标签 + 细进度条 + 右侧数值」。时钟页用它把"今天/本月/今年过了多少"摊开
static void drawMiniBar(int y, const char* label, float frac, uint16_t col, const char* val) {
  const int x0 = 54, x1 = SW - 54, w = x1 - x0;
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  cv.setTextDatum(middle_left);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString(label, 15, y + 1);
  cv.setTextDatum(middle_right);
  cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  cv.drawString(val, SW - 15, y + 1);
  cv.fillRoundRect(x0, y, w, 3, 1, DIM_BORDER);
  int fw = (int)(w * frac);
  if (fw < 2 && frac > 0) fw = 2;
  if (fw > 0) {
    cv.fillRoundRect(x0, y, fw, 3, 1, col);
    cv.fillCircle(x0 + fw, y + 1, 2, col);
  }
}

// 沿矩形边框走一圈，f∈[0,1) 从上边中点起、顺时针，返回边框上的点 (x,y) 和朝内的法线方向 (nx,ny)。
static void rectRingPoint(float f, int left, int top, int right, int bottom, int& x, int& y, int& nx, int& ny) {
  int w = right - left, h = bottom - top;
  float d = f * (2.0f * (w + h)), halfW = w / 2.0f;
  if (d < halfW)            { x = left + w / 2 + (int)d;      y = top;                nx = 0;  ny = 1;  return; }
  d -= halfW;
  if (d < h)                 { x = right;                      y = top + (int)d;       nx = -1; ny = 0;  return; }
  d -= h;
  if (d < w)                 { x = right - (int)d;              y = bottom;             nx = 0;  ny = -1; return; }
  d -= w;
  if (d < h)                 { x = left;                        y = bottom - (int)d;     nx = 1;  ny = 0;  return; }
  d -= h;
  x = left + (int)d; y = top; nx = 0; ny = 1;
}

// 秒针环：贴着整块屏幕边缘的矩形轨道
static void drawSecondRing(int s) {
  const int left = 3, top = 3, right = SW - 3, bottom = SH - 3;
  for (int i = 0; i < 60; i++) {
    int x, y, nx, ny;
    rectRingPoint(i / 60.0f, left, top, right, bottom, x, y, nx, ny);
    // 避开底部居中的页码点区域（SH-4, x=SW/2 附近），防止刻度线穿过页码指示器
    if (y == bottom && x > SW / 2 - 14 && x < SW / 2 + 14) continue;
    bool cur = (i == s);
    int len = cur ? 9 : 3;
    int x1 = x + nx * len, y1 = y + ny * len;
    if (cur) {
      int tx = -ny, ty = nx;
      cv.drawLine(x, y, x1, y1, ACCENT);
      cv.drawLine(x + tx, y + ty, x1 + tx, y1 + ty, ACCENT);
    } else {
      cv.drawLine(x, y, x1, y1, DIM_BORDER);
    }
  }
}

// 时间主体：单一固定的层次化现代极客版（大时分 + 右上角微缩秒数 + SEC 标签）
static void drawTimeBody(int h, int m, int s) {
  char hm[8]; snprintf(hm, sizeof(hm), "%02d:%02d", h, m);
  char ss[4]; snprintf(ss, sizeof(ss), "%02d", s);

  // 时分居中偏左
  cv.setFont(&fonts::Font7);
  cv.setTextColor(ACCENT, TFT_BLACK);
  cv.setTextDatum(middle_right);
  cv.setTextSize(0.76, 0.82);
  cv.drawString(hm, SW / 2 + 28, 48);

  // 秒数放在右侧偏上（平滑无衬线数字，与大数字形成层次反差）
  cv.setFont(&fonts::Font4);
  cv.setTextColor(cv.color565(0, 255, 180), TFT_BLACK);
  cv.setTextDatum(top_left);
  cv.setTextSize(0.9, 0.9);
  cv.drawString(ss, SW / 2 + 34, 32);

  // 秒数下方 SEC 标识
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("SEC", SW / 2 + 35, 52);
}

// 表盘 1：现有表盘（默认，Apple Watch Ultra 风格层次进度条）
static void drawStyleHierarchic() {
  cv.fillScreen(TFT_BLACK);
  int h, m, s; nowHM(h, m, s);
  struct tm ti;
  bool haveDate = timeSynced && getLocalTime(&ti, 0);

  drawSecondRing(s);

  // 顶部：已同步显示完整年月日与星期；未同步显示同步提示
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  cv.setTextDatum(top_center);
  if (haveDate) {
    char d[32];
    snprintf(d, sizeof(d), "%04d-%02d-%02d %s",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, WEEKDAYS[ti.tm_wday % 7]);
    cv.setTextColor(TFT_CYAN, TFT_BLACK);
    cv.drawString(d, SW / 2, 12);
  } else {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("WAITING FOR SYNC (NTP/GNSS)", SW / 2, 12);
  }

  // 时间主体
  drawTimeBody(h, m, s);

  // 下半部分生活进度条：排版结构始终固定一致
  if (haveDate) {
    int year = ti.tm_year + 1900;
    char v[12];
    float df = (ti.tm_hour * 60 + ti.tm_min) / 1440.0f;
    snprintf(v, sizeof(v), "%d%%", (int)(df * 100));
    drawMiniBar(82, "DAY", df, ACCENT, v);

    int dim = daysInMonth(year, ti.tm_mon + 1);
    snprintf(v, sizeof(v), "%d/%d", ti.tm_mday, dim);
    drawMiniBar(99, "MONTH", (float)ti.tm_mday / dim, cv.color565(90, 170, 230), v);

    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    float yf = (ti.tm_yday + 1) / (leap ? 366.0f : 365.0f);
    snprintf(v, sizeof(v), "%d%%", (int)(yf * 100));
    drawMiniBar(116, "YEAR", yf, cv.color565(150, 120, 230), v);
  } else {
    drawMiniBar(82, "DAY",   0.0f, DIM_BORDER, "--%");
    drawMiniBar(99, "MONTH", 0.0f, DIM_BORDER, "--/--");
    drawMiniBar(116, "YEAR", 0.0f, DIM_BORDER, "--%");
  }

  drawPageDots();
}

// 表盘 2：经典模拟指针表盘（12刻度，3/6/9/12 加粗，毫秒插值秒针平滑扫秒）
static void drawStyleAnalog() {
  cv.fillScreen(TFT_BLACK);
  int h, m, s;
  bool synced = nowHM(h, m, s);
  struct tm ti{};
  bool haveDate = synced && getLocalTime(&ti, 0);

  const int cx = SW / 2;
  const int cy = SH / 2;
  const int r = (SH - 16) / 2;

  // 外圈表盘底环
  cv.drawCircle(cx, cy, r, DIM_BORDER);

  // 12 个刻度（3/6/9/12 加粗）
  for (int i = 0; i < 12; i++) {
    float ang = i * (2.0f * (float)M_PI / 12.0f) - (float)M_PI_2;
    float cosA = cosf(ang), sinA = sinf(ang);
    bool major = (i % 3 == 0);
    int rIn = major ? (r - 10) : (r - 5);
    int rOut = r - 2;

    int x0 = cx + (int)(cosA * rIn);
    int y0 = cy + (int)(sinA * rIn);
    int x1 = cx + (int)(cosA * rOut);
    int y1 = cy + (int)(sinA * rOut);

    if (major) {
      float nx = -sinA, ny = cosA;
      cv.drawLine(x0, y0, x1, y1, ACCENT);
      cv.drawLine(x0 + (int)roundf(nx), y0 + (int)roundf(ny),
                  x1 + (int)roundf(nx), y1 + (int)roundf(ny), ACCENT);
    } else {
      cv.drawLine(x0, y0, x1, y1, DIM_BORDER);
    }
  }

  // 秒针平滑走动：从"秒数刚变化"那一刻起按毫秒插值。millis() % 1000 跟真实秒的边界不对齐，
  // 秒针会在每个整秒处往回/往前跳一下
  static int lastSec = -1;
  static uint32_t secStartMs = 0;
  if (s != lastSec) { lastSec = s; secStartMs = millis(); }
  uint32_t ms = millis() - secStartMs;
  if (ms > 999) ms = 999;
  float secF = s + ms / 1000.0f;
  float minF = m + secF / 60.0f;
  float hourF = (h % 12) + minF / 60.0f;

  float angH = hourF * (2.0f * (float)M_PI / 12.0f) - (float)M_PI_2;
  float angM = minF * (2.0f * (float)M_PI / 60.0f) - (float)M_PI_2;
  float angS = secF * (2.0f * (float)M_PI / 60.0f) - (float)M_PI_2;

  // 时针（短而粗）
  int rH = (int)(r * 0.50f);
  float cosH = cosf(angH), sinH = sinf(angH);
  int xH = cx + (int)(cosH * rH), yH = cy + (int)(sinH * rH);
  float nxH = -sinH, nyH = cosH;
  cv.drawLine(cx, cy, xH, yH, TFT_WHITE);
  cv.drawLine(cx + (int)roundf(nxH), cy + (int)roundf(nyH),
              xH + (int)roundf(nxH), yH + (int)roundf(nyH), TFT_WHITE);
  cv.drawLine(cx - (int)roundf(nxH), cy - (int)roundf(nyH),
              xH - (int)roundf(nxH), yH - (int)roundf(nyH), TFT_WHITE);

  // 分针（较长稍细）
  int rM = (int)(r * 0.76f);
  float cosM = cosf(angM), sinM = sinf(angM);
  int xM = cx + (int)(cosM * rM), yM = cy + (int)(sinM * rM);
  float nxM = -sinM, nyM = cosM;
  cv.drawLine(cx, cy, xM, yM, cv.color565(180, 220, 255));
  cv.drawLine(cx + (int)roundf(nxM), cy + (int)roundf(nyM),
              xM + (int)roundf(nxM), yM + (int)roundf(nyM), cv.color565(180, 220, 255));

  // 秒针（细长带尾巴，平滑扫秒）
  int rS = (int)(r * 0.88f);
  int rTail = 10;
  float cosS = cosf(angS), sinS = sinf(angS);
  int xS = cx + (int)(cosS * rS), yS = cy + (int)(sinS * rS);
  int xTail = cx - (int)(cosS * rTail), yTail = cy - (int)(sinS * rTail);
  cv.drawLine(xTail, yTail, xS, yS, ACCENT);

  // 中心轴盖
  cv.fillCircle(cx, cy, 3, ACCENT);
  cv.fillCircle(cx, cy, 1, TFT_BLACK);

  // 角落小字显示日期和星期
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  if (haveDate) {
    char dBuf[16];
    snprintf(dBuf, sizeof(dBuf), "%02d-%02d", ti.tm_mon + 1, ti.tm_mday);
    cv.setTextDatum(top_left);
    cv.setTextColor(TFT_CYAN, TFT_BLACK);
    cv.drawString(dBuf, 12, 10);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString(WEEKDAYS[ti.tm_wday % 7], 12, 21);
  } else {
    cv.setTextDatum(top_left);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("--/--", 12, 10);
    cv.drawString("---", 12, 21);
  }

  // 右上角数字时钟读数备用辅助
  char digBuf[16];
  snprintf(digBuf, sizeof(digBuf), "%02d:%02d", h, m);
  cv.setTextDatum(top_right);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString(digBuf, SW - 12, 10);

  drawPageDots();
}

// 表盘 3：大字数码表盘（Font7 七段数码管铺满，冒号闪烁，底部日期/星期/电量）
static void drawStyleDigital() {
  cv.fillScreen(TFT_BLACK);
  int h, m, s;
  bool synced = nowHM(h, m, s);
  struct tm ti{};
  bool haveDate = synced && getLocalTime(&ti, 0);

  char hh[4], mm[4], ss[4];
  snprintf(hh, sizeof(hh), "%02d", h);
  snprintf(mm, sizeof(mm), "%02d", m);
  snprintf(ss, sizeof(ss), "%02d", s);

  const int cy = 52;
  const int colonX = SW / 2 - 14;

  // 大号 HH:MM
  cv.setFont(&fonts::Font7);
  cv.setTextSize(1.22, 1.30);

  cv.setTextDatum(middle_right);
  cv.setTextColor(ACCENT, TFT_BLACK);
  cv.drawString(hh, colonX - 8, cy);

  cv.setTextDatum(middle_left);
  cv.drawString(mm, colonX + 8, cy);

  // 冒号每秒闪烁：亮时为 ACCENT，灭时为暗色 0x0821（数码管未点亮发光体质感）
  bool colonLit = (millis() / 500) % 2 == 0;
  uint16_t colonCol = colonLit ? ACCENT : 0x0821;
  cv.fillCircle(colonX, cy - 12, 3, colonCol);
  cv.fillCircle(colonX, cy + 12, 3, colonCol);

  // 秒数小字（位于分钟右侧）
  cv.setFont(&fonts::Font4);
  cv.setTextSize(0.85, 0.85);
  cv.setTextDatum(top_left);
  cv.setTextColor(cv.color565(0, 255, 180), TFT_BLACK);
  cv.drawString(ss, SW - 36, cy - 18);

  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  cv.setTextDatum(top_left);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("SEC", SW - 36, cy + 6);

  // 分隔线
  cv.drawFastHLine(12, 98, SW - 24, DIM_BORDER);

  // 底部一行：日期、星期和电池电量
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  cv.setTextDatum(middle_left);
  if (haveDate) {
    char dBuf[32];
    snprintf(dBuf, sizeof(dBuf), "%04d-%02d-%02d %s",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, WEEKDAYS[ti.tm_wday % 7]);
    cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    cv.drawString(dBuf, 14, 114);
  } else {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("----/--/--  NO SYNC", 14, 114);
  }

  // 电池电量与图标
  int lvl = powerBatteryLevel();
  char batBuf[16];
  if (lvl >= 0) snprintf(batBuf, sizeof(batBuf), "%d%%", lvl);
  else strcpy(batBuf, "--%");
  cv.setTextDatum(middle_right);
  uint16_t batCol = (lvl <= 20) ? TFT_RED : (lvl <= 50 ? TFT_YELLOW : TFT_GREEN);
  cv.setTextColor(batCol, TFT_BLACK);
  cv.drawString(batBuf, SW - 38, 114);
  drawBattery(SW - 34, 108);

  drawPageDots();
}

QlockWords qlockCalcWords(int h, int m, int s) {
  // 按 5 分钟取整（四舍五入到最近的 5 分钟）
  int roundM = ((m * 60 + s + 150) / 300) * 5;
  int roundH = h % 12;
  if (roundH == 0) roundH = 12;
  int h24 = h;   // 取整后的 24 小时制钟点，AM/PM 要跟着它走，不能用原始 h

  if (roundM >= 60) {
    roundM = 0;
    roundH = (roundH % 12) + 1;
    h24 = (h24 + 1) % 24;
  }

  QlockWords w{};
  w.it_is = true;
  w.roundM = roundM;

  int targetH = roundH;

  if (roundM == 0) {
    targetH = roundH;
  } else if (roundM == 5) {
    w.five_min = true; w.past = true; targetH = roundH;
  } else if (roundM == 10) {
    w.ten_min = true; w.past = true; targetH = roundH;
  } else if (roundM == 15) {
    w.quarter = true; w.past = true; targetH = roundH;
  } else if (roundM == 20) {
    w.twenty = true; w.past = true; targetH = roundH;
  } else if (roundM == 25) {
    w.twenty = true; w.five_min = true; w.past = true; targetH = roundH;
  } else if (roundM == 30) {
    w.half = true; w.past = true; targetH = roundH;
  } else if (roundM == 35) {
    w.twenty = true; w.five_min = true; w.to = true; targetH = (roundH % 12) + 1;
  } else if (roundM == 40) {
    w.twenty = true; w.to = true; targetH = (roundH % 12) + 1;
  } else if (roundM == 45) {
    w.quarter = true; w.to = true; targetH = (roundH % 12) + 1;
  } else if (roundM == 50) {
    w.ten_min = true; w.to = true; targetH = (roundH % 12) + 1;
  } else if (roundM == 55) {
    w.five_min = true; w.to = true; targetH = (roundH % 12) + 1;
  }

  w.oclock = (roundM == 0);
  // 11:58 取整成 12:00 应该是 PM（中午），23:40 说成 "TWENTY TO TWELVE" 指的是午夜 AM
  int target24 = w.to ? (h24 + 1) % 24 : h24;
  w.am = (target24 < 12);
  w.pm = !w.am;
  w.hour = targetH;
  return w;
}

// 表盘 4：极简文字表盘（QlockTwo 风格矩阵，按 5 分钟取整高亮对应词）
static void drawStyleText() {
  cv.fillScreen(TFT_BLACK);
  int h, m, s;
  bool synced = nowHM(h, m, s);
  struct tm ti{};
  bool haveDate = synced && getLocalTime(&ti, 0);

  QlockWords qw = qlockCalcWords(h, m, s);
  bool hl_it_is = qw.it_is;
  bool hl_half = qw.half;
  bool hl_m_ten = qw.ten_min;
  bool hl_quarter = qw.quarter;
  bool hl_twenty = qw.twenty;
  bool hl_m_five = qw.five_min;
  bool hl_past = qw.past;
  bool hl_to = qw.to;
  bool hl_oclock = qw.oclock;
  bool hl_am = qw.am;
  bool hl_pm = qw.pm;

  bool hl_hours[13] = {false};
  if (qw.hour >= 1 && qw.hour <= 12) hl_hours[qw.hour] = true;

  const uint16_t cActive = ACCENT;
  const uint16_t cDim    = cv.color565(36, 44, 52); // 暗色微光底色，QlockTwo 沉浸质感

  cv.setFont(&fonts::Font2);
  cv.setTextDatum(middle_center);

  const int rowPitch = (SH - 26) / 6;
  const int startY = 12;

  auto drawWord = [&](const char* word, int x, int y, bool active) {
    cv.setTextColor(active ? cActive : cDim, TFT_BLACK);
    cv.drawString(word, x, y);
  };

  // Row 0: IT IS, HALF, TEN, QUARTER
  int y = startY;
  drawWord("IT IS",    SW * 1 / 8, y, hl_it_is);
  drawWord("HALF",     SW * 3 / 8, y, hl_half);
  drawWord("TEN",      SW * 5 / 8, y, hl_m_ten);
  drawWord("QUARTER",  SW * 7 / 8, y, hl_quarter);

  // Row 1: TWENTY, FIVE, PAST, TO
  y += rowPitch;
  drawWord("TWENTY",   SW * 1 / 8, y, hl_twenty);
  drawWord("FIVE",     SW * 3 / 8, y, hl_m_five);
  drawWord("PAST",     SW * 5 / 8, y, hl_past);
  drawWord("TO",       SW * 7 / 8, y, hl_to);

  // Row 2: ONE, TWO, THREE, FOUR
  y += rowPitch;
  drawWord("ONE",      SW * 1 / 8, y, hl_hours[1]);
  drawWord("TWO",      SW * 3 / 8, y, hl_hours[2]);
  drawWord("THREE",    SW * 5 / 8, y, hl_hours[3]);
  drawWord("FOUR",     SW * 7 / 8, y, hl_hours[4]);

  // Row 3: FIVE, SIX, SEVEN, EIGHT
  y += rowPitch;
  drawWord("FIVE",     SW * 1 / 8, y, hl_hours[5]);
  drawWord("SIX",      SW * 3 / 8, y, hl_hours[6]);
  drawWord("SEVEN",    SW * 5 / 8, y, hl_hours[7]);
  drawWord("EIGHT",    SW * 7 / 8, y, hl_hours[8]);

  // Row 4: NINE, TEN, ELEVEN, TWELVE
  y += rowPitch;
  drawWord("NINE",     SW * 1 / 8, y, hl_hours[9]);
  drawWord("TEN",      SW * 3 / 8, y, hl_hours[10]);
  drawWord("ELEVEN",   SW * 5 / 8, y, hl_hours[11]);
  drawWord("TWELVE",   SW * 7 / 8, y, hl_hours[12]);

  // Row 5: AM, O'CLOCK, PM
  y += rowPitch;
  drawWord("AM",       SW * 1 / 4, y, hl_am);
  drawWord("O'CLOCK",  SW * 1 / 2, y, hl_oclock);
  drawWord("PM",       SW * 3 / 4, y, hl_pm);

  // 未对时提示
  if (!haveDate) {
    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextDatum(top_center);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    // 放在最后一行词和页码点之间：顶部会压到第一行（Font2 16px 以 y=12 居中）
    cv.drawString("WAITING FOR SYNC", SW / 2, SH - 22);
  }

  drawPageDots();
}

void drawClock() {
  drawClockStyle(currentClockStyle);
}

void drawClockStyle(ClockStyle style) {
  switch (style) {
    case CLOCK_STYLE_ANALOG:  drawStyleAnalog(); break;
    case CLOCK_STYLE_DIGITAL: drawStyleDigital(); break;
    case CLOCK_STYLE_TEXT:    drawStyleText(); break;
    case CLOCK_STYLE_HIERARCHIC:
    default:
      drawStyleHierarchic();
      break;
  }
}
