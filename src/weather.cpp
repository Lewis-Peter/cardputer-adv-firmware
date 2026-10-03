#include "weather.h"
#include "bg_fetch.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <cmath>
#include "ui_common.h"
#include "net_job.h"
#include "wifi_net.h"
#include "geoloc.h"
#include "http_json.h"

// ---- 数据 ----
static const int FC_DAYS = 5;

struct DayFc { int code; float tmax, tmin; char label[8]; };

static bool     haveData = false;
static String   place    = "";
static String   errMsg   = "";
static float    curTemp = 0, curFeels = 0, curWind = 0;
static int      curHum = 0, curCode = 0, curWindDir = 0, curPressure = 0;
static int      uvMax = 0, popMax = 0;
static bool     curIsDay = true;
static bool     fromGps = false;
static double   curLat = 0, curLon = 0;   // P3 自己算太阳高度角要用
static DayFc    days[FC_DAYS];
static int      dayCount = 0;
static uint32_t fetchedAtMs = 0;

bool weatherImperial = false;

void weatherUnitsInit() {
  weatherImperial = loadBool("wxunit", "imperial", false);
}

void weatherUnitsSet(bool imperial) {
  weatherImperial = imperial;
  saveBool("wxunit", "imperial", weatherImperial);
}

static const char* tempUnit() { return weatherImperial ? "F" : "C"; }
static const char* windUnit() { return weatherImperial ? "mph" : "km/h"; }

// P2 未来 24 小时
static float    hourTemp[24];
static uint8_t  hourPop[24];
static int      hourCount = 0;
static int      hourStartH = 0;      // 第一个采样点是几点（0~23）

// P5 空气质量（另一个端点，拉不到不影响其它页，所以单独记状态）
static int      aqi = -1, pm25 = -1, pm10v = -1, o3v = -1;
static bool     everFetched = false;
static char     fetchedAtStr[8] = "--:--";

// 数据超过这么久再进来就自动刷一次（手动 r 键随时可以强制刷）
static const uint32_t STALE_MS = 30 * 60 * 1000;

// ---- WMO 天气代码 ----
// Open-Meteo 用的是 WMO 4677 那套码；这里只归到 7 个大类，够画图标 + 一行说明
enum WxKind { WX_CLEAR, WX_PARTLY, WX_CLOUD, WX_RAIN, WX_SNOW, WX_STORM, WX_FOG };

static WxKind wxKindOf(int code) {
  if (code == 0) return WX_CLEAR;
  if (code == 1 || code == 2) return WX_PARTLY;
  if (code == 3) return WX_CLOUD;
  if (code == 45 || code == 48) return WX_FOG;
  if (code >= 95) return WX_STORM;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
  return WX_RAIN;   // 51..67 毛毛雨/雨/冻雨，80..82 阵雨
}

static const char* wxText(int code) {
  switch (code) {
    case 0:  return "clear";
    case 1:  return "mainly clear";
    case 2:  return "partly cloudy";
    case 3:  return "overcast";
    case 45: case 48: return "fog";
    case 51: case 53: case 55: return "drizzle";
    case 56: case 57: return "freezing drizzle";
    case 61: return "light rain";
    case 63: return "rain";
    case 65: return "heavy rain";
    case 66: case 67: return "freezing rain";
    case 71: return "light snow";
    case 73: return "snow";
    case 75: return "heavy snow";
    case 77: return "snow grains";
    case 80: return "light showers";
    case 81: return "showers";
    case 82: return "heavy showers";
    case 85: case 86: return "snow showers";
    case 95: return "thunderstorm";
    case 96: case 99: return "thunder + hail";
    default: return "unknown";
  }
}

// 天气图标：都用矢量画，跟 icons.cpp 里那套一个风格（不引位图，省 flash）
static void drawWxIcon(int cx, int cy, int r, int code, bool isDay) {
  WxKind k = wxKindOf(code);
  uint16_t sunCol = isDay ? TFT_YELLOW : cv.color565(180, 200, 255);   // 夜间画月亮色
  uint16_t cloudCol = TFT_LIGHTGREY;

  if (k == WX_CLEAR || k == WX_PARTLY) {
    int sx = (k == WX_PARTLY) ? cx - r / 3 : cx;
    int sy = (k == WX_PARTLY) ? cy - r / 3 : cy;
    int sr = (k == WX_PARTLY) ? r / 2 : r * 2 / 3;
    if (isDay) {
      cv.fillCircle(sx, sy, sr, sunCol);
      for (int i = 0; i < 8; i++) {           // 光芒
        float a = i * PI / 4;
        cv.drawLine(sx + cosf(a) * (sr + 2), sy + sinf(a) * (sr + 2),
                    sx + cosf(a) * (sr + 5), sy + sinf(a) * (sr + 5), sunCol);
      }
    } else {
      cv.fillCircle(sx, sy, sr, sunCol);                        // 月亮 = 圆挖一块
      cv.fillCircle(sx + sr / 2, sy - sr / 2, sr, TFT_BLACK);
    }
    if (k == WX_CLEAR) return;
  }

  if (k == WX_FOG) {
    for (int i = 0; i < 4; i++)
      cv.drawFastHLine(cx - r + (i % 2) * 3, cy - r / 2 + i * (r / 3), r * 2 - 6, cloudCol);
    return;
  }

  // 剩下的都带云：一坨三个圆 + 一条底边
  int by = cy + r / 3;
  cv.fillCircle(cx - r / 2, by, r / 2, cloudCol);
  cv.fillCircle(cx + r / 3, by, r * 2 / 5, cloudCol);
  cv.fillCircle(cx - r / 12, by - r / 4, r / 2, cloudCol);
  cv.fillRect(cx - r / 2, by, r, r / 2, cloudCol);

  if (k == WX_RAIN) {
    for (int i = 0; i < 3; i++) {
      int x = cx - r / 2 + i * (r / 2);
      cv.drawLine(x, by + r / 2 + 2, x - 2, by + r, cv.color565(80, 160, 255));
    }
  } else if (k == WX_SNOW) {
    for (int i = 0; i < 3; i++) {
      int x = cx - r / 2 + i * (r / 2), y = by + r * 3 / 4;
      cv.drawFastHLine(x - 2, y, 5, TFT_WHITE);
      cv.drawFastVLine(x, y - 2, 5, TFT_WHITE);
    }
  } else if (k == WX_STORM) {
    int x = cx - 2, y = by + r / 2;
    cv.fillTriangle(x + 4, y, x - 3, y + r / 2, x + 1, y + r / 2, TFT_YELLOW);
    cv.fillTriangle(x + 1, y + r / 2, x + 5, y + r / 2, x - 1, y + r, TFT_YELLOW);
  }
}

// 空气质量：另一个免 key 端点，同样支持明文 HTTP。拉不到就把 aqi 置 -1，
// 那一页显示 n/a，不影响别的页面——所以这里所有失败都是静默 return。
static void fetchAirQuality(const GeoFix& fix) {
  aqi = pm25 = pm10v = o3v = -1;
  char url[220];
  snprintf(url, sizeof(url),
           "http://air-quality-api.open-meteo.com/v1/air-quality"
           "?latitude=%.4f&longitude=%.4f&current=pm10,pm2_5,us_aqi,ozone",
           fix.lat, fix.lon);

  WiFiClient client;
  JsonDocument doc;
  String ignored;
  HttpJsonOptions options(8000, 10000);
  options.stream = false;
  options.jsonError = nullptr;
  if (!fetchJsonHttp(client, url, doc, nullptr, options, ignored)) return;
  JsonObject c = doc["current"];
  if (c.isNull()) return;
  aqi   = (int)lroundf(c["us_aqi"] | -1.0f);
  pm25  = (int)lroundf(c["pm2_5"]  | -1.0f);
  pm10v = (int)lroundf(c["pm10"]   | -1.0f);
  o3v   = (int)lroundf(c["ozone"]  | -1.0f);
}

// ---- 拉数据 ----
static void weatherFetch() {
  errMsg = "";
  if (!wifiEnsureConnected()) { errMsg = "wifi not connected"; dirty = true; return; }

  bgFetchStatus("locating");   // 工人线程不能画屏：交给主线程的指示去画（见 bg_fetch.h）
  GeoFix fix;
  if (!geoGet(fix, &errMsg)) { dirty = true; return; }
  place = fix.name; fromGps = fix.fromGps;
  curLat = fix.lat; curLon = fix.lon;

  bgFetchStatus("fetching");

  // timezone=auto 让日出日落直接是当地时间，不用自己换算时区
  // ⚠️ 512 不是随手写的：公制拼出来 416~418 字节，英制那截后缀再加 48 字节 = 464~466。
  // 原来写的 420 刚好只够公制（余量 2~4 字节），一切到 Imperial 就被 snprintf 截成
  // "...&timezone=auto&te"，单位参数整个丢掉——接口要么 400、要么当没看见照发公制，
  // 后者更坏：屏幕上是摄氏数字标着 °F，而且 drawWeatherAir() 会把已经是 km/h 的风速
  // 再乘一次 1.60934，风力等级虚高 1.6 倍。往这条 URL 里加参数前先重算这个数。
  char url[512];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,"
           "weather_code,wind_speed_10m,wind_direction_10m,pressure_msl"
           "&hourly=temperature_2m,precipitation_probability&forecast_hours=24"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
           "uv_index_max,precipitation_probability_max"
           "&forecast_days=%d&timezone=auto%s",
           fix.lat, fix.lon, FC_DAYS,
           weatherImperial ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");

  // 明文 HTTP：只读公开天气数据、请求不带任何凭据，没必要为它付 TLS 那几百毫秒 + 40KB 连续堆。
  // ⚠️ 这个接口回的是 Transfer-Encoding: chunked，但下面用的是 getString()，
  // HTTPClient 会自己解块——要是改成 getStream() 直接喂解析器就会把块头当数据，记住这条。
  WiFiClient client;
  JsonDocument doc;
  HttpJsonOptions options(15000, 20000);
  options.stream = false;
  options.jsonError = "bad json";
  options.includeJsonDetail = false;
  if (!fetchJsonHttp(client, url, doc, nullptr, options, errMsg)) { dirty = true; return; }

  JsonObject cur = doc["current"];
  if (cur.isNull()) { errMsg = "no current data"; dirty = true; return; }
  curTemp     = cur["temperature_2m"]       | 0.0f;
  curFeels    = cur["apparent_temperature"] | 0.0f;
  curHum      = cur["relative_humidity_2m"] | 0;
  curWind     = cur["wind_speed_10m"]       | 0.0f;
  curWindDir  = (int)lroundf(cur["wind_direction_10m"] | 0.0f);
  curPressure = (int)lroundf(cur["pressure_msl"]       | 0.0f);
  curCode     = cur["weather_code"]         | 0;
  curIsDay    = (int)(cur["is_day"] | 1) != 0;

  JsonObject daily = doc["daily"];
  uvMax  = (int)lroundf(daily["uv_index_max"][0] | 0.0f);
  popMax = daily["precipitation_probability_max"][0] | 0;
  // "2026-07-30T05:12" —— 只要后面 5 个字符的 HH:MM

  dayCount = 0;
  JsonArray dCode = daily["weather_code"];
  JsonArray dMax  = daily["temperature_2m_max"];
  JsonArray dMin  = daily["temperature_2m_min"];
  JsonArray dTime = daily["time"];
  for (int i = 0; i < FC_DAYS && i < (int)dCode.size(); i++) {
    days[i].code = dCode[i] | 0;
    days[i].tmax = dMax[i]  | 0.0f;
    days[i].tmin = dMin[i]  | 0.0f;
    // 日期串是 "YYYY-MM-DD"，标签只留 MM-DD；第一条固定叫 today
    const char* t = dTime[i] | "";
    if (i == 0) snprintf(days[i].label, sizeof(days[i].label), "today");
    else if (strlen(t) >= 10) snprintf(days[i].label, sizeof(days[i].label), "%s", t + 5);
    else snprintf(days[i].label, sizeof(days[i].label), "d+%d", i);
    dayCount++;
  }

  // 未来 24 小时
  hourCount = 0;
  JsonObject hr = doc["hourly"];
  JsonArray hT = hr["temperature_2m"], hP = hr["precipitation_probability"];
  const char* h0 = hr["time"][0] | "";
  hourStartH = (strlen(h0) >= 13) ? atoi(h0 + 11) : 0;   // "2026-07-30T15:00" 取 HH
  for (int i = 0; i < 24 && i < (int)hT.size(); i++) {
    hourTemp[i] = hT[i] | 0.0f;
    hourPop[i]  = (uint8_t)constrain((int)(hP[i] | 0), 0, 100);
    hourCount++;
  }

  fetchAirQuality(fix);   // 单独一个端点，失败不影响其它页

  haveData = true;
  everFetched = true;
  fetchedAtMs = millis();
  int h, m, s;
  if (nowHM(h, m, s)) snprintf(fetchedAtStr, sizeof(fetchedAtStr), "%02d:%02d", h, m);
  else                snprintf(fetchedAtStr, sizeof(fetchedAtStr), "--:--");
  dirty = true;
}

// 延后首拉，别在按键路径里同步拉网络（见 net_job.h）。
// ⚠️ 这一页比 ADS-B/Sats 更需要它：weatherFetch() 串了四步阻塞操作
// （连 Wi-Fi 15s + IP 定位 8s + forecast 20s + 空气质量 10s），而 weatherEnter()
// 是从主菜单的 Enter 键回调里直接调的——以前按一下进 Weather，最坏能把主循环冻住 50 秒，
// 期间键盘、BtnA、自动熄屏全部没反应。
static DeferredFetch job;

void weatherEnter() {
  if (!haveData || millis() - fetchedAtMs > STALE_MS) job.request();
  dirty = true;
}

void weatherUpdate() {
  if (job.due()) bgFetchRun(weatherFetch);   // 交给后台工人（bg_fetch.h）
}

void weatherKey(char k) {
  // 走同一条延后路径：按键回调里同步拉网络照样会卡住主循环
  if (k == 'r' || k == 'R' || k == '\n') job.request();
  dirty = true;
}

// ---- 绘制小零件 ----
// ⚠️ job.markShown() 挂在这里而不是六个 drawWeatherXxx() 各写一遍：六页全都以
// drawHeader() 开头，挂一处就覆盖全部，也不会漏（漏了那一页就永远停在 loading）。
static void drawHeader(const char* title) {
  job.markShown();
  cv.fillScreen(TFT_BLACK);
  drawPageHeader(title);
}

// 右上角一行：地点 + 拉取时刻。P1 因为地方宽裕拆成两行，后面几页用这个紧凑版，
// 顺带把标题右边那条空带填掉。
static void drawMeta() {
  if (!haveData) return;
  cv.setTextDatum(top_right); cv.setTextSize(1);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString(trunc(place, 10) + " @" + fetchedAtStr, SW - 6, 6);
}

// 没数据时各页共用的占位（要么还在拉，要么还没拉过，要么拉失败了）
static bool drawEmptyState() {
  if (haveData) return false;
  const int x = 16, y = 48, w = SW - 32, h = 38;
  cv.drawRoundRect(x, y, w, h, 5, DIM_BORDER);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  if (job.pending) {
    // 已经排上队、等这一帧推上屏之后 weatherUpdate() 就开工（见 net_job.h）。
    // 之前这里会显示 "no data - press r"，因为那会儿拉取是同步的，根本不存在"等待中"这个状态。
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("loading...", SW / 2, SH / 2);
  } else if (errMsg.length()) {
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.drawString(trunc(errMsg, 32), SW / 2, y + 12);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString(everFetched ? "r  retry" : "r  retry   /   needs WiFi", SW / 2, y + 27);
  } else {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("no data", SW / 2, y + 12);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("press r to refresh", SW / 2, y + 27);
  }
  return true;
}

// 天气模块统一配色规范
static const uint16_t WX_CARD_BG    = cv.color565(18, 22, 28);    // #12161c 暗底卡片
static const uint16_t WX_BORDER     = cv.color565(36, 42, 50);    // #242a32 边框线
static const uint16_t WX_TEXT_MUTED = cv.color565(120, 130, 140);// #78828c 次级灰
static const uint16_t WX_BLUE       = cv.color565(70, 160, 245);  // #46a0f5 晴冷/雨水浅蓝
static const uint16_t WX_WARM_ORANGE= cv.color565(255, 140, 40);  // 暖温/高热橙

// 蒲福风力等级（km/h -> 0~12 级）+ 配色
static int beaufort(float kmh) {
  static const int t[12] = {1, 6, 12, 20, 29, 39, 50, 62, 75, 89, 103, 118};
  int b = 0;
  for (int i = 0; i < 12; i++) if (kmh >= t[i]) b = i + 1;
  return b;
}
static uint16_t bfColor(int lv) {
  if (lv <= 2) return cv.color565(74, 184, 160);
  if (lv <= 4) return cv.color565(122, 192, 96);
  if (lv <= 6) return cv.color565(224, 176, 64);
  if (lv <= 8) return cv.color565(224, 128, 64);
  return cv.color565(224, 80, 64);
}

// 航海级风向罗盘：双色指针 + 四向刻度 + 中心轴心
static void drawWindArrow(int cx, int cy, int r, int dirFrom, int spd) {
  cv.drawCircle(cx, cy, r, WX_BORDER);
  cv.drawCircle(cx, cy, r - 1, cv.color565(12, 15, 20));

  // 四向微刻度
  cv.drawFastVLine(cx, cy - r, 3, WX_TEXT_MUTED);
  cv.drawFastVLine(cx, cy + r - 3, 3, WX_BORDER);
  cv.drawFastHLine(cx - r, cy, 3, WX_BORDER);
  cv.drawFastHLine(cx + r - 3, cy, 3, WX_BORDER);

  cv.setTextDatum(middle_center); cv.setTextSize(1);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("N", cx, cy - r - 6);

  uint16_t col = bfColor(beaufort(spd));
  float b = (dirFrom + 180) * PI / 180.0f;
  float dx = sinf(b), dy = -cosf(b), px = -dy, py = dx;
  float L = r * 0.78f;
  int xt = cx + dx * L, yt = cy + dy * L;
  int xb = cx - dx * (L * 0.5f), yb = cy - dy * (L * 0.5f);

  // 指针主杆
  cv.drawLine(xb, yb, xt, yt, col);
  // 前端箭头
  cv.fillTriangle(xt, yt, xt - dx * 7 + px * 4, yt - dy * 7 + py * 4,
                          xt - dx * 7 - px * 4, yt - dy * 7 - py * 4, col);
  // 尾部配重反向小角
  cv.fillTriangle(xb, yb, xb + dx * 4 + px * 3, yb + dy * 4 + py * 3,
                          xb + dx * 4 - px * 3, yb + dy * 4 - py * 3, cv.color565(60, 70, 80));
  // 中心轴心
  cv.fillCircle(cx, cy, 2, TFT_WHITE);
}

// 一格 2x2 统计卡片：带圆角暗底槽
static void wxStatCard(int cx, int cy, int cw, int ch, const char* lab, const char* val, uint16_t vc) {
  cv.fillRoundRect(cx, cy, cw, ch, 3, WX_CARD_BG);
  cv.drawRoundRect(cx, cy, cw, ch, 3, WX_BORDER);

  cv.setTextDatum(top_center); cv.setTextSize(1);
  cv.setTextColor(WX_TEXT_MUTED, WX_CARD_BG);
  cv.drawString(lab, cx + cw / 2, cy + 3);

  cv.setTextColor(vc, WX_CARD_BG);
  cv.drawString(val, cx + cw / 2, cy + 14);
}

// 温度统计卡片：带圆角暗底槽 + 精致独立度数圈
static void wxTempCard(int cx, int cy, int cw, int ch, const char* lab, float temp, uint16_t vc) {
  cv.fillRoundRect(cx, cy, cw, ch, 3, WX_CARD_BG);
  cv.drawRoundRect(cx, cy, cw, ch, 3, WX_BORDER);

  cv.setTextDatum(top_center); cv.setTextSize(1);
  cv.setTextColor(WX_TEXT_MUTED, WX_CARD_BG);
  cv.drawString(lab, cx + cw / 2, cy + 3);

  char tBuf[16];
  snprintf(tBuf, sizeof(tBuf), "%.0f", temp);
  int tw = cv.textWidth(tBuf);
  const char* u = tempUnit();
  int uw = cv.textWidth(u);
  int totalW = tw + 4 + uw;
  int sx = cx + cw / 2 - totalW / 2;
  int vy = cy + (ch > 28 ? 16 : 14);

  cv.setTextDatum(top_left);
  cv.setTextColor(vc, WX_CARD_BG);
  cv.drawString(tBuf, sx, vy);
  cv.drawCircle(sx + tw + 1, vy + 2, 1, vc);
  cv.drawString(u, sx + tw + 4, vy);
}

// ---- P1 实况 ----
void drawWeather() {
  drawHeader("Weather");
  if (drawEmptyState()) return;

  cv.setTextDatum(top_right); cv.setTextSize(1);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  if (fromGps) {
    char loc[24];
    snprintf(loc, sizeof(loc), "%.2f,%.2f", curLat, curLon);
    cv.drawString(loc, SW - 8, 4);
  } else {
    cv.drawString(trunc(place, 20), SW - 8, 4);
  }
  cv.drawString(String("@ ") + fetchedAtStr, SW - 8, 16);

  // 主天气图标
  drawWxIcon(38, 54, 22, curCode, curIsDay);

  // 主温度（Font7 高清数码管字体）
  char b[16];
  snprintf(b, sizeof(b), "%.0f", curTemp);
  cv.setFont(&fonts::Font7);
  cv.setTextDatum(top_left);
  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  cv.drawString(b, 78, 30);
  int tw = cv.textWidth(b);
  cv.setFont(&fonts::Font0); // 恢复默认字体

  // 单位上角标度数符号 + C/F
  cv.setTextDatum(top_left); cv.setTextSize(2);
  cv.setTextColor(ACCENT, TFT_BLACK);
  int unitX = 78 + tw + 4;
  cv.drawCircle(unitX + 3, 35, 2, ACCENT);
  cv.drawString(tempUnit(), unitX + 9, 32);

  // 天气描述
  cv.setTextSize(1);
  cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  cv.drawString(wxText(curCode), 80, 68);

  // 底部三联微型暗底胶囊卡片（FEELS / HUM / WIND）
  const int cardY = 88, cardH = 34, gap = 5, margin = 4;
  const int cardW = (SW - 2 * margin - 2 * gap) / 3; // 74px

  auto drawP1Card = [&](int cx, const char* title, const char* val, uint16_t col) {
    cv.fillRoundRect(cx, cardY, cardW, cardH, 3, WX_CARD_BG);
    cv.drawRoundRect(cx, cardY, cardW, cardH, 3, WX_BORDER);
    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.setTextColor(WX_TEXT_MUTED, WX_CARD_BG);
    cv.drawString(title, cx + cardW / 2, cardY + 3);
    cv.setTextColor(col, WX_CARD_BG);
    cv.drawString(val, cx + cardW / 2, cardY + 16);
  };

  char valBuf[16];
  // Card 0: FEELS
  wxTempCard(margin, cardY, cardW, cardH, "FEELS", curFeels, TFT_WHITE);

  // Card 1: HUMIDITY
  snprintf(valBuf, sizeof(valBuf), "%d%%", curHum);
  drawP1Card(margin + cardW + gap, "HUM", valBuf, curHum >= 70 ? WX_BLUE : TFT_WHITE);

  // Card 2: WIND
  snprintf(valBuf, sizeof(valBuf), "%s %.0f", cardOf(curWindDir), curWind);
  drawP1Card(margin + (cardW + gap) * 2, "WIND", valBuf, TFT_WHITE);

  drawPageDots();
}


// ---- P2 未来 24 小时：温度折线 + 降水概率柱 ----
void drawWeatherHour() {
  drawHeader("Next 24h");
  if (drawEmptyState()) return;
  drawMeta();

  if (hourCount < 2) {
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("no hourly data", SW / 2, SH / 2);
    drawPageDots();
    return;
  }

  const int gx = 28, gw = SW - gx - 8, gy = 30, gh = 56, baseY = gy + gh;
  const float step = (float)gw / (hourCount - 1);

  float lo = hourTemp[0], hi = hourTemp[0];
  int iMin = 0, iMax = 0;
  for (int i = 1; i < hourCount; i++) {
    if (hourTemp[i] < lo) { lo = hourTemp[i]; iMin = i; }
    if (hourTemp[i] > hi) { hi = hourTemp[i]; iMax = i; }
  }
  if (hi - lo < 2.0f) { hi = lo + 2.0f; }

  // 降水概率柱（带微圆角）
  for (int i = 0; i < hourCount; i++) {
    if (hourPop[i] <= 0) continue;
    int x = gx + (int)(i * step);
    int h = hourPop[i] * (gh / 2) / 100;
    if (h < 2) h = 2;
    cv.fillRoundRect(x - 2, baseY - h, 4, h, 1, cv.color565(35, 75, 140));
  }

  // 折线下方 Area 微阴影
  for (int i = 0; i < hourCount; i++) {
    int x = gx + (int)(i * step);
    int y = baseY - (int)((hourTemp[i] - lo) / (hi - lo) * (gh - 6)) - 3;
    cv.drawFastVLine(x, y + 1, baseY - y - 1, cv.color565(12, 28, 22));
  }

  // 温度折线（双线加粗）
  int px = -1, py = 0;
  int minX = 0, minY = 0, maxX = 0, maxY = 0;
  for (int i = 0; i < hourCount; i++) {
    int x = gx + (int)(i * step);
    int y = baseY - (int)((hourTemp[i] - lo) / (hi - lo) * (gh - 6)) - 3;
    if (px >= 0) {
      cv.drawLine(px, py, x, y, ACCENT);
      cv.drawLine(px, py + 1, x, y + 1, ACCENT);
    }
    if (i == iMin) { minX = x; minY = y; }
    if (i == iMax) { maxX = x; maxY = y; }
    px = x; py = y;
  }
  cv.drawFastHLine(gx, baseY, gw, DIM_BORDER);

  // 最高温与最低温高光标记点 (Peak & Valley Highlight)
  char b[24];
  // 最高点：暖橙色圆点 + 标签
  cv.fillCircle(maxX, maxY, 3, WX_WARM_ORANGE);
  cv.fillCircle(maxX, maxY, 1, TFT_WHITE);
  snprintf(b, sizeof(b), "%.0f", hi);
  int tw = cv.textWidth(b);
  cv.setTextDatum(bottom_center); cv.setTextSize(1);
  cv.setTextColor(WX_WARM_ORANGE, TFT_BLACK);
  cv.drawString(b, maxX - 1, maxY - 4);
  cv.drawCircle(maxX - 1 + tw / 2 + 2, maxY - 10, 1, WX_WARM_ORANGE);

  // 最低点：浅青色圆点 + 标签
  cv.fillCircle(minX, minY, 3, WX_BLUE);
  cv.fillCircle(minX, minY, 1, TFT_WHITE);
  snprintf(b, sizeof(b), "%.0f", lo);
  tw = cv.textWidth(b);
  cv.setTextColor(WX_BLUE, TFT_BLACK);
  if (minY >= baseY - 14) {
    // 靠近底部时显示在圆点上方，避免遮挡 x 轴刻度
    cv.setTextDatum(bottom_center); cv.setTextSize(1);
    cv.drawString(b, minX - 1, minY - 4);
    cv.drawCircle(minX - 1 + tw / 2 + 2, minY - 10, 1, WX_BLUE);
  } else {
    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.drawString(b, minX - 1, minY + 4);
    cv.drawCircle(minX - 1 + tw / 2 + 2, minY + 6, 1, WX_BLUE);
  }

  // 左侧参考标尺
  cv.setTextDatum(middle_right); cv.setTextSize(1);
  cv.setTextColor(WX_TEXT_MUTED, TFT_BLACK);
  snprintf(b, sizeof(b), "%.0f%s", hi, tempUnit()); cv.drawString(b, gx - 3, gy + 4);
  snprintf(b, sizeof(b), "%.0f%s", lo, tempUnit()); cv.drawString(b, gx - 3, baseY - 4);

  // 时间刻度：每 6 小时一个
  cv.setTextDatum(top_center); cv.setTextSize(1);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  for (int i = 0; i < hourCount; i += 6) {
    int x = gx + (int)(i * step);
    cv.drawFastVLine(x, baseY + 1, 3, TFT_DARKGREY);
    snprintf(b, sizeof(b), "%02d", (hourStartH + i) % 24);
    cv.drawString(b, x, baseY + 5);
  }

  // 底部降水结论
  int pkI = 0;
  for (int i = 1; i < hourCount; i++) if (hourPop[i] > hourPop[pkI]) pkI = i;
  cv.setTextDatum(bottom_center); cv.setTextSize(1);
  if (hourPop[pkI] >= 30) {
    cv.setTextColor(WX_BLUE, TFT_BLACK);
    snprintf(b, sizeof(b), "rain %d%% @%02d:00", hourPop[pkI], (hourStartH + pkI) % 24);
  } else {
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    snprintf(b, sizeof(b), "no rain expected");
  }
  cv.drawString(b, SW / 2, SH - 9);

  drawPageDots();
}

// ---- P3 风与空气 ----
void drawWeatherAir() {
  drawHeader("Wind/Air");
  if (drawEmptyState()) return;
  drawMeta();

  int bf = beaufort(weatherImperial ? curWind * 1.60934f : curWind);

  // 左侧罗盘
  drawWindArrow(44, 52, 23, curWindDir,
                (int)(weatherImperial ? curWind * 1.60934f : curWind));

  char v[32];
  cv.setTextDatum(top_center); cv.setTextSize(1);
  snprintf(v, sizeof(v), "%s %.0f%s", cardOf(curWindDir), curWind, windUnit());
  cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  cv.drawString(v, 44, 82);

  // 右侧 2x2 统计卡片
  const int cw = 62, ch = 26;
  const int c0 = 100, c1 = 168;
  const int r0 = 26, r1 = 56;

  wxTempCard(c0, r0, cw, ch, "FEELS", curFeels, TFT_WHITE);

  snprintf(v, sizeof(v), "%d", uvMax);
  wxStatCard(c1, r0, cw, ch, "UV", v, uvMax >= 8 ? TFT_RED : (uvMax >= 6 ? WX_WARM_ORANGE : TFT_WHITE));

  snprintf(v, sizeof(v), "%d", curPressure);
  wxStatCard(c0, r1, cw, ch, "hPa", v, TFT_WHITE);

  snprintf(v, sizeof(v), "%d%%", popMax);
  wxStatCard(c1, r1, cw, ch, "RAIN", v, popMax >= 50 ? WX_BLUE : TFT_WHITE);

  // 12 段蒲福风力条（横跨整屏）
  const int bx = 10, bw = SW - 20, segs = 12, gap = 2;
  const int segw = (bw - (segs - 1) * gap) / segs, by = 104, bh = 6;
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("BEAUFORT WIND", bx, by - 12);

  cv.setTextDatum(top_right);
  cv.setTextColor(bfColor(bf), TFT_BLACK);
  snprintf(v, sizeof(v), "Level %d", bf);
  cv.drawString(v, bx + bw, by - 12);

  for (int i = 0; i < segs; i++)
    cv.fillRoundRect(bx + i * (segw + gap), by, segw, bh, 2, i < bf ? bfColor(i + 1) : DIM_BORDER);

  drawPageDots();
}

// ---- P4 空气质量 ----
static const char* aqiLevel(int v) {
  if (v <= 50)  return "GOOD";
  if (v <= 100) return "MODERATE";
  if (v <= 150) return "SENSITIVE";
  if (v <= 200) return "UNHEALTHY";
  if (v <= 300) return "VERY BAD";
  return "HAZARDOUS";
}
static uint16_t aqiColor(int v) {
  if (v <= 50)  return cv.color565(0, 228, 100);
  if (v <= 100) return cv.color565(255, 230, 40);
  if (v <= 150) return cv.color565(255, 126, 0);
  if (v <= 200) return cv.color565(255, 60, 60);
  if (v <= 300) return cv.color565(180, 90, 200);
  return cv.color565(160, 60, 60);
}

void drawWeatherAqi() {
  drawHeader("Air Quality");
  if (drawEmptyState()) return;
  drawMeta();

  if (aqi < 0) {
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("air quality n/a", SW / 2, SH / 2);
    drawPageDots();
    return;
  }

  char b[24];
  const uint16_t col = aqiColor(aqi);

  // AQI 大数字：数码管 Font7
  snprintf(b, sizeof(b), "%d", aqi);
  cv.setFont(&fonts::Font7);
  cv.setTextDatum(top_left);
  cv.setTextColor(col, TFT_BLACK);
  cv.drawString(b, 10, 25);
  int tw = cv.textWidth(b);
  cv.setFont(&fonts::Font0);

  // 右侧说明 + 彩色胶囊 Badge
  cv.setTextSize(1);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("US AQI", 10 + tw + 10, 28);

  const char* lvl = aqiLevel(aqi);
  int badgeW = (int)strlen(lvl) * 6 + 12;
  int badgeX = 10 + tw + 10, badgeY = 44, badgeH = 14;
  cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 3, col);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  cv.setTextColor(TFT_BLACK, col);
  cv.drawString(lvl, badgeX + badgeW / 2, badgeY + badgeH / 2 + 1);

  // 0~300 分段指示条
  const int bx = 10, bw = SW - 20, by = 74, bh = 6;
  static const int SEG[5] = { 50, 100, 150, 200, 300 };
  int prev = 0;
  for (int i = 0; i < 5; i++) {
    int x0 = bx + bw * prev / 300, x1 = bx + bw * min(SEG[i], 300) / 300;
    cv.fillRect(x0, by, x1 - x0 - 1, bh, aqiColor(SEG[i] - 1));
    prev = SEG[i];
  }
  int mx = bx + bw * min(aqi, 300) / 300;
  cv.fillTriangle(mx, by - 5, mx - 4, by - 1, mx + 4, by - 1, TFT_WHITE);

  // 底部三大污染物微型卡片（PM2.5 / PM10 / O3）
  const int cardY = 88, cardH = 34, gap = 5, margin = 4;
  const int cardW = (SW - 2 * margin - 2 * gap) / 3;

  auto aqiItemCard = [&](int cx, const char* lab, int v, uint16_t valCol) {
    cv.fillRoundRect(cx, cardY, cardW, cardH, 3, WX_CARD_BG);
    cv.drawRoundRect(cx, cardY, cardW, cardH, 3, WX_BORDER);
    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.setTextColor(WX_TEXT_MUTED, WX_CARD_BG);
    cv.drawString(lab, cx + cardW / 2, cardY + 3);
    char t[16];
    if (v < 0) snprintf(t, sizeof(t), "--");
    else       snprintf(t, sizeof(t), "%d", v);
    cv.setTextSize(2);
    cv.setTextColor(valCol, WX_CARD_BG);
    cv.drawString(t, cx + cardW / 2, cardY + 14);
  };

  // PM2.5 颜色判定：<=12 优, <=35 良, <=55 偏高
  uint16_t c25 = pm25 < 0 ? TFT_DARKGREY : (pm25 <= 12 ? cv.color565(0, 228, 100) : (pm25 <= 35 ? cv.color565(255, 230, 40) : WX_WARM_ORANGE));
  uint16_t c10 = pm10v < 0 ? TFT_DARKGREY : (pm10v <= 54 ? cv.color565(0, 228, 100) : (pm10v <= 154 ? cv.color565(255, 230, 40) : WX_WARM_ORANGE));
  uint16_t cO3 = o3v < 0 ? TFT_DARKGREY : (o3v <= 54 ? cv.color565(0, 228, 100) : (o3v <= 70 ? cv.color565(255, 230, 40) : WX_WARM_ORANGE));

  aqiItemCard(margin, "PM2.5", pm25, c25);
  aqiItemCard(margin + cardW + gap, "PM10", pm10v, c10);
  aqiItemCard(margin + (cardW + gap) * 2, "O3", o3v, cO3);

  drawPageDots();
}

// ---- P5 五天预报 ----
void drawWeatherFc() {
  drawHeader("Forecast");
  if (drawEmptyState()) return;
  drawMeta();

  float lo = 1e9f, hi = -1e9f;
  for (int i = 0; i < dayCount; i++) { lo = min(lo, days[i].tmin); hi = max(hi, days[i].tmax); }
  if (hi - lo < 1.0f) hi = lo + 1.0f;

  const int cw = dayCount > 0 ? SW / dayCount : SW;
  const int barTop = 76, barH = 28;

  // 星期计算支持
  int todayWd = 0;
  if (timeSynced) {
    time_t now = time(nullptr);
    struct tm ti{};
    if (localtime_r(&now, &ti)) todayWd = ti.tm_wday;
  }
  static const char* const WKNAMES[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

  for (int i = 0; i < dayCount; i++) {
    int cx = i * cw + cw / 2;
    drawWxIcon(cx, 36, 11, days[i].code, true);

    // 星期与日期展示
    cv.setTextDatum(top_center); cv.setTextSize(1);
    if (i == 0) {
      cv.setTextColor(ACCENT, TFT_BLACK);
      cv.drawString("TODAY", cx, 52);
    } else {
      cv.setTextColor(TFT_WHITE, TFT_BLACK);
      int wdayIdx = (todayWd + i) % 7;
      cv.drawString(WKNAMES[wdayIdx], cx, 52);
      cv.setTextColor(WX_TEXT_MUTED, TFT_BLACK);
      cv.drawString(days[i].label, cx, 62);
    }

    int yTop = barTop + (int)((hi - days[i].tmax) / (hi - lo) * barH);
    int yBot = barTop + (int)((hi - days[i].tmin) / (hi - lo) * barH);
    if (yBot <= yTop) yBot = yTop + 2;

    // 温差胶囊条：首日为高亮绿，其余天用暖-冷微胶囊
    uint16_t barCol = (i == 0) ? ACCENT : cv.color565(40, 140, 100);
    cv.fillRoundRect(cx - 3, yTop, 6, yBot - yTop, 3, barCol);

    // 最高温（暖白/橙）
    char b[16];
    uint16_t hiCol = (i == 0 ? TFT_WHITE : cv.color565(255, 230, 200));
    cv.setTextColor(hiCol, TFT_BLACK);
    snprintf(b, sizeof(b), "%.0f", days[i].tmax);
    int tw = cv.textWidth(b);
    cv.setTextDatum(bottom_center);
    cv.drawString(b, cx - 1, yTop - 1);
    cv.drawCircle(cx - 1 + tw / 2 + 2, yTop - 7, 1, hiCol);

    // 最低温（浅冷蓝）
    uint16_t loCol = (i == 0 ? TFT_LIGHTGREY : WX_BLUE);
    cv.setTextColor(loCol, TFT_BLACK);
    snprintf(b, sizeof(b), "%.0f", days[i].tmin);
    tw = cv.textWidth(b);
    cv.setTextDatum(top_center);
    cv.drawString(b, cx - 1, yBot + 2);
    cv.drawCircle(cx - 1 + tw / 2 + 2, yBot + 4, 1, loCol);
  }

  drawPageDots();
}
