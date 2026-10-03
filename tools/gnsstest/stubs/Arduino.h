#pragma once
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <chrono>
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
#ifndef radians
#define radians(d) ((d) * DEG_TO_RAD)
#endif
#ifndef degrees
#define degrees(r) ((r) * RAD_TO_DEG)
#endif
#ifndef sq
#define sq(x) ((x) * (x))
#endif

#ifndef TWO_PI
#define TWO_PI 6.283185307179586476925286766559
#endif

typedef uint8_t byte;
class __FlashStringHelper;
#define pgm_read_byte(addr) (*(const unsigned char*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))
#define pgm_read_ptr(addr) (*(const void* const*)(addr))

class Print;
class Printable {
public:
  virtual ~Printable() {}
  virtual size_t printTo(Print& p) const = 0;
};

using std::min;
using std::max;

extern uint32_t g_fakeNowMs;
inline unsigned long millis() { return g_fakeNowMs; }
inline void delay(uint32_t ms) { g_fakeNowMs += ms; }
inline void yield() {}

class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) { return 1; }
  virtual size_t write(const uint8_t* b, size_t n) { return n; }
  size_t print(const char* s) { return s ? strlen(s) : 0; }
  size_t println(const char* = "") { return 1; }
};

class Stream : public Print {
public:
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual int peek() { return -1; }
  virtual size_t readBytes(char* buf, size_t len) { return 0; }
  virtual size_t readBytes(uint8_t* buf, size_t len) { return 0; }
  virtual bool find(const char* target) { return false; }
  virtual bool findUntil(const char* target, const char* term) { return false; }
};

#define SERIAL_8N1 0
using std::isnan;
using std::isinf;

#include <deque>
#include <vector>

extern std::deque<char> g_simGpsRxBuf;

class HardwareSerial : public Stream {
public:
  HardwareSerial(int) {}
  void begin(unsigned long, uint32_t = 0, int8_t = -1, int8_t = -1) {}
  void setRxBufferSize(size_t) {}
  int available() override { return (int)g_simGpsRxBuf.size(); }
  int read() override {
    if (g_simGpsRxBuf.empty()) return -1;
    char c = g_simGpsRxBuf.front();
    g_simGpsRxBuf.pop_front();
    return (unsigned char)c;
  }
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t*, size_t n) override { return n; }
};

struct SimSerial {
  std::string captured;
  bool captureEnabled = true;

  void clear() { captured.clear(); }
  std::vector<std::string> getLines() const {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : captured) {
      if (c == '\n') {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        lines.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) {
      if (cur.back() == '\r') cur.pop_back();
      lines.push_back(cur);
    }
    return lines;
  }

  template <typename... A> void printf(const char* f, A... a) {
    char b[512];
    snprintf(b, sizeof(b), f, a...);
    if (captureEnabled) captured += b;
  }
  void println(const char* s = "") {
    if (captureEnabled) {
      if (s) captured += s;
      captured += "\n";
    }
  }
  void print(const char* s) {
    if (s && captureEnabled) captured += s;
  }
  explicit operator bool() const { return false; }
};
extern SimSerial Serial;

struct SimEsp {
  size_t getFreeHeap() { return 120 * 1024; }
  size_t getHeapSize() { return 320 * 1024; }
};
extern SimEsp ESP;

#define pdMS_TO_TICKS(ms) (ms)
inline void vTaskDelay(uint32_t) {}

class String {
public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(int v) { char b[24]; snprintf(b, sizeof(b), "%d", v); s = b; }
  String(unsigned v) { char b[24]; snprintf(b, sizeof(b), "%u", v); s = b; }
  String(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); s = b; }
  String(double v, int d = 2) { char f[8], b[32]; snprintf(f, sizeof(f), "%%.%df", d); snprintf(b, sizeof(b), f, v); s = b; }
  const char* c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  int indexOf(char c) const { auto p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const char* c) const { auto p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
  int lastIndexOf(char c) const { auto p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  bool endsWith(const char* t) const { size_t n = strlen(t); return s.size() >= n && s.compare(s.size() - n, n, t) == 0; }
  bool endsWith(const String& t) const { return endsWith(t.c_str()); }
  void toLowerCase() { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); }
  bool startsWith(const char* p) const { return s.rfind(p, 0) == 0; }
  String substring(size_t a) const { return String(a <= s.size() ? s.substr(a) : std::string()); }
  String substring(size_t a, size_t b) const { return String(s.substr(a, b - a)); }
  void remove(size_t i) { if (i < s.size()) s.erase(i); }
  void reserve(size_t n) { s.reserve(n); }
  bool concat(const char* c) { if (c) s += c; return true; }
  bool concat(char c) { s += c; return true; }
  bool concat(const String& o) { s += o.s; return true; }
  char operator[](size_t i) const { return s[i]; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }
  String& operator+=(char o) { s += o; return *this; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const String& o) const { return s != o.s; }
};

inline String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, const char* b) { String r(a); r += b; return r; }
inline String operator+(const char* a, const String& b) { String r(a); r += b; return r; }

#define F(s) (s)
#define PROGMEM
#define PSTR(s) (s)
#define OUTPUT 1
#define INPUT 0
#define HIGH 1
#define LOW 0
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }
inline bool getLocalTime(struct tm* info, uint32_t = 5000) {
  time_t t = time(nullptr);
  if (!info) return false;
  localtime_r(&t, info);
  return true;
}
