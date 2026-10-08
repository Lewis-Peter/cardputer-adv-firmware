#include "router.h"
#include "router_types.h"
#include "router_hosts.h"
#include "router_hist.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "ui_common.h"
#include "net_job.h"
#include "wifi_net.h"
#include "secrets.h"
#include "http_json.h"

static const uint32_t POLL_MS = 1500;
static const int HIST_N = 60;          // 下行/上行速率曲线保留的采样点数

static long      histDown[HIST_N];
static long      histUp[HIST_N];
static int       histHead = 0, histCount = 0;
static long      spdDown = 0, spdUp = 0, memInuse = 0;
static long long totalDown = 0, totalUp = 0;
static int       conns = 0, delayMs = 0;
static char      nodeName[32] = "";
static char      version[16] = "";
static bool      online = false;
static String    errMsg = "";

enum RouterPage {
  R_PAGE_TRAFFIC = 0,
  R_PAGE_FLOW    = 1,
  R_PAGE_TYPES   = 2,
  R_PAGE_NODES   = 3,
  R_PAGE_HOSTS   = 4,
  R_PAGE_HIST    = 5
};
static RouterPage routerPage = R_PAGE_TRAFFIC;

struct ProxyNode {
  char name[32];
  int  delay;    // 0 = 未测, >0 = ms, -1 = 超时
  bool isNow;
};
static const int MAX_NODES = 16;
static ProxyNode proxyNodes[MAX_NODES];
static int       proxyNodeCount = 0;
static int       nodeCursor = 0;
static int       nodeScroll = 0;
static String    switchStatus = "";
static uint32_t  switchStatusTime = 0;

// ---- /traffic 常开流 ----
static WiFiClient trafClient;
static HTTPClient trafHttp;
static bool       trafOpen = false;
static String     trafBuf;

static uint32_t  lastPollMs = 0;
static uint32_t  lastOpenTryMs = 0;
static int       tick = 0;
static String    clashErr;

static bool clashLine(const char* path, String& out, int skipObjects = 0) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2500);
  char url[160];
  snprintf(url, sizeof(url), "%s%s", CLASH_BASE, path);
  clashErr = "";
  if (!http.begin(client, url)) { clashErr = "http begin failed"; return false; }
  http.addHeader("Authorization", String("Bearer ") + CLASH_SECRET);
  int code = http.GET();
  if (code != 200) { clashErr = String("http ") + code; http.end(); return false; }

  WiFiClient* s = http.getStreamPtr();
  out = "";
  bool started = false;
  int skip = skipObjects;
  uint32_t t0 = millis();
  while (millis() - t0 < 1200) {
    if (!s->available()) { delay(5); continue; }
    char ch = s->read();
    if (!started) { if (ch == '{') { started = true; out += ch; } }
    else {
      out += ch;
      if (ch == '}') {
        if (skip <= 0) break;
        skip--; started = false; out = "";
      }
    }
    if (out.length() > 400) break;
  }
  http.end();
  return started && out.indexOf('}') > 0;
}

// RFC 3986 百分号编码，用于转义含中文、emoji、空格的 Clash 节点与组名
static String urlEncode(const char* src) {
  if (!src) return "";
  String out;
  out.reserve(strlen(src) * 3);
  const char* hex = "0123456789ABCDEF";
  for (const char* p = src; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

static bool clashGet(const char* path, JsonDocument& doc) {
  WiFiClient client;
  char url[256];
  snprintf(url, sizeof(url), "%s%s", CLASH_BASE, path);
  String authorization = String("Bearer ") + CLASH_SECRET;
  HttpJsonOptions options(2500, 4000, nullptr, authorization.c_str());
  options.stream = false;
  clashErr = "";
  return fetchJsonHttp(client, url, doc, nullptr, options, clashErr);
}

// Fixed-size aggregation; full names remain intact for identity comparisons.
struct RouterFlow {
  String names[3][3];
  int counts[3][4] = {};
  int links[2][4][4] = {};
  int total = 0;
  int bucket(int col, const char* name) {
    for (int i = 0; i < 3; ++i) {
      if (names[col][i] == name) return i;
      if (!names[col][i].length()) { names[col][i] = name; return i; }
    }
    return 3;
  }
};
static RouterFlow flow;
static bool flowValid = false, flowLimited = false;
static uint32_t flowUpdated = 0;
static String flowError;
static int flowFocus = -1;

static RouterTypeStats typeStats;
static bool typeByBytes = false;
static RouterHostStats hostStats;
static bool hostByBytes = true;
static RouterHistory hist;     // 每 12 秒一个点，约 24 分钟

static int clashCountConns() {
  RouterFlow next;
  RouterTypeStats nextTypes;
  RouterHostStats nextHosts;
  WiFiClient client;
  String auth = String("Bearer ") + CLASH_SECRET;
  HttpJsonOptions options(2500, 2500, nullptr, auth.c_str());
  JsonDocument filter;
  filter["metadata"]["sourceIP"] = true;
  filter["metadata"]["network"] = true;
  filter["metadata"]["host"] = true;
  filter["metadata"]["destinationIP"] = true;
  filter["metadata"]["destinationPort"] = true;
  filter["download"] = true;
  filter["upload"] = true;
  filter["rule"] = true;
  filter["chains"][0] = true;
  uint32_t started = millis();
  bool limited = false;
  String err;
  bool ok = fetchJsonStreamArray(client, String(CLASH_BASE) + "/connections",
      "\"connections\"", &filter, options,
      [&](JsonDocument& item, int) {
        // Check before consuming: only a further item proves the data was truncated.
        if (next.total >= 1000 || millis() - started >= 2500) { limited = true; return false; }
        const char* source = item["metadata"]["sourceIP"] | "LOCAL";
        const char* rule = item["rule"] | "UNKNOWN";
        // mihomo chains are ordered from the final outbound back to the group.
        const char* exit = item["chains"][0] | "UNKNOWN";
        int ids[3] = { next.bucket(0, source), next.bucket(1, rule), next.bucket(2, exit) };
        for (int c = 0; c < 3; ++c) ++next.counts[c][ids[c]];
        ++next.links[0][ids[0]][ids[1]];
        ++next.links[1][ids[1]][ids[2]];
        ++next.total;
        addRouterType(nextTypes, item);
        addRouterHost(nextHosts, item);
        return true;
      }, err, 1001);
  if (!ok) { flowError = err; return -1; }
  flow = next;
  typeStats = nextTypes;
  hostStats = nextHosts;
  if (flowFocus >= 0 && !flow.counts[flowFocus/4][flowFocus%4]) flowFocus = -1;
  flowValid = true;
  flowLimited = limited;
  flowUpdated = millis();
  flowError = "";
  return next.total;
}

static void trafficClose() {
  if (trafOpen) { trafHttp.end(); trafOpen = false; }
  trafClient.stop();
  trafBuf = "";
}

static bool trafficEnsureOpen() {
  if (trafOpen) return true;
  char url[160];
  snprintf(url, sizeof(url), "%s/traffic", CLASH_BASE);
  trafHttp.setConnectTimeout(2500);
  trafHttp.setTimeout(4000);
  trafHttp.setReuse(false);
  clashErr = "";
  if (!trafHttp.begin(trafClient, url)) { clashErr = "http begin failed"; return false; }
  trafHttp.addHeader("Authorization", String("Bearer ") + CLASH_SECRET);
  int code = trafHttp.GET();
  if (code != 200) { clashErr = String("http ") + code; trafHttp.end(); return false; }
  trafOpen = true;
  trafBuf = "";
  return true;
}

static bool trafficPoll(String& out) {
  if (!trafOpen) return false;
  WiFiClient* s = trafHttp.getStreamPtr();
  if (!s) { trafficClose(); return false; }
  bool got = false;
  while (s->available()) {
    char c = (char)s->read();
    if (trafBuf.length() == 0) {
      if (c == '{') trafBuf += c;
    } else {
      trafBuf += c;
      if (c == '}')                 { out = trafBuf; trafBuf = ""; got = true; }
      else if (trafBuf.length() > 400) trafBuf = "";
    }
  }
  if (!s->connected() && !s->available()) trafficClose();
  return got;
}

void routerExit() {
  trafficClose();
}

static void setErrFromClashErr() {
  if (clashErr.startsWith("http 401") || clashErr.startsWith("http 403"))
    errMsg = "auth failed - check CLASH_SECRET";
  else if (clashErr.startsWith("http "))
    errMsg = clashErr + " from " + CLASH_BASE;
  else
    errMsg = String("no reply from ") + CLASH_BASE;
}

static void pumpTraffic() {
  if (!wifiEnsureConnected()) {
    if (online || !errMsg.length()) { online = false; errMsg = "wifi not connected"; dirty = true; }
    trafficClose();
    return;
  }
  if (!trafOpen) {
    if (millis() - lastOpenTryMs < POLL_MS) return;
    lastOpenTryMs = millis();
    if (!trafficEnsureOpen()) { online = false; setErrFromClashErr(); dirty = true; return; }
  }

  String t;
  if (!trafficPoll(t)) return;

  JsonDocument d;
  if (deserializeJson(d, t)) return;
  spdDown   = d["down"]      | 0L;
  spdUp     = d["up"]        | 0L;
  totalDown = d["downTotal"] | 0LL;
  totalUp   = d["upTotal"]   | 0LL;
  histDown[histHead] = spdDown;
  histUp[histHead]   = spdUp;
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
  online = true;
  errMsg = "";
  dirty = true;
}

static bool switchProxyNode(const char* target) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2500);
  String url = String(CLASH_BASE) + "/proxies/" + urlEncode(CLASH_PROXY_GROUP);
  if (!http.begin(client, url)) return false;
  http.addHeader("Authorization", String("Bearer ") + CLASH_SECRET);
  http.addHeader("Content-Type", "application/json");
  JsonDocument bodyDoc;
  bodyDoc["name"] = target;
  String payload;
  serializeJson(bodyDoc, payload);
  int code = http.PUT(payload);
  http.end();
  if (code == 204 || code == 200) {
    strncpy(nodeName, target, sizeof(nodeName) - 1);
    nodeName[sizeof(nodeName) - 1] = 0;
    for (int i = 0; i < proxyNodeCount; i++) {
      proxyNodes[i].isNow = (strcmp(proxyNodes[i].name, target) == 0);
    }
    switchStatus = String("[OK] ACTIVE: ") + target;
    switchStatusTime = millis();
    return true;
  }
  switchStatus = String("[!] ERR: HTTP ") + code;
  switchStatusTime = millis();
  return false;
}

static void testNodeDelay(int idx) {
  if (idx < 0 || idx >= proxyNodeCount) return;
  String dp = String("/proxies/") + urlEncode(proxyNodes[idx].name) +
              "/delay?url=http://www.gstatic.com/generate_204&timeout=1500";
  JsonDocument d;
  if (clashGet(dp.c_str(), d)) {
    proxyNodes[idx].delay = d["delay"] | -1;
    if (proxyNodes[idx].isNow) delayMs = proxyNodes[idx].delay;
  } else {
    proxyNodes[idx].delay = -1;
  }
}

static void pollRouter() {
  lastPollMs = millis();
  if (!online) return;

  if (tick % 8 == 0) {
    String m;
    if (clashLine("/memory", m, 1)) {
      JsonDocument d;
      if (!deserializeJson(d, m)) memInuse = d["inuse"] | 0L;
    }
  }

  if (tick % 4 == 0) {
    int c = clashCountConns();
    if (c >= 0) conns = c;
  }
  if (tick % 6 == 0) {
    JsonDocument d;
    String gp = String("/proxies/") + urlEncode(CLASH_PROXY_GROUP);
    if (clashGet(gp.c_str(), d)) {
      const char* n = d["now"] | "--";
      strncpy(nodeName, n, sizeof(nodeName) - 1);
      nodeName[sizeof(nodeName) - 1] = 0;

      JsonArray arr = d["all"].as<JsonArray>();
      if (!arr.isNull()) {
        proxyNodeCount = 0;
        for (JsonVariant v : arr) {
          if (proxyNodeCount >= MAX_NODES) break;
          const char* nm = v.as<const char*>();
          if (nm) {
            strncpy(proxyNodes[proxyNodeCount].name, nm, sizeof(proxyNodes[proxyNodeCount].name) - 1);
            proxyNodes[proxyNodeCount].name[sizeof(proxyNodes[proxyNodeCount].name) - 1] = 0;
            proxyNodes[proxyNodeCount].isNow = (strcmp(proxyNodes[proxyNodeCount].name, nodeName) == 0);
            if (proxyNodes[proxyNodeCount].delay == 0 && proxyNodes[proxyNodeCount].isNow && delayMs > 0) {
              proxyNodes[proxyNodeCount].delay = delayMs;
            }
            proxyNodeCount++;
          }
        }
        if (nodeCursor >= proxyNodeCount && proxyNodeCount > 0) nodeCursor = proxyNodeCount - 1;
      }
    }
    if (!version[0]) {
      JsonDocument vd;
      if (clashGet("/version", vd)) {
        const char* vv = vd["version"] | "";
        strncpy(version, vv, sizeof(version) - 1);
        version[sizeof(version) - 1] = 0;
      }
    }
  }
  if (tick % 20 == 0 && nodeName[0]) {
    String dp = String("/proxies/") + urlEncode(nodeName) +
                "/delay?url=http://www.gstatic.com/generate_204&timeout=1500";
    JsonDocument d;
    if (clashGet(dp.c_str(), d)) delayMs = d["delay"] | 0;
  }

  if (tick % 8 == 0 && memInuse > 0) hist.push(int32_t(memInuse / 1024), conns);
  tick++;
  dirty = true;
}

static DeferredFetch job;

void routerEnter() {
  trafficClose();
  lastOpenTryMs = 0;
  tick = 0; online = false; errMsg = "";
  routerPage = R_PAGE_TRAFFIC;
  switchStatus = "";
  flowValid = false; flowError = ""; flowFocus = -1; typeByBytes = false; hostByBytes = true; hist.clear();
  job.request();
  lastPollMs = millis();
  dirty = true;
}

void routerUpdate() {
  pumpTraffic();
  if (job.due()) { pollRouter(); return; }
  if (millis() - lastPollMs >= POLL_MS) pollRouter();
}

bool routerKey(char k) {
  if (k == '`') {
    if (routerPage != R_PAGE_TRAFFIC) {
      routerPage = R_PAGE_TRAFFIC;
      dirty = true;
      return false;
    }
    return true; // 返回主菜单
  }

  if (k == 'n' || k == 'N') {
    routerPage = static_cast<RouterPage>((routerPage + 1) % 6);
    dirty = true;
    return false;
  }

  if (routerPage == R_PAGE_TRAFFIC) {
    if (k == 'r' || k == 'R') {
      version[0] = 0; tick = 0; job.request();
    }
  } else if (routerPage == R_PAGE_FLOW) {
    if (k == 'r' || k == 'R') { tick = 0; job.request(); }
    if (k == ';' || k == ',' || k == '.' || k == '/') {
      int step = (k == ';' || k == ',') ? -1 : 1;
      int candidate = flowFocus < 0 ? (step > 0 ? 11 : 0) : flowFocus;
      for (int n = 0; n < 12; ++n) {
        candidate = (candidate + step + 12) % 12;
        if (flow.counts[candidate/4][candidate%4]) { flowFocus = candidate; break; }
      }
    }
  } else if (routerPage == R_PAGE_TYPES) {
    if (k == 'r' || k == 'R') { tick = 0; job.request(); }
    if (k == 'm' || k == 'M') typeByBytes = !typeByBytes;
  } else if (routerPage == R_PAGE_HIST) {
    if (k == 'r' || k == 'R') { tick = 0; job.request(); }
  } else if (routerPage == R_PAGE_HOSTS) {
    if (k == 'r' || k == 'R') { tick = 0; job.request(); }
    if (k == 'm' || k == 'M') hostByBytes = !hostByBytes;
  } else { // R_PAGE_NODES
    if (k == ';' || k == ',') { // UP
      if (nodeCursor > 0) {
        nodeCursor--;
        if (nodeCursor < nodeScroll) nodeScroll = nodeCursor;
      }
    } else if (k == '.' || k == '/') { // DOWN
      if (nodeCursor < proxyNodeCount - 1) {
        nodeCursor++;
        if (nodeCursor >= nodeScroll + 4) nodeScroll = nodeCursor - 3;
      }
    } else if (k == '\n') { // Enter -> 切换节点
      if (proxyNodeCount > 0 && nodeCursor < proxyNodeCount) {
        switchProxyNode(proxyNodes[nodeCursor].name);
      }
    } else if (k == 't' || k == 'T' || k == ' ') { // 测速当前节点
      if (proxyNodeCount > 0 && nodeCursor < proxyNodeCount) {
        testNodeDelay(nodeCursor);
      }
    } else if (k == 'r' || k == 'R') {
      job.request();
    }
  }
  dirty = true;
  return false;
}

static void fmtSpd(long b, char* num, size_t nn, char* unit, size_t un) {
  if (b < 1024)          { snprintf(num, nn, "%ld", b);              snprintf(unit, un, "B/s"); }
  else if (b < 1048576)  { snprintf(num, nn, "%ld", b / 1024);       snprintf(unit, un, "KB/s"); }
  else                   { snprintf(num, nn, "%.2f", b / 1048576.0); snprintf(unit, un, "MB/s"); }
}

static void rStat(int cx, int y, int cw, int ch, const char* lab, const char* val, uint16_t vc) {
  cv.fillRoundRect(cx, y, cw, ch, 3, 0x0821);
  cv.drawRoundRect(cx, y, cw, ch, 3, 0x18C3);
  cv.setTextDatum(top_center); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString(lab, cx + cw / 2, y + 3);
  cv.setTextColor(vc, 0x0821);
  cv.drawString(val, cx + cw / 2, y + 15);
}

static void drawRouterTraffic() {
  char rightBuf[32];
  if (online) {
    const char* v = version;
    if (v[0] == 'v' || v[0] == 'V') v++;
    if (version[0]) snprintf(rightBuf, sizeof(rightBuf), "v%s * LIVE", v);
    else snprintf(rightBuf, sizeof(rightBuf), "* LIVE");
  } else {
    snprintf(rightBuf, sizeof(rightBuf), "OFFLINE");
  }
  drawPageHeader("Router", rightBuf, online ? 0x07E0 : TFT_RED);

  if (!online) {
    const int bx = 20, by = 36, bw = SW - 40, bh = 60;
    cv.fillRoundRect(bx, by, bw, bh, 4, 0x0821);
    cv.drawRoundRect(bx, by, bw, bh, 4, !errMsg.length() ? 0x18C3 : TFT_RED);

    cv.setTextDatum(top_center); cv.setTextSize(1);
    if (!errMsg.length()) {
      cv.setTextColor(0x07FF, 0x0821);
      cv.drawString("ROUTER GATEWAY", SW / 2, by + 10);
      cv.setTextColor(TFT_LIGHTGREY, 0x0821);
      cv.drawString("connecting stream...", SW / 2, by + 26);
      cv.setTextColor(TFT_DARKGREY, 0x0821);
      cv.drawString(trunc(CLASH_BASE, 30), SW / 2, by + 42);
      return;
    }

    cv.setTextColor(TFT_RED, 0x0821);
    cv.drawString("Connection Failed", SW / 2, by + 10);
    cv.setTextColor(TFT_WHITE, 0x0821);
    cv.drawString(trunc(errMsg, 30), SW / 2, by + 26);
    cv.setTextColor(TFT_DARKGREY, 0x0821);
    cv.drawString(errMsg.indexOf("CLASH_SECRET") >= 0
                    ? "check CLASH_SECRET  r=retry"
                    : "check CLASH_BASE  r=retry",
                  SW / 2, by + 42);
    return;
  }

  char num[16], unit[8], b[40];

  // 1. 顶部双向对称卡片 (Download & Upload)
  const int tby = 15, tbh = 26;
  // 下行卡片 (左)
  cv.fillRoundRect(4, tby, 114, tbh, 3, 0x0821);
  cv.drawRoundRect(4, tby, 114, tbh, 3, 0x18C3);
  cv.fillRect(4, tby, 2, tbh, 0x07E0); // 边缘光条
  cv.fillTriangle(10, tby + 8, 18, tby + 8, 14, tby + 16, 0x07E0);
  cv.setTextDatum(top_right); cv.setTextSize(1);
  cv.setTextColor(0x2965, 0x0821);
  cv.drawString("DL", 114, tby + 2);
  fmtSpd(spdDown, num, sizeof(num), unit, sizeof(unit));
  cv.setTextDatum(middle_left); cv.setTextSize(2);
  cv.setTextColor(0x07E0, 0x0821);
  cv.drawString(num, 23, tby + 13);
  int nx = 23 + cv.textWidth(num) + 3;
  cv.setTextSize(1);
  cv.setTextColor(0x8CD2, 0x0821);
  cv.drawString(unit, nx, tby + 16);

  // 上行卡片 (右)
  cv.fillRoundRect(122, tby, 114, tbh, 3, 0x0821);
  cv.drawRoundRect(122, tby, 114, tbh, 3, 0x18C3);
  cv.fillRect(122, tby, 2, tbh, 0xFBE0); // 边缘光条
  cv.fillTriangle(128, tby + 16, 136, tby + 16, 132, tby + 8, 0xFBE0);
  cv.setTextDatum(top_right); cv.setTextSize(1);
  cv.setTextColor(0x5288, 0x0821);
  cv.drawString("UP", 232, tby + 2);
  fmtSpd(spdUp, num, sizeof(num), unit, sizeof(unit));
  cv.setTextDatum(middle_left); cv.setTextSize(2);
  cv.setTextColor(0xFBE0, 0x0821);
  cv.drawString(num, 141, tby + 13);
  int ux = 141 + cv.textWidth(num) + 3;
  cv.setTextSize(1);
  cv.setTextColor(0xFDA0, 0x0821);
  cv.drawString(unit, ux, tby + 16);

  // 2. 双轨流量示波器卡片 (Dual Waveform Scope)
  const int gx = 4, gy = 44, gw = 232, gh = 44;
  cv.fillRoundRect(gx, gy, gw, gh, 3, 0x0821);
  cv.drawRoundRect(gx, gy, gw, gh, 3, 0x18C3);
  cv.fillRect(gx, gy, 2, gh, 0x07FF);

  // 网格虚线 (33% 与 66% 高度)
  int y33 = gy + gh * 1 / 3;
  int y66 = gy + gh * 2 / 3;
  for (int x = gx + 4; x < gx + gw - 4; x += 6) {
    cv.drawFastHLine(x, y33, 3, 0x1082);
    cv.drawFastHLine(x, y66, 3, 0x1082);
  }

  // 动态峰值计算
  long mx = 1024;
  for (int i = 0; i < histCount; i++) {
    if (histDown[i] > mx) mx = histDown[i];
    if (histUp[i] > mx)   mx = histUp[i];
  }

  // 绘制下行波形 (翡翠绿 + 渐变填充)
  int px = -1, py = 0;
  for (int i = 0; i < histCount; i++) {
    int idx = (histHead - histCount + i + HIST_N) % HIST_N;
    int x = gx + 2 + i * (gw - 4) / HIST_N;
    int y = gy + gh - 2 - (int)((long long)histDown[idx] * (gh - 4) / mx);
    if (y < gy + 2) y = gy + 2;
    cv.drawFastVLine(x, y, gy + gh - 2 - y, 0x0A24);
    if (px >= 0) {
      cv.drawLine(px, py, x, y, 0x07E0);
      cv.drawLine(px, py - 1, x, y - 1, 0x05E0);
    }
    px = x; py = y;
  }
  if (px >= 0) {
    cv.fillCircle(px, py, 2, 0x07FF);
  }

  // 绘制上行波形 (琥珀橙描线)
  int upx = -1, upy = 0;
  for (int i = 0; i < histCount; i++) {
    int idx = (histHead - histCount + i + HIST_N) % HIST_N;
    int x = gx + 2 + i * (gw - 4) / HIST_N;
    int y = gy + gh - 2 - (int)((long long)histUp[idx] * (gh - 4) / mx);
    if (y < gy + 2) y = gy + 2;
    if (upx >= 0) {
      cv.drawLine(upx, upy, x, y, 0xFBE0);
    }
    upx = x; upy = y;
  }
  if (upx >= 0) {
    cv.fillCircle(upx, upy, 2, 0xFBE0);
  }

  // 示波器左下角微图例
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.fillCircle(gx + 8, gy + gh - 5, 2, 0x07E0);
  cv.setTextColor(0x8CD2, 0x0821);
  cv.drawString("DL", gx + 13, gy + gh - 2);
  cv.fillCircle(gx + 32, gy + gh - 5, 2, 0xFBE0);
  cv.setTextColor(0xFDA0, 0x0821);
  cv.drawString("UP", gx + 37, gy + gh - 2);

  // 峰值胶囊标签
  fmtSpd(mx, num, sizeof(num), unit, sizeof(unit));
  snprintf(b, sizeof(b), "MAX %s%s", num, unit);
  int bw = cv.textWidth(b) + 8;
  int bx = gx + gw - bw - 4;
  cv.fillRoundRect(bx, gy + 3, bw, 10, 2, 0x0821);
  cv.drawRoundRect(bx, gy + 3, bw, 10, 2, 0x18C3);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString(b, bx + bw / 2, gy + 8);

  // 3. 底部四联指标遥测舱 (CONNS, MEM, PING, NODE)
  uint16_t dc = delayMs == 0    ? 0x8410
              : delayMs < 150   ? 0x07E0
              : delayMs < 300   ? 0xFDA0
                                : 0xF800;
  snprintf(b, sizeof(b), "%s%d", flowLimited ? ">=" : "", conns);                rStat(4,   91, 56, 27, "CONNS", b, TFT_WHITE);
  snprintf(b, sizeof(b), "%ldM", memInuse / 1048576); rStat(63,  91, 56, 27, "MEM",   b, 0xCE79);
  snprintf(b, sizeof(b), "%dms", delayMs);            rStat(122, 91, 56, 27, "PING",  b, dc);
  rStat(181, 91, 55, 27, "NODE", nodeName[0] ? trunc(String(nodeName), 8).c_str() : "--", 0xFDA0);

  // 4. 底部状态栏 (总流量与彩色快捷键)
  cv.setTextDatum(middle_left); cv.setTextSize(1);
  if (totalDown > 0 || totalUp > 0) {
    char td[16], tu[16];
    if (totalDown >= 1073741824LL) snprintf(td, sizeof(td), "%.1fG", totalDown / 1073741824.0);
    else snprintf(td, sizeof(td), "%.0fM", totalDown / 1048576.0);
    if (totalUp >= 1073741824LL) snprintf(tu, sizeof(tu), "%.1fG", totalUp / 1073741824.0);
    else snprintf(tu, sizeof(tu), "%.0fM", totalUp / 1048576.0);

    cv.setTextColor(0x07E0, TFT_BLACK);
    snprintf(b, sizeof(b), "v%s", td);
    cv.drawString(b, 4, 127);
    int tx = 4 + cv.textWidth(b) + 6;
    cv.setTextColor(0xFBE0, TFT_BLACK);
    snprintf(b, sizeof(b), "^%s", tu);
    cv.drawString(b, tx, 127);
  } else {
    cv.setTextColor(0x632C, TFT_BLACK);
    cv.drawString("Mihomo Stream", 4, 127);
  }

  // 右侧彩色快捷键 (绝对坐标)
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("N", 112, 127);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("flow", 120, 127);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", 158, 127);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("ref", 166, 127);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 196, 127);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("back", 204, 127);
}

static void drawRouterNodes() {
  char rightBuf[32];
  snprintf(rightBuf, sizeof(rightBuf), "GROUP: %s", CLASH_PROXY_GROUP);
  drawPageHeader("Router Nodes", rightBuf, 0xFDA0);

  // 主节点列表卡片 (y = 15..118, h = 104)
  const int cy = 15, ch = 104;
  cv.fillRoundRect(4, cy, SW - 8, ch, 3, 0x0821);
  cv.drawRoundRect(4, cy, SW - 8, ch, 3, 0x18C3);
  cv.fillRect(4, cy, 2, ch, 0xFDA0);

  // 标题行
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("SELECT ACTIVE PROXY NODE:", 10, cy + 4);

  char cBuf[16];
  snprintf(cBuf, sizeof(cBuf), "%d/%d", proxyNodeCount > 0 ? (nodeCursor + 1) : 0, proxyNodeCount);
  cv.setTextDatum(top_right);
  cv.setTextColor(0x8410, 0x0821);
  cv.drawString(cBuf, SW - 10, cy + 4);

  cv.drawFastHLine(6, cy + 15, SW - 12, 0x18C3);

  // 节点行渲染 (最多显示 4 行)
  const int maxRows = 4;
  int startY = cy + 18;
  for (int r = 0; r < maxRows; r++) {
    int idx = nodeScroll + r;
    if (idx >= proxyNodeCount) break;

    int rowY = startY + r * 17;
    bool isSel = (idx == nodeCursor);
    const ProxyNode& node = proxyNodes[idx];

    if (isSel) {
      cv.fillRoundRect(8, rowY, SW - 16, 15, 2, 0x0186);
      cv.drawRoundRect(8, rowY, SW - 16, 15, 2, 0x03DF);
    }

    // 激活状态指示标签
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    if (node.isNow) {
      cv.fillRoundRect(12, rowY + 2, 42, 11, 2, 0x0280);
      cv.drawRoundRect(12, rowY + 2, 42, 11, 2, 0x04A0);
      cv.setTextColor(0x07E0, 0x0280);
      cv.setTextDatum(middle_center);
      cv.drawString("ACTIVE", 33, rowY + 7);
    } else {
      cv.setTextDatum(middle_center);
      cv.setTextColor(0x4208, isSel ? 0x0186 : 0x0821);
      cv.drawString("[-]", 33, rowY + 7);
    }

    // 节点名称
    cv.setTextDatum(middle_left);
    cv.setTextColor(isSel ? 0xFFFF : (node.isNow ? 0xFDA0 : 0xCE79), isSel ? 0x0186 : 0x0821);
    cv.drawString(trunc(String(node.name), 18), 58, rowY + 7);

    // 延迟徽章
    const int badgeW = 42, badgeH = 11;
    const int badgeX = SW - 12 - badgeW;
    const int badgeY = rowY + 2;

    if (node.delay > 0) {
      uint16_t bBg = node.delay < 150 ? 0x0280 : (node.delay < 300 ? 0x3A00 : 0x3800);
      uint16_t bBdr = node.delay < 150 ? 0x04A0 : (node.delay < 300 ? 0x7BE0 : 0x7800);
      uint16_t bTxt = node.delay < 150 ? 0x07E0 : (node.delay < 300 ? 0xFDA0 : 0xF800);
      cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, bBg);
      cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, bBdr);
      cv.setTextDatum(middle_center);
      cv.setTextColor(bTxt, bBg);
      char dBuf[16];
      snprintf(dBuf, sizeof(dBuf), "%dms", node.delay);
      cv.drawString(dBuf, badgeX + badgeW / 2, badgeY + badgeH / 2);
    } else if (node.delay == -1) {
      cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x3800);
      cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x7800);
      cv.setTextDatum(middle_center);
      cv.setTextColor(0xF800, 0x3800);
      cv.drawString("TIMEOUT", badgeX + badgeW / 2, badgeY + badgeH / 2);
    } else {
      cv.setTextDatum(middle_center);
      cv.setTextColor(0x4208, isSel ? 0x0186 : 0x0821);
      cv.drawString("[TEST]", badgeX + badgeW / 2, badgeY + badgeH / 2);
    }
  }

  // 底部 Toast 反馈横幅 (若有操作提示)
  if (switchStatus.length() > 0 && millis() - switchStatusTime < 3500) {
    cv.fillRoundRect(8, cy + ch - 17, SW - 16, 14, 2, 0x0280);
    cv.drawRoundRect(8, cy + ch - 17, SW - 16, 14, 2, 0x04A0);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(0x07E0, 0x0280);
    cv.drawString(switchStatus, SW / 2, cy + ch - 10);
  } else {
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(0x4208, 0x0821);
    cv.drawString("; / . nav   Enter:apply   T:ping", SW / 2, cy + ch - 10);
  }

  // 底部彩色快捷键
  cv.setTextDatum(middle_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 6, 127);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("apply", 40, 127);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("T", 76, 127);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("ping", 85, 127);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("N", 116, 127);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("scope", 125, 127);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 196, 127);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("back", 204, 127);
}

// Fixed node cards and consistently scaled connection ribbons.
static void drawRouterFlow() {
  char status[32];
  bool stale = !online || flowError.length() || millis() - flowUpdated > 12000;
  if (stale && flowValid) snprintf(status, sizeof(status), "STALE %d", flow.total);
  else if (flowLimited && flowValid) snprintf(status, sizeof(status), "PART >=%d", flow.total);
  else snprintf(status, sizeof(status), "%d CONNS", flow.total);
  cv.setTextSize(1);
  String title = "Route Flow";
  if (flowValid && flowFocus >= 0) {
    int c = flowFocus/4, i = flowFocus%4;
    title = i == 3 ? String("OTHER") : flow.names[c][i];
  }
  title = truncPx(title, SW - 20 - cv.textWidth(flowValid ? status : "WAIT"));
  drawPageHeader(title.c_str(), flowValid ? status : "WAIT", stale ? TFT_ORANGE : TFT_GREEN);
  cv.setTextDatum(top_left);
  const int xs[3] = {4, 91, 178};
  const char* headings[3] = {"SOURCE", "RULE", "EXIT"};
  for (int c = 0; c < 3; ++c) {
    cv.setTextColor(0x8410, TFT_BLACK);
    cv.drawString(headings[c], xs[c], 17);
  }
  if (!flowValid || !flow.total) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(stale ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
    cv.drawString(!online ? "Router offline" : flowError.length() ? "Connections fetch failed" :
                  !flowValid ? "Loading connections..." : "No active connections", SW / 2, 70);
  } else {
    // Fixed rows keep rare categories legible. All ribbons use the same scale;
    // the busiest node occupies at most 20 pixels of its 26-pixel row.
    const int rowH = 26, cardH = 24;
    int maximum = 1;
    for (int c = 0; c < 3; ++c) for (int i = 0; i < 4; ++i)
      if (flow.counts[c][i] > maximum) maximum = flow.counts[c][i];
    auto ribbonY = [&](int col, int row, int used) {
      int center = 30 + row * rowH + cardH/2;
      return center - flow.counts[col][row] * 20 / maximum / 2 + used * 20 / maximum;
    };
    const uint16_t colors[4] = {0x05EA, 0x03BF, 0xFCA0, 0x8410};
    const uint16_t ribbonColors[4] = {0x02C5, 0x01D0, 0x8240, 0x4208};
    for (int gap = 0; gap < 2; ++gap) {
      int srcUsed[4] = {}, dstUsed[4] = {};
      for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b) {
        int n = flow.links[gap][a][b];
        if (!n) continue;
        int sy = ribbonY(gap, a, srcUsed[a]);
        int ey = ribbonY(gap+1, b, dstUsed[b]);
        int sh = ribbonY(gap, a, srcUsed[a]+n) - sy;
        int eh = ribbonY(gap+1, b, dstUsed[b]+n) - ey;
        srcUsed[a] += n; dstUsed[b] += n;
        int x0 = xs[gap] + 60, x1 = xs[gap+1];
        for (int x = x0; x < x1; ++x) {
          int t = (x-x0) * 256 / (x1-x0);
          int u = t*t*(768-2*t) / 65536;
          int y = sy + (ey-sy)*u/256;
          int h = sh + (eh-sh)*u/256;
          cv.drawFastVLine(x, y, h > 0 ? h : 1, ribbonColors[a]);
        }
      }
    }
    for (int c = 0; c < 3; ++c) for (int i = 0; i < 4; ++i) {
      int count = flow.counts[c][i];
      if (!count) continue;
      int y = 30 + i * rowH;
      cv.fillRoundRect(xs[c], y, 60, cardH, 2, 0x0821);
      cv.drawRoundRect(xs[c], y, 60, cardH, 2,
                       flowFocus == c*4+i ? TFT_WHITE : 0x18C3);
      cv.fillRect(xs[c], y+3, 2, cardH-6, colors[i]);
      String label = i == 3 ? String("OTHER") : flow.names[c][i];
      if (c == 0 && label.length() > 9) label = label.substring(label.length()-9);
      cv.setTextDatum(top_left); cv.setTextColor(TFT_WHITE, 0x0821);
      cv.drawString(truncPx(label, 54), xs[c]+4, y+4);
      char countText[16];
      snprintf(countText, sizeof(countText), "%d", count);
      cv.setTextColor(colors[i], 0x0821);
      cv.drawString(countText, xs[c]+4, y+14);
    }
  }

}

static void drawRouterTypes() {
  bool stale = !online || flowError.length() || millis() - flowUpdated > 12000;
  const char* metric = typeByBytes ? "BYTES" : "COUNT";
  char status[32];
  snprintf(status, sizeof(status), "%s%s", !flowValid ? "WAIT " : stale ? "STALE " :
           flowLimited ? "PART " : "", metric);
  drawPageHeader("Traffic Types", status, stale ? TFT_ORANGE : TFT_GREEN);
  cv.setTextSize(1);
  if (!flowValid || !flow.total) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(stale ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
    cv.drawString(!online ? "Router offline" : flowError.length() ? "Connections fetch failed" :
                  !flowValid ? "Loading connections..." : "No active connections", SW/2, 70);
    return;
  }
  const char* networks[3] = {"TCP", "UDP", "UNKNOWN"};
  const uint16_t colors[5] = {0x07E0, 0x07FF, 0xFDA0, 0xB81F, 0x8410};
  for (int i = 0; i < 3; ++i) {
    int x = 4 + i*79;
    char count[16];
    snprintf(count, sizeof(count), "%d", typeStats.network[i]);
    rStat(x, 16, 74, 29, networks[i], count, i == 2 ? 0x8410 : colors[i]);
  }
  cv.setTextDatum(top_left); cv.setTextColor(0x8410, TFT_BLACK);
  cv.drawString(typeByBytes ? "ACTIVE CONN BYTES / PORT" : "ACTIVE CONNECTIONS / PORT", 6, 49);
  const char* labels[5] = {"TCP:443", "UDP:443", "PORT:80", "PORT:53", "OTHER"};
  uint64_t total = typeByBytes ? typeStats.totalBytes : uint64_t(flow.total);
  for (int i = 0; i < 5; ++i) {
    int y = 61 + i*15;
    uint64_t value = typeByBytes ? typeStats.bytes[i] : uint64_t(typeStats.ports[i]);
    // Floating point scaling avoids overflow for long-lived large transfers.
    int width = total ? int(double(value)/double(total)*108) : 0;
    if (width > 108) width = 108;
    cv.setTextDatum(middle_left); cv.setTextColor(colors[i], TFT_BLACK);
    cv.drawString(labels[i], 6, y+5);
    cv.fillRoundRect(65, y+1, 108, 9, 2, 0x0821);
    if (value) cv.fillRoundRect(65, y+1, width > 0 ? width : 1, 9, 2, colors[i]);
    String text;
    if (typeByBytes) text = fmtBytes(value);
    else text = String(typeStats.ports[i]);
    cv.setTextDatum(middle_right); cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.drawString(truncPx(text, 60), SW-5, y+5);
  }
}

static void drawRouterHosts() {
  bool stale = !online || flowError.length() || millis() - flowUpdated > 12000;
  char status[32];
  snprintf(status, sizeof(status), "%s%s", !flowValid ? "WAIT " : stale ? "STALE " :
           flowLimited ? "PART " : "", hostByBytes ? "BYTES" : "COUNT");
  drawPageHeader("Top Hosts", status, stale ? TFT_ORANGE : TFT_GREEN);
  cv.setTextSize(1);
  if (!flowValid || !hostStats.total) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(stale ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
    cv.drawString(!online ? "Router offline" : flowError.length() ? "Connections fetch failed" :
                  !flowValid ? "Loading connections..." : "No active connections", SW/2, 70);
    return;
  }
  int top[5];
  int n = topRouterHosts(hostStats, hostByBytes, top, 5);
  uint64_t maxV = 1;
  for (int i = 0; i < n; ++i) {
    const auto& sl = hostStats.slots[top[i]];
    uint64_t v = hostByBytes ? sl.bytes : uint64_t(sl.count);
    if (v > maxV) maxV = v;
  }
  for (int i = 0; i < n; ++i) {
    const auto& sl = hostStats.slots[top[i]];
    int y = 17 + i * 21;
    uint64_t v = hostByBytes ? sl.bytes : uint64_t(sl.count);
    int barW = SW - 12;
    int w = maxV ? int(double(v) / double(maxV) * barW) : 0;
    if (w > barW) w = barW;
    cv.fillRoundRect(6, y + 17, barW, 2, 1, 0x0821);
    if (w > 0) cv.fillRoundRect(6, y + 17, w, 2, 1, 0x07FF);

    cv.setTextDatum(top_left); cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.drawString(truncPx(String(sl.host), 150), 6, y);
    String val = hostByBytes ? fmtBytes(sl.bytes) : String(sl.count);
    cv.setTextDatum(top_right); cv.setTextColor(TFT_GREEN, TFT_BLACK);
    cv.drawString(truncPx(val, 65), SW - 6, y);
    cv.setTextDatum(top_left); cv.setTextColor(0x8410, TFT_BLACK);
    char meta[40];
    snprintf(meta, sizeof(meta), "x%d  %s", sl.count, sl.exit[0] ? sl.exit : "DIRECT");
    cv.drawString(truncPx(String(meta), 160), 6, y + 8);
  }

  // 底部快捷键提示
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("M", 6, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(hostByBytes ? "count" : "bytes", 16, SH - 2);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("N", 68, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("next", 76, SH - 2);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", 120, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("ref", 128, SH - 2);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 168, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("back", 176, SH - 2);
}

static void drawHistChart(int y, int h, const char* label, const int32_t* arr,
                          const char* unit, uint16_t col, bool minZero) {
  const int gx = 4, gw = 232;
  cv.fillRoundRect(gx, y, gw, h, 3, CARD_BG);
  cv.drawRoundRect(gx, y, gw, h, 3, 0x18C3);
  cv.fillRect(gx, y, 2, h, col);

  int32_t lo, hi;
  histRange(hist, arr, lo, hi);
  if (minZero) lo = 0;
  if (hi - lo < 1) hi = lo + 1;

  for (int i = 0; i < hist.count; ++i) {
    int x = gx + 4 + i * (gw - 8) / RouterHistory::N;
    int yy = y + h - 3 - int((long long)(arr[hist.idx(i)] - lo) * (h - 16) / (hi - lo));
    cv.drawFastVLine(x, yy, y + h - 2 - yy, 0x0A24);
    cv.drawPixel(x, yy, col);
  }

  cv.setTextDatum(top_left); cv.setTextColor(col, CARD_BG);
  cv.drawString(label, gx + 6, y + 3);

  char v[40];
  int32_t cur = hist.count ? arr[hist.idx(hist.count - 1)] : 0;
  snprintf(v, sizeof(v), "%ld%s [lo %ld, hi %ld]", (long)cur, unit, (long)lo, (long)hi);
  cv.setTextDatum(top_right); cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
  cv.drawString(v, gx + gw - 6, y + 3);
}

static void drawRouterHist() {
  bool stale = !online;
  char status[24];
  int durM = hist.count * 12 / 60;
  snprintf(status, sizeof(status), "%s%dm (%d pts)", stale ? "OFF " : "", durM, hist.count);
  drawPageHeader("History", status, stale ? TFT_ORANGE : TFT_GREEN);
  cv.setTextSize(1);
  if (hist.count < 2) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(stale ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
    cv.drawString(!online ? "Router offline" : "Collecting history samples...", SW / 2, 70);
    return;
  }
  drawHistChart(17, 51, "MEM", hist.mem, "KB", 0x07FF, false);
  drawHistChart(71, 51, "CONNS", hist.conns, "", 0xFDA0, true);

  // 底部快捷键提示
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("N", 6, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("next", 14, SH - 2);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", 58, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("ref", 66, SH - 2);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 106, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("back", 114, SH - 2);
}

void drawRouter() {
  job.markShown();
  cv.fillScreen(TFT_BLACK);

  if (routerPage == R_PAGE_FLOW) {
    drawRouterFlow();
  } else if (routerPage == R_PAGE_TYPES) {
    drawRouterTypes();
  } else if (routerPage == R_PAGE_HIST) {
    drawRouterHist();
  } else if (routerPage == R_PAGE_HOSTS) {
    drawRouterHosts();
  } else if (routerPage == R_PAGE_NODES) {
    drawRouterNodes();
  } else {
    drawRouterTraffic();
  }
}
