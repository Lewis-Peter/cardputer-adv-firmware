#include "typhoon.h"
#include "bg_fetch.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <cmath>
#include "tls_ca.h"
#include "ui_common.h"
#include "net_job.h"
#include "wifi_net.h"
#include "geoloc.h"
#include "worldmap.h"
#include "http_json.h"

static const int MAX_TC     = 4;    // 西北太平洋同时活跃 4 个已经很罕见
static const int MAX_TRACK  = 44;   // JMA 的历史路径一般 30~40 点
static const int MAX_FC     = 8;    // 预报点：+12/24/45/69/93/117h 一般 6 个
static const uint32_t AUTO_MS = 30UL * 60 * 1000;   // JMA 三小时一报，半小时重拉足够

struct TrackPt { float lat, lon; };

// 活跃气旋清单（targetTc.json）。名字要等详情拉回来才有，所以先留空。
struct TcMeta { char id[10], num[6], cat[8], name[16]; };
static TcMeta  list[MAX_TC];
static int     listN = 0;
static int     selIdx = 0;

// 只保留**选中那一个**的详情：翻到别的台风就重拉。全存下来的话光路径点就要 1.3KB，
// 而这块板子没有 PSRAM，能省则省。
static struct {
  int   idx = -1;          // 对应 list[] 的下标，-1 = 还没加载
  bool  ok = false;
  int   pressure = 0, windMs = 0, speedKmh = 0;
  float lat = 0, lon = 0;
  TrackPt past[MAX_TRACK]; int pastN = 0;
  TrackPt fc[MAX_FC];      int fcH[MAX_FC]; int fcN = 0;
} det;

static String   errMsg = "";
static bool     everFetched = false;
static uint32_t lastFetchMs = 0;
static uint32_t selChangedMs = 0;
static bool     haveMe = false;
static float    meLat = 0, meLon = 0;

// ---- 取数 ----

static bool jmaGet(WiFiClientSecure& client, const char* path, JsonDocument& doc, const JsonDocument& filter) {
  char url[96];
  snprintf(url, sizeof(url), "https://www.jma.go.jp%s", path);

  // jma.go.jp 只有 HTTPS（明文会跳转）。
  // client 和证书包在调用方的 lease 之前建好传进来（见 globals.h 的 CanvasLease 规矩）。
  HttpJsonOptions options(8000, 12000);
  return fetchJsonHttp(client, url, doc, &filter, options, errMsg);
}

static void fetchList() {
  // 错误字符串先备好容量：下面 TLS 那段借走了画布，期间再给它长内存会落进画布腾出的洞里
  errMsg.reserve(96);
  errMsg = "";
  listN = 0;
  det.idx = -1; det.ok = false;
  lastFetchMs = millis();

  if (!wifiEnsureConnected()) { errMsg = "wifi not connected"; dirty = true; return; }
  // 证书校验要比有效期，时钟没对上必然失败——分开报，别混成网络错
  if (!tlsClockReady()) { errMsg = "waiting for clock (NTP)"; dirty = true; return; }

  GeoFix fix;
  haveMe = geoGet(fix, nullptr);      // 拿不到定位也不算错：路径图照画，只是没有"离你多远"
  if (haveMe) { meLat = fix.lat; meLon = fix.lon; }

  WiFiClientSecure client;
  tlsUseCaBundle(client);
  // 同 okx.cpp：覆盖框架默认 120s 握手超时，避免 JMA 握手丢包卡死主循环
  client.setHandshakeTimeout(8);

  CanvasLease lease;
  JsonDocument filter;
  JsonObject e = filter.add<JsonObject>();   // 顶层是同构对象数组，filter[0] 套到每个元素
  e["tropicalCyclone"] = true; e["typhoonNumber"] = true; e["category"] = true;

  JsonDocument doc;
  if (!jmaGet(client, "/bosai/typhoon/data/targetTc.json", doc, filter)) { dirty = true; return; }

  for (JsonObject o : doc.as<JsonArray>()) {
    if (listN >= MAX_TC) break;
    const char* id = o["tropicalCyclone"] | "";
    if (!id[0]) continue;
    TcMeta& m = list[listN];
    strncpy(m.id,  id,                       sizeof(m.id)  - 1); m.id[sizeof(m.id) - 1]   = 0;
    strncpy(m.num, o["typhoonNumber"] | "",  sizeof(m.num) - 1); m.num[sizeof(m.num) - 1] = 0;
    strncpy(m.cat, o["category"] | "",       sizeof(m.cat) - 1); m.cat[sizeof(m.cat) - 1] = 0;
    m.name[0] = 0;
    listN++;
  }
  if (selIdx >= listN) selIdx = 0;
  everFetched = true;
  selChangedMs = millis();
  dirty = true;
}

// 实况：强度/中心气压/最大风速/移速/当前中心位置
static bool fetchSpecs(WiFiClientSecure& client, TcMeta& m) {
  JsonDocument filter;
  JsonObject e = filter.add<JsonObject>();
  e["name"]["en"] = true; e["category"]["en"] = true;
  e["advancedHours"] = true;
  e["pressure"] = true; e["speed"]["km/h"] = true;
  e["maximumWind"]["sustained"]["m/s"] = true;
  e["position"]["deg"] = true;

  char path[64];
  snprintf(path, sizeof(path), "/bosai/typhoon/data/%s/specifications.json", m.id);
  JsonDocument doc;
  if (!jmaGet(client, path, doc, filter)) return false;

  for (JsonObject o : doc.as<JsonArray>()) {
    const char* nm = o["name"]["en"] | "";
    if (nm[0]) { strncpy(m.name, nm, sizeof(m.name) - 1); m.name[sizeof(m.name) - 1] = 0; }
    // ⚠️ 每个**预报**时段也各带一份 pressure/maximumWind/speed，光看"有没有 pressure"
    // 会一路覆盖到 +117h 那段，把预报值当成实况显示（实测把 910hPa 显示成了 935hPa）。
    // 实况段的标志是 advancedHours == 0。part 字段有时是字符串有时是对象，不能拿来判。
    if (!o["advancedHours"].isNull() && (int)o["advancedHours"] == 0) {
      det.pressure = atoi(o["pressure"] | "0");
      det.windMs   = atoi(o["maximumWind"]["sustained"]["m/s"] | "0");
      det.speedKmh = atoi(o["speed"]["km/h"] | "0");
      const char* c = o["category"]["en"] | "";
      if (c[0]) { strncpy(m.cat, c, sizeof(m.cat) - 1); m.cat[sizeof(m.cat) - 1] = 0; }
      JsonArray pos = o["position"]["deg"];
      if (pos.size() == 2) { det.lat = pos[0]; det.lon = pos[1]; }
    }
  }
  return true;
}

// 路径：历史点（preTyphoon 热带低压阶段 + typhoon 台风阶段）+ 官方预报点
static bool fetchTrack(WiFiClientSecure& client, TcMeta& m) {
  JsonDocument filter;
  JsonObject e = filter.add<JsonObject>();
  e["advancedHours"] = true; e["center"] = true; e["track"] = true;

  char path[64];
  snprintf(path, sizeof(path), "/bosai/typhoon/data/%s/forecast.json", m.id);
  JsonDocument doc;
  if (!jmaGet(client, path, doc, filter)) return false;

  det.pastN = 0; det.fcN = 0;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (o["advancedHours"].isNull()) continue;             // title 段没有这个字段
    int hours = o["advancedHours"];
    if (hours == 0) {
      // 实况段：track 里是走过的路。先热带低压阶段，再台风阶段，接起来就是完整路径。
      for (const char* key : {"preTyphoon", "typhoon"})
        for (JsonArray p : o["track"][key].as<JsonArray>()) {
          if (det.pastN >= MAX_TRACK || p.size() != 2) continue;
          det.past[det.pastN].lat = p[0];
          det.past[det.pastN].lon = p[1];
          det.pastN++;
        }
      JsonArray c = o["center"];
      if (c.size() == 2) { det.lat = c[0]; det.lon = c[1]; }
    } else if (det.fcN < MAX_FC) {
      JsonArray c = o["center"];
      if (c.size() != 2) continue;
      det.fc[det.fcN].lat = c[0];
      det.fc[det.fcN].lon = c[1];
      det.fcH[det.fcN] = hours;
      det.fcN++;
    }
  }
  return det.pastN > 0 || det.fcN > 0;
}

// 详情是两次请求（各一次 TLS 握手），所以只在选中项稳定下来之后才拉——
// 连着按 ;/. 翻台风时不会每翻一格就卡好几秒。跟 adsb.cpp 的航线查询同一套路子。
static const uint32_t DETAIL_DEBOUNCE_MS = 400;

// 要不要拉详情。判断留在主线程（见 typhoonUpdate）：放进工人里就是每一轮 loop 起一个任务空跑
static bool detailNeeded() { return listN > 0 && det.idx != selIdx; }

static void ensureDetail() {
  if (!detailNeeded()) return;
  // 错误字符串先备好容量，避免错误分支动态分配落进画布空洞
  errMsg.reserve(96);
  errMsg = "";
  det.idx = selIdx;
  det.ok = false;
  det.pastN = det.fcN = 0;
  det.pressure = det.windMs = det.speedKmh = 0;
  TcMeta& m = list[selIdx];

  // client 和证书包在 lease 之前建好
  WiFiClientSecure client;
  tlsUseCaBundle(client);
  client.setHandshakeTimeout(8);   // 同 okx.cpp：别让握手丢包卡死主循环 120s

  // 为什么一个 lease 覆盖整个过程（specs + track 两次握手）：
  // 1. ensureDetail() 在后台工人上一口气跑完（bg_fetch.h），期间主线程不画 cv，
  //    中间恢复画布无视觉收益；
  // 2. 避免两次连续释放/申请 64.8KB 画布导致无谓的堆抖动和可能的碎片化失败；
  // 3. 将 64.8KB 空洞持续留给第二次 TLS 握手（~33KB 缓冲）；
  // 4. client 复用同一个实例并在两次请求间 client.stop()，释放前一次握手的 TLS 缓冲；
  // 5. 临时 doc/filter 在各自子函数（fetchSpecs/fetchTrack）返回时析构，逆序析构保证先于 lease。
  {
    CanvasLease lease;
    bool okSpecs = fetchSpecs(client, m);
    client.stop(); // 释放上一个连接的 TLS 缓冲
    bool okTrack = okSpecs && fetchTrack(client, m);
    det.ok = okSpecs && okTrack;
  }
  dirty = true;
}

// ---- 几何 ----

static float bearingDeg(float lat1, float lon1, float lat2, float lon2) {
  const float D = 3.14159265f / 180.0f;
  float y = sinf((lon2 - lon1) * D) * cosf(lat2 * D);
  float x = cosf(lat1 * D) * sinf(lat2 * D) -
            sinf(lat1 * D) * cosf(lat2 * D) * cosf((lon2 - lon1) * D);
  float b = atan2f(y, x) / D;
  if (b < 0) b += 360;
  return b;
}

// 沿**官方预报路径**找离本机最近的那一点：当前位置 + 各预报点连成折线，
// 每段再插值取样。返回是否算得出来；atHorizon=最近点落在预报路径的末端，
// 也就是"还在接近，但已经超出预报时效"——这种情况不能说成"最近就到这儿了"。
struct Cpa { float km; float hours; bool atHorizon; };

static bool computeCpa(Cpa& out) {
  if (!haveMe || det.fcN == 0) return false;
  const int SUB = 10;
  float bestKm = 1e9f, bestH = 0;
  int   lastSampleBest = 0, sample = 0;

  float plat = det.lat, plon = det.lon, ph = 0;
  for (int i = 0; i < det.fcN; i++) {
    float clat = det.fc[i].lat, clon = det.fc[i].lon, ch = (float)det.fcH[i];
    for (int s = 1; s <= SUB; s++) {
      float t = (float)s / SUB;
      float la = plat + (clat - plat) * t;
      float lo = plon + (clon - plon) * t;
      float d = gcKm(meLat, meLon, la, lo);
      sample++;
      if (d < bestKm) { bestKm = d; bestH = ph + (ch - ph) * t; lastSampleBest = sample; }
    }
    plat = clat; plon = clon; ph = ch;
  }
  out.km = bestKm;
  out.hours = bestH;
  out.atHorizon = (lastSampleBest == sample);     // 最近点就是折线终点
  return true;
}

// ---- 生命周期 ----

static DeferredFetch job;

void typhoonEnter() {
  selIdx = 0; listN = 0; errMsg = ""; everFetched = false;
  det.idx = -1; det.ok = false;
  job.request();
  lastFetchMs = millis();
  selChangedMs = millis();
  dirty = true;
}

void typhoonUpdate() {
  // 拉取交给后台工人（bg_fetch.h）；lastFetchMs 先在主线程记上，工人还没开跑时别被重复触发
  if (job.due()) { lastFetchMs = millis(); bgFetchRun(fetchList); return; }
  if (millis() - lastFetchMs >= AUTO_MS) { lastFetchMs = millis(); bgFetchRun(fetchList); return; }
  // job.pending：按了 r、列表马上要重拉，别再按旧列表拉一遍详情（两次 TLS，见 net_job.h）
  if (!job.pending && millis() - selChangedMs >= DETAIL_DEBOUNCE_MS && detailNeeded()) bgFetchRun(ensureDetail);
}

void typhoonKey(char k) {
  if (k == 'r' || k == 'R') { det.idx = -1; job.request(); }
  else if (listN > 1 && (k == ';' || k == ',')) { selIdx = (selIdx - 1 + listN) % listN; selChangedMs = millis(); }
  else if (listN > 1 && (k == '.' || k == '/')) { selIdx = (selIdx + 1) % listN; selChangedMs = millis(); }
  dirty = true;
}

// ---- 绘制 ----

// 强度越高越告警。JMA 的 category：TD 热带低压 / TS 热带风暴 / STS 强热带风暴 / TY 台风
static uint16_t catColor(const char* cat) {
  if (!strcmp(cat, "TY"))  return cv.color565(232, 72, 72);
  if (!strcmp(cat, "STS")) return cv.color565(224, 140, 56);
  if (!strcmp(cat, "TS"))  return cv.color565(224, 196, 64);
  return ICON_DIM;
}

// 离得越近越红。跟 catColor 分开：一个强台风在 3000km 外并不需要报红。
static uint16_t distColor(float km) {
  if (km < 300)  return cv.color565(232, 72, 72);
  if (km < 800)  return cv.color565(224, 140, 56);
  if (km < 1500) return cv.color565(224, 196, 64);
  return ACCENT;
}

// 标题两页共用："#2613 DOLPHIN"，右上角是第几个/共几个
static void header(const char* right, uint16_t rightCol) {
  char t[32];
  if (listN > 0 && list[selIdx].name[0])
    snprintf(t, sizeof(t), "#%s %s", list[selIdx].num, list[selIdx].name);
  else if (listN > 0)
    snprintf(t, sizeof(t), "#%s", list[selIdx].num);
  else
    snprintf(t, sizeof(t), "TYPHOON");
  drawPageHeader(t, right, rightCol);
}

// 没数据/出错时两页共用的占位，返回 true 表示已经画完、调用方直接 return
static bool drawEmpty() {
  if (listN > 0 && det.ok) return false;
  cv.setTextDatum(top_center); cv.setTextSize(1);
  if (errMsg.length()) {
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.drawString(trunc(errMsg, 38), SW / 2, 54);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("r  retry", SW / 2, 70);
  } else if (!everFetched) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("loading...", SW / 2, 60);
  } else if (listN == 0) {
    cv.setTextColor(ACCENT, TFT_BLACK);
    cv.drawString("no active typhoon", SW / 2, 50);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    // 说清楚覆盖范围，免得有人在大西洋飓风季看到这行以为是坏了
    cv.drawString("JMA covers the NW Pacific only", SW / 2, 68);
    cv.drawString("(Atlantic hurricanes not listed)", SW / 2, 80);
  } else {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("loading details...", SW / 2, 60);
  }
  drawPageDots();
  return true;
}

void drawTyphoon() {
  job.markShown();
  cv.fillScreen(TFT_BLACK);

  // 32 = 前缀 4 + 两个 int 各最多 11 位 + 分隔符 + 结尾符。listN 实际不会超过 MAX_TC，
  // 但按 int 的最坏宽度给足，编译器才不用猜（-Wformat-truncation 就是这么叫起来的）。
  char right[32] = "";
  if (listN > 1) snprintf(right, sizeof(right), ";/. %d/%d", selIdx + 1, listN);
  header(listN > 1 ? right : nullptr, ICON_DIM);
  if (drawEmpty()) return;

  TcMeta& m = list[selIdx];

  // 强度徽标 + 中心气压（这一页最该被一眼看到的两个数）
  cv.setTextDatum(top_left); cv.setTextSize(2);
  cv.setTextColor(catColor(m.cat), TFT_BLACK);
  cv.drawString(m.cat, 10, 18);

  char b[48];
  cv.setTextDatum(top_right);
  snprintf(b, sizeof(b), "%d hPa", det.pressure);
  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  cv.drawString(b, SW - 10, 18);

  // 最大风速 + 移向移速（移向从最后两个路径点算，比 JMA 那个日文方位名通用）
  cv.setTextSize(1); cv.setTextDatum(top_left);
  cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (det.pastN >= 2) {
    float brg = bearingDeg(det.past[det.pastN - 2].lat, det.past[det.pastN - 2].lon,
                           det.lat, det.lon);
    snprintf(b, sizeof(b), "max %dm/s   moving %s %dkm/h", det.windMs, cardOf(brg), det.speedKmh);
  } else {
    snprintf(b, sizeof(b), "max %dm/s   %dkm/h", det.windMs, det.speedKmh);
  }
  cv.drawString(b, 10, 40);

  cv.drawFastHLine(10, 54, SW - 20, DIM_BORDER);

  // ---- 对本机位置的影响 ----
  if (!haveMe) {
    cv.setTextDatum(top_center);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("no fix - distance unavailable", SW / 2, 76);
    drawPageDots();
    return;
  }

  float nowKm = gcKm(meLat, meLon, det.lat, det.lon);
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("FROM YOU", 10, 60);

  cv.setTextDatum(top_left); cv.setTextSize(2);
  cv.setTextColor(distColor(nowKm), TFT_BLACK);
  snprintf(b, sizeof(b), "%.0f km", nowKm);
  cv.drawString(b, 10, 72);

  cv.setTextSize(1);
  Cpa cpa;
  if (computeCpa(cpa)) {
    cv.setTextDatum(top_right);
    cv.setTextColor(distColor(cpa.km), TFT_BLACK);
    if (cpa.atHorizon) {
      // 最近点就落在预报末端 = 预报时效内还在接近，别谎报一个"最近距离"
      snprintf(b, sizeof(b), "still closing at +%dh", (int)(cpa.hours + 0.5f));
    } else if (cpa.km >= nowKm - 20) {
      snprintf(b, sizeof(b), "moving away");
    } else {
      snprintf(b, sizeof(b), "closest %.0fkm in %dh", cpa.km, (int)(cpa.hours + 0.5f));
    }
    cv.drawString(b, SW - 10, 66);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("per JMA forecast", SW - 10, 78);
  }

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  snprintf(b, sizeof(b), "center %.1f%c %.1f%c", fabsf(det.lat), det.lat >= 0 ? 'N' : 'S',
           fabsf(det.lon), det.lon >= 0 ? 'E' : 'W');
  cv.drawString(b, 10, 100);
  cv.setTextDatum(top_right);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString(".  track map", SW - 10, 100);

  drawPageDots();
}

// ---- 路径图 ----

// 视口：由路径点算出经纬包围盒，等比缩放（经度按纬度收窄，不然高纬度会被横向拉长）
struct View { float mlat, mlon, s, kx; int cx, cy; };

// 把经度搬到离参考点最近的那个等价值上（±360）。跨日期变更线时 179.9E 和 179.9W 实际只差
// 0.2°，直接拿原值算包围盒会得到 359.8° 宽的视口，整张图缩成一个点。样本里的白海豚就在
// 179.6E——离变更线只有 0.4°，这不是理论风险。
static float lonNear(float lon, float ref) {
  while (lon - ref >  180.0f) lon -= 360.0f;
  while (lon - ref < -180.0f) lon += 360.0f;
  return lon;
}

static void project(const View& v, float lat, float lon, int& x, int& y) {
  x = v.cx + (int)((lon - v.mlon) * v.kx * v.s);
  y = v.cy - (int)((lat - v.mlat) * v.s);
}

static void dashLine(int x0, int y0, int x1, int y1, uint16_t col) {
  int dx = x1 - x0, dy = y1 - y0;
  int n = (int)(sqrtf((float)(dx * dx + dy * dy)) / 3.0f);
  if (n < 1) n = 1;
  for (int i = 0; i < n; i += 2) {
    int ax = x0 + dx * i / n, ay = y0 + dy * i / n;
    int bx = x0 + dx * (i + 1) / n, by = y0 + dy * (i + 1) / n;
    cv.drawLine(ax, ay, bx, by, col);
  }
}

void drawTyphoonTrack() {
  job.markShown();
  cv.fillScreen(TFT_BLACK);

  char right[16] = "";
  if (listN > 0 && det.ok) snprintf(right, sizeof(right), "%dhPa", det.pressure);
  header(right[0] ? right : nullptr, ICON_DIM);
  if (drawEmpty()) return;

  // 绘图区：左边留经纬标注，底部留经度标注
  const int px0 = 20, px1 = SW - 4, py0 = 16, py1 = SH - 14;
  const int pw = px1 - px0, ph = py1 - py0;

  // 包围盒只按路径点算。把本机位置也算进去的话，台风在几千公里外时会把整条路径压成一个点；
  // 所以本机在框内就画 ✖，在框外只在边缘标一下方向。
  float latMin = 90, latMax = -90, lonMin = 180, lonMax = -180;
  auto grow = [&](float la, float lo) {
    if (la < latMin) latMin = la;
    if (la > latMax) latMax = la;
    if (lo < lonMin) lonMin = lo;
    if (lo > lonMax) lonMax = lo;
  };
  // 所有经度都相对"当前中心"解缠，之后的包围盒/投影全在这个连续坐标系里算
  for (int i = 0; i < det.pastN; i++) grow(det.past[i].lat, lonNear(det.past[i].lon, det.lon));
  for (int i = 0; i < det.fcN; i++)   grow(det.fc[i].lat,   lonNear(det.fc[i].lon,   det.lon));
  grow(det.lat, det.lon);

  float dlat = latMax - latMin, dlon = lonMax - lonMin;
  if (dlat < 4) { float c = (latMin + latMax) / 2; latMin = c - 2; latMax = c + 2; dlat = 4; }
  if (dlon < 4) { float c = (lonMin + lonMax) / 2; lonMin = c - 2; lonMax = c + 2; dlon = 4; }
  dlat *= 1.15f; dlon *= 1.15f;                       // 留边，别让路径贴着框

  View v;
  v.mlat = (latMin + latMax) / 2;
  v.mlon = (lonMin + lonMax) / 2;
  v.kx   = cosf(v.mlat * 3.14159265f / 180.0f);       // 1°经度在这个纬度上有多"窄"
  float sLat = ph / dlat, sLon = pw / (dlon * v.kx);
  v.s  = sLat < sLon ? sLat : sLon;                   // 取小的那个：等比，不变形
  v.cx = px0 + pw / 2;
  v.cy = py0 + ph / 2;

  // 海岸线：逐屏幕像素反投影查陆地掩码，只画"陆/海分界"的那一格。
  //
  // 一开始是按掩码格子找边缘格再投影上屏的，结果一个像素都看不见——掩码是 240x109 管全球
  // (1.5°/px)，一个西北太平洋视口里只剩 30 来个边缘格，散在屏上就是几十个孤立点。
  // 反过来按屏幕像素扫，边界就是连续的（虽然仍是 1.5° 的台阶状，这是掩码精度的上限）。
  {
    const uint16_t landCol = cv.color565(24, 74, 58);
    bool prevRow[240] = {false};
    bool haveRow = false;
    const float invS = 1.0f / v.s, invSx = 1.0f / (v.kx * v.s);
    for (int y = py0; y < py1; y++) {
      float la = v.mlat + (float)(v.cy - y) * invS;
      bool prevLeft = false;
      for (int x = px0; x < px1 && x < 240; x++) {
        float lo = v.mlon + (float)(x - v.cx) * invSx;
        bool land = worldIsLandAtLatLon(la, lonNear(lo, 0.0f));   // 掩码只认 ±180，绕回来再查
        // 跟左邻和上邻比：两个方向都查，南北向的海岸线才不会漏掉
        if ((x > px0 && land != prevLeft) || (haveRow && land != prevRow[x]))
          cv.drawPixel(x, y, landCol);
        prevLeft = land;
        prevRow[x] = land;      // 先读后写，读到的还是上一行的值
      }
      haveRow = true;
    }
  }

  // 10° 经纬网 + 标注
  cv.setTextSize(1);
  for (int la = -80; la <= 80; la += 10) {
    int x, y; project(v, (float)la, v.mlon, x, y);
    if (y < py0 + 4 || y >= py1 - 2) continue;
    for (int gx = px0; gx < px1; gx += 6) cv.drawPixel(gx, y, DIM_BORDER);
    cv.setTextDatum(middle_left);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    char s[8]; snprintf(s, sizeof(s), "%d%c", abs(la), la >= 0 ? 'N' : 'S');
    cv.drawString(s, 1, y);
  }
  for (int lo = -180; lo <= 540; lo += 10) {          // 上界放宽：解缠后的视口可能整体落到 180 以东
    int x, y; project(v, v.mlat, (float)lo, x, y);
    if (x < px0 + 10 || x >= px1 - 10) continue;
    for (int gy = py0; gy < py1; gy += 6) cv.drawPixel(x, gy, DIM_BORDER);
    cv.setTextDatum(top_center);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    int lw = (int)lonNear((float)lo, 0.0f);          // 标注要绕回 ±180，不然会画出 "190E"
    char s[12]; snprintf(s, sizeof(s), "%d%c", abs(lw), lw >= 0 ? 'E' : 'W');
    cv.drawString(s, x, py1 + 2);
  }

  // 走过的路：实线暗绿
  for (int i = 1; i < det.pastN; i++) {
    int x0, y0, x1_, y1_;
    project(v, det.past[i - 1].lat, lonNear(det.past[i - 1].lon, det.lon), x0, y0);
    project(v, det.past[i].lat, lonNear(det.past[i].lon, det.lon), x1_, y1_);
    cv.drawLine(x0, y0, x1_, y1_, ICON_DIM);
  }

  // 预报路径：虚线主色 + 空心点（虚线本身就在说"这是预测，不是既成事实"）
  {
    int px = 0, py = 0;
    project(v, det.lat, det.lon, px, py);
    for (int i = 0; i < det.fcN; i++) {
      int x, y; project(v, det.fc[i].lat, lonNear(det.fc[i].lon, det.lon), x, y);
      dashLine(px, py, x, y, ACCENT);
      cv.drawCircle(x, y, 2, ACCENT);
      px = x; py = y;
    }
    if (det.fcN > 0) {                       // 末端标出预报时效，别让人以为路径到此为止
      int x, y; project(v, det.fc[det.fcN - 1].lat, lonNear(det.fc[det.fcN - 1].lon, det.lon), x, y);
      char s[8]; snprintf(s, sizeof(s), "+%dh", det.fcH[det.fcN - 1]);
      cv.setTextDatum(bottom_center);
      cv.setTextColor(ACCENT, TFT_BLACK);
      cv.drawString(s, x, y - 4);
    }
  }

  // 本机位置
  if (haveMe) {
    int x, y; project(v, meLat, lonNear(meLon, det.lon), x, y);
    if (x >= px0 && x < px1 && y >= py0 && y < py1) {
      cv.drawLine(x - 3, y - 3, x + 3, y + 3, TFT_CYAN);
      cv.drawLine(x - 3, y + 3, x + 3, y - 3, TFT_CYAN);
    } else {
      // 在视口外：贴边画个小箭头，至少能看出台风在你的哪一侧
      int ex = x < px0 ? px0 + 3 : (x >= px1 ? px1 - 3 : x);
      int ey = y < py0 ? py0 + 3 : (y >= py1 ? py1 - 3 : y);
      cv.fillCircle(ex, ey, 2, TFT_CYAN);
      cv.setTextDatum(x < px0 ? top_left : top_right);
      cv.setTextColor(TFT_CYAN, TFT_BLACK);
      cv.drawString("you", x < px0 ? ex + 4 : ex - 4, ey - 3);
    }
  }

  // 当前中心画在最上层，别被路径线盖住
  {
    int x, y; project(v, det.lat, det.lon, x, y);
    cv.fillCircle(x, y, 3, TFT_WHITE);
    cv.drawCircle(x, y, 5, catColor(list[selIdx].cat));
  }

  drawPageDots();
}
