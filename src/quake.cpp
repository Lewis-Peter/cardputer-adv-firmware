#include "quake.h"
#include "bg_fetch.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <cmath>
#include <ctime>
#include "tls_ca.h"
#include "ui_common.h"
#include "net_job.h"
#include "wifi_net.h"
#include "geoloc.h"
#include "worldmap.h"
#include "http_json.h"
#include "list_sel.h"

static const int MAX_EQ = 16;                        // 屏幕一次只看得了几条，留 16 够翻
static const uint32_t AUTO_MS = 10UL * 60 * 1000;    // USGS 摘要源约 1 分钟更新，10 分钟重拉够用
static const int ROWS_VISIBLE = 5;

// 两个源：一个看"今天地球哪儿动了"，一个看"这一周有没有真的大事"。
// ⚠️ 2.5_day 平时 100~150 条、整包 200KB+，只有逐元素解析扛得住（见 quake.h）。
static const struct { const char* file; const char* label; } FEEDS[] = {
  {"2.5_day",  "M2.5+ 24h"},
  {"4.5_week", "M4.5+ 7d"},
};
static const int FEED_COUNT = (int)(sizeof(FEEDS) / sizeof(FEEDS[0]));
static int feedIdx = 0;

struct Eq {
  float  mag;
  float  lat, lon;
  int    depthKm;
  time_t when;                 // 秒级 epoch（源里是毫秒，读进来就除掉）
  char   place[34];            // "125 km SSE of Sand Point, Alaska" 这种，超长截断
};
static Eq  eqs[MAX_EQ];
static int eqN = 0;
static int selIdx = 0;
static int scrollTop = 0;

static String   errMsg = "";
static bool     everFetched = false;
static uint32_t lastFetchMs = 0;
static bool     haveMe = false;
static float    meLat = 0, meLon = 0;

// ---- 取数 ----

// 按时间倒序插进 eqs[]，满了就把最旧的挤掉。
//
// 不假设源是排好序的：USGS 的摘要源**目前**是新的在前，但那不是接口契约的一部分，
// 而"只读前 16 条"一旦碰上没排序的源就会拿到随便 16 条。反正是流式读，读完全部再挑
// 最新的 16 条不多花一个字节堆——代价只有每条最多 16 次比较。
static void insertEq(const Eq& e) {
  int pos = 0;
  while (pos < eqN && eqs[pos].when >= e.when) pos++;
  if (pos >= MAX_EQ) return;                       // 比现有 16 条都旧，丢掉
  int last = (eqN < MAX_EQ) ? eqN : MAX_EQ - 1;
  for (int i = last; i > pos; i--) eqs[i] = eqs[i - 1];
  eqs[pos] = e;
  if (eqN < MAX_EQ) eqN++;
}

static void fetchQuakes() {
  // 错误字符串先备好容量：下面 TLS 那段借走了画布，期间再给它长内存会落进画布腾出的洞里
  errMsg.reserve(96);
  errMsg = "";
  lastFetchMs = millis();
  eqN = 0;

  if (!wifiEnsureConnected()) { errMsg = "wifi not connected"; dirty = true; return; }
  // 证书校验要比对有效期，时钟没对上必然握手失败——分开报，别混成网络错。
  // 这一页还有第二个理由必须等时钟：所有"多久之前"都是拿 time(nullptr) 减出来的。
  if (!tlsClockReady()) { errMsg = "waiting for clock (NTP)"; dirty = true; return; }

  GeoFix fix;
  haveMe = geoGet(fix, nullptr);        // 拿不到定位不算错：列表和地图照画，只是没有"离你多远"
  if (haveMe) { meLat = fix.lat; meLon = fix.lon; }

  char url[128];
  snprintf(url, sizeof(url),
           "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/%s.geojson",
           FEEDS[feedIdx].file);

  // TLS 握手要两块 ~16.7KB 缓冲外加证书链，流式抓取期间把画布借出去，出作用域自动还。
  // client 和证书包在 lease 之前建（tlsUseCaBundle 每次都会重新 calloc 一块永不释放的证书索引）；
  // lease 必须声明在 filter 之前（逆序析构：filter 先释放，画布后恢复）。
  // eqs 是静态数组，errMsg 在进来之前已 reserve，作用域里只有用完即放的分配。
  WiFiClientSecure client;
  tlsUseCaBundle(client);               // 真校验，不是 setInsecure（见 tls_ca.h）
  // 同 okx.cpp：覆盖框架默认 120s 握手超时，避免 USGS 握手丢包卡死主循环
  client.setHandshakeTimeout(10);

  CanvasLease lease;
  JsonDocument filter;
  filter["properties"]["mag"]   = true;
  filter["properties"]["place"] = true;
  filter["properties"]["time"]  = true;
  filter["geometry"]["coordinates"] = true;

  HttpJsonOptions options(10000, 20000);
  bool ok = fetchJsonStreamArray(client, url, "\"features\":[", &filter, options,
    [](JsonDocument& elem, int) -> bool {
      JsonObject pr = elem["properties"];
      JsonArray  co = elem["geometry"]["coordinates"];
      if (!pr.isNull() && !pr["mag"].isNull() && co.size() >= 2) {
        Eq e;
        e.mag = pr["mag"] | 0.0f;
        e.lon = co[0] | 0.0f;
        e.lat = co[1] | 0.0f;
        e.depthKm = (int)lroundf(co.size() >= 3 ? (float)(co[2] | 0.0f) : 0.0f);
        long long ms = pr["time"] | 0LL;
        e.when = (time_t)(ms / 1000);
        const char* p = pr["place"] | "";
        strncpy(e.place, p, sizeof(e.place) - 1);
        e.place[sizeof(e.place) - 1] = 0;
        insertEq(e);
      }
      return true;
    }, errMsg);

  if (!ok) {
    eqN = 0;
    dirty = true;
    return;
  }

  if (selIdx >= eqN) selIdx = 0;
  scrollTop = 0;
  everFetched = true;
  dirty = true;
}

// ---- 生命周期 ----

static DeferredFetch job;

void quakeEnter() {
  feedIdx = loadUChar("quake", "feed", 0);
  if (feedIdx >= FEED_COUNT) feedIdx = 0;    // 存的是 uint8_t，只可能大不可能负
  selIdx = 0; scrollTop = 0;
  errMsg = ""; everFetched = false; eqN = 0;
  job.request();
  lastFetchMs = millis();
  dirty = true;
}

void quakeUpdate() {
  // 拉取交给后台工人（bg_fetch.h）；lastFetchMs 先在主线程记上，工人还没开跑时别被重复触发
  if (job.due()) { lastFetchMs = millis(); bgFetchRun(fetchQuakes); return; }
  if (millis() - lastFetchMs >= AUTO_MS) { lastFetchMs = millis(); bgFetchRun(fetchQuakes); }
}

void quakeKey(char k) {
  if (k == 'r' || k == 'R') job.request();          // 走延后路径，别在按键回调里同步拉
  else if (k == 'm' || k == 'M') {                  // 换数据源，选择记进 NVS
    feedIdx = (feedIdx + 1) % FEED_COUNT;
    saveUChar("quake", "feed", (uint8_t)feedIdx);
    eqN = 0; everFetched = false;
    job.request();
  } else if (eqN > 0 && (k == ';' || k == ',')) {
    listMove(selIdx, scrollTop, eqN, ROWS_VISIBLE, -1);
  } else if (eqN > 0 && (k == '.' || k == '/')) {
    listMove(selIdx, scrollTop, eqN, ROWS_VISIBLE, 1);
  }
  dirty = true;
}

// ---- 绘制 ----

// 震级配色。分档按感受而不是等分：4 以下基本无感，5 起开始有破坏，6 起是新闻，7 起是灾难。
static uint16_t magColor(float m) {
  if (m >= 7.0f) return cv.color565(242, 64, 64);
  if (m >= 6.0f) return cv.color565(232, 116, 48);
  if (m >= 5.0f) return cv.color565(226, 190, 60);
  if (m >= 4.0f) return ACCENT;
  return ICON_DIM;
}

// "3m" / "5h" / "2d"——一格只放得下三个字符，所以按量级换单位
static void ageStr(char* out, size_t n, time_t when) {
  long s = (long)(time(nullptr) - when);
  if (s < 0) s = 0;                                  // 源里的时间比本机还新（时钟刚对上）
  if (s < 3600)        snprintf(out, n, "%ldm", s / 60);
  else if (s < 86400)  snprintf(out, n, "%ldh", s / 3600);
  else                 snprintf(out, n, "%ldd", s / 86400);
}

// 没数据/出错时两页共用的占位，返回 true 表示已经画完、调用方直接 return
static bool drawEmpty() {
  if (eqN > 0) return false;
  cv.setTextDatum(top_center); cv.setTextSize(1);
  if (errMsg.length()) {
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.drawString(trunc(errMsg, 38), SW / 2, 54);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("r  retry     m  switch feed", SW / 2, 70);
  } else if (!everFetched) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("loading...", SW / 2, 60);
  } else {
    // 这不是故障。4.5+/7d 空着确实少见，2.5+/24h 空着基本只可能是源出问题了，
    // 但两种都不该报红——说清楚"什么都没发生"本身就是一条信息。
    cv.setTextColor(ACCENT, TFT_BLACK);
    cv.drawString("nothing above threshold", SW / 2, 50);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("m  switch feed", SW / 2, 68);
  }
  drawPageDots();
  return true;
}

void drawQuake() {
  job.markShown();                 // ⚠️ 必须第一行，见 net_job.h
  cv.fillScreen(TFT_BLACK);
  drawPageHeader("Quake", FEEDS[feedIdx].label, ICON_DIM);
  if (drawEmpty()) return;

  // 列表：一行 15px，从 y=16 起排 5 行
  const int rowH = 15, y0 = 16;
  for (int r = 0; r < ROWS_VISIBLE; r++) {
    int i = scrollTop + r;
    if (i >= eqN) break;
    const Eq& e = eqs[i];
    int y = y0 + r * rowH;
    bool sel = (i == selIdx);
    if (sel) {
      // CARD_BG 单独用太暗，在这块屏上跟黑底几乎分不出来（菜单里是靠边框才看得见的）。
      // 左边再加一条 ACCENT 竖条，一眼就知道选中的是哪一行。
      cv.fillRoundRect(2, y - 1, SW - 4, rowH - 1, 3, CARD_BG);
      cv.fillRect(2, y, 2, rowH - 3, ACCENT);
    }

    char b[16];
    cv.setTextSize(1);
    // 震级：整行里唯一该被一眼扫到的东西，所以左边独占一格、按震级上色
    snprintf(b, sizeof(b), "%.1f", e.mag);
    cv.setTextDatum(top_left);
    cv.setTextColor(magColor(e.mag), sel ? CARD_BG : TFT_BLACK);
    cv.drawString(b, 8, y + 3);          // 8 不是 6：选中行左边那条 ACCENT 竖条占了 2..3

    ageStr(b, sizeof(b), e.when);
    cv.setTextDatum(top_right);
    cv.setTextColor(TFT_DARKGREY, sel ? CARD_BG : TFT_BLACK);
    cv.drawString(b, SW - 6, y + 3);
    int ageW = cv.textWidth(b);

    // 地名占中间剩下的全部：左边给震级留 26px，右边给 age 留出它的实际宽度 + 间距
    cv.setTextDatum(top_left);
    cv.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? CARD_BG : TFT_BLACK);
    int placeX = 32, placeW = SW - 6 - ageW - 4 - placeX;
    int placeCh = placeW / 6;                       // Font0 是 6px/字符
    if (placeCh < 1) placeCh = 1;
    cv.drawString(trunc(e.place, placeCh), placeX, y + 3);
  }
  drawScrollBar(SW - 4, y0, ROWS_VISIBLE * rowH, scrollTop, eqN, ROWS_VISIBLE);

  // 选中那条的详情。y 98/110 两行，正好压在 debug 条(124)之上
  const Eq& s = eqs[selIdx];
  cv.drawFastHLine(6, 94, SW - 12, DIM_BORDER);
  char b[48];
  cv.setTextSize(1); cv.setTextDatum(top_left);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  snprintf(b, sizeof(b), "depth %dkm", s.depthKm);
  cv.drawString(b, 6, 98);

  cv.setTextDatum(top_right);
  if (haveMe) {
    float km = gcKm(meLat, meLon, s.lat, s.lon);
    // 离得近才值得强调。>2000km 的地震跟你没关系，别用高亮色抢注意力
    cv.setTextColor(km < 500 ? cv.color565(242, 64, 64) : km < 2000 ? TFT_LIGHTGREY : ICON_DIM,
                    TFT_BLACK);
    snprintf(b, sizeof(b), "%.0f km away", km);
  } else {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    snprintf(b, sizeof(b), "no position");
  }
  cv.drawString(b, SW - 6, 98);

  // 本地时间。源给的是 UTC 毫秒，这里按设备时区显示——"什么时候"要能跟手表对上才有用
  struct tm lt{};
  cv.setTextDatum(top_left);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (localtime_r(&s.when, &lt))
    snprintf(b, sizeof(b), "%02d-%02d %02d:%02d", lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
  else
    snprintf(b, sizeof(b), "--");
  cv.drawString(b, 6, 110);

  cv.setTextDatum(top_right);
  snprintf(b, sizeof(b), "%d/%d   ;/. r m", selIdx + 1, eqN);
  cv.drawString(b, SW - 6, 110);

  drawPageDots();
}

void drawQuakeMap() {
  job.markShown();
  cv.fillScreen(TFT_BLACK);

  char right[24];
  snprintf(right, sizeof(right), "%d shown", eqN);
  drawPageHeader("Quake map", eqN ? right : nullptr, ICON_DIM);
  if (drawEmpty()) return;

  // 整幅等距圆柱世界图，跟晨昏线那页同一个投影和同一份掩码。
  // ⚠️ 底边收到 SH-24 是给下面那行说明留位置：页码点画在 SH-4、说明行画在 113..121，
  // 两者不能叠——说明行要是按别处的习惯锚在 SH-2，就正好压在页码点上。
  const int mapTop = 14, mapBot = SH - 24;
  const int mapH = mapBot - mapTop;
  const int rows = mapH < WORLD_MASK_H ? mapH : WORLD_MASK_H;
  const int cols = SW < WORLD_MASK_W ? SW : WORLD_MASK_W;
  const uint16_t landCol = cv.color565(22, 62, 42);
  for (int x = 0; x < cols; x++) {
    int runStart = 0;
    bool runLand = worldIsLand(x, 0);
    for (int r = 1; r <= rows; r++) {
      bool land = (r < rows) ? worldIsLand(x, r) : !runLand;   // 末行强制收尾
      if (land != runLand) {
        if (runLand) cv.drawFastVLine(x, mapTop + runStart, r - runStart, landCol);
        runStart = r; runLand = land;
      }
    }
  }
  cv.drawFastHLine(0, mapTop + rows / 2, SW, cv.color565(40, 40, 40));   // 赤道

  auto toXY = [&](float lat, float lon, int& x, int& y) {
    // x 故意**不夹**：绕线附近的点要靠 plotDot 补画到另一边，夹了就绕不回来。
    // 纬度没有这个问题（地图上下不相连），照夹。
    x = (int)((lon + 180.0f) / 360.0f * cols);
    y = mapTop + (int)((90.0f - lat) / 180.0f * rows);
    if (y < mapTop) y = mapTop; else if (y >= mapTop + rows) y = mapTop + rows - 1;
  };

  // 本机位置先画，让它压在最底下——地震点才是主角。
  // 跟下面的震中点一样要绕线补画：住在斐济/新西兰那边的话 x 同样会贴着屏幕边。
  if (haveMe) {
    int x, y; toXY(meLat, meLon, x, y);
    const uint16_t me = cv.color565(90, 130, 200);
    drawWrapped(x, cols, 3, [&](int xx) {
      cv.drawLine(xx - 3, y - 3, xx + 3, y + 3, me);
      cv.drawLine(xx - 3, y + 3, xx + 3, y - 3, me);
    });
  }

  // 画一个点。⚠️ 贴着国际日期变更线的震中（比如斐济那一带的 179.x°W）算出来 x 只有 1~2，
  // 圆直接被屏幕左边切掉一半，看着像画错了。地图本来就是绕成一圈的，所以越界的那半边
  // 补画到另一边去——多出来的部分自然被 sprite 裁掉，位置一个像素都没挪。
  auto plotDot = [&](int x, int y, int rad, uint16_t col, bool ring) {
    drawWrapped(x, cols, rad + (ring ? 3 : 0), [&](int xx) {
      cv.fillCircle(xx, y, rad, col);
      if (ring) cv.drawCircle(xx, y, rad + 3, TFT_WHITE);
    });
  };

  // 从旧到新画，新的压在上面；选中那条最后画，一定看得见
  for (int i = eqN - 1; i >= 0; i--) {
    if (i == selIdx) continue;
    int x, y; toXY(eqs[i].lat, eqs[i].lon, x, y);
    plotDot(x, y, 1 + (int)(eqs[i].mag / 2.5f), magColor(eqs[i].mag), false);   // M2.5→1 M5→3 M7.5→4
  }
  const Eq& sel = eqs[selIdx];
  {
    int x, y; toXY(sel.lat, sel.lon, x, y);
    plotDot(x, y, 1 + (int)(sel.mag / 2.5f), magColor(sel.mag), true);
  }

  // 底部一行：选中那条是谁
  char b[56], age[16];
  ageStr(age, sizeof(age), sel.when);
  cv.setTextSize(1);
  cv.setTextDatum(top_right);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString(age, SW - 4, 113);
  int ageW = cv.textWidth(age);

  snprintf(b, sizeof(b), "M%.1f  %s", sel.mag, sel.place);
  int capCh = (SW - 8 - ageW - 4) / 6;
  if (capCh < 1) capCh = 1;
  cv.setTextDatum(top_left);
  cv.setTextColor(magColor(sel.mag), TFT_BLACK);
  cv.drawString(trunc(b, capCh), 4, 113);

  drawPageDots();
}
