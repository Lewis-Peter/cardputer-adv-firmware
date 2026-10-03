#include "adsb.h"
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
#include "http_json.h"
#include "list_sel.h"

static const int   MAX_PLANES  = 12;
static const int   RADIUS_NM   = 50;              // 查询半径（海里）
static const uint32_t AUTO_MS  = 25000;           // 停在这页时的自动刷新间隔

struct Plane {
  char  cs[10], type[6], reg[10];
  float lat, lon, dist, brg;                       // dist=km, brg=方位角(度)
  int   alt, spd, hdg, vrate;                      // ft / km/h / 度 / ft/min
};

static Plane   planes[MAX_PLANES];
static int     planeCount = 0;
static int     selIdx = 0;
static String  errMsg = "";
static bool    everFetched = false;
static uint32_t lastFetchMs = 0;
static bool    fromGps = false;

// ---- 航线（起降机场 + 行程进度）----
//
// ADS-B 报文里没有航线：机载广播只有位置/高度/速度/呼号。起降机场得拿呼号去排班库
// 另查一次，这里用 adsbdb.com。公开数据、请求不带任何凭据，跟 sats.cpp / github.cpp
// 是同一类，按 README「安全边界」那节用 setInsecure()。它只有 HTTPS（明文会 301），
// 所以这一页破例吃一次 TLS 握手——但单条响应才 ~700B，且只查当前选中的那一架。
//
// ⚠️ 排班库是按呼号查的静态表，不是实时航迹：呼号复用/换季会给出完全不搭的航线
// （实测有架在伦敦上空的飞机被返回成 ORD->LIR）。所以进度一律拿实测位置反算，
// 对不上就不画——见 routeProgress()。
enum { RT_PENDING = 0, RT_OK, RT_MISS, RT_ERR };

struct Route {
  char    cs[10];                    // 这条缓存对应的呼号，空 = 空槽
  char    orig[5], dest[5];
  float   olat, olon, dlat, dlon;
  uint8_t state;
};

static const int RT_CACHE_N = 6;     // 翻回上一架时不用重查，6 条覆盖来回翻的范围
static Route   rcache[RT_CACHE_N];
static int     rcacheNext = 0;
static uint32_t selChangedMs = 0;    // 选中项变化的时刻，给航线查询做防抖

static Route* routeFind(const char* cs) {
  for (int i = 0; i < RT_CACHE_N; i++)
    if (rcache[i].cs[0] && strcmp(rcache[i].cs, cs) == 0) return &rcache[i];
  return nullptr;
}

static void routeCacheClear() {
  for (int i = 0; i < RT_CACHE_N; i++) rcache[i].cs[0] = 0;
  rcacheNext = 0;
}

static void fetchRoute(const char* cs) {
  if (!tlsClockReady()) return; // HTTPS 航线查询需要系统时钟校验 TLS 证书

  Route r{};
  strncpy(r.cs, cs, 9); r.cs[9] = 0;
  r.state = RT_ERR;

  char url[80];
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", cs);

  // 临时错误字符串先备好容量，避免错误分支动态分配落进画布空洞
  String fetchErr;
  fetchErr.reserve(64);

  // TLS 握手要两块 ~16.7KB 缓冲，查航线期间把画布借出去，出作用域自动还。
  // client 和证书包在 lease 之前建（tlsUseCaBundle 每次都会重新 calloc 一块永不释放的证书索引）；
  // lease 必须声明在 filter/doc 之前（逆序析构：它们先释放，画布后恢复）。
  WiFiClientSecure client;
  tlsUseCaBundle(client);            // 真校验（见 tls_ca.h），不再是 setInsecure
  // 同 okx.cpp：覆盖框架默认 120s 握手超时，避免航线查询握手丢包卡死主循环
  client.setHandshakeTimeout(8);

  {
    CanvasLease lease;
    // 只留进度条要用的四个字段，别把整份机场/航司信息拖进堆
    JsonDocument filter;
    JsonObject fr = filter["response"]["flightroute"].to<JsonObject>();
    for (const char* end : {"origin", "destination"}) {
      JsonObject ap = fr[end].to<JsonObject>();
      ap["iata_code"] = true; ap["latitude"] = true; ap["longitude"] = true;
    }
    JsonDocument doc;
    HttpJsonOptions options(8000, 10000);
    if (fetchJsonHttp(client, url, doc, &filter, options, fetchErr)) {
      JsonObject o = doc["response"]["flightroute"]["origin"];
      JsonObject d = doc["response"]["flightroute"]["destination"];
      if (!o.isNull() && !d.isNull() && !o["latitude"].isNull() && !d["latitude"].isNull()) {
        strncpy(r.orig, o["iata_code"] | "???", 4); r.orig[4] = 0;
        strncpy(r.dest, d["iata_code"] | "???", 4); r.dest[4] = 0;
        r.olat = o["latitude"]; r.olon = o["longitude"];
        r.dlat = d["latitude"]; r.dlon = d["longitude"];
        r.state = RT_OK;
      } else {
        r.state = RT_MISS;
      }
    } else if (fetchErr == "http 404") {
      r.state = RT_MISS;               // 库里没有这个呼号，别再重试
    }
  } // filter 与 doc 先析构，随后 lease 析构恢复画布

  rcache[rcacheNext] = r;
  rcacheNext = (rcacheNext + 1) % RT_CACHE_N;
  dirty = true;
}

// 行程进度 0..1，同时给出剩余里程。用「已飞 /(已飞+待飞)」而不是投影到大圆上：
// 结果天然落在 [0,1]，也不用处理飞机在航线延长线外的情况。
// 返回 -1 = 这条航线跟飞机实际位置对不上（呼号排班库常有的错配），宁可不画。
static float routeProgress(const Route& r, float lat, float lon, float* remainKm) {
  float total = gcKm(r.olat, r.olon, r.dlat, r.dlon);
  if (total < 1.0f) return -1.0f;
  float flown = gcKm(r.olat, r.olon, lat, lon);
  float left  = gcKm(lat, lon, r.dlat, r.dlon);
  // 绕飞/等待航线也会让折线长于大圆，留 25% + 200km 的余量再判定为错配
  if (flown + left > total * 1.25f + 200.0f) return -1.0f;
  if (remainKm) *remainKm = left;
  return flown / (flown + left);
}

static void fetchPlanes() {
  errMsg = "";
  lastFetchMs = millis();

  if (!wifiEnsureConnected()) { errMsg = "wifi not connected"; dirty = true; return; }

  GeoFix fix;
  if (!geoGet(fix, &errMsg)) { dirty = true; return; }
  fromGps = fix.fromGps;

  // 明文 HTTP：跟地图瓦片同一个道理——公开数据、请求不带任何凭据，
  // 而 TLS 握手要几百毫秒 + 40KB 连续堆（实测握手期间 largestBlock 从 90KB 掉到 47KB），
  // 进这一页那"卡顿一秒"主要就是它。实测这个接口明文返回 200 且是 Content-Length
  // 定长编码（不是 chunked），配下面的 getStream() 流式解析刚好。
  char url[128];
  snprintf(url, sizeof(url), "http://api.adsb.lol/v2/lat/%.4f/lon/%.4f/dist/%d",
           fix.lat, fix.lon, RADIUS_NM);

  WiFiClient client;
  // 机场附近这个接口能返回一两百架、几十上百 KB。板子没有 PSRAM，整包 JSON 进堆很危险，
  // 所以用 filter 边流式解析边扔——只留下面这几个字段，堆占用跟返回大小基本脱钩。
  JsonDocument filter;
  JsonObject fac = filter["ac"].add<JsonObject>();
  fac["flight"] = true; fac["t"] = true; fac["r"] = true;
  fac["lat"] = true; fac["lon"] = true; fac["alt_baro"] = true;
  fac["gs"] = true; fac["track"] = true; fac["baro_rate"] = true;

  JsonDocument doc;
  HttpJsonOptions options(10000, 15000);   // UA 走 http_json.h 里的统一默认值
  if (!fetchJsonHttp(client, url, doc, &filter, options, errMsg)) { dirty = true; return; }

  int n = 0;
  for (JsonObject a : doc["ac"].as<JsonArray>()) {
    if (a["lat"].isNull() || a["lon"].isNull()) continue;

    Plane p{};
    const char* cs = a["flight"] | "";
    strncpy(p.cs, cs, 9); p.cs[9] = 0;
    for (int i = 8; i >= 0; i--) {                  // 呼号是右侧补空格的定长字段，去掉尾空格
      if (p.cs[i] == ' ') p.cs[i] = 0;
      else if (p.cs[i]) break;
    }
    strncpy(p.type, a["t"] | "--", 5); p.type[5] = 0;
    strncpy(p.reg,  a["r"] | "",   9); p.reg[9]  = 0;
    p.lat = a["lat"]; p.lon = a["lon"];
    p.alt   = a["alt_baro"].is<int>() ? (int)a["alt_baro"] : 0;   // 地面上会是字符串 "ground"
    p.spd   = (int)((float)(a["gs"]        | 0.0f) * 1.852f);     // 节 -> km/h
    p.hdg   = (int)((float)(a["track"]     | 0.0f));
    p.vrate = (int)((float)(a["baro_rate"] | 0.0f));

    // 等距圆柱近似：这个距离尺度（<150km）上误差可以忽略，比正经大圆公式省一堆三角函数
    float dlat = p.lat - fix.lat;
    // 经度差要先归一化到 ±180：跨日期变更线时(比如本机 179.8E、飞机 179.8W)实际只差
    // 0.4°≈44km，直接相减却是 359.6°，会算出三万多公里、排序和方位角全错。
    float dlonDeg = p.lon - fix.lon;
    if (dlonDeg >  180.0f) dlonDeg -= 360.0f;
    if (dlonDeg < -180.0f) dlonDeg += 360.0f;
    float dlon = dlonDeg * cosf(fix.lat * PI / 180.0f);
    p.dist = sqrtf(dlat * dlat + dlon * dlon) * 111.0f;
    p.brg  = atan2f(dlon, dlat) * 180.0f / PI;
    if (p.brg < 0) p.brg += 360;

    // 只保留最近的 MAX_PLANES 架：插入排序进有序数组，不用先收全再排
    int pos = n;
    while (pos > 0 && planes[pos - 1].dist > p.dist) {
      if (pos < MAX_PLANES) planes[pos] = planes[pos - 1];
      pos--;
    }
    if (pos < MAX_PLANES) planes[pos] = p;
    if (n < MAX_PLANES) n++;
  }
  planeCount = n;
  if (selIdx >= planeCount) selIdx = 0;
  everFetched = true;
  // 自动刷新会重排列表，同一个下标很可能已经换了一架飞机——重新走一遍航线查询
  // （缓存命中的话不会真发请求）
  selChangedMs = millis();
  dirty = true;
}

static DeferredFetch job;   // 延后首拉，别在按键/绘制路径里同步拉网络（见 net_job.h）

// 航线查询也是同步阻塞的（还多一次 TLS 握手），所以：一是等选中项稳定下来再查，
// 连着翻页时不会每翻一格就卡一秒；二是这段静默期正好让"route ..."那帧先推上屏。
static const uint32_t ROUTE_DEBOUNCE_MS = 350;

// 交给后台工人查的那个呼号：主线程填好再起任务（工人的入口不带参数）
static char routeCs[10];
static void fetchRouteJob() { fetchRoute(routeCs); }

static void routeEnsureSel() {
  if (planeCount == 0) return;
  const char* cs = planes[selIdx].cs;
  if (!cs[0]) return;                 // 没呼号就没得查
  if (routeFind(cs)) return;          // 命中缓存（含 MISS/ERR，避免反复重试）
  // 这条检查必须留在主线程：fetchRoute() 里时钟没对上是直接返回、不写缓存的，
  // 放进工人里就变成每一轮 loop 都起一个任务、一进去就空手返回。
  if (!tlsClockReady()) return;
  strncpy(routeCs, cs, sizeof(routeCs) - 1);
  routeCs[sizeof(routeCs) - 1] = 0;
  bgFetchRun(fetchRouteJob);
}

void adsbEnter() {
  selIdx = 0; planeCount = 0; errMsg = ""; everFetched = false;
  routeCacheClear();
  job.request();
  lastFetchMs = millis();
  selChangedMs = millis();
  dirty = true;
}

void adsbUpdate() {
  // 拉取交给后台工人（bg_fetch.h）；lastFetchMs 先在主线程记上，工人还没开跑时别被重复触发
  if (job.due()) { lastFetchMs = millis(); bgFetchRun(fetchPlanes); return; }
  if (millis() - lastFetchMs >= AUTO_MS) { lastFetchMs = millis(); bgFetchRun(fetchPlanes); return; }

  // job.pending：按了 r、列表马上要重拉，别再按旧列表查一趟航线（见 net_job.h）
  if (!job.pending && millis() - selChangedMs >= ROUTE_DEBOUNCE_MS) routeEnsureSel();
}

void adsbKey(char k) {
  // 走同一条延后路径：按键回调里同步拉网络照样会卡住主循环一秒
  if (k == 'r' || k == 'R') { routeCacheClear(); job.request(); }
  else if (planeCount > 0 && (k == ';' || k == ',')) { listMoveIndex(selIdx, planeCount, -1); selChangedMs = millis(); }
  else if (planeCount > 0 && (k == '.' || k == '/')) { listMoveIndex(selIdx, planeCount, +1); selChangedMs = millis(); }
  dirty = true;
}

// 飞机剪影：机身 + 后掠翼 + 尾翼，整体按航向 hdg 旋转
static void drawPlaneGlyph(int cx, int cy, float s, float hdg, uint16_t col) {
  float a = hdg * PI / 180.0f;
  float fx = sinf(a), fy = -cosf(a), rx = cosf(a), ry = sinf(a);
  #define PX(al, si) (int)(cx + (al) * s * fx + (si) * s * rx)
  #define PY(al, si) (int)(cy + (al) * s * fy + (si) * s * ry)
  cv.fillTriangle(PX(1.5,0),PY(1.5,0), PX(-1.3,-0.22),PY(-1.3,-0.22), PX(-1.3,0.22),PY(-1.3,0.22), col);
  cv.fillTriangle(PX(0.35,0),PY(0.35,0), PX(-0.55,-1.35),PY(-0.55,-1.35), PX(-0.85,-1.25),PY(-0.85,-1.25), col);
  cv.fillTriangle(PX(0.35,0),PY(0.35,0), PX(-0.55,1.35),PY(-0.55,1.35), PX(-0.85,1.25),PY(-0.85,1.25), col);
  cv.fillTriangle(PX(-1.0,0),PY(-1.0,0), PX(-1.35,-0.55),PY(-1.35,-0.55), PX(-1.45,-0.48),PY(-1.45,-0.48), col);
  cv.fillTriangle(PX(-1.0,0),PY(-1.0,0), PX(-1.35,0.55),PY(-1.35,0.55), PX(-1.45,0.48),PY(-1.45,0.48), col);
  #undef PX
  #undef PY
}

void drawAdsb() {
  job.markShown();   // ⚠️ 必须在最前面，下面几个空状态分支会提前 return（见 net_job.h）
  cv.fillScreen(TFT_BLACK);

  // 仰角上限只取 30 度：真实几何下 35000ft 的飞机在 50km 外也才 12 度，
  // 上限设太大(之前是 60)会把所有飞机压成贴着地平线的一条线。
  // 再对比例开个平方根，把低仰角那一段拉开——头顶正上方的飞机本来就少。
  // 地平线从 78 收到 68，是为了给下面的详情卡腾出航线那一行。天空图上半部本来就空
  // （maxEl=30，35000ft 的飞机在 50km 外也才 12 度），压掉 10px 几乎看不出来。
  const int ytop = 14, yhor = 68, maxEl = 30, hh = yhor - ytop;
  drawSkyBg(ytop, yhor, maxEl);

  // 顶栏（画在天空之上）：左=第几架/共几架，中=按键提示，右=半径/定位源/数据年龄
  cv.fillRect(0, 0, SW, ytop, TFT_BLACK);
  char hdr[36];
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(ACCENT, TFT_BLACK);
  if (planeCount > 0) snprintf(hdr, sizeof(hdr), "AIRCRAFT %d/%d", selIdx + 1, planeCount);
  else                snprintf(hdr, sizeof(hdr), "AIRCRAFT %d", planeCount);
  cv.drawString(hdr, 6, 3);
  cv.setTextDatum(top_center);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString(";/. pick  r", SW / 2 + 8, 3);
  cv.setTextDatum(top_right);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  snprintf(hdr, sizeof(hdr), "%dnm %s %lus", RADIUS_NM, fromGps ? "gps" : "ip",
           (unsigned long)((millis() - lastFetchMs) / 1000));
  cv.drawString(hdr, SW - 4, 3);

  // 远的先画，近的压在上面；选中的那架画成白色并标呼号
  for (int i = planeCount - 1; i >= 0; i--) {
    float altm = planes[i].alt * 0.3048f, distm = planes[i].dist * 1000.0f;
    float el = (distm > 1) ? atan2f(altm, distm) * 180.0f / PI : maxEl;
    if (el > maxEl) el = maxEl;
    if (el < 0) el = 0;
    int x = (int)(planes[i].brg / 360.0f * SW);
    int y = yhor - (int)(sqrtf(el / maxEl) * hh);
    if (y < ytop + 5) y = ytop + 5;               // 贴顶的收一下（纵轴不是环形，只能夹）
    uint16_t col = (i == selIdx) ? TFT_WHITE
                 : planes[i].alt > 25000 ? cv.color565(74, 184, 160)
                 : planes[i].alt > 10000 ? cv.color565(224, 176, 64)
                                         : cv.color565(224, 128, 64);
    float sz = 6.0f - planes[i].dist / 26.0f;
    if (sz < 3.0f) sz = 3.0f;
    // 正北方向的飞机在环形横轴上会被劈成两半（两端都是 N），绕着画而不是往里夹——
    // 夹是把方位说谎 8 度左右，而方位正是这一页要传达的东西。见 ui_common.h 的 drawWrapped。
    drawWrapped(x, SW, (int)(sz * 1.5f) + 1, [&](int xx) {
      drawPlaneGlyph(xx, y, sz, planes[i].hdg, col);
    });
    if (i == selIdx && planes[i].cs[0]) {
      // 呼号只画一次：两边各来一个反而更乱。它是标注不是位置，夹回屏内没有信息损失。
      cv.setTextDatum(bottom_center); cv.setTextSize(1);
      cv.setTextColor(TFT_WHITE);
      int lx = x, w = (int)strlen(planes[i].cs) * 6 / 2;
      if (lx - w < 2) lx = 2 + w;                 // 呼号别顶出左右边界
      if (lx + w > SW - 2) lx = SW - 2 - w;
      cv.drawString(planes[i].cs, lx, y - (int)(sz * 1.5f) - 2);
    }
  }

  // 底部详情卡。cardTop 必须留在地平线方位刻度(yhor+3 起那行字)下面，不然 N/E/S/W 会被盖掉
  const int cardTop = 80, cy0 = cardTop + 2;

  if (planeCount == 0) {
    cv.setTextDatum(top_center); cv.setTextSize(1);
    if (errMsg.length()) {
      cv.setTextColor(TFT_RED, TFT_BLACK);
      cv.drawString(trunc(errMsg, 38), SW / 2, cy0 + 6);
      cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
      cv.drawString("r = retry", SW / 2, cy0 + 22);
    } else {
      cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
      cv.drawString(everFetched ? "no aircraft in range" : "loading...", SW / 2, cy0 + 12);
    }
    return;
  }

  Plane& p = planes[selIdx];
  cv.drawRoundRect(4, cardTop, SW - 8, SH - cardTop - 2, 5, DIM_BORDER);

  cv.setTextDatum(top_left); cv.setTextSize(2);
  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  cv.drawString(p.cs[0] ? p.cs : "------", 10, cy0);
  cv.setTextDatum(top_right); cv.setTextSize(1);
  cv.setTextColor(ACCENT, TFT_BLACK);
  char b[40];
  snprintf(b, sizeof(b), "%s %s", p.type, p.reg);
  cv.drawString(b, SW - 10, cy0 + 2);

  // ---- 航线行：起点 ━━━●──── 终点 ----
  const int rowRoute = cardTop + 20;
  Route* rt = p.cs[0] ? routeFind(p.cs) : nullptr;
  float remainKm = 0, prog = -1.0f;
  if (rt && rt->state == RT_OK) prog = routeProgress(*rt, p.lat, p.lon, &remainKm);

  if (prog >= 0) {
    cv.setTextSize(1);
    cv.setTextColor(ACCENT, TFT_BLACK);
    cv.setTextDatum(top_left);  cv.drawString(rt->orig, 10, rowRoute);
    cv.setTextDatum(top_right); cv.drawString(rt->dest, SW - 10, rowRoute);

    int x1 = 10 + (int)strlen(rt->orig) * 6 + 6;
    int x2 = SW - 10 - (int)strlen(rt->dest) * 6 - 6;
    int by = rowRoute + 4;
    if (x2 > x1 + 8) {
      cv.drawFastVLine(x1, by - 2, 5, ICON_DIM);          // 两端小刻度，免得进度 0%/100% 时看不出轨道
      cv.drawFastVLine(x2, by - 2, 5, ICON_DIM);
      cv.drawFastHLine(x1, by, x2 - x1, DIM_BORDER);
      int px = x1 + (int)((x2 - x1) * prog);
      cv.drawFastHLine(x1, by, px - x1, ACCENT);
      cv.fillCircle(px, by, 2, TFT_WHITE);
    }
  } else {
    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    const char* msg = !p.cs[0]           ? "no callsign"
                    : !rt                ? "route ..."
                    : rt->state == RT_OK ? "route doesn't match position"
                    : rt->state == RT_MISS ? "no route data"
                                           : "route lookup failed";
    cv.drawString(msg, SW / 2, rowRoute);
  }

  // 升降状态：±128 ft/min 以内当作平飞（ADS-B 的气压升降率本来就抖）
  const char* vr = p.vrate > 128 ? "CLIMB" : (p.vrate < -128 ? "DESC" : "LEVEL");
  uint16_t vc = p.vrate > 128 ? cv.color565(106, 192, 128)
              : p.vrate < -128 ? cv.color565(224, 128, 64) : ICON_DIM;
  cv.setTextDatum(top_left); cv.setTextSize(1);
  snprintf(b, sizeof(b), "FL%03d  %s %d", p.alt / 100, vr, abs(p.vrate));
  cv.setTextColor(vc, TFT_BLACK);
  cv.drawString(b, 10, cardTop + 32);

  if (prog >= 0) {                       // 行程进度的数值版，跟上面那条轨道同一行右侧对齐
    snprintf(b, sizeof(b), "%d%%  %.0fkm to go", (int)(prog * 100 + 0.5f), remainKm);
    cv.setTextDatum(top_right);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString(b, SW - 10, cardTop + 32);
  }

  snprintf(b, sizeof(b), "%dkm/h  HDG%03d  %s %.0fkm", p.spd, p.hdg, cardOf(p.brg), p.dist);
  cv.setTextDatum(top_left);
  cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  cv.drawString(b, 10, cardTop + 43);
}
