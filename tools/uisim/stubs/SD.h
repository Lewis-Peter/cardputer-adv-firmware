#pragma once
#include "sim_arduino.h"

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

class File {
public:
  operator bool() const { return false; }
  bool isDirectory() { return false; }
  File openNextFile() { return File(); }
  const char* name() { return ""; }
  int available() { return 0; }
  int read() { return -1; }
  size_t readBytes(char*, size_t) { return 0; }
  String readStringUntil(char) { return String(""); }
  bool println(const String&) { return true; }
  bool println(const char*) { return true; }
  size_t write(const uint8_t*, size_t) { return 0; }
  size_t size() { return 0; }
  void flush() {}
  void close() {}
};

class SDClass {
  static std::vector<std::string>& files() {
    static std::vector<std::string> f;
    return f;
  }
public:
  static void simAddFile(const std::string& path) { files().push_back(path); }
  static void simClearFiles() { files().clear(); }
  bool exists(const char* path) {
    if (!path) return false;
    for (const auto& f : files()) {
      if (f == path) return true;
    }
    return false;
  }
  bool exists(const String& s) { return exists(s.c_str()); }
  bool mkdir(const char*) { return true; }
  bool mkdir(const String& s) { return mkdir(s.c_str()); }
  File open(const char*, const char* = FILE_READ) { return File(); }
  File open(const String& s, const char* m = FILE_READ) { return open(s.c_str(), m); }
};
extern SDClass SD;
