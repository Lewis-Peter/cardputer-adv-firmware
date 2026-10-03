// 给 power_util.cpp 的最小替身：只要能编过 globals.h，并且让测试能控制时钟和电压。
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <cstdio>
#include <cmath>

extern uint32_t g_fakeMs;        // 测试驱动的时钟
extern int      g_fakeMv;        // 测试驱动的电池电压
inline uint32_t millis() { return g_fakeMs; }
// 测试台里时间是测试自己驱动的，delay 只推进假时钟，不真睡
inline void delay(uint32_t ms) { g_fakeMs += ms; }

// ⚠️ 这个桩要跟着 src 用到的 M5 接口面走。2026-08-26 补的三个是 powerDiag()（串口 BATT）
// 要读的"插没插"信号——**本机三个全是无效值**，桩就照实返回无效值，别编一个好看的：
//   isCharging() -> charge_unknown(2)   getBatteryCurrent() -> 0   getVBUSVoltage() -> -1
struct SimPower {
  int16_t getBatteryVoltage() { return (int16_t)g_fakeMv; }
  int     isCharging()        { return 2; }   // is_discharging=0, is_charging=1, charge_unknown=2
  int32_t getBatteryCurrent() { return 0; }
  int16_t getVBUSVoltage()    { return -1; }
};

// 最小 Serial 替身：powerDiag() 往它打诊断。测试台里就直接落到 stdout。
struct SimSerial {
  template <typename... A> void printf(const char* f, A... a) { std::printf(f, a...); }
  void println(const char* s = "") { std::printf("%s\n", s); }
  void print(const char* s) { std::printf("%s", s); }
  // 原生 USB CDC 在真机上用 operator bool 表示"主机开着串口"。测试台里没有 USB，返回 false。
  explicit operator bool() const { return false; }
};
extern SimSerial Serial;
struct SimDisplay { int width() { return 240; } int height() { return 135; } };
class M5Canvas { public: M5Canvas(void* = nullptr) {} };
class SimM5 { public: SimPower Power; SimDisplay Display; };
extern SimM5 M5;

typedef std::string String;
static const uint16_t TFT_BLACK = 0, TFT_WHITE = 0xFFFF;
