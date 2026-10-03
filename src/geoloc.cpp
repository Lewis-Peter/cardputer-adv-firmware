#include "geoloc.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "gnss.h"
#include <ctime>
#include "wifi_net.h"
#include "net_resolve.h"

static GeoFix cached;   // 只缓存 IP 定位的结果；GPS 每次现读
static int32_t cachedDay = -1;

// IP 定位结果落 NVS。理由：一次 ip-api 请求（DNS+TCP+HTTP）实测要一秒上下，
// 而它是同步做的——开机后第一次进天气/飞机页就得白等这一秒（实测整个进入过程 3 秒，
// 其中大头就是它）。位置这东西一天内基本不会变，存下来下次开机直接用。
// 用"儒略日"当版本号：跨天了才重新查一次；没对上时间就无条件复用（总比卡一秒强）。
static const char* GEO_NS = "geoloc";

static int32_t todayIndex() {
  if (!timeSynced) return -1;
  return (int32_t)(time(nullptr) / 86400);
}

static bool geoLoadCache() {
  PrefsLock lock;   // 四个键当一个整体读，别跟另一个线程的 end() 交错（见 globals.h）
  double la = loadDouble(GEO_NS, "lat", 1e9);
  double lo = loadDouble(GEO_NS, "lon", 1e9);
  String nm = loadString(GEO_NS, "name", "");
  int32_t day = (int32_t)loadInt(GEO_NS, "day", -1);
  if (la > 1e8 || lo > 1e8) return false;
  if (day < 0) return false;                                  // 无有效日期记录视为过期
  int32_t today = todayIndex();
  if (today >= 0 && day != today) return false;               // 跨天了，重新查
  cached.lat = la; cached.lon = lo; cached.name = nm;
  cached.fromGps = false; cached.valid = true;
  cachedDay = day;
  return true;
}

static void geoSaveCache() {
  PrefsLock lock;   // 同上，四个键当一个整体写
  int32_t today = todayIndex();
  if (today < 0) return;                                      // NTP 未同步时不写 NVS 缓存
  saveDouble(GEO_NS, "lat", cached.lat);
  saveDouble(GEO_NS, "lon", cached.lon);
  saveString(GEO_NS, "name", cached.name);
  saveInt(GEO_NS, "day", today);
}

void geoInvalidate() {
  cached.valid = false;
  cachedDay = -1;
  PrefsLock lock;   // 可能在后台拉取的工人上跑（见 globals.h）
  prefs.begin(GEO_NS, false);
  prefs.clear();
  prefs.end();
}

bool geoGetOffline(GeoFix& out) {
  // 天气之类只要大致位置：10 分钟内的 GPS 定位都比 IP 定位准
  if (gnssHasFix(10UL * 60 * 1000)) {
    out.lat = gnssLat(); out.lon = gnssLng();
    out.name = "GPS"; out.fromGps = true; out.valid = true;
    return true;
  }

  // 跨天后使内存缓存失效
  int32_t today = todayIndex();
  if (cached.valid && today >= 0 && cachedDay >= 0 && today != cachedDay) {
    geoInvalidate();
  }

  if (cached.valid) { out = cached; return true; }
  if (geoLoadCache()) { out = cached; return true; }   // 上次开机查过的，直接用，省掉那一秒
  return false;
}

bool geoGet(GeoFix& out, String* err) {
  if (geoGetOffline(out)) return true;

  if (!wifiEnsureConnected()) { if (err) *err = "wifi not connected"; return false; }

  // 先有上限地解析域名（见 net_resolve.h）：有信号没网时别在框架里白等 15 秒，后台拉取被取消时也能立刻放弃
  NetResolveResult nr = netResolve("ip-api.com");
  if (nr != NR_OK) { if (err) *err = String("geo ") + netResolveErrText(nr); return false; }

  // 明文 HTTP 是有意的：只要个大概坐标，没必要为它多做一次 TLS 握手（S3 上很贵）
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, "http://ip-api.com/json/?fields=status,city,regionName,lat,lon")) {
    if (err) *err = "geo begin failed";
    return false;
  }
  int code = http.GET();
  if (code != 200) { if (err) *err = String("geo http ") + code; http.end(); return false; }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) { if (err) *err = "geo bad json"; return false; }
  if (String(doc["status"] | "") != "success") { if (err) *err = "geo lookup failed"; return false; }

  cached.lat = doc["lat"] | 0.0;
  cached.lon = doc["lon"] | 0.0;
  cached.name = String(doc["city"] | "");
  if (cached.name.length() == 0) cached.name = String(doc["regionName"] | "unknown");
  cached.fromGps = false;
  cached.valid = true;
  cachedDay = todayIndex();
  geoSaveCache();
  out = cached;
  return true;
}
