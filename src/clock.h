#pragma once
#include "globals.h"

enum ClockStyle {
  CLOCK_STYLE_HIERARCHIC = 0, // 层次进度条 (默认)
  CLOCK_STYLE_ANALOG     = 1, // 指针表盘 (经典模拟表)
  CLOCK_STYLE_DIGITAL    = 2, // 大字数码表盘 (7段数码管)
  CLOCK_STYLE_TEXT       = 3, // 极简文字表盘 (QlockTwo 风格)
  CLOCK_STYLE_COUNT      = 4
};

extern ClockStyle currentClockStyle;

struct QlockWords {
  bool it_is;
  bool half;
  bool ten_min;
  bool quarter;
  bool twenty;
  bool five_min;
  bool past;
  bool to;
  int  hour;     // 1..12
  bool oclock;
  bool am;
  bool pm;
  int  roundM;   // 0, 5, 10, ... 55
};

QlockWords qlockCalcWords(int h, int m, int s = 0);

void clockInit();
void clockCycleStyle();
void clockKey(char k);
bool clockNeedsFastUpdate();
void drawClock();
void drawClockStyle(ClockStyle style);
