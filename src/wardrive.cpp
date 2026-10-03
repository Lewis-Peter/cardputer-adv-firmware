#include "wardrive.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>
#include "ui_common.h"
#include "sd_files.h"
#include "gnss.h"
#include "wifi_net.h"
#include "hotspot.h"

static const char* LOG_PATH = "/wardrive.csv";
static const uint32_t SCAN_INTERVAL_MS = 8000;   // 一轮扫完后，AUTO 模式隔多久再扫一次
static const uint32_t SCAN_TIMEOUT_MS  = 10000;  // 异步扫描迟迟不返回时的兜底超时

// 去重表：环形覆盖最老的一条
static const int SEEN_MAX = 128;
static uint8_t seenBssid[SEEN_MAX][6];
static int seenCount = 0;
static int seenNext  = 0;

// 实时捕获 AP 环形队列（给屏幕卡片渲染提供结构化数据）
struct WardriveRec {
  char ssid[24];
  uint8_t mac[6];
  int8_t rssi;
  uint8_t chan;
  uint8_t authmode;
  bool hasGps;
};
static const int FEED_N = 16;
static WardriveRec feedLog[FEED_N];
static int feedHead = 0, feedCount = 0;

static bool scanning = false;
static bool autoMode = false;
static uint32_t scanStartMs = 0;
static uint32_t nextScanMs = 0;
static int roundNum = 0;
static int totalUnique = 0;
static int lastRoundNew = 0;

static bool isSeen(const uint8_t* bssid) {
  for (int i = 0; i < seenCount; i++) if (memcmp(seenBssid[i], bssid, 6) == 0) return true;
  return false;
}

static void markSeen(const uint8_t* bssid) {
  memcpy(seenBssid[seenNext], bssid, 6);
  seenNext = (seenNext + 1) % SEEN_MAX;
  if (seenCount < SEEN_MAX) seenCount++;
}

static void resetStats() {
  seenCount = 0; seenNext = 0; totalUnique = 0; roundNum = 0; lastRoundNew = 0;
  feedCount = 0; feedHead = 0;
}

static const char* authShort(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP:  return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA12";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    default: return "ENC";
  }
}

static void startScan() {
  esp_wifi_set_promiscuous(false);
  const wifi_mode_t scanMode = hotspotScanMode();
  WiFi.mode(scanMode);
  uint32_t t0 = millis();
  while (WiFi.getMode() != scanMode && (millis() - t0 < 300)) delay(10);
  WiFiGenericClass::waitStatusBits(STA_STARTED_BIT, 300);
  delay(30);

  if (WiFiGenericClass::getStatusBits() & WIFI_SCANNING_BIT) {
    esp_wifi_scan_stop();
    delay(30);
  }
  WiFi.scanDelete();
  int16_t res = WiFi.scanNetworks(true);   // 异步扫描
  if (res == WIFI_SCAN_FAILED) {
    delay(100);
    WiFi.mode(scanMode);
    WiFi.scanDelete();
    res = WiFi.scanNetworks(true);
  }
  if (res != WIFI_SCAN_FAILED) {
    scanning = true;
    scanStartMs = millis();
  } else {
    scanning = false;
    nextScanMs = millis() + 1500;
  }
}

void wardriveEnter() {
  // 热点开着就 AP+STA 共存扫描，不把连着热点的设备踢掉
  esp_wifi_set_promiscuous(false);
  const wifi_mode_t scanMode = hotspotScanMode();
  WiFi.mode(scanMode);
  uint32_t t0 = millis();
  while (WiFi.getMode() != scanMode && (millis() - t0 < 300)) delay(10);
  WiFiGenericClass::waitStatusBits(STA_STARTED_BIT, 300);
  WiFi.disconnect();
  delay(40);
  resetStats();
  autoMode = false;
  scanning = false;
  startScan();
}

void wardriveExit() {
  if ((WiFiGenericClass::getStatusBits() & WIFI_SCANNING_BIT) || scanning) {
    esp_wifi_scan_stop();
    delay(30);
    scanning = false;
  }
  WiFi.scanDelete();
  autoMode = false;
  if (hotspotRestoreAfterScan()) return;   // 热点开着：退回纯 AP，bootWifiStart 会切 STA 把热点关掉
  if (WiFi.status() != WL_CONNECTED) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
  bootWifiStart();
}

void wardriveUpdate() {
  if (!scanning) {
    if (autoMode && (int32_t)(millis() - nextScanMs) >= 0) startScan();
    return;
  }

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (millis() - scanStartMs > SCAN_TIMEOUT_MS) {
      if (WiFiGenericClass::getStatusBits() & WIFI_SCANNING_BIT) {
        esp_wifi_scan_stop();
        delay(30);
      }
      scanning = false; WiFi.scanDelete();
      nextScanMs = millis() + SCAN_INTERVAL_MS;
    }
    return;
  }

  scanning = false;
  if (n <= 0) {
    WiFi.scanDelete();
    nextScanMs = millis() + SCAN_INTERVAL_MS;
    return;
  }

  roundNum++;
  lastRoundNew = 0;

  bool haveFix = gnssHasFix();
  double lat = haveFix ? gnssLat() : 0.0;
  double lon = haveFix ? gnssLng() : 0.0;
  String stamp = nowStamp();

  for (int i = 0; i < n; i++) {
    const wifi_ap_record_t* r = (const wifi_ap_record_t*)WiFi.getScanInfoByIndex(i);
    if (!r || isSeen(r->bssid)) continue;
    markSeen(r->bssid);
    totalUnique++; lastRoundNew++;

    char bssidStr[18];
    snprintf(bssidStr, sizeof(bssidStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             r->bssid[0], r->bssid[1], r->bssid[2], r->bssid[3], r->bssid[4], r->bssid[5]);
    String ssid = String((const char*)r->ssid);
    if (ssid.length() == 0) ssid = "(hidden)";

    // 记录到本地结构化队列
    WardriveRec& rec = feedLog[feedHead];
    strncpy(rec.ssid, ssid.c_str(), sizeof(rec.ssid) - 1);
    rec.ssid[sizeof(rec.ssid) - 1] = '\0';
    memcpy(rec.mac, r->bssid, 6);
    rec.rssi = r->rssi;
    rec.chan = r->primary;
    rec.authmode = r->authmode;
    rec.hasGps = haveFix;
    feedHead = (feedHead + 1) % FEED_N;
    if (feedCount < FEED_N) feedCount++;

    // 写入 SD 卡 CSV
    char line[160];
    if (haveFix) {
      snprintf(line, sizeof(line), "%s,%.6f,%.6f,%s,%s,%d,%d,%s",
               stamp.c_str(), lat, lon, ssid.c_str(), bssidStr, r->rssi, r->primary, authShort(r->authmode));
    } else {
      snprintf(line, sizeof(line), "%s,,,%s,%s,%d,%d,%s",
               stamp.c_str(), ssid.c_str(), bssidStr, r->rssi, r->primary, authShort(r->authmode));
    }
    sdAppend(LOG_PATH, String(line));
  }
  WiFi.scanDelete();
  nextScanMs = millis() + SCAN_INTERVAL_MS;
}

void wardriveKey(char k) {
  if (k == '\n') {
    if (scanning && (millis() - scanStartMs > 3000)) {
      esp_wifi_scan_stop();
      scanning = false;
      WiFi.scanDelete();
    }
    if (!scanning) startScan();
  } else if (k == 'a' || k == 'A' || k == ' ') {
    autoMode = !autoMode;
    if (autoMode) nextScanMs = millis();
  } else if (k == 'r' || k == 'R') {
    resetStats();
  }
}

static void rPod(int cx, int y, int cw, int ch, const char* lab, const char* val, uint16_t vc, uint16_t lc = 0x7BEF) {
  cv.fillRoundRect(cx, y, cw, ch, 3, 0x0821);
  cv.drawRoundRect(cx, y, cw, ch, 3, 0x18C3);
  cv.setTextDatum(top_center); cv.setTextSize(1);
  cv.setTextColor(lc, 0x0821);
  cv.drawString(lab, cx + cw / 2, y + 3);
  cv.setTextColor(vc, 0x0821);
  cv.drawString(val, cx + cw / 2, y + 14);
}

void drawWardrive() {
  cv.fillScreen(TFT_BLACK);

  // 顶栏状态装配
  char rightBuf[32];
  const char* sdStatus = sdReady() ? "SD:OK" : "NO SD";
  if (autoMode) {
    int32_t left = (int32_t)(nextScanMs - millis());
    uint32_t remain = left > 0 ? (uint32_t)left / 1000 : 0;
    snprintf(rightBuf, sizeof(rightBuf), "AUTO %us  %s", remain, sdStatus);
  } else {
    snprintf(rightBuf, sizeof(rightBuf), "%s  %s", scanning ? "SCANNING" : "MANUAL", sdStatus);
  }
  drawPageHeader("Wardrive", rightBuf, autoMode ? 0xFDA0 : 0x07E0);

  char b[48];

  // 1. 四联赛博遥测指标舱 (y = 15..41, h = 26)
  snprintf(b, sizeof(b), "%d", roundNum);
  rPod(4, 15, 56, 26, "ROUNDS", b, TFT_WHITE, 0x07FF);

  snprintf(b, sizeof(b), "%d", totalUnique);
  rPod(63, 15, 56, 26, "UNIQUE", b, 0x07E0, 0x07FF);

  snprintf(b, sizeof(b), "+%d", lastRoundNew);
  rPod(122, 15, 56, 26, "NEW +", b, 0xFDA0, 0x07FF);

  int nsat = gnssSats();
  if (gnssHasFix()) {
    snprintf(b, sizeof(b), "%d SATS", nsat);
    rPod(181, 15, 55, 26, "GPS FIX", b, 0x07E0, 0x07FF);
  } else if (nsat > 0) {
    snprintf(b, sizeof(b), "%d SATS", nsat);
    rPod(181, 15, 55, 26, "SEARCH", b, 0xFDA0, 0x632C);
  } else {
    rPod(181, 15, 55, 26, "NO GPS", "--", 0x4208, 0x632C);
  }

  // 2. GPS 经纬度位置横幅 (y = 44..55, h = 12)
  const int warnY = 44, warnH = 12;
  if (gnssHasFix()) {
    cv.fillRoundRect(4, warnY, SW - 8, warnH, 2, 0x0280);
    cv.drawRoundRect(4, warnY, SW - 8, warnH, 2, 0x04A0);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(0x07E0, 0x0280);
    snprintf(b, sizeof(b), "GPS FIX: %.5f, %.5f (%.0fm)", gnssLat(), gnssLng(), gnssAlt());
    cv.drawString(b, SW / 2, warnY + warnH / 2);
  } else {
    cv.fillRoundRect(4, warnY, SW - 8, warnH, 2, 0x0821);
    cv.drawRoundRect(4, warnY, SW - 8, warnH, 2, 0x18C3);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(0x632C, 0x0821);
    if (nsat > 0) {
      snprintf(b, sizeof(b), "TRACKING %d SATS | LOGGING CELL BSSID", nsat);
    } else {
      snprintf(b, sizeof(b), "WARDRIVING AIRSPACE: CH1-13 2.4GHz");
    }
    cv.drawString(b, SW / 2, warnY + warnH / 2);
  }

  // 3. 捕获 AP 实时日志卡片 (y = 59..121, h = 63)
  const int feedY = 59, feedH = 63;
  cv.fillRoundRect(4, feedY, SW - 8, feedH, 3, 0x0821);
  cv.drawRoundRect(4, feedY, SW - 8, feedH, 3, 0x18C3);
  cv.fillRect(4, feedY, 2, feedH, 0x07E0);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("DISCOVERED AP FEED:", 10, feedY + 3);

  cv.setTextDatum(top_right);
  cv.setTextColor(0x8410, 0x0821);
  snprintf(b, sizeof(b), "%d LOGGED", totalUnique);
  cv.drawString(b, SW - 10, feedY + 3);

  cv.drawFastHLine(6, feedY + 13, SW - 12, 0x18C3);

  // 逐条渲染捕获到的 AP
  if (feedCount == 0) {
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(0x4208, 0x0821);
    cv.drawString(scanning ? "Scanning 2.4G airspace channels..." : "Press Enter or 'A' to scan airspace", SW / 2, feedY + 37);
  } else {
    int maxRows = 4;
    int count = feedCount;
    for (int i = 0; i < min(count, maxRows); i++) {
      int idx = (feedHead - 1 - i + FEED_N * 2) % FEED_N;
      const WardriveRec& r = feedLog[idx];
      int rowY = feedY + 16 + i * 11;

      // MAC 后缀
      cv.setTextDatum(top_left); cv.setTextSize(1);
      cv.setTextColor(0x8CD2, 0x0821);
      snprintf(b, sizeof(b), "%02X:%02X:%02X", r.mac[3], r.mac[4], r.mac[5]);
      cv.drawString(b, 10, rowY);

      // 信道
      cv.setTextColor(0x07FF, 0x0821);
      snprintf(b, sizeof(b), "c%02d", r.chan);
      cv.drawString(b, 62, rowY);

      // 加密徽章
      bool isOpen = (r.authmode == WIFI_AUTH_OPEN);
      cv.setTextColor(isOpen ? 0x07E0 : 0xFDA0, 0x0821);
      cv.drawString(isOpen ? "[OPN]" : "[ENC]", 84, rowY);

      // SSID 名称
      cv.setTextColor(TFT_WHITE, 0x0821);
      cv.drawString(trunc(String(r.ssid), 12), 118, rowY);

      // 信号强度胶囊徽章
      const int badgeW = 32, badgeH = 9;
      const int badgeX = SW - 8 - badgeW;
      uint16_t bBg = r.rssi >= -65 ? 0x0280 : (r.rssi >= -78 ? 0x3A00 : 0x3800);
      uint16_t bBdr = r.rssi >= -65 ? 0x04A0 : (r.rssi >= -78 ? 0x7BE0 : 0x7800);
      uint16_t bTxt = r.rssi >= -65 ? 0x07E0 : (r.rssi >= -78 ? 0xFDA0 : 0xF800);

      cv.fillRoundRect(badgeX, rowY - 1, badgeW, badgeH, 2, bBg);
      cv.drawRoundRect(badgeX, rowY - 1, badgeW, badgeH, 2, bBdr);
      cv.setTextDatum(middle_center);
      cv.setTextColor(bTxt, bBg);
      snprintf(b, sizeof(b), "%d", r.rssi);
      cv.drawString(b, badgeX + badgeW / 2, rowY + badgeH / 2 - 1);
    }
  }

  // 4. 底部高对比度彩色快捷键 (y = 125..134)
  const int footY = 125;
  cv.setTextDatum(middle_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 4, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" scan", 32, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("A", 68, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(autoMode ? " stop" : " auto", 74, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", 120, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" reset", 126, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 188, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" back", 194, footY);
}
