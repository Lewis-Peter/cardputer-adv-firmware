#pragma once
#include "sim_arduino.h"

// M5GFX 没有 LovyanGFX.hpp 这个入口，直接引它内部的 sprite 头（够画图 + 量字宽了）
#include "lgfx/v1/LGFX_Sprite.hpp"
#include "lgfx/v1/lgfx_fonts.hpp"
#include "lgfx/v1/misc/enum.hpp"

using LovyanGFX = lgfx::v1::LovyanGFX;

// 固件里的 M5Canvas 就是带父设备的 LGFX_Sprite。模拟器里不需要父设备（纯内存画布），
// 顺便把 ARDUINO 分支里才有的 String 版 drawString/textWidth 补回来。
class M5Canvas : public ::LGFX_Sprite {
public:
  M5Canvas(LovyanGFX* parent = nullptr) : ::LGFX_Sprite(parent) {}
  using ::LGFX_Sprite::drawString;
  using ::LGFX_Sprite::textWidth;
  using ::LGFX_Sprite::drawJpg;
  int32_t drawString(const String& t, int32_t x, int32_t y) { return ::LGFX_Sprite::drawString(t.c_str(), x, y); }
  int32_t textWidth(const String& t) { return ::LGFX_Sprite::textWidth(t.c_str()); }
  bool drawJpg(Stream*, int32_t = 0, int32_t = 0) { return true; }
};

class SimSpeaker {
public:
  void begin() {}
  void end() {}
  void tone(uint16_t, int, int = -1, bool = true) {}
  void stop() {}
  void setVolume(uint8_t) {}
};

class SimMicConfig {
public:
  uint32_t sample_rate = 16000;
  int dma_buf_len = 256;
  int dma_buf_count = 3;
  int over_sampling = 1;
};

class SimMic {
public:
  bool isEnabled() { return true; }
  bool isRecording() { return false; }
  SimMicConfig config() { return SimMicConfig(); }
  void config(const SimMicConfig&) {}
  bool begin() { return true; }
  void end() {}
  bool record(int16_t* buf, size_t n, uint32_t, bool = false) {
    if (!buf) return false;
    static float phase = 0.0f;
    for (size_t i = 0; i < n; i++) {
      float s = sinf(phase) * 10000.0f + sinf(phase * 2.5f) * 5000.0f + sinf(phase * 5.1f) * 2500.0f;
      buf[i] = (int16_t)s;
      phase += 0.392699f;
    }
    return true;
  }
};

class SimI2C {
public:
  bool writeRegister8(uint8_t, uint8_t, uint8_t, uint32_t = 0) { return true; }
  bool bitOn(uint8_t, uint8_t, uint8_t, uint32_t = 0) { return true; }
  bool bitOff(uint8_t, uint8_t, uint8_t, uint32_t = 0) { return true; }
  bool scanID(uint8_t) { return true; }
};

// ⚠️ 必须真的是个 LGFX 设备，不能是个空类：src/globals.cpp 里写的是
// `M5Canvas cv(&M5.Display);`，要能把它当 LovyanGFX* 传进去。
// 这不是为了让模拟器多画一块屏（它照旧不输出到任何地方），而是为了让 globals.cpp
// 能在桌面上过一遍语法——那个文件里的两条 static_assert 是加 app 时唯一的护栏。
class SimPower {
public:
  int16_t getBatteryVoltage() { return 4120; }
  int getBatteryLevel() { return 80; }
  bool isCharging() { return false; }
};

class SimBtn {
public:
  bool wasPressed() { return false; }
};

class SimDisplay : public lgfx::v1::LGFX_Device {};
class SimM5 {
public:
  SimSpeaker Speaker;
  SimDisplay Display;
  SimPower   Power;
  SimMic     Mic;
  SimI2C     In_I2C;
  SimBtn     BtnA;
  void update() {}
};
extern SimM5 M5;

