#pragma once
#include "Arduino.h"
#include <vector>
#include <string>

// 模拟数据流
class MockStream : public Stream {
public:
  std::string data;
  size_t pos = 0;
  bool simulateTimeout = false;

  MockStream() : pos(0) {}
  MockStream(const std::string& d) : data(d), pos(0) {}

  void setData(const std::string& d) {
    data = d;
    pos = 0;
  }

  int available() override {
    return (pos < data.size()) ? (int)(data.size() - pos) : 0;
  }

  int read() override {
    if (pos < data.size()) return (unsigned char)data[pos++];
    return -1;
  }

  int peek() override {
    if (pos < data.size()) return (unsigned char)data[pos];
    return -1;
  }
};

struct MockHttpConfig {
  bool beginSuccess = true;
  int httpCode = 200;
  std::string responseBody;
  MockStream stream;
  std::string capturedUserAgent;
  std::string capturedAuth;
  uint32_t capturedConnectTimeout = 0;
  uint32_t capturedTimeout = 0;
  bool useHttp10Called = false;
  bool endCalled = false;
  bool beginCalled = false;

  void reset() {
    beginSuccess = true;
    httpCode = 200;
    responseBody.clear();
    stream.setData("");
    stream.simulateTimeout = false;
    capturedUserAgent.clear();
    capturedAuth.clear();
    capturedConnectTimeout = 0;
    capturedTimeout = 0;
    useHttp10Called = false;
    endCalled = false;
    beginCalled = false;
  }
};

extern MockHttpConfig g_mockHttp;

class Client {};

class HTTPClient {
public:
  void useHTTP10(bool) { g_mockHttp.useHttp10Called = true; }
  void setConnectTimeout(uint32_t ms) { g_mockHttp.capturedConnectTimeout = ms; }
  void setTimeout(uint32_t ms) { g_mockHttp.capturedTimeout = ms; }

  template <typename C>
  bool begin(C&, const String&) { g_mockHttp.beginCalled = true; return g_mockHttp.beginSuccess; }
  bool begin(const String&) { g_mockHttp.beginCalled = true; return g_mockHttp.beginSuccess; }

  void setUserAgent(const char* ua) { if (ua) g_mockHttp.capturedUserAgent = ua; }
  void setUserAgent(const String& ua) { g_mockHttp.capturedUserAgent = ua.c_str(); }

  void addHeader(const char* name, const char* val) {
    if (name && strcmp(name, "Authorization") == 0 && val) {
      g_mockHttp.capturedAuth = val;
    }
  }

  int GET() { return g_mockHttp.httpCode; }

  Stream& getStream() { return g_mockHttp.stream; }
  String getString() { return String(g_mockHttp.responseBody.c_str()); }

  void end() { g_mockHttp.endCalled = true; }
};
