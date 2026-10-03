#pragma once
// Preferences 打桩：astro.cpp 通过 globals.h -> globals.cpp 间接依赖，但测试不调 NVS
struct Preferences {
  void begin(const char*, bool = false) {}
  void end() {}
  int    getInt(const char*, int d = 0) { return d; }
  bool   getBool(const char*, bool d = false) { return d; }
  void   putInt(const char*, int) {}
  void   putBool(const char*, bool) {}
  size_t putString(const char*, const char*) { return 0; }
  String getString(const char*, const String& d = "") { return d; }
};
