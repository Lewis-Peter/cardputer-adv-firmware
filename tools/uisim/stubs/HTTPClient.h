#pragma once
#include "WiFi.h"
// 假 HTTPClient：每次 begin() 按 URL 选择响应，并把它装进可流式读取的客户端。
// 这样 weatherFetch() 是"真跑"的——真解析、真填那些 static 变量，页面上出现的是真实布局。
extern String simCannedResponse;
extern String simResponseForPath(const char* url);
class HTTPClient {
  WiFiClient stream_;
  String response_;
public:
  bool begin(WiFiClient&, const char* url) { response_ = simResponseForPath(url); stream_.simSetResponse(response_); return true; }
  bool begin(WiFiClient& c, const String& url) { return begin(c, url.c_str()); }
  void addHeader(const String&, const String&) {}
  void setConnectTimeout(int) {}
  // 2026-08-26 补：src 改用 setUserAgent() 之后桩才发现缺这个。
  // （addHeader("User-Agent",...) 在真 HTTPClient 里是被静默忽略的，见 http_json.h）
  void setUserAgent(const char*) {}
  void setTimeout(int) {}
  void useHTTP10(bool) {}
  // 2026-09-02 补：/traffic 那条常开流禁用 keep-alive 复用（见 router.cpp trafficEnsureOpen）。
  void setReuse(bool) {}
  int GET() { return 200; }
  int PUT(const String&) { return 200; }
  int POST(const String&) { return 200; }
  int getSize() { return 0; }
  String getString() { return response_; }
  WiFiClient& getStream() { return stream_; }
  WiFiClient* getStreamPtr() { return &stream_; }
  void end() {}
};
