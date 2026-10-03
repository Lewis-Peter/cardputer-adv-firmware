#pragma once
#include <cstdint>
#include <cstdio>
// NVS 替身：用一个全局变量当"闪存"，测试可以直接摆布它来模拟"上次关机前存的电量"
extern uint8_t g_nvsPct;
class Preferences {
public:
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  uint8_t getUChar(const char*, uint8_t d = 0) { return g_nvsPct ? g_nvsPct : d; }
  void putUChar(const char*, uint8_t v) { g_nvsPct = v; }
};
