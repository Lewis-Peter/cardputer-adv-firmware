#pragma once
// 离线地图瓦片（gnss.cpp）要用 SD；测试里当"卡上什么都没有"处理，SD 全部返回 miss
#include "Arduino.h"

#define FILE_READ "r"

class File : public Stream {
public:
  operator bool() const { return false; }
  bool isDirectory() { return false; }
  File openNextFile() { return File(); }
  const char* name() { return ""; }
  int available() { return 0; }
  int read() { return -1; }
  void close() {}
};

class SDClass {
public:
  bool exists(const char*) { return false; }
  File open(const char*, const char* = FILE_READ) { return File(); }
};
static SDClass SD;
