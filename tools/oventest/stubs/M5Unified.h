#pragma once
#include "Arduino.h"

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

namespace fonts {
  static const int Font0 = 0, Font2 = 2, Font4 = 4, Font7 = 7, Font8 = 8;
}

static const uint16_t TFT_BLACK = 0x0000;
static const uint16_t TFT_WHITE = 0xFFFF;
static const uint16_t TFT_RED = 0xF800;
static const uint16_t TFT_GREEN = 0x07E0;
static const uint16_t TFT_BLUE = 0x001F;
static const uint16_t TFT_YELLOW = 0xFFE0;
static const uint16_t TFT_CYAN = 0x07FF;
static const uint16_t TFT_DARKGRAY = 0x7BEF;
static const uint16_t TFT_DARKGREY = 0x7BEF;
static const uint16_t TFT_LIGHTGRAY = 0xC618;
static const uint16_t TFT_LIGHTGREY = 0xC618;
static const uint16_t TFT_NAVY = 0x000F;
static const uint16_t TFT_ORANGE = 0xFD20;
static const uint16_t TFT_PURPLE = 0x780F;
static const uint16_t TFT_DARKGREEN = 0x03E0;
static const uint16_t TFT_MAROON = 0x7800;
static const uint16_t TFT_OLIVE = 0x7BE0;
static const uint16_t TFT_MAGENTA = 0xF81F;

struct M5Canvas {
  M5Canvas(void* = nullptr) {}
  void fillScreen(uint16_t) {}
  void fillRect(int, int, int, int, uint16_t) {}
  void drawRect(int, int, int, int, uint16_t) {}
  void fillRoundRect(int, int, int, int, int, uint16_t) {}
  void drawRoundRect(int, int, int, int, int, uint16_t) {}
  void drawCircle(int, int, int, uint16_t) {}
  void fillCircle(int, int, int, uint16_t) {}
  void drawLine(int, int, int, int, uint16_t) {}
  void drawFastHLine(int, int, int, uint16_t) {}
  void drawFastVLine(int, int, int, uint16_t) {}
  void drawPixel(int, int, uint16_t) {}
  void drawString(const char*, int, int) {}
  void drawString(const String&, int, int) {}
  void setTextDatum(uint8_t) {}
  void setTextColor(uint16_t, uint16_t = 0) {}
  void setTextSize(float, float = 0.0f) {}
  void setFont(const void*) {}
  int32_t textWidth(const char*) { return 6; }
  int32_t textWidth(const String&) { return 6; }
  int32_t fontHeight() { return 8; }
  void pushSprite(int, int) {}
  void pushSprite(void*, int, int) {}
  void deleteSprite() {}
  void* createSprite(int, int) { return nullptr; }
  uint16_t* getBuffer() { return nullptr; }
  int width() const { return 240; }
  int height() const { return 135; }
  void startWrite() {}
  void endWrite() {}
  uint16_t color565(uint8_t, uint8_t, uint8_t) { return 0; }
  uint16_t readPixel(int, int) { return 0; }
  void fillSprite(uint16_t) {}
  void setColorDepth(int) {}
  bool drawJpg(Stream*, int, int) { return true; }
  void fillTriangle(int, int, int, int, int, int, uint16_t) {}
  void drawTriangle(int, int, int, int, int, int, uint16_t) {}
};

struct SimI2C {
  bool bitOn(uint8_t, uint8_t, uint8_t, uint32_t = 100000) { return true; }
  bool bitOff(uint8_t, uint8_t, uint8_t, uint32_t = 100000) { return true; }
  bool scanID(uint8_t) { return true; }
};

struct SimDisplay {
  int width() { return 240; }
  int height() { return 135; }
  void startWrite() {}
  void endWrite() {}
  void waitDisplay() {}
  void sleep() {}
  void wakeup() {}
  void setBrightness(uint8_t) {}
};

struct SimPower {
  int16_t getBatteryVoltage() { return 4000; }
  int getBatteryLevel() { return 80; }
  bool isCharging() { return false; }
};

struct SimBtn {
  bool isPressed() { return false; }
  bool wasPressed() { return false; }
  bool wasReleased() { return false; }
};

struct SimSpeaker {
  void tone(uint16_t, int) {}
  void stop() {}
  void setVolume(uint8_t) {}
};

struct SimM5 {
  SimDisplay Display;
  SimPower Power;
  SimBtn BtnA, BtnB, BtnC;
  SimSpeaker Speaker;
  SimI2C In_I2C;
  void update() {}
  void begin() {}
};

extern SimM5 M5;
extern M5Canvas cv;
