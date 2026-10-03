// M5Unified 替身——calctest 专用。
// calc.cpp 和 conv.cpp 通过 globals.h 间接引入 M5Unified.h，
// 但它们只用到画布绘制接口（主机端忽略所有绘制调用）。
// 这里把 TFT 颜色常量和 M5Canvas、SimM5 定义出来让代码通过编译。
#pragma once
#include "Arduino.h"

// ---- 文字对齐枚举（drawCalc/drawConv 里用到，测试台不绘制但编译必须认识它） ----
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

// ---- TFT 颜色常量（RGB565） ----
static const uint16_t TFT_BLACK    = 0x0000;
static const uint16_t TFT_WHITE    = 0xFFFF;
static const uint16_t TFT_RED      = 0xF800;
static const uint16_t TFT_GREEN    = 0x07E0;
static const uint16_t TFT_BLUE     = 0x001F;
static const uint16_t TFT_YELLOW   = 0xFFE0;
static const uint16_t TFT_CYAN     = 0x07FF;
static const uint16_t TFT_DARKGRAY = 0x7BEF;
static const uint16_t TFT_DARKGREY = 0x7BEF;
static const uint16_t TFT_LIGHTGREY= 0xC618;

// ---- 空画布替身：所有绘制操作都是 no-op ----
struct M5Canvas {
  M5Canvas(void* = nullptr) {}
  void fillScreen(uint16_t) {}
  void fillRect(int, int, int, int, uint16_t) {}
  void fillRoundRect(int, int, int, int, int, uint16_t) {}
  void drawRoundRect(int, int, int, int, int, uint16_t) {}
  void drawFastHLine(int, int, int, uint16_t) {}
  void fillTriangle(int, int, int, int, int, int, uint16_t) {}
  void drawString(const char*, int, int) {}
  void drawString(const String&, int, int) {}
  void setTextDatum(uint8_t) {}
  void setTextColor(uint16_t, uint16_t = 0) {}
  void setTextSize(float, float = 0.0f) {}
  int32_t textWidth(const char*) { return 6; }
  int32_t textWidth(const String&) { return 6; }
  int32_t fontHeight() { return 8; }
  void pushSprite(int, int) {}
  void deleteSprite() {}
  void* createSprite(int, int) { return nullptr; }
  int width()  const { return 240; }
  int height() const { return 135; }
  uint16_t color565(uint8_t, uint8_t, uint8_t) { return 0; }
};

struct SimI2C {
  bool scanID(uint8_t) { return true; }
};
struct SimDisplay {
  int width()  { return 240; }
  int height() { return 135; }
  void setBrightness(uint8_t) {}
};
struct SimPower {
  int getBatteryLevel() { return 80; }
  bool isCharging() { return false; }
};
struct SimBtn {
  bool isPressed()   { return false; }
  bool wasPressed()  { return false; }
  bool wasReleased() { return false; }
};
struct SimSpeaker {
  void tone(uint16_t, int) {}
  void stop() {}
  void setVolume(uint8_t) {}
};

struct SimM5 {
  SimDisplay Display;
  SimPower   Power;
  SimBtn     BtnA, BtnB, BtnC;
  SimSpeaker Speaker;
  SimI2C     In_I2C;
  void update() {}
  void begin()  {}
};

extern SimM5  M5;
extern M5Canvas cv;
