#include "Arduino.h"
#include "M5Unified.h"
#include "clock.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cassert>

// ---- 替身全局状态 ----
uint32_t g_fakeNowMs = 0;
SimSerial Serial;
SimM5 M5;
M5Canvas cv;
Preferences prefs;

bool dirty = false;
bool screenOff = false;
int SW = 240, SH = 135;
uint16_t ACCENT = 0x07E0, CARD_BG = 0x18C3, DIM_BORDER = 0x2965, ICON_DIM = 0x4A69;

void drawPageDots() {}
bool nowHM(int& h, int& m, int& s) { h = 12; m = 0; s = 0; return true; }
int powerBatteryLevel() { return 80; }
void drawBattery(int, int) {}
int loadInt(const char*, const char*, int def) { return def; }
void saveInt(const char*, const char*, int) {}

bool timeSynced = false;
bool timeFromGps = false;
extern const uint8_t WORLD_MASK[3270] = {0};
const char* TZ_INFO = "UTC0";

struct AstroInfo;
void astroCalc(double, double, time_t, AstroInfo&) {}
struct MoonInfo;
void moonPhase(time_t, MoonInfo&) {}

// ---- 测试框架 ----
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* name, const char* detail = "") {
  if (cond) {
    g_pass++;
    printf("  [PASS] %s\n", name);
  } else {
    g_fail++;
    printf("  [FAIL] %s %s\n", name, detail);
  }
}

// 辅助打印当前选取的词组合
static void formatWords(const QlockWords& w, char* out, size_t sz) {
  static const char* H_NAMES[] = {
    "", "ONE", "TWO", "THREE", "FOUR", "FIVE",
    "SIX", "SEVEN", "EIGHT", "NINE", "TEN", "ELEVEN", "TWELVE"
  };
  std::string s;
  if (w.it_is) s += "IT IS ";
  if (w.half) s += "HALF ";
  if (w.quarter) s += "QUARTER ";
  if (w.twenty) s += "TWENTY ";
  if (w.five_min) s += "FIVE ";
  if (w.ten_min) s += "TEN ";
  if (w.past) s += "PAST ";
  if (w.to) s += "TO ";
  if (w.hour >= 1 && w.hour <= 12) {
    s += H_NAMES[w.hour];
    s += " ";
  }
  if (w.oclock) s += "O'CLOCK ";
  if (w.am) s += "AM";
  if (w.pm) s += "PM";
  snprintf(out, sz, "%s", s.c_str());
}

// =================================================================
// 1. 关键边界时间测试 (11:58, 23:58, 12:30, 0:03 等)
// =================================================================
void testKeyBoundaries() {
  printf("--- 1. 关键边界时间高亮取词测试 ---\n");
  char buf[128];

  // 1.1 11:58: 接近中午，四舍五入到 12:00 PM
  // "IT IS TWELVE O'CLOCK PM"
  {
    QlockWords w = qlockCalcWords(11, 58, 0);
    formatWords(w, buf, sizeof(buf));
    check(w.hour == 12 && w.oclock && w.pm && !w.am && !w.past && !w.to,
          "11:58 -> IT IS TWELVE O'CLOCK PM (正午)", buf);
  }

  // 1.2 23:58: 接近午夜，四舍五入到 12:00 AM
  // "IT IS TWELVE O'CLOCK AM"
  {
    QlockWords w = qlockCalcWords(23, 58, 0);
    formatWords(w, buf, sizeof(buf));
    check(w.hour == 12 && w.oclock && w.am && !w.pm && !w.past && !w.to,
          "23:58 -> IT IS TWELVE O'CLOCK AM (午夜)", buf);
  }

  // 1.3 12:30: 中午 12 点半，HALF PAST TWELVE PM
  // "IT IS HALF PAST TWELVE PM"
  {
    QlockWords w = qlockCalcWords(12, 30, 0);
    formatWords(w, buf, sizeof(buf));
    check(w.hour == 12 && w.half && w.past && w.pm && !w.am && !w.to && !w.oclock,
          "12:30 -> IT IS HALF PAST TWELVE PM", buf);
  }

  // 1.4 00:03: 午夜刚过 3 分钟，四舍五入到 5 分钟 (FIVE PAST TWELVE AM)
  // "IT IS FIVE PAST TWELVE AM"
  {
    QlockWords w = qlockCalcWords(0, 3, 0);
    formatWords(w, buf, sizeof(buf));
    check(w.hour == 12 && w.five_min && w.past && w.am && !w.pm && !w.to && !w.oclock,
          "00:03 -> IT IS FIVE PAST TWELVE AM", buf);
  }

  // 1.5 11:40: 上午 11:40，差 20 分到 12 点 (TWENTY TO TWELVE PM)
  // "IT IS TWENTY TO TWELVE PM"
  {
    QlockWords w = qlockCalcWords(11, 40, 0);
    formatWords(w, buf, sizeof(buf));
    check(w.hour == 12 && w.twenty && w.to && w.pm && !w.am && !w.past && !w.oclock,
          "11:40 -> IT IS TWENTY TO TWELVE PM", buf);
  }

  // 1.6 23:40: 晚上 23:40，差 20 分到午夜 (TWENTY TO TWELVE AM)
  // "IT IS TWENTY TO TWELVE AM"
  {
    QlockWords w = qlockCalcWords(23, 40, 0);
    formatWords(w, buf, sizeof(buf));
    check(w.hour == 12 && w.twenty && w.to && w.am && !w.pm && !w.past && !w.oclock,
          "23:40 -> IT IS TWENTY TO TWELVE AM", buf);
  }

  // 1.7 00:00: 午夜整点
  {
    QlockWords w = qlockCalcWords(0, 0, 0);
    formatWords(w, buf, sizeof(buf));
    check(w.hour == 12 && w.oclock && w.am && !w.pm,
          "00:00 -> IT IS TWELVE O'CLOCK AM", buf);
  }

  // 1.8 12:00: 正午整点
  {
    QlockWords w = qlockCalcWords(12, 0, 0);
    formatWords(w, buf, sizeof(buf));
    check(w.hour == 12 && w.oclock && w.pm && !w.am,
          "12:00 -> IT IS TWELVE O'CLOCK PM", buf);
  }

  // 1.9 12:45: 差一刻到 1 点 (QUARTER TO ONE PM)
  {
    QlockWords w = qlockCalcWords(12, 45, 0);
    formatWords(w, buf, sizeof(buf));
    check(w.hour == 1 && w.quarter && w.to && w.pm && !w.am,
          "12:45 -> IT IS QUARTER TO ONE PM", buf);
  }

  // 1.10 00:45: 差一刻到凌晨 1 点 (QUARTER TO ONE AM)
  {
    QlockWords w = qlockCalcWords(0, 45, 0);
    formatWords(w, buf, sizeof(buf));
    check(w.hour == 1 && w.quarter && w.to && w.am && !w.pm,
          "00:45 -> IT IS QUARTER TO ONE AM", buf);
  }
}

// =================================================================
// 2. 12 档 5 分钟取整词形结构验证
// =================================================================
void testMinuteSlots() {
  printf("--- 2. 12 档 5 分钟词组合与修饰词测试 ---\n");

  // 以 8 点为例测试 12 档
  int baseH = 8; // 8:xx AM
  struct SlotExpect {
    int m;
    bool half;
    bool quarter;
    bool twenty;
    bool five_min;
    bool ten_min;
    bool past;
    bool to;
    bool oclock;
    int  targetH;
  } expectedSlots[12] = {
    { 0,  false, false, false, false, false, false, false, true,  8 }, // O'CLOCK
    { 5,  false, false, false, true,  false, true,  false, false, 8 }, // FIVE PAST
    { 10, false, false, false, false, true,  true,  false, false, 8 }, // TEN PAST
    { 15, false, true,  false, false, false, true,  false, false, 8 }, // QUARTER PAST
    { 20, false, false, true,  false, false, true,  false, false, 8 }, // TWENTY PAST
    { 25, false, false, true,  true,  false, true,  false, false, 8 }, // TWENTY FIVE PAST
    { 30, true,  false, false, false, false, true,  false, false, 8 }, // HALF PAST
    { 35, false, false, true,  true,  false, false, true,  false, 9 }, // TWENTY FIVE TO
    { 40, false, false, true,  false, false, false, true,  false, 9 }, // TWENTY TO
    { 45, false, true,  false, false, false, false, true,  false, 9 }, // QUARTER TO
    { 50, false, false, false, false, true,  false, true,  false, 9 }, // TEN TO
    { 55, false, false, false, true,  false, false, true,  false, 9 }, // FIVE TO
  };

  for (int i = 0; i < 12; i++) {
    const auto& exp = expectedSlots[i];
    QlockWords w = qlockCalcWords(baseH, exp.m, 0);

    bool ok = (w.half == exp.half) &&
              (w.quarter == exp.quarter) &&
              (w.twenty == exp.twenty) &&
              (w.five_min == exp.five_min) &&
              (w.ten_min == exp.ten_min) &&
              (w.past == exp.past) &&
              (w.to == exp.to) &&
              (w.oclock == exp.oclock) &&
              (w.hour == exp.targetH);

    char name[64];
    snprintf(name, sizeof(name), "Slot :%02d 词组合与目标时钟", exp.m);
    char buf[128];
    formatWords(w, buf, sizeof(buf));
    check(ok, name, buf);
  }
}

// =================================================================
// 3. 24 小时 x 60 分钟 (1440 分钟) 遍历全量不变量测试
// =================================================================
void testExhaustive24Hours() {
  printf("--- 3. 24 小时 x 60 分钟 (共 1440 组) 全量遍历不变量测试 ---\n");

  int validCount = 0;
  bool allOk = true;

  for (int h = 0; h < 24; h++) {
    for (int m = 0; m < 60; m++) {
      QlockWords w = qlockCalcWords(h, m, 0);

      // 不变量 1: IT IS 必须为 true
      if (!w.it_is) { allOk = false; break; }

      // 不变量 2: 目标小时必须在 1..12 之间
      if (w.hour < 1 || w.hour > 12) { allOk = false; break; }

      // 不变量 3: AM 与 PM 互斥且有且仅有一个为 true
      if (w.am == w.pm) { allOk = false; break; }

      // 不变量 4: O'CLOCK / PAST / TO 互斥且有且仅有一个为 true
      int relationCount = (w.oclock ? 1 : 0) + (w.past ? 1 : 0) + (w.to ? 1 : 0);
      if (relationCount != 1) { allOk = false; break; }

      // 不变量 5: roundM == 0 必须是 oclock
      if (w.roundM == 0 && !w.oclock) { allOk = false; break; }

      // 不变量 6: roundM in [5, 30] 必须是 past
      if (w.roundM > 0 && w.roundM <= 30 && !w.past) { allOk = false; break; }

      // 不变量 7: roundM in [35, 55] 必须是 to
      if (w.roundM >= 35 && !w.to) { allOk = false; break; }

      // 不变量 8: 分钟修饰词唯一性
      if (w.roundM == 0) {
        if (w.half || w.quarter || w.twenty || w.five_min || w.ten_min) { allOk = false; break; }
      } else if (w.roundM == 30) {
        if (!w.half || w.quarter || w.twenty || w.five_min || w.ten_min) { allOk = false; break; }
      } else if (w.roundM == 15 || w.roundM == 45) {
        if (!w.quarter || w.half || w.twenty || w.five_min || w.ten_min) { allOk = false; break; }
      }

      validCount++;
    }
  }

  check(allOk && validCount == 1440, "24 小时 1440 分钟无缝遍历，全部满足状态机不变量");
}

int main() {
  printf("==========================================\n");
  printf("  Cardputer ADV: Clock QlockTwo Host Tests\n");
  printf("==========================================\n");

  testKeyBoundaries();
  testMinuteSlots();
  testExhaustive24Hours();

  printf("==========================================\n");
  printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
  printf("==========================================\n");

  return g_fail > 0 ? 1 : 0;
}
