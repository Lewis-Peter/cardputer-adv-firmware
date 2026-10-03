#pragma once
// Arduino 主机桩，供 astrotest 编译 src/astro.cpp 和 src/moon.cpp。
// 依照 tools/clocktest/stubs/Arduino.h 的写法，只取 astro/moon 实际需要的部分。
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <ctime>
#include <algorithm>

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295769236907684886
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG 57.295779513082320876798154814105
#endif
#ifndef constrain
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif

#ifndef TWO_PI
#define TWO_PI 6.283185307179586476925286766559
#endif

typedef uint8_t byte;
#define F(s) (s)
#define PROGMEM

using std::min;
using std::max;

// millis / delay 打桩（astro/moon 不直接调这两个，但 String 等可能间接用到）
extern uint32_t g_fakeNowMs;
inline unsigned long millis() { return g_fakeNowMs; }
inline void delay(uint32_t ms) { g_fakeNowMs += ms; }

// Arduino String 最小实现（moon.cpp 里有 String 类型使用）
class String {
public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(int v)      { char b[24]; snprintf(b, sizeof(b), "%d", v);  s = b; }
  String(unsigned v) { char b[24]; snprintf(b, sizeof(b), "%u", v);  s = b; }
  String(double v, int d = 2) {
    char f[8], b[32];
    snprintf(f, sizeof(f), "%%.%df", d);
    snprintf(b, sizeof(b), f, v);
    s = b;
  }
  const char* c_str()  const { return s.c_str(); }
  size_t length()      const { return s.size(); }
  bool   operator==(const String& o) const { return s == o.s; }
  bool   operator!=(const String& o) const { return s != o.s; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o)   { s += o;   return *this; }
};

inline String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, const char*   b) { String r(a); r += b; return r; }
inline String operator+(const char*   a, const String& b) { String r(a); r += b; return r; }
