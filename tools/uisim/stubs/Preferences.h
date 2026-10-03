#pragma once
#include "sim_arduino.h"
class Preferences {
public:
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  String getString(const char*, const char* d = "") { return String(d); }
  String getString(const char*, const String& d) { return d; }
  void putString(const char*, const String&) {}
  int getInt(const char*, int d = 0) { return d; }
  void putInt(const char*, int) {}
  bool getBool(const char*, bool d = false) { return d; }
  void putBool(const char*, bool) {}
  // 2026-09-02 补：quake.cpp 用 loadUChar/saveUChar 记"选了哪个数据源"。
  // ⚠️ 这个桩**永远返回默认值**（不真的存），所以模拟器里换源不会跨次保留——
  // 要渲另一个源的画面就在 sim_main 里直接改。
  uint8_t getUChar(const char*, uint8_t d = 0) { return d; }
  void putUChar(const char*, uint8_t) {}
  // 补齐 globals.cpp 那六对 NVS helper 用到的全部方法。补全的收益不只是"少几个红字"：
  // 补上之后 src/globals.cpp 能在桌面上过一遍语法，那里面的两条 static_assert
  // （GROUPS[] 首尾相接盖满 APPS[]、组名放得进 tab 条）就跟着一起验了——
  // 加 app 最容易错、错了又最难看出来的正是这两张表。
  uint32_t getUInt(const char*, uint32_t d = 0) { return d; }
  void putUInt(const char*, uint32_t) {}
  double getDouble(const char*, double d = 0) { return d; }
  void putDouble(const char*, double) {}
};
