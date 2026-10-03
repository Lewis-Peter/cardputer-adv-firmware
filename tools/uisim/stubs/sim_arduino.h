// 桌面模拟器用的极简 Arduino 兼容层：只提供固件绘制代码真正用到的那点东西。
// （不能 #define ARDUINO——那会让 LovyanGFX 去找真正的 Arduino.h。）
#pragma once
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>   // Print::printf 的可变参数
#include <cmath>
#include <chrono>
#include <ctime>
#include <algorithm>

inline bool getLocalTime(struct tm* info, uint32_t ms = 5000) {
  time_t t = time(nullptr);
  if (!info) return false;
  localtime_r(&t, info);
  return true;
}

// Arduino.h 里那几个宏/常量。固件代码随手就会用，模拟器编译的是同一份 src/，
// 所以这里缺一个就整个编不过——加新页面时如果 build.sh 挂在 "undeclared identifier"，
// 十有八九是又用到了一个这里还没补的 Arduino 内建。
typedef uint8_t byte;
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#ifndef TWO_PI
#define TWO_PI 6.283185307179586476925286766559
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
#ifndef radians
#define radians(d) ((d) * DEG_TO_RAD)
#endif
#ifndef degrees
#define degrees(r) ((r) * RAD_TO_DEG)
#endif
#ifndef sq
#define sq(x) ((x) * (x))
#endif

using std::min;
using std::max;

// ---- 极简 Print / Printable ----
// ⚠️ 只是为了让 ArduinoJson 编得过：它 7.4.x 把 Printable 的转换器挂在
// ARDUINOJSON_ENABLE_ARDUINO_STREAM 下面（看着像笔误，按语义该是 _PRINT），
// 所以一旦打开 Stream 支持，就必须连 Print/Printable 一起给出来。固件代码不用它们。
//
// print/println/printf 是后补的，为的是 src/ram_profile.cpp：它整段输出都是格式串
// （%9lu / %+9ld / %-14s），而格式串跟参数类型对不上在板子上是**静默**的——照样烧得进去，
// 只是打出来的数是垃圾。这里给 printf 挂上 format attribute，桌面这一遍就能把它挡掉。
// write() 默认实现成"丢掉"：桌面上没人真要看这些字节，要的只是编译器走一遍。
class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) { (void)c; return 1; }
  virtual size_t write(const uint8_t* buf, size_t n) {
    size_t i = 0;
    while (i < n && write(buf[i])) i++;
    return i;
  }
  size_t print(const char* s)   { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
  size_t println()              { return write((uint8_t)'\n'); }
  size_t println(const char* s) { return print(s) + println(); }
  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char b[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    size_t len = (size_t)n < sizeof(b) ? (size_t)n : sizeof(b) - 1;
    return write((const uint8_t*)b, len);
  }
};
class Printable {
public:
  virtual ~Printable() {}
  virtual size_t printTo(Print& p) const = 0;
};

// ---- 极简 Stream ----
// 为什么要有它：src/ 里最吃紧的那几页（github.cpp、quake.cpp）**不走 fetchJsonHttp**，
// 而是拿 http.getStream() 逐元素解析——那是这个项目对付"TLS 峰值 + 大 JSON"的标准做法
// （见 github.cpp 顶上那段 NoMemory 的账）。没有 Stream，这类页面一个都进不了模拟器，
// 而它们恰恰是最该被离线跑一遍的。
//
// ArduinoJson 只要求 readBytes()（见它的 ArduinoStreamReader）；find/findUntil/parseInt
// 是固件那边走数组框架用的，按 Arduino 的语义实现：
//   find(t)            —— 一直读到匹配上 t，读完都没有就 false
//   findUntil(t, term) —— 同上，但先撞上 term 就停下并返回 false
//   parseInt()         —— 跳过非数字，然后读一串数字（带可选负号）
// ⚠️ 真机上 Stream 就是从 Print 派生的（所以 Serial 既能 read 又能 printf）。
// 这里跟着派生，ram_profile.cpp 那种 `void f(Stream&)` 里调 out.printf 的写法才编得过。
class Stream : public Print {
public:
  virtual ~Stream() {}
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;

  size_t readBytes(char* buf, size_t n) {
    size_t got = 0;
    while (got < n) { int c = read(); if (c < 0) break; buf[got++] = (char)c; }
    return got;
  }
  bool find(const char* target) { return findUntil(target, nullptr); }
  bool findUntil(const char* target, const char* terminator) {
    size_t ti = 0, ei = 0;
    size_t tlen = target ? strlen(target) : 0;
    size_t elen = terminator ? strlen(terminator) : 0;
    if (tlen == 0) return true;
    for (;;) {
      int c = read();
      if (c < 0) return false;
      ti = (c == target[ti]) ? ti + 1 : (c == target[0] ? 1 : 0);
      if (ti == tlen) return true;
      if (elen) {
        ei = (c == terminator[ei]) ? ei + 1 : (c == terminator[0] ? 1 : 0);
        if (ei == elen) return false;
      }
    }
  }
  long parseInt() {
    int c = peek();
    while (c >= 0 && c != '-' && (c < '0' || c > '9')) { read(); c = peek(); }
    bool neg = false;
    if (c == '-') { read(); neg = true; c = peek(); }
    long v = 0;
    bool any = false;
    while (c >= '0' && c <= '9') { v = v * 10 + (read() - '0'); any = true; c = peek(); }
    if (!any) return 0;
    return neg ? -v : v;
  }
};

// std::string 的薄包装，补上 Arduino String 那几个方法
class String {
public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(int v)          { char b[24]; snprintf(b, sizeof(b), "%d", v);   s = b; }
  String(unsigned v)     { char b[24]; snprintf(b, sizeof(b), "%u", v);   s = b; }
  String(long v)         { char b[24]; snprintf(b, sizeof(b), "%ld", v);  s = b; }
  String(double v, int d = 2) { char f[8], b[32]; snprintf(f, sizeof(f), "%%.%df", d); snprintf(b, sizeof(b), f, v); s = b; }
  const char* c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  int indexOf(char c) const { auto p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
  // 2026-08-26 补：router.cpp 用这两个区分 401/403 和"真连不上"、以及提示行该指哪个配置项
  int indexOf(const char* c) const { auto p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
  // 2026-09-02 补：ir.cpp 从 /ir/*.ir 的文件名里切后缀要用这两个
  int lastIndexOf(char c) const { auto p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  bool endsWith(const char* t) const {
    size_t n = strlen(t);
    return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
  }
  bool endsWith(const String& t) const { return endsWith(t.c_str()); }
  void toLowerCase() { for (auto& c : s) c = std::tolower((unsigned char)c); }
  bool startsWith(const char* p) const { return s.rfind(p, 0) == 0; }
  bool startsWith(const String& p) const { return s.rfind(p.s, 0) == 0; }
  String substring(size_t a) const { return String(a <= s.size() ? s.substr(a) : std::string()); }
  String substring(size_t a, size_t b) const { return String(s.substr(a, b - a)); }
  void remove(size_t i) { if (i < s.size()) s.erase(i); }
  void reserve(size_t n) { s.reserve(n); }
  bool concat(char c) { s += c; return true; }
  bool concat(const char* c) { s += c; return true; }
  int toInt() const { return atoi(s.c_str()); }
  void trim() {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) { s.clear(); return; }
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);
  }
  char operator[](size_t i) const { return s[i]; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o)   { s += o;   return *this; }
  String& operator+=(char o)          { s += o;   return *this; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const String& o) const { return s != o.s; }
};
inline String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, const char* b)   { String r(a); r += b; return r; }
inline String operator+(const char* a, const String& b)   { String r(a); r += b; return r; }
inline String operator+(const String& a, int b)           { String r(a); r += String(b); return r; }
inline String operator+(const String& a, char b)          { String r(a); r += b; return r; }

#define SERIAL_8N1 0x06
class HardwareSerial : public Stream {
  int port_ = 0;
  static std::string& getBuf(int port) {
    static std::string bufs[4];
    return bufs[port >= 0 && port < 4 ? port : 0];
  }
public:
  HardwareSerial(int port = 0) : port_(port) {}
  void setRxBufferSize(size_t) {}
  void begin(unsigned long, uint32_t = 0, int = -1, int = -1) {}
  void simFeed(const char* s) { if (s) getBuf(port_).append(s); }
  void simFeed(const std::string& s) { getBuf(port_).append(s); }
  void simClear() { getBuf(port_).clear(); }
  static void simFeedPort(int port, const char* s) { if (s) getBuf(port).append(s); }
  static void simClearPort(int port) { getBuf(port).clear(); }
  int available() override { return (int)getBuf(port_).size(); }
  int read() override {
    std::string& b = getBuf(port_);
    if (b.empty()) return -1;
    char c = b.front();
    b.erase(b.begin());
    return (uint8_t)c;
  }
  int peek() override {
    std::string& b = getBuf(port_);
    if (b.empty()) return -1;
    return (uint8_t)b.front();
  }
  size_t write(uint8_t c) override { (void)c; return 1; }
  using Print::write;
};
extern HardwareSerial Serial;

inline uint32_t& simTimeOffset() {
  static uint32_t offset = 0;
  return offset;
}
inline uint32_t millis() {
  using namespace std::chrono;
  static auto t0 = steady_clock::now();
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count() + simTimeOffset();
}
inline void simAdvanceMillis(uint32_t ms) {
  simTimeOffset() += ms;
}
inline void delay(uint32_t) {}
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (ms)
#endif
inline void vTaskDelay(uint32_t) {}
inline float temperatureRead() { return 42.0f; }

class SimEsp {
public:
  uint32_t getHeapSize() { return 320 * 1024; }
  uint32_t getFreeHeap() { return 110 * 1024; }
  uint32_t getSketchSize() { return 3600 * 1024; }
  uint32_t getFreeSketchSpace() { return 4500 * 1024; }
  uint32_t getMinFreeHeap() { return 72 * 1024; }
};
inline SimEsp ESP;
