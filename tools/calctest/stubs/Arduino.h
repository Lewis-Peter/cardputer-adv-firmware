// 主机端 Arduino 替身头——给 calctest 用。
// 仅包含 calc.cpp 和 conv.cpp 实际依赖的那些东西；
// 不需要 Serial、ESP、gpio 等外设，也没有 sim 实体。
#pragma once
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <algorithm>

// ---- 数学常量（ESP-Arduino 原版全局定义的） ----
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif

using std::min;
using std::max;

// ESP32 Arduino 在全局命名空间里暴露这些 C 数学函数；
// 在 GCC 16 的 C++17 模式下 <cmath> 把它们放进了 std::，
// 需要显式 using 才能让 src/ 里的无限定名字通过编译。
using std::isnan;
using std::isinf;
using std::floor;
using std::fabs;
using std::fmod;
using std::pow;
using std::sqrt;
using std::log;
using std::log10;
using std::exp;
using std::sin;
using std::cos;
using std::tan;
using std::asin;
using std::acos;
using std::atan;
using std::atan2;
using std::ceil;
using std::round;
using std::atof;
using std::abs;

// ---- millis() 占位（calc.cpp/conv.cpp 里不调，但 globals.h 的 include 链可能触碰到） ----
extern uint32_t g_fakeNowMs;
inline unsigned long millis() { return g_fakeNowMs; }
inline void delay(uint32_t ms) { g_fakeNowMs += ms; }
inline void yield() {}

// ---- Arduino String 类（只实现 calc/conv 用到的接口） ----
class String {
public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(int v)    { char b[24]; snprintf(b, sizeof(b), "%d", v);  s = b; }
  String(unsigned v){ char b[24]; snprintf(b, sizeof(b), "%u", v); s = b; }
  String(long v)   { char b[24]; snprintf(b, sizeof(b), "%ld", v); s = b; }
  String(double v, int d = 2) {
    char f[8], b[32];
    snprintf(f, sizeof(f), "%%.%df", d);
    snprintf(b, sizeof(b), f, v);
    s = b;
  }
  const char* c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  int indexOf(char c) const {
    auto p = s.find(c);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(const char* c) const {
    auto p = s.find(c);
    return p == std::string::npos ? -1 : (int)p;
  }
  String substring(size_t a) const {
    return String(a <= s.size() ? s.substr(a) : std::string());
  }
  String substring(size_t a, size_t b) const {
    return String(s.substr(a, b - a));
  }
  void remove(size_t i) { if (i < s.size()) s.erase(i); }
  void reserve(size_t n) { s.reserve(n); }
  bool concat(const char* c) { if (c) s += c; return true; }
  bool concat(char c)  { s += c; return true; }
  bool concat(const String& o) { s += o.s; return true; }
  char operator[](size_t i) const { return s[i]; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o)   { s += o;   return *this; }
  String& operator+=(char o)          { s += o;   return *this; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const String& o) const { return s != o.s; }
  bool operator==(const char* o)    const { return s == o; }
  bool operator!=(const char* o)    const { return s != o; }
};

inline String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, const char*   b) { String r(a); r += b; return r; }
inline String operator+(const char*   a, const String& b) { String r(a); r += b; return r; }

#define F(s) (s)
#define PROGMEM
#define PSTR(s) (s)
