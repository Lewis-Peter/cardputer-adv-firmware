#pragma once

#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "net_resolve.h"

// 统一的 User-Agent。Arduino 默认发的是 "ESP32HTTPClient"，太泛——adsb.lol 已经开始按它
// 拦截（403 "User-Agent too generic; include valid contact info."），别的免费公共接口
// 跟进只是时间问题。而且对这类白嫖来的服务，说清自己是谁本来就是基本礼貌。
// ⚠️ adsb.lol 明确要"联系方式"。这个项目还没开源，先只放项目名（实测放行）；
//    将来公开了在括号里补上仓库 URL 或邮箱最好。
static const char* const HTTP_UA = "cardputer-adv/1.0";

struct HttpJsonOptions {
  uint32_t connectTimeoutMs;
  uint32_t timeoutMs;
  const char* userAgent;
  const char* authorization;
  bool stream;
  const char* jsonError;
  bool includeJsonDetail;

  HttpJsonOptions(uint32_t connectTimeout, uint32_t timeout,
                  const char* agent = HTTP_UA, const char* auth = nullptr)
      : connectTimeoutMs(connectTimeout), timeoutMs(timeout), userAgent(agent),
        authorization(auth), stream(true), jsonError("json: "), includeJsonDetail(true) {}
};

// 连之前先用 netResolve() 把域名解析掉：有上限（框架那条路径最长能卡 15~31s）、能被后台拉取的
// 取消打断，查到之后 HTTPClient 自己那次解析直接命中 lwIP 的缓存。见 net_resolve.h。
static inline bool httpPreResolve(const String& url, String& err) {
  char host[64];
  if (!netUrlHost(url.c_str(), host, sizeof(host))) return true;   // 抠不出来就交给 HTTPClient 自己报错
  NetResolveResult r = netResolve(host);
  if (r == NR_OK) return true;
  err = netResolveErrText(r);
  return false;
}

// Returns true on success. On failure, err contains the short caller-facing message.
// The document is left in an undefined state on failure.
template <typename Client>
bool fetchJsonHttp(Client& client, const String& url, JsonDocument& doc,
                   const JsonDocument* filter, const HttpJsonOptions& options,
                   String& err) {
  if (!httpPreResolve(url, err)) return false;
  HTTPClient http;
  http.useHTTP10(true);   // 下面直接对 getStream() 流式解析：不能让服务端回 chunked
  http.setConnectTimeout(options.connectTimeoutMs);
  http.setTimeout(options.timeoutMs);
  if (!http.begin(client, url)) {
    err = "http begin failed";
    return false;
  }
  // ⚠️ 必须用 setUserAgent()，addHeader("User-Agent", ...) **会被静默忽略**。
  // Arduino 的 HTTPClient::addHeader() 里有一张自己管的头名单，User-Agent 就在里面：
  //     if(!name.equalsIgnoreCase("Connection") &&
  //        !name.equalsIgnoreCase("User-Agent") && ...)   <- 不满足就直接不加
  // 于是这行代码从写下来那天起就没生效过，实际发出去的一直是默认的 "ESP32HTTPClient"。
  // 2026-08-26 被 adsb.lol 撞破：它开始按 UA 拦截，回
  //     403 "User-Agent too generic; include valid contact info."
  // 而恰恰只封 ESP32HTTPClient 这一个——本来要发的 cardputer-adv/1.0 是放行的。
  if (options.userAgent) http.setUserAgent(options.userAgent);
  if (options.authorization) http.addHeader("Authorization", options.authorization);

  int code = http.GET();
  if (code != 200) {
    err = String("http ") + code;
    http.end();
    return false;
  }

  DeserializationError e = DeserializationError::Ok;
  if (options.stream) {
    e = filter ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter))
               : deserializeJson(doc, http.getStream());
  } else {
    String body = http.getString();
    e = filter ? deserializeJson(doc, body, DeserializationOption::Filter(*filter))
               : deserializeJson(doc, body);
  }
  http.end();

  if (e) {
    err = options.jsonError ? options.jsonError : "json: ";
    if (options.includeJsonDetail) err += e.c_str();
    return false;
  }
  return true;
}

// 专为超大 JSON 数组设计的轻量流式解析器（逐项反序列化，零大块堆分配，防止 OOM）
// 例如 GitHub 365天热力图、USGS 上百条 GeoJSON feature 等。
// onItem 签名：bool onItem(JsonDocument& elem, int index)
// 返回 false 表示终止继续遍历，返回 true 继续下一项
template <typename Client, typename ItemCallback>
bool fetchJsonStreamArray(Client& client, const String& url, const char* arrayKey,
                          const JsonDocument* filter, const HttpJsonOptions& options,
                          ItemCallback onItem, String& err, int maxItems = 1000) {
  if (!httpPreResolve(url, err)) return false;
  HTTPClient http;
  http.useHTTP10(true);   // 下面直接对 getStream() 流式解析：不能让服务端回 chunked
  http.setConnectTimeout(options.connectTimeoutMs);
  http.setTimeout(options.timeoutMs);
  if (!http.begin(client, url)) {
    err = "http begin failed";
    return false;
  }
  if (options.userAgent) http.setUserAgent(options.userAgent);
  if (options.authorization) http.addHeader("Authorization", options.authorization);

  int code = http.GET();
  if (code != 200) {
    err = String("http ") + code;
    http.end();
    return false;
  }

  Stream& st = http.getStream();
  if (arrayKey && strlen(arrayKey) > 0) {
    if (!st.find(arrayKey)) {
      err = "array not found";
      http.end();
      return false;
    }
  }

  auto peekNonWs = [&st, &options]() -> int {
    uint32_t t0 = millis();
    while (millis() - t0 < options.timeoutMs) {
      if (st.available() > 0) {
        int c = st.peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
          st.read();
          continue;
        }
        return c;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    return -1;
  };

  int firstChar = peekNonWs();
  if (firstChar == ':') {
    st.read();
    firstChar = peekNonWs();
  }
  if (firstChar == '[') {
    st.read();
    firstChar = peekNonWs();
  }
  if (firstChar == ']') {
    st.read();
    http.end();
    return true; // 空数组正常结束
  }
  if (firstChar < 0) {
    err = "stream timeout";
    http.end();
    return false;
  }

  int count = 0;
  bool completed = false;
  while (count < maxItems) {
    JsonDocument elem;
    DeserializationError e = filter ? deserializeJson(elem, st, DeserializationOption::Filter(*filter))
                                    : deserializeJson(elem, st);
    if (e) {
      err = String("json error: ") + e.c_str();
      http.end();
      return false;
    }

    bool keepGoing = onItem(elem, count);
    count++;
    if (!keepGoing) {
      completed = true; // 回调主动要求停止
      break;
    }
    if (count >= maxItems) {
      completed = true; // 达到 maxItems
      break;
    }

    // 读取元素之间的分隔符：必须是 ',' 或 ']'
    int sep = -1;
    char c = 0;
    while (st.readBytes(&c, 1) == 1) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
      sep = (unsigned char)c;
      break;
    }

    if (sep == ']') {
      completed = true; // 遇到数组结尾
      break;
    } else if (sep == ',') {
      continue; // 下一个元素
    } else {
      err = (sep < 0) ? "stream timeout" : "unexpected separator";
      http.end();
      return false;
    }
  }

  http.end();
  return completed;
}

