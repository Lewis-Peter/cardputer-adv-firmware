// Preferences 替身——calctest 专用。
// calc.cpp 和 conv.cpp 不直接调用 NVS，但 globals.h 声明了 Preferences prefs，
// 编译器必须看到这个类型定义。所有方法均返回默认值，不写盘。
#pragma once
#include <cstdint>
#include "Arduino.h"

class Preferences {
public:
  bool   begin(const char*, bool = false) { return true; }
  void   end() {}
  bool   putBool(const char*, bool)         { return true; }
  bool   getBool(const char*, bool def = false) { return def; }
  size_t putInt(const char*, int32_t)       { return 4; }
  int32_t getInt(const char*, int32_t def = 0) { return def; }
  size_t putUInt(const char*, uint32_t)     { return 4; }
  uint32_t getUInt(const char*, uint32_t def = 0) { return def; }
  size_t putDouble(const char*, double)     { return 8; }
  double getDouble(const char*, double def = 0.0) { return def; }
  size_t putString(const char*, const String&) { return 0; }
  String getString(const char*, const String& def = "") { return def; }
  size_t putUChar(const char*, uint8_t)     { return 1; }
  uint8_t getUChar(const char*, uint8_t def = 0) { return def; }
};
