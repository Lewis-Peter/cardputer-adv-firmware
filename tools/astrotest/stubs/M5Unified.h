#pragma once
#include "Arduino.h"

// M5Unified 极简打桩：只提供 astro.cpp/moon.cpp 的绘制调用（全部空实现）。
// 因为测试台只测数值计算，绘制路径完全不走。

enum textdatum_t {
  TL_DATUM = 0, top_left = 0,
  TC_DATUM = 1, top_center = 1,
  TR_DATUM = 2, top_right = 2,
  ML_DATUM = 3, middle_left = 3,
  MC_DATUM = 4, middle_center = 4,
  MR_DATUM = 5, middle_right = 5,
  BL_DATUM = 6, bottom_left = 6,
  BC_DATUM = 7, bottom_center = 7,
  BR_DATUM = 8, bottom_right = 8
};

static const uint16_t TFT_BLACK    = 0x0000;
static const uint16_t TFT_WHITE    = 0xFFFF;
static const uint16_t TFT_RED      = 0xF800;
static const uint16_t TFT_GREEN    = 0x07E0;
static const uint16_t TFT_DARKGREY = 0x7BEF;

struct M5Canvas {
  M5Canvas(void* = nullptr) {}
  void fillScreen(uint16_t) {}
  void fillRect(int, int, int, int, uint16_t) {}
  void fillRoundRect(int, int, int, int, int, uint16_t) {}
  void drawRoundRect(int, int, int, int, int, uint16_t) {}
  void drawCircle(int, int, int, uint16_t) {}
  void fillCircle(int, int, int, uint16_t) {}
  void drawLine(int, int, int, int, uint16_t) {}
  void drawFastHLine(int, int, int, uint16_t) {}
  void drawFastVLine(int, int, int, uint16_t) {}
  void drawPixel(int, int, uint16_t) {}
  void drawRect(int, int, int, int, uint16_t) {}
  void drawString(const char*, int, int) {}
  void drawString(const String&, int, int) {}
  void setTextDatum(uint8_t) {}
  void setTextColor(uint16_t, uint16_t = 0) {}
  void setTextSize(float, float = 0.0f) {}
  int32_t textWidth(const char*) { return 6; }
  int32_t fontHeight() { return 8; }
  int width()  const { return 240; }
  int height() const { return 135; }
  uint16_t color565(uint8_t, uint8_t, uint8_t) { return 0; }
};

struct SimM5 {
  void update() {}
};

extern SimM5 M5;
extern M5Canvas cv;
