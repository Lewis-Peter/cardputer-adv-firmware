#include "sats.h"
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
#include "secrets.h"
#include "http_json.h"

static const int MAX_SATS = 60;
// /above 的搜索半径按**类别**走，不能一刀切。这个半径是"卫星星下点离观察者多少度以内"，
// 而一颗卫星能被看见的条件是星下点落在地平线圆内，那个圆的半径 = acos(Re/(Re+h))：
//     Starlink  h≈550km   -> 23.0°
//     GPS       h≈20200km -> 76.1°
// 原来两者都写死 70°：对 GPS 偏小（漏掉边上的），对 Starlink 则是灾难——实测拉回 122 颗
// 共 18330 字节，其中绝大多数在地平线以下、进来就被 `el < 0` 扔掉，纯属白下载。
// 而这一页还背着 TLS，17KB 的响应直接把堆挤到 6.2KB，body 读不完，报 json: IncompleteInput。
// 收到贴合各自地平线的半径之后：Starlink 4 颗 680 字节，GPS 11 颗 1842 字节。
static const uint32_t AUTO_MS = 30000;

struct SatCat { const char* name; int id; int radiusDeg; };
static const SatCat CATS[] = { {"STARLINK", 52, 25}, {"GPS", 20, 90} };
static const int CAT_COUNT = sizeof(CATS) / sizeof(CATS[0]);
static int catIdx = 0;

struct Sat { char name[14]; float az, el, alt; };

static Sat*    sats = nullptr;
static int     satCount = 0;
static String  errMsg = "";
static bool    everFetched = false;
static uint32_t lastFetchMs = 0;
static bool    fromGps = false;

// 星下点(纬经)+轨道高度 -> 从观测点看过去的方位角/仰角。
// 两点都换成地心直角坐标(ECEF)，差矢量再投到观测点的东/北/天(ENU)三个方向上。
static void azel(const GeoFix& obs, float slat, float slon, float salt, float& az, float& el) {
  const float R = 6371.0f, D = PI / 180.0f;
  float ro = R + 0.005f, rs = R + salt;
  float la0 = obs.lat * D, lo0 = obs.lon * D, la = slat * D, lo = slon * D;
  float xo = ro * cosf(la0) * cosf(lo0), yo = ro * cosf(la0) * sinf(lo0), zo = ro * sinf(la0);
  float xs = rs * cosf(la)  * cosf(lo),  ys = rs * cosf(la)  * sinf(lo),  zs = rs * sinf(la);
  float dx = xs - xo, dy = ys - yo, dz = zs - zo;
  float E = -sinf(lo0) * dx + cosf(lo0) * dy;
  float N = -sinf(la0) * cosf(lo0) * dx - sinf(la0) * sinf(lo0) * dy + cosf(la0) * dz;
  float U =  cosf(la0) * cosf(lo0) * dx + cosf(la0) * sinf(lo0) * dy + sinf(la0) * dz;
  az = atan2f(E, N) * 180.0f / PI;
  if (az < 0) az += 360;
  el = atan2f(U, sqrtf(E * E + N * N)) * 180.0f / PI;
}

static void fetchSats() {
  // 错误字符串先备好容量：下面 TLS 那段借走了画布，期间再给它长内存会落进画布腾出的洞里
  errMsg.reserve(96);
  errMsg = "";
  lastFetchMs = millis();
  satCount = 0;
  if (!sats) sats = (Sat*)malloc(sizeof(Sat) * MAX_SATS);
  if (!sats) { errMsg = "out of memory"; dirty = true; return; }

  if (strlen(N2YO_API_KEY) == 0) { errMsg = "no N2YO key in secrets.h"; dirty = true; return; }
  if (!wifiEnsureConnected()) { errMsg = "wifi not connected"; dirty = true; return; }
  // 证书校验要比有效期，时钟没对上必然失败——分开报，别混成网络错
  if (!tlsClockReady()) { errMsg = "waiting for clock (NTP)"; dirty = true; return; }

  GeoFix fix;
  if (!geoGet(fix, &errMsg)) { dirty = true; return; }
  fromGps = fix.fromGps;

  char url[200];
  snprintf(url, sizeof(url),
           "https://api.n2yo.com/rest/v1/satellite/above/%.4f/%.4f/0/%d/%d/&apiKey=%s",
           fix.lat, fix.lon, CATS[catIdx].radiusDeg, CATS[catIdx].id, N2YO_API_KEY);

  // TLS 握手要两块 ~16.7KB 缓冲外加证书链，冷启动时空闲堆会被挤到 ~6KB，堆碎过之后实测到过
  // 1.6KB（USB 串口因此掉线）。握手和解析期间屏幕反正不动，把画布借出来用，出作用域自动还。
  // lease 必须声明在 filter/doc 之前（逆序析构：它们先释放，画布后恢复）；client 在它之前建好，
  // 连接和 mbedTLS 缓冲在 fetchJsonHttp 里 http.end() 时就已经释放了。
  // sats 数组、errMsg 都在进来之前备好了，这个作用域里只有用完就放的分配。
  // ⚠️ 这一页必须留 HTTPS，不能学 ADS-B/天气/瓦片换明文：N2YO 是把 API key 塞在 URL 里的，
  // 走明文等于把 key 明着发出去。代价是握手那几百毫秒和 40KB 连续堆，认了。
  // client 和证书包要在 lease 之前建：tlsUseCaBundle() 每次都会重新 calloc 一块永不释放的
  // 证书索引（~484B），放进 lease 里它会落在画布腾出的洞里把画布钉死。
  WiFiClientSecure client;
  // ⚠️ 这一页尤其该真校验：N2YO 把 API key 塞在 URL 里。原来 setInsecure() 只挡得住
  // 被动窃听，主动中间人拿张自签证书就能收下那个 key——README 安全边界第 2 条写的
  // "Sats 保留 HTTPS 的理由只兑现了一半"，说的就是这个。现在兑现另一半。
  tlsUseCaBundle(client);
  // 同 okx.cpp：覆盖框架默认 120s 握手超时，避免 N2YO 握手丢包卡死主循环
  client.setHandshakeTimeout(10);

  CanvasLease lease;
  JsonDocument filter;
  JsonObject fa = filter["above"].add<JsonObject>();
  fa["satname"] = true; fa["satlat"] = true; fa["satlng"] = true; fa["satalt"] = true;
  // info.satcount 也要留：**一颗都没有时 N2YO 根本不返回 above 键**（实测尼莫点半径 5°
  // 只回 {"info":{...,"satcount":0}}），光看 above 在不在分不出"天上没有"和"响应坏了"。
  JsonObject fi = filter["info"].to<JsonObject>();
  fi["satcount"] = true;

  JsonDocument doc;
  HttpJsonOptions options(10000, 15000);
  // ⚠️ 必须 stream=false。N2YO 回的是 Transfer-Encoding: chunked，而 HTTPClient 的
  // getStream() 给的是**没解块的原始字节**——解析器会把十六进制的块长度当数据吃掉。
  // 更坏的是它不报错：块长度 "44a" 被当成数字 44 解析成功，doc["above"] 为空，
  // 于是页面理直气壮地显示 "STARLINK 0 / nothing overhead"，而头顶其实有 112 颗。
  // getString() 会替我们解块。weather.cpp 顶上早就记了这条坑，这一页当时没照做。
  options.stream = false;
  if (!fetchJsonHttp(client, url, doc, &filter, options, errMsg)) { dirty = true; return; }

  // 分清三件事，别都显示成 0：
  //   连 info 都没有        -> 响应根本不是我们以为的东西（协议变了/又踩了解析假成功）
  //   satcount == 0        -> 天上真的没有，above 键本来就不会出现，这是正常的
  //   satcount > 0 却没 above -> 那才是响应坏了
  JsonVariant info = doc["info"];
  if (info.isNull()) { errMsg = "bad response (no 'info')"; dirty = true; return; }
  const int satcount = info["satcount"] | -1;
  JsonArray above = doc["above"].as<JsonArray>();
  if (above.isNull() && satcount != 0) { errMsg = "bad response (no 'above')"; dirty = true; return; }

  int n = 0;
  for (JsonObject s : above) {
    if (n >= MAX_SATS) break;
    const char* nm = s["satname"] | "SAT";
    const char* dash = strrchr(nm, '-');            // "STARLINK-1234" 只留编号，屏幕放不下全名
    strncpy(sats[n].name, dash ? dash + 1 : nm, 13);
    sats[n].name[13] = 0;
    azel(fix, s["satlat"] | 0.0f, s["satlng"] | 0.0f, s["satalt"] | 550.0f,
         sats[n].az, sats[n].el);
    sats[n].alt = s["satalt"] | 0.0f;
    n++;
  }
  // 按仰角从高到低排（列表里先看最"当头"的几颗）
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (sats[j].el > sats[i].el) { Sat t = sats[i]; sats[i] = sats[j]; sats[j] = t; }

  satCount = n;
  everFetched = true;
  dirty = true;
}

static DeferredFetch job;   // 延后首拉，别在按键/绘制路径里同步拉网络（见 net_job.h）

void satsEnter() {
  if (!sats) sats = (Sat*)malloc(sizeof(Sat) * MAX_SATS);
  satCount = 0; errMsg = ""; everFetched = false;
  job.request();
  lastFetchMs = millis();
  dirty = true;
}

void satsExit() {
  if (sats) {
    free(sats);
    sats = nullptr;
    satCount = 0;
  }
}

void satsUpdate() {
  // 拉取交给后台工人（bg_fetch.h）；lastFetchMs 先在主线程记上，工人还没开跑时别被重复触发
  if (job.due()) { lastFetchMs = millis(); bgFetchRun(fetchSats); return; }
  if (millis() - lastFetchMs >= AUTO_MS) { lastFetchMs = millis(); bgFetchRun(fetchSats); }
}

void satsKey(char k) {
  // 走同一条延后路径：按键回调里同步拉网络照样会卡住主循环一秒
  if (k == 'r' || k == 'R') job.request();
  else if (k == 'c' || k == 'C' || k == ';' || k == '.') {
    catIdx = (catIdx + 1) % CAT_COUNT;
    satCount = 0;                     // 换类别，旧的那批先清掉，别让人以为是新数据
    job.request();
  }
  dirty = true;
}

void drawSats() {
  job.markShown();   // ⚠️ 必须在最前面，下面几个空状态分支会提前 return（见 net_job.h）
  cv.fillScreen(TFT_BLACK);

  // 1. 顶栏标准化
  char stBuf[32];
  snprintf(stBuf, sizeof(stBuf), "%s (%d)", CATS[catIdx].name, satCount);
  drawPageHeader("Satellite Radar", stBuf, satCount > 0 ? ACCENT : 0x9CD3);

  // 2. 天空雷达视图
  const int ytop = 13, yhor = 88, maxEl = 90;
  drawSkyBg(ytop, yhor, maxEl);

  for (int i = 0; i < satCount; i++) {
    int x, y;
    if (!skyCoord(sats[i].az, sats[i].el, ytop, yhor, maxEl, x, y)) continue;
    float el = sats[i].el;
    uint16_t col = el > 60 ? TFT_WHITE : (el > 30 ? cv.color565(154, 122, 232) : ICON_DIM);
    int r = el > 60 ? 3 : 2;
    const bool glow = el > 70;
    // 横轴是环形的（两端都是 N），正北的星必须绕着画，见 ui_common.h 的 drawWrapped。
    drawWrapped(x, SW, r + (glow ? 2 : 0), [&](int xx) {
      cv.fillCircle(xx, y, r, col);
      if (glow) cv.drawCircle(xx, y, r + 2, 0x2492);   // 高仰角加个辉光圈
    });
  }

  // 3. 底部目标 HUD 卡片
  if (satCount == 0) {
    cv.fillRoundRect(8, 98, SW - 16, 26, 3, 0x0821);
    cv.drawRoundRect(8, 98, SW - 16, 26, 3, 0x18C3);
    cv.setTextDatum(middle_center);
    cv.setTextSize(1);
    if (errMsg.length()) {
      cv.setTextColor(TFT_RED, 0x0821);
      cv.drawString(trunc(errMsg, 36), SW / 2, 111);
    } else {
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString(everFetched ? "No satellites overhead" : "Scanning sky...", SW / 2, 111);
    }
  } else {
    for (int i = 0; i < satCount && i < 2; i++) {
      int cardY = 98 + i * 14;
      cv.fillRoundRect(4, cardY, SW - 8, 12, 2, 0x0821);
      cv.drawRoundRect(4, cardY, SW - 8, 12, 2, i == 0 ? 0x2492 : 0x18C3);

      cv.setTextDatum(middle_left);
      cv.setTextSize(1);

      // 序号
      char tag[8]; snprintf(tag, sizeof(tag), "#%d", i + 1);
      cv.setTextColor(i == 0 ? 0x07FF : 0x632C, 0x0821);
      cv.drawString(tag, 7, cardY + 6);

      // 卫星名称
      cv.setTextColor(i == 0 ? TFT_WHITE : 0xCE79, 0x0821);
      cv.drawString(sats[i].name, 24, cardY + 6);

      // 仰角
      char elBuf[12]; snprintf(elBuf, sizeof(elBuf), "EL %2.0f", sats[i].el);
      cv.setTextColor(0x07E0, 0x0821);
      cv.drawString(elBuf, 74, cardY + 6);

      // 方位角
      char azBuf[16]; snprintf(azBuf, sizeof(azBuf), "AZ %03.0f %-2s", sats[i].az, cardOf(sats[i].az));
      cv.setTextColor(i == 0 ? TFT_WHITE : 0x9CD3, 0x0821);
      cv.drawString(azBuf, 122, cardY + 6);

      // 距离/高度
      char altBuf[16]; snprintf(altBuf, sizeof(altBuf), "%.0fkm", sats[i].alt);
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString(altBuf, 192, cardY + 6);
    }
  }

  // 4. 底部状态与快捷键
  cv.setTextDatum(top_left);
  cv.setTextSize(1);
  const int footY = 126;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("C", 6, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" cat", 12, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", 48, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" ref", 54, footY);

  char ageBuf[20];
  snprintf(ageBuf, sizeof(ageBuf), "%s %lus", fromGps ? "gps" : "ip",
           (unsigned long)((millis() - lastFetchMs) / 1000));
  cv.setTextColor(0x632C, TFT_BLACK); cv.drawString(ageBuf, 110, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 198, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" back", 204, footY);
}
