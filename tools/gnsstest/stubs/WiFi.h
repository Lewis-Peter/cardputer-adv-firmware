#pragma once
#include "Arduino.h"

enum wl_status_t {
  WL_NO_SHIELD = 255,
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6
};

class SimWiFi {
public:
  wl_status_t status() { return WL_DISCONNECTED; }
  String macAddress() { return "00:11:22:33:44:55"; }
  String SSID() { return ""; }
  int32_t RSSI() { return 0; }
  int disconnect(bool = false, bool = false) { return 0; }
};

extern SimWiFi WiFi;
