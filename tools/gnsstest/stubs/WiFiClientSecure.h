#pragma once
#include "WiFi.h"

class WiFiClient : public Stream {
public:
  int connect(const char*, uint16_t) { return 0; }
  int connect(const char*, uint16_t, int) { return 0; }
  void stop() {}
  uint8_t connected() { return 0; }
  operator bool() { return false; }
};

class WiFiClientSecure : public WiFiClient {
public:
  void setInsecure() {}
  void setCACert(const char*) {}
};
