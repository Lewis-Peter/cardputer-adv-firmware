// UI 模拟器：在 Mac/Linux 上把固件的绘制函数原样跑一遍，输出 PNG，用来核对屏幕布局有没有重叠/出界。
// 关键点是 **直接编译 src/ 里那份真代码**（不是照抄一份），所以看到的就是设备上会画出来的东西。
// 网络/传感器那几层用 stubs/ 里的假实现顶上，HTTP 返回预置 JSON，让取数逻辑照常跑完。
#include "sim_arduino.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <SD.h>
#include <vector>
#include <string>
#include <ctime>
#include <thread>
#include <chrono>
#include <cmath>

#include "../../src/globals.h"
#include "../../src/ui_common.h"
#include "../../src/busy_pill.h"
#include "../../src/geoloc.h"
#include "../../src/wifi_net.h"
#include "../../src/weather.h"
#include "../../src/pages.h"
#include "../../src/moon.h"
#include "../../src/astro.h"
#include "../../src/adsb.h"
#include "../../src/sats.h"
#include "../../src/router.h"
#include "../../src/clock.h"
#include "../../src/stopwatch.h"
#include "../../src/typhoon.h"
#include "../../src/quake.h"
#include "../../src/fx.h"
#include "../../src/okx.h"
#include "../../src/tls_ca.h"
#include "../../src/settings_ui.h"
#include "../../src/ram_profile.h"
#include "../../src/gnss.h"
#include "../../src/spectrum.h"
#include "../../src/hash_oven.h"
#include "../../src/ssh_app.h"
#include "../../src/sd_files.h"
#include "../../src/ridapp.h"
#include "../../src/rid_alert.h"
#include "../../src/rid_radar.h"

// ---------------- 模拟器桩实现 ----------------
SimM5 M5;
SimWiFi WiFi;
HardwareSerial Serial;
String simCannedResponse;
bool sdMounted = true;
SDClass SD;                 // stubs/SD.h 只有 extern 声明；gnss.cpp 的离线瓦片要用
uint64_t sdSizeMB = 16384, sdUsedMB = 2048;
namespace kbd { void flushEvents() {} }   // 拉网络的页拉完会调它；真身在 keyboard_adv.cpp（要 I2C）
// 后台拉取与 DNS 预解析的替身：真身要 FreeRTOS / lwIP。模拟器里拉取一律同步跑（跟起不来任务时的
// 退回路径一样），所以永远"不忙"，渲染照常走 cv。
#include "../../src/bg_fetch.h"
#include "../../src/net_resolve.h"
bool bgFetchRun(BgFetchFn fn) { fn(); return false; }
bool bgFetchBusy() { return false; }
bool bgFetchOnWorker() { return false; }
void bgFetchCancel() {}
bool bgFetchCancelled() { return false; }
void bgFetchStatus(const char*) {}
bool bgFetchService() { return false; }
void bgFetchDrawIndicator(bool) {}
NetResolveResult netResolve(const char*, uint32_t) { return NR_OK; }

void btReleaseForOtherApps() {}
bool btStreamIsActive() { return false; }
bool btHoldsHeap() { return false; }
void bootWifiStart() {}
bool hotspotSuspend() { return false; }
void hotspotResume() {}
bool hotspotIsSuspended() { return false; }
bool chatBusy() { return false; }

namespace kbd {
  void begin() {}
  char readKey() { return 0; }
  uint8_t modMask() { return 0; }
  uint8_t modSticky() { return 0; }
  void consumeSticky() {}
  void setAutoRepeat(bool) {}
}

// 根证书 bundle 在设备上是链接器从 data/cert/ 嵌进 flash 的（见 src/tls_ca.cpp），
// 模拟器里既没有那个符号、也没有真握手，用一对空实现顶掉；时钟就当永远对好了。
void tlsUseCaBundle(WiFiClientSecure&) {}
bool tlsClockReady() { return true; }

// 每个接口都从这里取假响应；traffic 特意有几档波动，能让 router 的曲线显出形状。
static std::vector<std::pair<std::string, String>> simResponses;
static int trafficSample = 0;
static void simSetResponse(const std::string& key, const String& body) {
  for (auto& e : simResponses)
    if (e.first == key) { e.second = body; return; }
  simResponses.push_back({key, body});
}

String simResponseForPath(const char* url) {
  std::string u = url ? url : "";
  if (u.find("/traffic") != std::string::npos) {
    static const long down[] = {184320, 327680, 245760, 491520, 376832, 696320, 442368, 278528, 614400, 352256};
    static const long up[]   = { 28672,  45056,  36864,  65536,  53248,  86016,  40960,  32768,  73728,  49152};
    int i = trafficSample++ % 10;
    char b[180];
    snprintf(b, sizeof(b), "{\"up\":%ld,\"down\":%ld,\"downTotal\":%lld,\"upTotal\":%lld}",
             up[i], down[i], 128LL * 1024 * 1024 * 1024 + i * 9000000LL,
             19LL * 1024 * 1024 * 1024 + i * 700000LL);
    return String(b);
  }
  for (const auto& entry : simResponses)
    if (u.find(entry.first) != std::string::npos) return entry.second;
  return simCannedResponse;
}

#include "../../src/led.h"

void ledApply() {}
static LedMode simLedMode = LED_MODE_BATTERY;
LedMode ledGetMode() { return simLedMode; }
void ledSetMode(LedMode mode) { simLedMode = mode; }
void ledCycleMode() { simLedMode = (LedMode)(((int)simLedMode + 1) % LED_MODE_COUNT); }
const char* ledModeName(LedMode m) {
  switch (m) {
    case LED_MODE_OFF:     return "OFF";
    case LED_MODE_BATTERY: return "Battery";
    case LED_MODE_BREATHE: return "Breathe";
    case LED_MODE_RAINBOW: return "Rainbow";
    case LED_MODE_CHASE:   return "Chase";
    case LED_MODE_MUSIC:   return "Music";
    default:               return "OFF";
  }
}
const char* ledModeName() { return ledModeName(simLedMode); }
bool ledOverrideActive() { return false; }
void ledSetOverride(bool) {}
void ledShowRGB(uint8_t, uint8_t, uint8_t) {}
bool ledIsOn() { return simLedMode != LED_MODE_OFF; }
bool playerIsPlaying() { return false; }
bool radioIsActive()   { return false; }

int  powerBatteryLevel() { return 76; }
bool powerCharging()     { return false; }
int  powerBatteryMv()    { return 4120; }

// 台风页的假响应直接读 data/ 里存的真实 JMA 报文
static String slurpFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("!! missing %s (run from tools/uisim)\n", path); return String(""); }
  std::string out; char buf[4096]; size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return String(out.c_str());
}

static String restampQuakes(const String& raw) {
  std::string in = raw.c_str();
  const std::string KEY = "\"time\":";
  long long newest = 0;
  for (size_t p = in.find(KEY); p != std::string::npos; p = in.find(KEY, p + 1)) {
    long long v = atoll(in.c_str() + p + KEY.size());
    if (v > newest) newest = v;
  }
  if (!newest) return raw;
  long long shift = (long long)time(nullptr) * 1000 - 20LL * 60 * 1000 - newest;
  std::string out;
  size_t at = 0;
  for (size_t p = in.find(KEY); p != std::string::npos; p = in.find(KEY, at)) {
    size_t vs = p + KEY.size(), ve = vs;
    while (ve < in.size() && (isdigit((unsigned char)in[ve]) || in[ve] == '-')) ve++;
    char b[32];
    snprintf(b, sizeof(b), "%lld", atoll(in.substr(vs, ve - vs).c_str()) + shift);
    out.append(in, at, vs - at);
    out.append(b);
    at = ve;
  }
  out.append(in, at, std::string::npos);
  return String(out.c_str());
}

// ---------------- 被 stub 掉的依赖 ----------------
bool wifiEnsureConnected() { return true; }
static double simLat = 39.10, simLon = 117.73;
static const char* simPlace = "BINHAI";
bool geoGet(GeoFix& out, String* /*err*/) {
  out.lat = simLat; out.lon = simLon;
  out.name = simPlace; out.fromGps = false; out.valid = true;
  return true;
}
bool geoGetOffline(GeoFix& out) { return geoGet(out, nullptr); }
void geoInvalidate() {}

// ---------------- 最小 PNG 输出 ----------------
static uint32_t crcTable[256];
static void crcInit() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
}
static uint32_t crc32buf(const uint8_t* b, size_t n, uint32_t c = 0xffffffffu) {
  for (size_t i = 0; i < n; i++) c = crcTable[(c ^ b[i]) & 0xff] ^ (c >> 8);
  return c;
}
static void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
static void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  be32(out, (uint32_t)data.size());
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  be32(out, crc32buf(td.data(), td.size()) ^ 0xffffffffu);
}
static void writePng(const char* path, const uint8_t* rgb, int w, int h, int scale) {
  int W = w * scale, H = h * scale;
  std::vector<uint8_t> raw;
  raw.reserve((size_t)H * (W * 3 + 1));
  for (int y = 0; y < H; y++) {
    raw.push_back(0);
    for (int x = 0; x < W; x++) {
      const uint8_t* p = rgb + ((size_t)(y / scale) * w + (x / scale)) * 3;
      raw.push_back(p[0]); raw.push_back(p[1]); raw.push_back(p[2]);
    }
  }
  std::vector<uint8_t> z{0x78, 0x01};
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = (pos + n) >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(n & 0xff); z.push_back(n >> 8);
    z.push_back(~n & 0xff); z.push_back((~n >> 8) & 0xff);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  be32(z, (b << 16) | a);

  std::vector<uint8_t> png{0x89,'P','N','G','\r','\n',0x1a,'\n'};
  std::vector<uint8_t> ihdr;
  be32(ihdr, W); be32(ihdr, H);
  ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", {});

  FILE* f = fopen(path, "wb");
  if (!f) { printf("cannot open %s\n", path); return; }
  fwrite(png.data(), 1, png.size(), f);
  fclose(f);
  printf("wrote %s (%dx%d)\n", path, W, H);
}

static void shoot(const char* path, void (*draw)(), Screen sc) {
  screen = sc;
  draw();
  std::vector<uint8_t> rgb((size_t)SW * SH * 3);
  for (int y = 0; y < SH; y++)
    for (int x = 0; x < SW; x++) {
      uint16_t c = cv.readPixel(x, y);
      uint8_t* p = &rgb[((size_t)y * SW + x) * 3];
      p[0] = ((c >> 11) & 0x1f) * 255 / 31;
      p[1] = ((c >> 5)  & 0x3f) * 255 / 63;
      p[2] = ( c        & 0x1f) * 255 / 31;
    }
  writePng(path, rgb.data(), SW, SH, 4);
}

// ---------------- GNSS NMEA 辅助函数 ----------------
static void feedNmea(const char* body) {
  uint8_t sum = 0;
  for (const char* p = body; *p; p++) sum ^= (uint8_t)*p;
  char buf[256];
  snprintf(buf, sizeof(buf), "$%s*%02X\r\n", body, sum);
  HardwareSerial::simFeedPort(1, buf);
}

static void simFeedGnssFix(double lat, double lon, float speedKnots, float altMeters, int fixQual = 1) {
  char b[256];
  int latDeg = (int)fabs(lat);
  double latMin = (fabs(lat) - latDeg) * 60.0;
  char latDir = lat >= 0 ? 'N' : 'S';

  int lonDeg = (int)fabs(lon);
  double lonMin = (fabs(lon) - lonDeg) * 60.0;
  char lonDir = lon >= 0 ? 'E' : 'W';

  snprintf(b, sizeof(b), "GNGGA,123519.00,%02d%07.4f,%c,%03d%07.4f,%c,%d,14,0.9,%.1f,M,-9.2,M,,",
           latDeg, latMin, latDir, lonDeg, lonMin, lonDir, fixQual, altMeters);
  feedNmea(b);

  snprintf(b, sizeof(b), "GNRMC,123519.00,A,%02d%07.4f,%c,%03d%07.4f,%c,%.1f,142.5,250926,,,A",
           latDeg, latMin, latDir, lonDeg, lonMin, lonDir, (double)speedKnots);
  feedNmea(b);

  feedNmea("GNGSA,A,3,01,02,03,04,05,06,07,08,,,,1.2,0.9,0.8");
  feedNmea("BDGSA,A,3,01,02,03,04,05,06,,,,,,,1.2,0.9,0.8");

  feedNmea("GPGSV,2,1,08,01,40,045,42,02,30,120,38,03,65,210,45,04,15,310,28");
  feedNmea("GPGSV,2,2,08,05,50,090,40,06,20,180,32,07,10,270,25,08,75,010,48");

  feedNmea("BDGSV,2,1,06,01,45,060,41,02,55,130,44,03,25,220,35,04,70,300,47");
  feedNmea("BDGSV,2,2,06,05,15,080,30,06,35,190,38");

  gnssPoll();
}

static bool selfTestFailed = false;

int main() {
  SW = 240;
  SH = 135;
  setenv("TZ", TZ_INFO, 1);
  tzset();
  crcInit();
  cv.setColorDepth(16);
  cv.createSprite(SW, SH);
  ACCENT     = cv.color565(0, 230, 120);
  CARD_BG    = cv.color565(10, 34, 22);
  DIM_BORDER = cv.color565(30, 55, 44);
  ICON_DIM   = cv.color565(0, 120, 66);
  timeSynced = true;
  sdMounted  = true;
  sdSizeMB   = 16384;
  sdUsedMB   = 2048;

  // 预置一份 Open-Meteo 的真实形状响应
  const String summerWeather =
    "{\"current\":{\"temperature_2m\":33.4,\"relative_humidity_2m\":78,"
    "\"apparent_temperature\":39.1,\"is_day\":1,\"weather_code\":80,"
    "\"wind_speed_10m\":23.6,\"wind_direction_10m\":215,\"pressure_msl\":1004.7},"
    "\"daily\":{\"time\":[\"2026-07-30\",\"2026-07-31\",\"2026-08-01\",\"2026-08-02\",\"2026-08-03\"],"
    "\"weather_code\":[80,3,61,95,1],"
    "\"temperature_2m_max\":[34.2,31.8,28.5,27.9,33.1],"
    "\"temperature_2m_min\":[26.1,24.7,23.2,22.8,25.4],"
    "\"sunrise\":[\"2026-07-30T05:12\"],\"sunset\":[\"2026-07-30T19:24\"],"
    "\"uv_index_max\":[9.2],\"precipitation_probability_max\":[65]},"
    "\"hourly\":{\"time\":["
    "\"2026-07-30T15:00\",\"2026-07-30T16:00\",\"2026-07-30T17:00\",\"2026-07-30T18:00\","
    "\"2026-07-30T19:00\",\"2026-07-30T20:00\",\"2026-07-30T21:00\",\"2026-07-30T22:00\","
    "\"2026-07-30T23:00\",\"2026-07-31T00:00\",\"2026-07-31T01:00\",\"2026-07-31T02:00\","
    "\"2026-07-31T03:00\",\"2026-07-31T04:00\",\"2026-07-31T05:00\",\"2026-07-31T06:00\","
    "\"2026-07-31T07:00\",\"2026-07-31T08:00\",\"2026-07-31T09:00\",\"2026-07-31T10:00\","
    "\"2026-07-31T11:00\",\"2026-07-31T12:00\",\"2026-07-31T13:00\",\"2026-07-31T14:00\"],"
    "\"temperature_2m\":[33.4,33.9,33.1,31.8,30.2,29.1,28.4,27.9,"
    "27.3,26.8,26.4,26.1,25.9,25.7,26.0,27.2,"
    "28.8,30.1,31.4,32.6,33.5,34.0,34.2,33.7],"
    "\"precipitation_probability\":[65,58,44,30,22,18,15,12,"
    "10,8,5,5,3,3,5,10,"
    "18,26,35,48,60,72,80,74]}}";

  const String winterWeather =
    "{\"current\":{\"temperature_2m\":-12.4,\"relative_humidity_2m\":41,"
    "\"apparent_temperature\":-19.6,\"is_day\":1,\"weather_code\":73,"
    "\"wind_speed_10m\":31.8,\"wind_direction_10m\":340,\"pressure_msl\":1029.3},"
    "\"daily\":{\"time\":[\"2027-01-12\",\"2027-01-13\",\"2027-01-14\",\"2027-01-15\",\"2027-01-16\"],"
    "\"weather_code\":[73,71,3,2,75],"
    "\"temperature_2m_max\":[-10.2,-13.5,-8.1,-4.6,-11.0],"
    "\"temperature_2m_min\":[-18.7,-21.4,-16.2,-12.9,-19.8],"
    "\"sunrise\":[\"2027-01-12T07:33\"],\"sunset\":[\"2027-01-12T17:08\"],"
    "\"uv_index_max\":[1.8],\"precipitation_probability_max\":[55]},"
    "\"hourly\":{\"time\":["
    "\"2027-01-12T15:00\",\"2027-01-12T16:00\",\"2027-01-12T17:00\",\"2027-01-12T18:00\","
    "\"2027-01-12T19:00\",\"2027-01-12T20:00\",\"2027-01-12T21:00\",\"2027-01-12T22:00\","
    "\"2027-01-12T23:00\",\"2027-01-13T00:00\",\"2027-01-13T01:00\",\"2027-01-13T02:00\","
    "\"2027-01-13T03:00\",\"2027-01-13T04:00\",\"2027-01-13T05:00\",\"2027-01-13T06:00\","
    "\"2027-01-13T07:00\",\"2027-01-13T08:00\",\"2027-01-13T09:00\",\"2027-01-13T10:00\","
    "\"2027-01-13T11:00\",\"2027-01-13T12:00\",\"2027-01-13T13:00\",\"2027-01-13T14:00\"],"
    "\"temperature_2m\":[-10.2,-11.1,-12.6,-14.0,-15.2,-16.1,-16.9,-17.5,"
    "-18.0,-18.4,-18.7,-19.0,-19.3,-19.6,-19.9,-20.3,"
    "-20.8,-20.1,-18.2,-15.9,-13.8,-12.2,-11.4,-11.9],"
    "\"precipitation_probability\":[55,48,40,33,28,22,18,15,"
    "12,10,8,6,5,5,8,14,"
    "22,30,38,44,50,54,52,45]}}";

  simCannedResponse = summerWeather;

  simResponses = {
    {"/v2/lat/", String(
      "{\"ac\":["
      "{\"flight\":\"CCA101 \" ,\"t\":\"A359\",\"r\":\"B-1891\",\"lat\":39.130,\"lon\":117.760,\"alt_baro\":3200,\"gs\":210,\"track\":82,\"baro_rate\":640},"
      "{\"flight\":\"CSN7623 \",\"t\":\"B738\",\"r\":\"B-5432\",\"lat\":39.280,\"lon\":117.400,\"alt_baro\":9800,\"gs\":265,\"track\":240,\"baro_rate\":-512},"
      "{\"flight\":\"CES512  \",\"t\":\"A320\",\"r\":\"B-6768\",\"lat\":38.780,\"lon\":117.920,\"alt_baro\":14500,\"gs\":390,\"track\":160,\"baro_rate\":0},"
      "{\"flight\":\"CHH889  \",\"t\":\"B77W\",\"r\":\"B-2045\",\"lat\":39.720,\"lon\":117.730,\"alt_baro\":22000,\"gs\":455,\"track\":315,\"baro_rate\":-768},"
      "{\"flight\":\"UAL88   \",\"t\":\"B789\",\"r\":\"N29971\",\"lat\":38.430,\"lon\":117.100,\"alt_baro\":31000,\"gs\":480,\"track\":55,\"baro_rate\":256},"
      "{\"flight\":\"JAL827  \",\"t\":\"B788\",\"r\":\"JA837J\",\"lat\":40.100,\"lon\":118.800,\"alt_baro\":38000,\"gs\":510,\"track\":190,\"baro_rate\":-128}]}" )},
    {"/above/", String(
      "{\"info\":{\"category\":\"Starlink\",\"transactionscount\":3,\"satcount\":12},"
      "\"above\":["
      "{\"satname\":\"STARLINK-1001\",\"satlat\":40.10,\"satlng\":117.73,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1002\",\"satlat\":39.10,\"satlng\":121.63,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1003\",\"satlat\":34.10,\"satlng\":117.73,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1004\",\"satlat\":44.10,\"satlng\":123.53,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1005\",\"satlat\":39.10,\"satlng\":106.13,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1006\",\"satlat\":31.10,\"satlng\":123.63,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1007\",\"satlat\":52.10,\"satlng\":117.73,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1008\",\"satlat\":29.10,\"satlng\":129.73,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1009\",\"satlat\":39.10,\"satlng\":139.63,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1010\",\"satlat\":58.10,\"satlng\":117.73,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1011\",\"satlat\":24.10,\"satlng\":99.73,\"satalt\":550},"
      "{\"satname\":\"STARLINK-1012\",\"satlat\":39.10,\"satlng\":89.33,\"satalt\":550}]}" )},
    {"/v0/callsign/", String(
      "{\"response\":{\"flightroute\":{\"callsign\":\"CCA101\","
      "\"origin\":{\"iata_code\":\"PEK\",\"latitude\":40.0801,\"longitude\":116.5846},"
      "\"destination\":{\"iata_code\":\"PVG\",\"latitude\":31.1434,\"longitude\":121.8052}}}}" )},
    {"/memory", "{\"inuse\":0,\"oslimit\":0}\n{\"inuse\":73400320,\"oslimit\":0}"},
    {"/v1/air-quality", "{\"current\":{\"pm10\":86.4,\"pm2_5\":52.7,\"us_aqi\":143,\"ozone\":118.0}}"},
    {"/connections", R"FLOW({
  "connections" : [
    {"id":"0","metadata":{"sourceIP":"192.168.1.10"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"1","metadata":{"sourceIP":"192.168.1.10"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"2","metadata":{"sourceIP":"192.168.1.10"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"3","metadata":{"sourceIP":"192.168.1.10"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"4","metadata":{"sourceIP":"192.168.1.10"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"5","metadata":{"sourceIP":"192.168.1.10"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"6","metadata":{"sourceIP":"192.168.1.10"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"7","metadata":{"sourceIP":"192.168.1.10"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"8","metadata":{"sourceIP":"192.168.1.10"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"9","metadata":{"sourceIP":"192.168.1.11"},"rule":"GeoIP","chains":["DIRECT","Proxy"]},
    {"id":"10","metadata":{"sourceIP":"192.168.1.11"},"rule":"GeoIP","chains":["DIRECT","Proxy"]},
    {"id":"11","metadata":{"sourceIP":"192.168.1.11"},"rule":"GeoIP","chains":["DIRECT","Proxy"]},
    {"id":"12","metadata":{"sourceIP":"192.168.1.11"},"rule":"GeoIP","chains":["DIRECT","Proxy"]},
    {"id":"13","metadata":{"sourceIP":"192.168.1.11"},"rule":"GeoIP","chains":["DIRECT","Proxy"]},
    {"id":"14","metadata":{"sourceIP":"192.168.1.11"},"rule":"GeoIP","chains":["DIRECT","Proxy"]},
    {"id":"15","metadata":{"sourceIP":"192.168.1.10"},"rule":"Match","chains":["JP-02","Proxy"]},
    {"id":"16","metadata":{"sourceIP":"192.168.1.10"},"rule":"Match","chains":["JP-02","Proxy"]},
    {"id":"17","metadata":{"sourceIP":"192.168.1.10"},"rule":"Match","chains":["JP-02","Proxy"]},
    {"id":"18","metadata":{"sourceIP":"192.168.1.10"},"rule":"Match","chains":["JP-02","Proxy"]},
    {"id":"19","metadata":{"sourceIP":"192.168.1.12"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"20","metadata":{"sourceIP":"192.168.1.12"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"21","metadata":{"sourceIP":"192.168.1.12"},"rule":"RuleSet","chains":["HK-01","Proxy"]},
    {"id":"22","metadata":{"sourceIP":"192.168.1.13"},"rule":"Domain","chains":["REJECT","Proxy"]},
    {"id":"23","metadata":{"sourceIP":"192.168.1.13"},"rule":"Domain","chains":["REJECT","Proxy"]},
    {"id":"24","metadata":{"sourceIP":"192.168.1.11"},"rule":"RuleSet","chains":["JP-02","Proxy"]},
    {"id":"25","metadata":{"sourceIP":"192.168.1.11"},"rule":"RuleSet","chains":["JP-02","Proxy"]},
    {"id":"26","metadata":{"sourceIP":"192.168.1.11"},"rule":"RuleSet","chains":["JP-02","Proxy"]}
  ]
})FLOW"},
    {"/proxies/Proxy", "{\"now\":\"HK-Premium-03\"}"},
    {"/version", "{\"version\":\"1.18.8\"}"},
    {"/proxies/HK-Premium-03/delay", "{\"delay\":143}"}
  };

  simResponses.push_back({"/summary/", restampQuakes(slurpFile("../data/usgs_quakes.json"))});
  simResponses.push_back({"..", slurpFile("../data/frankfurter_usdcny.json")});
  simResponses.push_back({"quotedPrice", slurpFile("../data/okx_exchange_rate.json")});
  simResponses.push_back({"targetTc.json",       slurpFile("../data/jma_target.json")});
  simResponses.push_back({"specifications.json", slurpFile("../data/jma_specifications.json")});
  simResponses.push_back({"forecast.json",       slurpFile("../data/jma_forecast.json")});

  // ==================== Weather ====================
  weatherEnter();
  drawWeather();
  weatherUpdate();

  shoot("out/weather_1_now.png",      drawWeather,     SCREEN_WEATHER);
  // 后台拉取的右上角指示（真机上直接画在 M5.Display 上、压在最后一帧页面上面；这里画到 cv 看排版）
  shoot("out/busy_1_weather_locating.png", [] { drawWeather(); drawBusyPill(cv, "locating", 1, false); }, SCREEN_WEATHER);
  shoot("out/weather_2_hourly.png",   drawWeatherHour, SCREEN_WEATHER_HOUR);
  shoot("out/weather_4_air.png",      drawWeatherAir,  SCREEN_WEATHER_AIR);
  shoot("out/weather_5_aqi.png",      drawWeatherAqi,  SCREEN_WEATHER_AQI);
  shoot("out/weather_6_forecast.png", drawWeatherFc,   SCREEN_WEATHER_FC);

  simCannedResponse = winterWeather;
  weatherKey('r'); drawWeather(); weatherUpdate();
  shoot("out/weather_2_hourly_cold.png",   drawWeatherHour, SCREEN_WEATHER_HOUR);
  shoot("out/weather_6_forecast_cold.png", drawWeatherFc,   SCREEN_WEATHER_FC);
  simCannedResponse = summerWeather;
  weatherKey('r'); drawWeather(); weatherUpdate();

  // ==================== Astro ====================
  astroEnter();
  drawAstroSun();
  astroUpdate();
  shoot("out/astro_1_daylight.png",   drawAstroSun,    SCREEN_ASTRO_SUN);
  shoot("out/astro_2_moon.png",       drawMoon,        SCREEN_MOON);
  shoot("out/astro_3_terminator.png", drawAstroTerm,   SCREEN_ASTRO_TERM);
  {
    const double lat0 = simLat, lon0 = simLon; const char* name0 = simPlace;
    simLat =  85.0; simLon = 15.0; simPlace = "POLAR-N";
    astroEnter(); drawAstroSun(); astroUpdate();
    shoot("out/astro_1_daylight_polar_n.png", drawAstroSun, SCREEN_ASTRO_SUN);
    simLat = -85.0; simLon = 15.0; simPlace = "POLAR-S";
    astroEnter(); drawAstroSun(); astroUpdate();
    shoot("out/astro_1_daylight_polar_s.png", drawAstroSun, SCREEN_ASTRO_SUN);
    simLat = lat0; simLon = lon0; simPlace = name0;
    astroEnter(); drawAstroSun(); astroUpdate();
  }

  // ==================== ADS-B & Sats ====================
  adsbEnter();
  drawAdsb();
  adsbUpdate();
  simAdvanceMillis(400);
  adsbUpdate();
  shoot("out/adsb.png", drawAdsb, SCREEN_ADSB);
  shoot("out/busy_2_adsb_loading.png", [] { drawAdsb(); drawBusyPill(cv, "loading", 0, false); }, SCREEN_ADSB);

  satsEnter();
  drawSats();
  satsUpdate();
  shoot("out/sats.png", drawSats, SCREEN_SATS);

  // ==================== Typhoon ====================
  typhoonEnter();
  drawTyphoon();
  typhoonUpdate();
  simAdvanceMillis(450);
  typhoonUpdate();
  shoot("out/typhoon_1_alert.png",    drawTyphoon,      SCREEN_TYPHOON);
  shoot("out/busy_3_typhoon_leaving.png", [] { drawTyphoon(); drawBusyPill(cv, "leaving", 2, true); }, SCREEN_TYPHOON);
  shoot("out/typhoon_2_track.png",    drawTyphoonTrack, SCREEN_TYPHOON_TRACK);

  // ==================== Quake ====================
  quakeEnter();
  drawQuake();
  quakeUpdate();
  shoot("out/quake_1_list.png",       drawQuake,     SCREEN_QUAKE);
  shoot("out/quake_2_map.png",        drawQuakeMap,  SCREEN_QUAKE_MAP);
  quakeKey(';');
  shoot("out/quake_3_scrolled.png",   drawQuake,     SCREEN_QUAKE);

  // ==================== FX & OKX ====================
  fxEnter();
  drawFx();
  fxUpdate();
  shoot("out/fx_1_rate.png",          drawFx,        SCREEN_FX);
  shoot("out/fx_2_days.png",          drawFxDays,    SCREEN_FX_DAYS);

  okxEnter();
  drawOkx();
  okxUpdate();
  shoot("out/okx.png",                drawOkx,       SCREEN_OKX);

  {
    const int nested = fxPointCount();
    simSetResponse("..", slurpFile("../data/frankfurter_usdcny_flat.json"));
    fxKey('r');
    drawFx();
    fxUpdate();
    const int flat = fxPointCount();
    const bool ok = (nested > 0 && nested == flat);
    printf("fx: nested=%d flat=%d -> %s\n", nested, flat,
           ok ? "ok" : "FAIL 两种形状解出的点数对不上");
    if (!ok) selfTestFailed = true;
  }

  // ==================== Router & Stopwatch ====================
  routerEnter();
  shoot("out/router_0_connecting.png", drawRouter, SCREEN_ROUTER);

  routerEnter();
  for (int i = 0; i < 9; i++) {
    simAdvanceMillis(1550);
    routerUpdate();
  }
  shoot("out/router.png",             drawRouter,    SCREEN_ROUTER);
  routerKey('n');
  shoot("out/router_2_flow.png", drawRouter, SCREEN_ROUTER);
  routerKey('.');
  shoot("out/router_2_flow_detail.png", drawRouter, SCREEN_ROUTER);
  // Real-device regression: tiny source buckets and a dominant OTHER bucket.
  String skew = "{\"connections\":[";
  for (int i = 0; i < 82; ++i) {
    if (i) skew += ',';
    const char* source = i == 0 ? "192.168.50.10" : i < 10 ? "192.168.50.104" :
                         i < 16 ? "192.168.50.105" : "192.168.50.106";
    const char* rule = i < 45 ? "RuleSet" : i < 76 ? "Match" : "DomainSuffix";
    const char* exit = i < 9 ? "Oracle" : i < 25 ? "Oracle-v6" : "DIRECT";
    skew += "{\"metadata\":{\"sourceIP\":\"";
    skew += source;
    skew += "\"},\"rule\":\"";
    skew += rule;
    skew += "\",\"chains\":[\"";
    skew += exit;
    skew += "\"]}";
  }
  skew += "]}";
  simSetResponse("/connections", skew);
  routerKey('r');
  simAdvanceMillis(1550); routerUpdate();
  shoot("out/router_2_flow_skew.png", drawRouter, SCREEN_ROUTER);
  // Malformed snapshots must preserve the last good topology, then an empty
  // (whitespace-separated) array must clear it successfully.
  simSetResponse("/connections", "{\"connections\": [{broken]}");
  routerKey('r');
  simAdvanceMillis(1550); routerUpdate();
  shoot("out/router_2_flow_stale.png", drawRouter, SCREEN_ROUTER);
  simSetResponse("/connections", "{\"connections\" : []}");
  routerKey('r');
  simAdvanceMillis(1550); routerUpdate();
  shoot("out/router_2_flow_empty.png", drawRouter, SCREEN_ROUTER);
  routerKey('n');
  shoot("out/router_3_types_empty.png", drawRouter, SCREEN_ROUTER);
  simSetResponse("/connections", R"TYPES({"connections":[
    {"metadata":{"network":"tcp","destinationPort":"443","host":"api.github.com"},"download":4294967296,"upload":100,"chains":["HK-Premium-03","Proxy"]},
    {"metadata":{"network":"udp","destinationPort":"443","host":"gateway.icloud.com"},"download":120000000,"upload":500000,"chains":["DIRECT"]},
    {"metadata":{"network":"tcp","destinationPort":80,"host":"archive.ubuntu.com"},"download":2000000,"upload":3000,"chains":["HK-Premium-03","Proxy"]},
    {"metadata":{"network":"udp","destinationPort":"53","destinationIP":"1.1.1.1"},"download":8000,"upload":1000,"chains":["DIRECT"]},
    {"metadata":{"network":"tcp","destinationPort":"22","host":"srv1.us-east.compute.internal"},"download":90000,"upload":10000,"chains":["US-Silicon-01"]},
    {"metadata":{"network":"tcp","destinationPort":"443","host":"very-long-subdomain-tracker.analytics.google.com"},"download":54000,"upload":2400,"chains":["JP-Tokyo-02"]},
    {"metadata":{},"download":400,"upload":600}
  ]})TYPES");
  routerKey('r');
  simAdvanceMillis(1550); routerUpdate();
  shoot("out/router_3_types_count.png", drawRouter, SCREEN_ROUTER);
  routerKey('m');
  shoot("out/router_3_types_bytes.png", drawRouter, SCREEN_ROUTER);
  routerKey('n');
  shoot("out/router_4_nodes.png", drawRouter, SCREEN_ROUTER);
  routerKey('n');
  shoot("out/router_5_hosts_bytes.png", drawRouter, SCREEN_ROUTER);
  routerKey('m');
  shoot("out/router_5_hosts_count.png", drawRouter, SCREEN_ROUTER);
  routerKey('n');
  // 模拟历史采样积累：推进多次 poll 产生连续的 MEM 和 CONNS 曲线
  for (int h = 0; h < 25; h++) {
    simAdvanceMillis(1550);
    routerUpdate();
  }
  shoot("out/router_6_hist.png", drawRouter, SCREEN_ROUTER);
  stopwatchEnter();
  shoot("out/stopwatch.png",          drawStopwatch, SCREEN_STOPWATCH);

  // ==================== Clock (4 styles, synced & unsynced) ====================
  currentClockStyle = CLOCK_STYLE_HIERARCHIC;
  timeSynced = true;
  shoot("out/clock_1_hierarchic.png", drawClock, SCREEN_CLOCK);
  timeSynced = false;
  shoot("out/clock_1_hierarchic_unsynced.png", drawClock, SCREEN_CLOCK);
  timeSynced = true;

  currentClockStyle = CLOCK_STYLE_ANALOG;
  shoot("out/clock_2_analog.png", drawClock, SCREEN_CLOCK);
  timeSynced = false;
  shoot("out/clock_2_analog_unsynced.png", drawClock, SCREEN_CLOCK);
  timeSynced = true;

  currentClockStyle = CLOCK_STYLE_DIGITAL;
  shoot("out/clock_3_digital.png", drawClock, SCREEN_CLOCK);
  timeSynced = false;
  shoot("out/clock_3_digital_unsynced.png", drawClock, SCREEN_CLOCK);
  timeSynced = true;

  currentClockStyle = CLOCK_STYLE_TEXT;
  shoot("out/clock_4_text.png", drawClock, SCREEN_CLOCK);
  timeSynced = false;
  shoot("out/clock_4_text_unsynced.png", drawClock, SCREEN_CLOCK);
  timeSynced = true;

  // ==================== GNSS Pages ====================
  gnssInit();

  // 1) NO FIX / RF OFF
  shoot("out/gnss_1_overview_nofix.png", drawGnss, SCREEN_GNSS);
  shoot("out/gnss_2_detail_nofix.png", drawGnssDetail, SCREEN_GNSS_DETAIL);
  shoot("out/gnss_5_speed_nofix.png", drawGnssSpeed, SCREEN_GNSS_SPEED);

  gnssAntennaOn = false;
  shoot("out/gnss_1_overview_rfoff.png", drawGnss, SCREEN_GNSS);
  gnssAntennaOn = true;

  // 2) Normal 3D Fix (Tianjin Binhai, 39.10N, 117.73E, 75km/h, alt 42.5m)
  simFeedGnssFix(39.1000, 117.7300, 40.5f, 42.5f);
  shoot("out/gnss_1_overview_fix.png", drawGnss, SCREEN_GNSS);

  // Detail page in 4 coordinate formats: DEG, DMS, GRID, UTM
  shoot("out/gnss_2_detail_deg.png", drawGnssDetail, SCREEN_GNSS_DETAIL);
  gnssDetailKey('c');
  shoot("out/gnss_2_detail_dms.png", drawGnssDetail, SCREEN_GNSS_DETAIL);
  gnssDetailKey('c');
  shoot("out/gnss_2_detail_grid.png", drawGnssDetail, SCREEN_GNSS_DETAIL);
  gnssDetailKey('c');
  shoot("out/gnss_2_detail_utm.png", drawGnssDetail, SCREEN_GNSS_DETAIL);
  gnssDetailKey('c'); // back to DEG

  // Detail page with negative coordinates (South/West hemisphere: Rio de Janeiro -22.9068, -43.1729)
  simFeedGnssFix(-22.9068, -43.1729, 25.0f, 15.0f);
  shoot("out/gnss_2_detail_neg_coords.png", drawGnssDetail, SCREEN_GNSS_DETAIL);
  simFeedGnssFix(39.1000, 117.7300, 40.5f, 42.5f);

  // 卫星页默认是天空图；按 m 切到表格（第 1 页、滚到第 2 页），再按 m 切回来
  shoot("out/gnss_3_sat_sky.png", drawGnssSat, SCREEN_GNSS_SAT);
  gnssSatKey('m');
  shoot("out/gnss_3_sat_p1.png", drawGnssSat, SCREEN_GNSS_SAT);
  gnssSatKey(']');
  gnssSatKey(']');
  gnssSatKey(']');
  shoot("out/gnss_3_sat_p2.png", drawGnssSat, SCREEN_GNSS_SAT);
  gnssSatKey('[');
  gnssSatKey('[');
  gnssSatKey('[');
  gnssSatKey('m');

  // 天空图压力场景：五个星座挤在同一片方位、有无 SNR 混杂、正北贴边、
  // QZSS 的三位数 PRN（193）——专看标签避让和 PRN 宽度
  feedNmea("GPGSV,2,1,06,10,20,100,45,11,22,104,40,12,24,098,,13,05,350,20");
  feedNmea("GPGSV,2,2,06,14,88,005,50,15,02,002,15");
  feedNmea("GLGSV,1,1,04,65,20,102,33,66,21,106,,67,60,355,28,68,10,005,");
  feedNmea("GAGSV,1,1,03,03,40,200,38,05,42,204,36,07,38,198,");
  feedNmea("GQGSV,1,1,02,193,50,250,41,194,48,255,");
  feedNmea("BDGSV,1,1,04,19,30,110,44,20,33,108,39,21,12,358,22,22,70,180,46");
  feedNmea("GNGSA,A,3,10,11,14,,,,,,,,,,1.2,0.9,0.8");
  feedNmea("BDGSA,A,3,19,22,,,,,,,,,,,1.2,0.9,0.8");
  gnssPoll();
  shoot("out/gnss_3_sat_sky_crowded.png", drawGnssSat, SCREEN_GNSS_SAT);

  // 空状态：每个星座都报 0 颗可见星（GSV 第 1 条会先清掉该星座的旧记录）
  const char* emptyGsv[] = {"GPGSV,1,1,00", "GLGSV,1,1,00", "GAGSV,1,1,00", "GQGSV,1,1,00", "BDGSV,1,1,00"};
  for (const char* s : emptyGsv) feedNmea(s);
  gnssPoll();
  shoot("out/gnss_3_sat_sky_empty.png", drawGnssSat, SCREEN_GNSS_SAT);
  simFeedGnssFix(39.1000, 117.7300, 40.5f, 42.5f);

  // Config page (Antenna ON and Antenna OFF)
  shoot("out/gnss_4_config_on.png", drawGnssConfig, SCREEN_GNSS_CONFIG);
  gnssAntennaOn = false;
  shoot("out/gnss_4_config_off.png", drawGnssConfig, SCREEN_GNSS_CONFIG);
  gnssAntennaOn = true;

  // Speedometer (120, 360, 1000 scales)
  simFeedGnssFix(39.1000, 117.7300, 40.5f, 42.5f);
  shoot("out/gnss_5_speed_120.png", drawGnssSpeed, SCREEN_GNSS_SPEED);
  simFeedGnssFix(39.1000, 117.7300, 154.0f, 42.5f);
  shoot("out/gnss_5_speed_360.png", drawGnssSpeed, SCREEN_GNSS_SPEED);
  simFeedGnssFix(39.1000, 117.7300, 459.0f, 10500.0f);
  shoot("out/gnss_5_speed_1000.png", drawGnssSpeed, SCREEN_GNSS_SPEED);

  // Trip / Diag: advance time and distance to accumulate trip stats and SNR curve
  for (int i = 0; i < 15; i++) {
    simAdvanceMillis(1000);
    simFeedGnssFix(39.1000 + i * 0.001, 117.7300 + i * 0.001, 35.0f + (i % 5) * 5.0f, 42.0f + (i % 3) * 2.0f);
  }
  shoot("out/gnss_6_trip.png", drawGnssTrip, SCREEN_GNSS_TRIP);

  // 地图页：WORLD → z8。SD 在位但卡上没瓦片、也没网：
  // 第一帧必须先看到 "loading map" 提示（gnssMapUpdate 在提示上屏前不碰 SD），
  // 之后逐块推进（每次最多一块），全缺就退回离线掩码并标 "no SD tile"
  WiFi.disconnect();
  shoot("out/gnss_7_map_world.png", drawGnssMap, SCREEN_GNSS_MAP);
  gnssMapZoom(+1); gnssMapZoom(+1);
  gnssMapUpdate();   // 提示还没上屏：这一步应当什么都不做
  shoot("out/gnss_7_map_z8_loading.png", drawGnssMap, SCREEN_GNSS_MAP);
  for (int i = 0; i < 8; i++) gnssMapUpdate();
  shoot("out/gnss_7_map_z8_nosdtile.png", drawGnssMap, SCREEN_GNSS_MAP);
  gnssMapExit();
  WiFi.reconnect();

  // ==================== Remote ID (Drone ID) ====================
  // 1) List view with injected sample drones & valid GNSS fix
  ridAppEnter();
  ridAppInjectSample();
  simFeedGnssFix(39.1500, 117.7600, 0.0f, 10.0f); // 距 sample A 约 700m
  ridAppUpdate();
  shoot("out/rid_1_list.png", drawRidApp, SCREEN_RID);

  // 2) Radar view with fix
  ridAppKey('m'); // 切雷达
  ridAppUpdate();
  shoot("out/rid_2_radar.png", drawRidApp, SCREEN_RID);

  // 3) Radar view without fix
  // 模拟定位超时失效 (maxAgeMs = 3000ms)
  simAdvanceMillis(4000);
  shoot("out/rid_2_radar_nofix.png", drawRidApp, SCREEN_RID);

  // 4) Alert active on header
  // 恢复 fix，将告警级别设为 1000m (a 键)，sample A (约 700m) 进入告警圈，切回列表视图看 ALERT 顶栏
  simFeedGnssFix(39.1500, 117.7600, 0.0f, 10.0f);
  ridAppKey('a'); // 200m
  ridAppKey('a'); // 500m
  ridAppKey('a'); // 1000m
  ridAppKey('m'); // 切回列表
  ridAppUpdate();
  shoot("out/rid_1_list_alert.png", drawRidApp, SCREEN_RID);
  ridAppExit();

  // ==================== Spectrum ====================
  shoot("out/spectrum_0_mic_unavailable.png", drawSpectrum, SCREEN_SPECTRUM);
  spectrumEnter();
  spectrumUpdate();
  shoot("out/spectrum_1_bars.png", drawSpectrum, SCREEN_SPECTRUM);

  spectrumKey('w'); // Waterfall mode
  for (int i = 0; i < 30; i++) spectrumUpdate();
  shoot("out/spectrum_2_waterfall.png", drawSpectrum, SCREEN_SPECTRUM);

  spectrumKey('l'); // VU LED mode
  shoot("out/spectrum_3_vu_led.png", drawSpectrum, SCREEN_SPECTRUM);

  spectrumKey('s'); // REC->serial
  shoot("out/spectrum_4_rec_serial.png", drawSpectrum, SCREEN_SPECTRUM);
  spectrumExit();

  // ==================== Hash Oven (5 submodes) ====================
  hashOvenEnter();
  shoot("out/hash_oven_0_cold.png", drawHashOven, SCREEN_HASHOVEN);
  hashOvenKey('\n'); // Start baking
  hashOvenUpdate();
  shoot("out/hash_oven_0_baking.png", drawHashOven, SCREEN_HASHOVEN);
  hashOvenKey('\n'); // Stop baking

  hashOvenKey('m'); // Mode 1: Cipher
  shoot("out/hash_oven_1_aes_enc.png", drawHashOven, SCREEN_HASHOVEN);
  hashOvenKey('a'); // RC4
  hashOvenKey('t'); // Decrypt
  shoot("out/hash_oven_1_rc4_dec.png", drawHashOven, SCREEN_HASHOVEN);

  hashOvenKey('m'); // Mode 2: Classic & CTF
  shoot("out/hash_oven_2_caesar.png", drawHashOven, SCREEN_HASHOVEN);
  hashOvenKey('a'); // Vigenere
  hashOvenKey('a'); // Base64
  shoot("out/hash_oven_2_base64.png", drawHashOven, SCREEN_HASHOVEN);

  hashOvenKey('m'); // Mode 3: Hash & Shadow
  shoot("out/hash_oven_3_sha512_standby.png", drawHashOven, SCREEN_HASHOVEN);
  hashOvenKey('\n'); // Calculate hash
  hashOvenUpdate();
  shoot("out/hash_oven_3_sha512_done.png", drawHashOven, SCREEN_HASHOVEN);
  hashOvenKey('a'); // MD5-crypt
  hashOvenKey('a'); // SHA-256
  hashOvenKey('\n');
  hashOvenUpdate();
  shoot("out/hash_oven_3_sha256.png", drawHashOven, SCREEN_HASHOVEN);

  hashOvenKey('m'); // Mode 4: Futility Crack
  shoot("out/hash_oven_4_crack_standby.png", drawHashOven, SCREEN_HASHOVEN);
  hashOvenKey('\n'); // Start crack
  for (int i = 0; i < 5; i++) hashOvenUpdate();
  shoot("out/hash_oven_4_crack_trying.png", drawHashOven, SCREEN_HASHOVEN);
  hashOvenKey('\n'); // Stop
  hashOvenExit();

  // ==================== Settings ====================
  settingsIndex = 0;
  shoot("out/settings_1_p1.png", drawSettings, SCREEN_SETTINGS);
  for (int i = 0; i < SET_COUNT; i++) if (SETTINGS[i].id == SET_PCMODE) settingsIndex = i;
  shoot("out/settings_1_p1_pcmode.png", drawSettings, SCREEN_SETTINGS);
  for (int i = 0; i < SET_COUNT; i++) if (SETTINGS[i].id == SET_GNSS) settingsIndex = i;
  shoot("out/settings_1_p1_gnss.png", drawSettings, SCREEN_SETTINGS);
  settingsIndex = 4;
  shoot("out/settings_1_p2.png", drawSettings, SCREEN_SETTINGS);
  settingsIndex = 8;
  shoot("out/settings_1_p3.png", drawSettings, SCREEN_SETTINGS);
  settingsIndex = 12;
  shoot("out/settings_1_p4.png", drawSettings, SCREEN_SETTINGS);

  // LED capsules (all 6 modes)
  // 按 SetId 找行号，不写死下标：设置项一插队（比如 PC MODE 插在 Wi-Fi 后面），写死的 4 就会停在别的行上
  for (int i = 0; i < SET_COUNT; i++) if (SETTINGS[i].id == SET_LED) settingsIndex = i;
  ledSetMode(LED_MODE_OFF);     shoot("out/settings_led_off.png",     drawSettings, SCREEN_SETTINGS);
  ledSetMode(LED_MODE_BATTERY); shoot("out/settings_led_battery.png", drawSettings, SCREEN_SETTINGS);
  ledSetMode(LED_MODE_BREATHE); shoot("out/settings_led_breathe.png", drawSettings, SCREEN_SETTINGS);
  ledSetMode(LED_MODE_RAINBOW); shoot("out/settings_led_rainbow.png", drawSettings, SCREEN_SETTINGS);
  ledSetMode(LED_MODE_CHASE);   shoot("out/settings_led_chase.png",   drawSettings, SCREEN_SETTINGS);
  ledSetMode(LED_MODE_MUSIC);   shoot("out/settings_led_music.png",   drawSettings, SCREEN_SETTINGS);
  ledSetMode(LED_MODE_BATTERY);

  // Settings sub-pages
  shoot("out/settings_sub_bright.png",  []() { drawBar("Brightness", 80, "[ / ] adj"); }, SCREEN_BRIGHTNESS);
  shoot("out/settings_sub_volume.png",  []() { drawBar("Volume", 60, "[ / ] adj"); },     SCREEN_VOLUME);
  shoot("out/settings_sub_sleep.png",   drawSleep,                                         SCREEN_SLEEP);
  shoot("out/settings_sub_tz.png",      drawTz,                                            SCREEN_TZ);
  shoot("out/settings_sub_battery.png", drawBatteryDetail,                                 SCREEN_BATTERY);
  shoot("out/settings_sub_format.png",  drawFormat,                                        SCREEN_FORMAT);

  // ==================== SSH Config ====================
  shoot("out/ssh_cfg_1_password.png", drawSshConfig, SCREEN_SSH);

  sshKey('.');
  sshKey('.');
  sshKey('.');
  sshKey('\n'); // Toggle to Key
  shoot("out/ssh_cfg_2_key_nokey.png", drawSshConfig, SCREEN_SSH);

  SDClass::simAddFile("/id_rsa");
  shoot("out/ssh_cfg_3_key_found.png", drawSshConfig, SCREEN_SSH);
  SDClass::simClearFiles();

  WiFi.disconnect();
  shoot("out/ssh_cfg_4_nowifi.png", drawSshConfig, SCREEN_SSH);
  WiFi.reconnect();

  // ==================== Main Menu ====================
  menuIndex = 0; menuSnapSelection();
  shoot("out/menu_0_tools.png", drawMenu, SCREEN_MENU);

  menuIndex = GROUPS[1].first; menuSnapSelection();
  shoot("out/menu_1_scan.png", drawMenu, SCREEN_MENU);

  menuIndex = GROUPS[2].first; menuSnapSelection();
  shoot("out/menu_2_net.png", drawMenu, SCREEN_MENU);

  menuIndex = GROUPS[3].first; menuSnapSelection();
  shoot("out/menu_3_hid.png", drawMenu, SCREEN_MENU);

  menuIndex = GROUPS[4].first; menuSnapSelection();
  shoot("out/menu_4_sig.png", drawMenu, SCREEN_MENU);

  menuIndex = GROUPS[5].first; menuSnapSelection();
  shoot("out/menu_5_sky.png", drawMenu, SCREEN_MENU);

  menuIndex = GROUPS[6].first; menuSnapSelection();
  shoot("out/menu_6_more.png", drawMenu, SCREEN_MENU);

  themeMode = 2; menuIndex = 0; menuSnapSelection();
  shoot("out/menu_theme_amber.png", drawMenu, SCREEN_MENU);
  themeMode = 0;

  // ==================== About ====================
  aboutPage = 2;
  shoot("out/about_3_ram_empty.png",  drawAbout, SCREEN_ABOUT);

  ramMark("boot");
  ramMark("M5.begin");
  ramMark("canvas");
  ramMark("speaker");
  ramMark("kbd");
  ramMark("bootAnim");
  ramMark("serial");
  ramMark("kbd+nvs");
  ramMark("sd");
  ramMark("gnss");
  ramMark("wifi start");
  ramMark("wifi up");

  aboutPage = 0;
  shoot("out/about_1_info.png", drawAbout, SCREEN_ABOUT);
  aboutPage = 1;
  shoot("out/about_2_usage.png", drawAbout, SCREEN_ABOUT);
  aboutPage = 2;
  aboutRamScroll = 0;
  shoot("out/about_3_ram.png", drawAbout, SCREEN_ABOUT);
  aboutRamScroll = 6;
  shoot("out/about_3_ram_scrolled.png", drawAbout, SCREEN_ABOUT);

  if (selfTestFailed) { printf("!! 自检失败，见上面的 FAIL\n"); return 1; }
  return 0;
}
