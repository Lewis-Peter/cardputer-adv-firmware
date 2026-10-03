#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "HTTPClient.h"

uint32_t g_fakeNowMs = 0;
MockHttpConfig g_mockHttp;

// netResolve() 的替身：真身在 net_resolve.cpp，要 lwIP。这里只控制它回什么、记下问的是谁。
#include "net_resolve.h"
static NetResolveResult g_resolveResult = NR_OK;
static std::string g_resolveHost;
NetResolveResult netResolve(const char* host, uint32_t) {
  g_resolveHost = host ? host : "";
  return g_resolveResult;
}

#include "http_json.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr) do { \
  if (expr) { \
    g_pass++; \
  } else { \
    g_fail++; \
    fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
  } \
} while (0)

#define CHECK_EQ(a, b) do { \
  if ((a) == (b)) { \
    g_pass++; \
  } else { \
    g_fail++; \
    fprintf(stderr, "FAIL: %s:%d: expected %s == %s\n", __FILE__, __LINE__, #a, #b); \
  } \
} while (0)

#define CHECK_STR_EQ(a, b) do { \
  std::string sa = (a); \
  std::string sb = (b); \
  if (sa == sb) { \
    g_pass++; \
  } else { \
    g_fail++; \
    fprintf(stderr, "FAIL: %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, sa.c_str(), sb.c_str()); \
  } \
} while (0)

static Client s_dummyClient;

// 1. 正常数组反序列化测试
static void testStreamArrayNormal() {
  printf("Test: StreamArray normal array...\n");
  g_mockHttp.reset();
  g_mockHttp.stream.setData(R"([{"id":1,"name":"alice"},{"id":2,"name":"bob"},{"id":3,"name":"charlie"}])");

  HttpJsonOptions opt(1000, 2000);
  String err;
  std::vector<int> ids;
  std::vector<std::string> names;

  bool ok = fetchJsonStreamArray(s_dummyClient, "http://example.com/api", nullptr, nullptr, opt,
    [&](JsonDocument& elem, int idx) {
      CHECK_EQ(idx, (int)ids.size());
      ids.push_back(elem["id"].as<int>());
      names.push_back(elem["name"].as<std::string>());
      return true;
    }, err);

  CHECK(ok);
  CHECK_STR_EQ(err.c_str(), "");
  CHECK_EQ(ids.size(), 3);
  if (ids.size() == 3) {
    CHECK_EQ(ids[0], 1);
    CHECK_EQ(ids[1], 2);
    CHECK_EQ(ids[2], 3);
    CHECK_STR_EQ(names[0], "alice");
    CHECK_STR_EQ(names[1], "bob");
    CHECK_STR_EQ(names[2], "charlie");
  }
  CHECK(g_mockHttp.endCalled);
}

// 2. 带空白换行的美化 JSON
static void testStreamArrayFormatted() {
  printf("Test: StreamArray formatted JSON with whitespace...\n");
  g_mockHttp.reset();
  const char* json =
    "  [\n"
    "    {\n"
    "      \"val\": 100\n"
    "    },\n"
    "    {\n"
    "      \"val\": 200\n"
    "    }\n"
    "  ]\n";
  g_mockHttp.stream.setData(json);

  HttpJsonOptions opt(1000, 2000);
  String err;
  std::vector<int> vals;

  bool ok = fetchJsonStreamArray(s_dummyClient, "http://example.com/api", nullptr, nullptr, opt,
    [&](JsonDocument& elem, int idx) {
      vals.push_back(elem["val"].as<int>());
      return true;
    }, err);

  CHECK(ok);
  CHECK_STR_EQ(err.c_str(), "");
  CHECK_EQ(vals.size(), 2);
  if (vals.size() == 2) {
    CHECK_EQ(vals[0], 100);
    CHECK_EQ(vals[1], 200);
  }
}

// 2b. 嵌套对象/数组与纯标量数组测试
static void testStreamArrayNestedAndScalars() {
  printf("Test: StreamArray nested objects and scalar arrays...\n");
  // 嵌套数组与对象
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"([{"meta":{"v":1},"tags":["a","b"]},{"meta":{"v":2},"tags":["c"]}])");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int items = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument& elem, int idx) {
        items++;
        if (idx == 0) {
          CHECK_EQ(elem["meta"]["v"].as<int>(), 1);
          CHECK_STR_EQ(elem["tags"][0].as<std::string>(), "a");
          CHECK_STR_EQ(elem["tags"][1].as<std::string>(), "b");
        } else if (idx == 1) {
          CHECK_EQ(elem["meta"]["v"].as<int>(), 2);
          CHECK_STR_EQ(elem["tags"][0].as<std::string>(), "c");
        }
        return true;
      }, err);
    CHECK(ok);
    CHECK_EQ(items, 2);
  }
  // 字符串数组
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"(["hello", "cardputer", "world"])");
    HttpJsonOptions opt(1000, 2000);
    String err;
    std::vector<std::string> words;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument& elem, int) {
        words.push_back(elem.as<std::string>());
        return true;
      }, err);
    CHECK(ok);
    CHECK_EQ(words.size(), 3);
    if (words.size() == 3) {
      CHECK_STR_EQ(words[0], "hello");
      CHECK_STR_EQ(words[1], "cardputer");
      CHECK_STR_EQ(words[2], "world");
    }
  }
  // 布尔/空值数组
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"([true, false, null])");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int count = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument& elem, int idx) {
        count++;
        if (idx == 0) CHECK(elem.as<bool>() == true);
        if (idx == 1) CHECK(elem.as<bool>() == false);
        if (idx == 2) CHECK(elem.isNull());
        return true;
      }, err);
    CHECK(ok);
    CHECK_EQ(count, 3);
  }
}

// 3. 空数组测试（纯 [] 和带空白 [   ]）
static void testStreamArrayEmpty() {
  printf("Test: StreamArray empty array...\n");
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData("[]");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument&, int) { called++; return true; }, err);
    CHECK(ok);
    CHECK_EQ(called, 0);
    CHECK_STR_EQ(err.c_str(), "");
  }
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData("  [ \t \r\n ]  ");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument&, int) { called++; return true; }, err);
    CHECK(ok);
    CHECK_EQ(called, 0);
    CHECK_STR_EQ(err.c_str(), "");
  }
}

// 4. 带 arrayKey 查找测试
static void testStreamArrayWithKey() {
  printf("Test: StreamArray with arrayKey...\n");
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"({"status":"ok","results":[{"n":10},{"n":20}]})");
    HttpJsonOptions opt(1000, 2000);
    String err;
    std::vector<int> nums;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", "\"results\":[", nullptr, opt,
      [&](JsonDocument& elem, int) {
        nums.push_back(elem["n"].as<int>());
        return true;
      }, err);
    CHECK(ok);
    CHECK_EQ(nums.size(), 2);
    if (nums.size() == 2) {
      CHECK_EQ(nums[0], 10);
      CHECK_EQ(nums[1], 20);
    }
  }
  // 带 key 的空数组
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"({"status":"empty","results":[]})");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", "\"results\":[", nullptr, opt,
      [&](JsonDocument&, int) { called++; return true; }, err);
    CHECK(ok);
    CHECK_EQ(called, 0);
  }
  // arrayKey 不匹配
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"({"status":"empty","other":[]})");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", "\"results\":[", nullptr, opt,
      [&](JsonDocument&, int) { called++; return true; }, err);
    CHECK(!ok);
    CHECK_EQ(called, 0);
    CHECK_STR_EQ(err.c_str(), "array not found");
  }
}

// 5. 中途截断测试
static void testStreamArrayTruncated() {
  printf("Test: StreamArray truncated stream...\n");
  // 元素内部截断
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"([{"id":1},{"id":)");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument& elem, int idx) {
        called++;
        CHECK_EQ(elem["id"].as<int>(), 1);
        return true;
      }, err);
    CHECK(!ok);
    CHECK_EQ(called, 1);
    CHECK(err.startsWith("json error:"));
  }
  // 逗号后无后续字符（截断）
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"([{"id":1},)");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument& elem, int idx) {
        called++;
        return true;
      }, err);
    CHECK(!ok);
    CHECK_EQ(called, 1);
    CHECK(err.startsWith("json error:"));
  }
  // 元素结束后直接 EOF，缺少 ']'
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"([{"id":1})");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument& elem, int idx) {
        called++;
        return true;
      }, err);
    CHECK(!ok);
    CHECK_EQ(called, 1);
    CHECK_STR_EQ(err.c_str(), "stream timeout");
  }
}

// 6. 分隔符异常测试
static void testStreamArrayBadSeparator() {
  printf("Test: StreamArray bad separator...\n");
  // 用分号而不是逗号
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"([{"id":1};{"id":2}])");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument&, int) { called++; return true; }, err);
    CHECK(!ok);
    CHECK_EQ(called, 1);
    CHECK_STR_EQ(err.c_str(), "unexpected separator");
  }
  // 缺失分隔符（用空格分隔两个对象）
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"([{"id":1} {"id":2}])");
    HttpJsonOptions opt(1000, 2000);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument&, int) { called++; return true; }, err);
    CHECK(!ok);
    CHECK_EQ(called, 1);
    CHECK_STR_EQ(err.c_str(), "unexpected separator");
  }
}

// 7. 超时测试
static void testStreamArrayTimeout() {
  printf("Test: StreamArray stream timeout...\n");
  // 首字符超时（空流，timeout 触发）
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData("");
    HttpJsonOptions opt(1000, 100);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument&, int) { called++; return true; }, err);
    CHECK(!ok);
    CHECK_EQ(called, 0);
    CHECK_STR_EQ(err.c_str(), "stream timeout");
  }
  // 全空白字符流（不断消耗空白直到超时）
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData("   \t  \n  ");
    HttpJsonOptions opt(1000, 100);
    String err;
    int called = 0;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument&, int) { called++; return true; }, err);
    CHECK(!ok);
    CHECK_EQ(called, 0);
    CHECK_STR_EQ(err.c_str(), "stream timeout");
  }
}

// 8. maxItems 截断与回调提前停止
static void testStreamArrayLimitsAndEarlyStop() {
  printf("Test: StreamArray maxItems and early stop...\n");
  // maxItems 截断
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"([{"n":1},{"n":2},{"n":3},{"n":4},{"n":5}])");
    HttpJsonOptions opt(1000, 2000);
    String err;
    std::vector<int> nums;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument& elem, int) {
        nums.push_back(elem["n"].as<int>());
        return true;
      }, err, 2);
    CHECK(ok);
    CHECK_EQ(nums.size(), 2);
    if (nums.size() == 2) {
      CHECK_EQ(nums[0], 1);
      CHECK_EQ(nums[1], 2);
    }
  }
  // onItem 返回 false 提前停止
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"([{"n":10},{"n":20},{"n":30},{"n":40}])");
    HttpJsonOptions opt(1000, 2000);
    String err;
    std::vector<int> nums;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
      [&](JsonDocument& elem, int idx) {
        nums.push_back(elem["n"].as<int>());
        if (idx == 1) return false; // 消费完第 2 项后退出
        return true;
      }, err);
    CHECK(ok);
    CHECK_EQ(nums.size(), 2);
    if (nums.size() == 2) {
      CHECK_EQ(nums[0], 10);
      CHECK_EQ(nums[1], 20);
    }
  }
}

// 9. Filter 过滤测试
static void testStreamArrayFilter() {
  printf("Test: StreamArray with JsonDocument filter...\n");
  g_mockHttp.reset();
  g_mockHttp.stream.setData(R"([{"keep":1,"drop":"big_payload_1"},{"keep":2,"drop":"big_payload_2"}])");
  HttpJsonOptions opt(1000, 2000);

  JsonDocument filter;
  filter["keep"] = true;

  String err;
  std::vector<int> keeps;
  bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, &filter, opt,
    [&](JsonDocument& elem, int) {
      keeps.push_back(elem["keep"].as<int>());
      CHECK(elem["drop"].isNull());
      return true;
    }, err);

  CHECK(ok);
  CHECK_EQ(keeps.size(), 2);
  if (keeps.size() == 2) {
    CHECK_EQ(keeps[0], 1);
    CHECK_EQ(keeps[1], 2);
  }
}

// 10. HTTP 失败场景（HTTP code != 200, begin failed）
static void testHttpFailures() {
  printf("Test: HTTP failures in fetchJsonStreamArray and fetchJsonHttp...\n");
  // 404 Not Found
  {
    g_mockHttp.reset();
    g_mockHttp.httpCode = 404;
    HttpJsonOptions opt(1000, 2000);
    String err;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test/404", nullptr, nullptr, opt,
      [](JsonDocument&, int) { return true; }, err);
    CHECK(!ok);
    CHECK_STR_EQ(err.c_str(), "http 404");
  }
  // begin failed
  {
    g_mockHttp.reset();
    g_mockHttp.beginSuccess = false;
    HttpJsonOptions opt(1000, 2000);
    String err;
    bool ok = fetchJsonStreamArray(s_dummyClient, "http://test/fail", nullptr, nullptr, opt,
      [](JsonDocument&, int) { return true; }, err);
    CHECK(!ok);
    CHECK_STR_EQ(err.c_str(), "http begin failed");
  }
}

// 11. fetchJsonHttp 测试
static void testFetchJsonHttp() {
  printf("Test: fetchJsonHttp stream and non-stream...\n");
  // stream = true 正常解析
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"({"code":0,"data":{"value":42}})");
    HttpJsonOptions opt(1000, 2000);
    opt.stream = true;
    JsonDocument doc;
    String err;
    bool ok = fetchJsonHttp(s_dummyClient, "http://test", doc, nullptr, opt, err);
    CHECK(ok);
    CHECK_EQ(doc["data"]["value"].as<int>(), 42);
    CHECK_STR_EQ(err.c_str(), "");
  }
  // stream = false 正常解析 (通过 getString)
  {
    g_mockHttp.reset();
    g_mockHttp.responseBody = R"({"name":"cardputer","active":true})";
    HttpJsonOptions opt(1000, 2000);
    opt.stream = false;
    JsonDocument doc;
    String err;
    bool ok = fetchJsonHttp(s_dummyClient, "http://test", doc, nullptr, opt, err);
    CHECK(ok);
    CHECK_STR_EQ(doc["name"].as<std::string>(), "cardputer");
    CHECK_EQ(doc["active"].as<bool>(), true);
  }
  // filter 过滤
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData(R"({"keep":99,"ignore":"secret"})");
    HttpJsonOptions opt(1000, 2000);
    JsonDocument filter;
    filter["keep"] = true;
    JsonDocument doc;
    String err;
    bool ok = fetchJsonHttp(s_dummyClient, "http://test", doc, &filter, opt, err);
    CHECK(ok);
    CHECK_EQ(doc["keep"].as<int>(), 99);
    CHECK(doc["ignore"].isNull());
  }
  // HTTP 非 200 错误
  {
    g_mockHttp.reset();
    g_mockHttp.httpCode = 500;
    HttpJsonOptions opt(1000, 2000);
    JsonDocument doc;
    String err;
    bool ok = fetchJsonHttp(s_dummyClient, "http://test", doc, nullptr, opt, err);
    CHECK(!ok);
    CHECK_STR_EQ(err.c_str(), "http 500");
  }
  // begin 失败
  {
    g_mockHttp.reset();
    g_mockHttp.beginSuccess = false;
    HttpJsonOptions opt(1000, 2000);
    JsonDocument doc;
    String err;
    bool ok = fetchJsonHttp(s_dummyClient, "http://test", doc, nullptr, opt, err);
    CHECK(!ok);
    CHECK_STR_EQ(err.c_str(), "http begin failed");
  }
  // JSON 反序列化语法错误 (含详细信息)
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData("{invalid_json}");
    HttpJsonOptions opt(1000, 2000);
    opt.includeJsonDetail = true;
    JsonDocument doc;
    String err;
    bool ok = fetchJsonHttp(s_dummyClient, "http://test", doc, nullptr, opt, err);
    CHECK(!ok);
    CHECK(err.startsWith("json: "));
  }
  // JSON 反序列化语法错误 (不含详细信息，自定义前缀)
  {
    g_mockHttp.reset();
    g_mockHttp.stream.setData("{invalid_json}");
    HttpJsonOptions opt(1000, 2000);
    opt.jsonError = "parse error";
    opt.includeJsonDetail = false;
    JsonDocument doc;
    String err;
    bool ok = fetchJsonHttp(s_dummyClient, "http://test", doc, nullptr, opt, err);
    CHECK(!ok);
    CHECK_STR_EQ(err.c_str(), "parse error");
  }
}

// 12. HTTP 头部、UA、超时等配置传递验证
static void testHttpOptionsConfig() {
  printf("Test: HTTP options UA, Auth, timeouts config...\n");
  g_mockHttp.reset();
  g_mockHttp.stream.setData("[]");
  HttpJsonOptions opt(1234, 5678, "custom-agent/2.0", "Bearer secret-token");
  String err;
  bool ok = fetchJsonStreamArray(s_dummyClient, "http://test", nullptr, nullptr, opt,
    [](JsonDocument&, int) { return true; }, err);
  CHECK(ok);
  CHECK_STR_EQ(g_mockHttp.capturedUserAgent, "custom-agent/2.0");
  CHECK_STR_EQ(g_mockHttp.capturedAuth, "Bearer secret-token");
  CHECK_EQ(g_mockHttp.capturedConnectTimeout, 1234);
  CHECK_EQ(g_mockHttp.capturedTimeout, 5678);
  CHECK(g_mockHttp.useHttp10Called);
}

// ---- DNS 预解析（net_resolve.h）----
static void testNetUrlHost() {
  printf("Test: netUrlHost extracts the host...\n");
  char h[64];
  CHECK(netUrlHost("https://api.adsbdb.com/v0/callsign/CCA123", h, sizeof(h)));
  CHECK_STR_EQ(std::string(h), std::string("api.adsbdb.com"));
  CHECK(netUrlHost("http://ip-api.com/json/?fields=lat", h, sizeof(h)));
  CHECK_STR_EQ(std::string(h), std::string("ip-api.com"));
  CHECK(netUrlHost("http://192.168.1.1:9090/traffic", h, sizeof(h)));
  CHECK_STR_EQ(std::string(h), std::string("192.168.1.1"));
  CHECK(netUrlHost("https://h.example?x=1", h, sizeof(h)));
  CHECK_STR_EQ(std::string(h), std::string("h.example"));
  CHECK(netUrlHost("example.org/path", h, sizeof(h)));          // 没写协议头也行
  CHECK_STR_EQ(std::string(h), std::string("example.org"));
  CHECK(!netUrlHost("https:///path", h, sizeof(h)));             // 空 host
  CHECK(!netUrlHost("", h, sizeof(h)));
  CHECK(!netUrlHost(nullptr, h, sizeof(h)));
  char tiny[8];
  CHECK(!netUrlHost("https://much-too-long.example/", tiny, sizeof(tiny)));   // 放不下就拒绝，不截断
  CHECK(netUrlHost("https://a.bc/", tiny, sizeof(tiny)));
  CHECK_STR_EQ(std::string(tiny), std::string("a.bc"));
  // user:pass@host 和 [IPv6] 不预解析（交给 HTTPClient），别把 "user" 当主机名去查
  CHECK(!netUrlHost("http://user:pw@host.example/x", h, sizeof(h)));
  CHECK(!netUrlHost("http://user@host.example/x", h, sizeof(h)));
  CHECK(!netUrlHost("http://[::1]:8080/x", h, sizeof(h)));
  // 路径里的 @ 不算 userinfo
  CHECK(netUrlHost("https://api.example.com/users/@me", h, sizeof(h)));
  CHECK_STR_EQ(std::string(h), std::string("api.example.com"));
  CHECK(netUrlHost("http://10.0.0.1:9090/proxies?x=a@b", h, sizeof(h)));
  CHECK_STR_EQ(std::string(h), std::string("10.0.0.1"));
}

static void testPreResolveGatesHttp() {
  printf("Test: failed DNS pre-resolve never reaches HTTP...\n");
  Client client;
  HttpJsonOptions opt(1000, 1000);

  g_mockHttp.reset();
  g_mockHttp.stream.setData(R"({"ok":1})");
  g_resolveResult = NR_TIMEOUT;
  JsonDocument doc;
  String err;
  CHECK(!fetchJsonHttp(client, String("https://api.example.com/v1/x"), doc, nullptr, opt, err));
  CHECK_STR_EQ(std::string(err.c_str()), std::string("dns timeout"));
  CHECK_STR_EQ(g_resolveHost, std::string("api.example.com"));
  CHECK(!g_mockHttp.beginCalled);

  g_mockHttp.reset();
  g_resolveResult = NR_CANCELLED;
  err = "";
  int items = 0;
  CHECK(!fetchJsonStreamArray(client, String("http://feed.example/all.json"), nullptr, nullptr, opt,
                              [&](JsonDocument&, int) { items++; return true; }, err));
  CHECK_STR_EQ(std::string(err.c_str()), std::string("cancelled"));
  CHECK(!g_mockHttp.beginCalled);
  CHECK(items == 0);

  g_mockHttp.reset();
  g_resolveResult = NR_FAIL;
  err = "";
  CHECK(!fetchJsonHttp(client, String("http://nx.example/"), doc, nullptr, opt, err));
  CHECK_STR_EQ(std::string(err.c_str()), std::string("dns failed"));

  // 解析成功才照常往下走
  g_mockHttp.reset();
  g_mockHttp.stream.setData(R"({"ok":1})");
  g_resolveResult = NR_OK;
  err = "";
  CHECK(fetchJsonHttp(client, String("https://api.example.com/v1/x"), doc, nullptr, opt, err));
  CHECK(g_mockHttp.beginCalled);
  CHECK(doc["ok"] == 1);
}

int main() {
  printf("=== Starting jsontest ===\n");
  testStreamArrayNormal();
  testStreamArrayFormatted();
  testStreamArrayNestedAndScalars();
  testStreamArrayEmpty();
  testStreamArrayWithKey();
  testStreamArrayTruncated();
  testStreamArrayBadSeparator();
  testStreamArrayTimeout();
  testStreamArrayLimitsAndEarlyStop();
  testStreamArrayFilter();
  testHttpFailures();
  testFetchJsonHttp();
  testHttpOptionsConfig();
  testNetUrlHost();
  testPreResolveGatesHttp();

  printf("\n=== jsontest Results: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
