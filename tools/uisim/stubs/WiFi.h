#pragma once
#include "sim_arduino.h"
#define WL_CONNECTED 3
class SimWiFi {
  int status_ = WL_CONNECTED;
public:
  int status() { return status_; }
  void setStatus(int s) { status_ = s; }
  void disconnect() { status_ = 0; }
  void reconnect() { status_ = WL_CONNECTED; }
  int8_t RSSI() { return -55; }
};
extern SimWiFi WiFi;
class WiFiClient : public Stream {
  String data_;
  size_t pos_ = 0;
public:
  void simSetResponse(const String& data) { data_ = data; pos_ = 0; }
  int available() override { return (int)(data_.length() - pos_); }
  int read() override { return pos_ < data_.length() ? (unsigned char)data_[pos_++] : -1; }
  using Stream::readBytes;
  int read(uint8_t* dst, size_t n) {
    size_t left = data_.length() - pos_;
    size_t take = left < n ? left : n;
    if (take) { memcpy(dst, data_.c_str() + pos_, take); pos_ += take; }
    return (int)take;
  }
  int peek() override { return pos_ < data_.length() ? (unsigned char)data_[pos_] : -1; }
  bool connected() { return pos_ < data_.length(); }
  // 2026-09-02 补：router.cpp 的常开 /traffic 流退出时会 stop() 掉 socket。
  void stop() { data_ = ""; pos_ = 0; }
};
