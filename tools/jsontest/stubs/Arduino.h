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
#include <vector>

extern uint32_t g_fakeNowMs;
inline unsigned long millis() { return g_fakeNowMs; }
inline void delay(uint32_t ms) { g_fakeNowMs += ms; }
inline void vTaskDelay(uint32_t ms) { g_fakeNowMs += (ms ? ms : 1); }
#define pdMS_TO_TICKS(ms) (ms)
inline void yield() {}

typedef uint8_t byte;
class __FlashStringHelper;
#define pgm_read_byte(addr) (*(const unsigned char*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))
#define pgm_read_ptr(addr) (*(const void* const*)(addr))

class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) { return 1; }
  virtual size_t write(const uint8_t* b, size_t n) { return n; }
  size_t print(const char* s) { return s ? strlen(s) : 0; }
  size_t println(const char* = "") { return 1; }
};

class Printable {
public:
  virtual ~Printable() {}
  virtual size_t printTo(Print& p) const = 0;
};

class Stream : public Print {
public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  virtual size_t readBytes(char* buf, size_t len) {
    size_t count = 0;
    while (count < len) {
      int c = read();
      if (c < 0) break;
      buf[count++] = (char)c;
    }
    return count;
  }
  virtual size_t readBytes(uint8_t* buf, size_t len) {
    return readBytes((char*)buf, len);
  }
  virtual bool find(const char* target) {
    if (!target || !*target) return true;
    size_t targetLen = strlen(target);
    size_t matched = 0;
    while (available() > 0) {
      int c = read();
      if (c == (unsigned char)target[matched]) {
        matched++;
        if (matched == targetLen) return true;
      } else {
        matched = (c == (unsigned char)target[0]) ? 1 : 0;
      }
    }
    return false;
  }
};

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
inline String operator+(const String& a, int b) { String r(a); r += String(b); return r; }
inline String operator+(const String& a, char b) { String r(a); r += b; return r; }
