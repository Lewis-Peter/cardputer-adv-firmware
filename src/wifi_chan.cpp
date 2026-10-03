#include "wifi_chan.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>
#include <cmath>
#include "ui_common.h"
#include "icons.h"
#include "hotspot.h"
#include "wifi_net.h"

struct ChanAp {
  char ssid[33]; int channel; int rssi; bool secure;
  uint8_t bssid[6];
  wifi_auth_mode_t auth;
  wifi_cipher_type_t cipher;
  uint8_t second;                        // wifi_second_chan_t：NONE/ABOVE/BELOW（=40MHz 副信道方向）
  bool phyB, phyG, phyN, phyLR, wps;
  char cc[3];                            // 国家码（AP beacon 里的监管域）
};

static const int CHAN_MAX = 24;
static ChanAp chanList[CHAN_MAX];
int chanCount = 0;
int chanSel = 0;
int wifiChanView = WIFI_CHAN_VIEW_SPECTRUM;

static const uint32_t CHAN_SCAN_TIMEOUT_MS = 7000;   // 异步扫描超时保护 (实际 ~2.3s 结束，7s 足够)
static const uint32_t CHAN_SCAN_INTERVAL_MS = 3000;  // AUTO 模式每轮扫描后间隔
static bool chanScanning = false;
static uint32_t scanStartMs = 0;
static bool autoMode = false;
static uint32_t nextScanMs = 0;
static int emptyScanRetries = 0;

static uint8_t selectedBssid[6] = {0};
static bool haveSelectedBssid = false;

// ---- 常见厂商 OUI 表（手工精简库，纯 Flash 存储，0 内存开销）----
struct OuiEntry { uint8_t b0, b1, b2; const char* name; };
static const OuiEntry OUI_TABLE[] = {
  {0xAC,0xDE,0x48,"Apple"}, {0x00,0x1E,0xC2,"Apple"}, {0x00,0x1F,0xF3,"Apple"}, {0x28,0x37,0x37,"Apple"},
  {0xF0,0x18,0x98,"Apple"}, {0x3C,0x22,0xFB,"Apple"}, {0x00,0x1C,0xB3,"Apple"}, {0xA4,0x83,0xE7,"Apple"},
  {0xD8,0x96,0x95,"Amazon"}, {0x74,0xC2,0x46,"Amazon"}, {0x40,0xB4,0xCD,"Amazon"}, {0x88,0x4A,0xEA,"Amazon"},
  {0x00,0x1A,0x11,"Google"}, {0xF4,0xF5,0xE8,"Google"}, {0x3C,0x5A,0xB4,"Google"}, {0x18,0xB4,0x30,"Google"},
  {0xB8,0x27,0xEB,"RaspberryPi"}, {0xDC,0xA6,0x32,"RaspberryPi"}, {0xE4,0x5F,0x01,"RaspberryPi"},
  {0x24,0x0A,0xC4,"Espressif"}, {0x30,0xAE,0xA4,"Espressif"}, {0xA0,0x20,0xA6,"Espressif"}, {0x7C,0x9E,0xBD,"Espressif"},
  {0x34,0x85,0x18,"Espressif"}, {0x48,0x27,0xE2,"Espressif"},
  {0xB0,0xB2,0x1C,"Xiaomi"}, {0x28,0x6C,0x07,"Xiaomi"}, {0x64,0xB4,0x73,"Xiaomi"}, {0x78,0x11,0xDC,"Xiaomi"},
  {0x04,0xCF,0x8C,"Xiaomi"},
  {0xE8,0x9F,0x80,"TP-Link"}, {0x50,0xC7,0xBF,"TP-Link"}, {0x14,0xCC,0x20,"TP-Link"}, {0x70,0x4F,0x57,"TP-Link"},
  {0x00,0x14,0x6C,"Netgear"}, {0x20,0xE5,0x2A,"Netgear"}, {0xA0,0x40,0xA0,"Netgear"},
  {0x00,0x1D,0x7E,"D-Link"}, {0x1C,0x7E,0xE5,"D-Link"},
  {0x00,0x24,0x36,"ASUS"}, {0x1C,0x87,0x2C,"ASUS"}, {0x2C,0x56,0xDC,"ASUS"},
  {0x00,0x26,0x5E,"Huawei"}, {0x48,0x46,0xFB,"Huawei"}, {0x00,0x18,0x82,"Huawei"}, {0x20,0x08,0xED,"Huawei"},
  {0x00,0x50,0xF2,"Microsoft"}, {0x28,0x18,0x78,"Microsoft"},
  {0xFC,0xA6,0x67,"Samsung"}, {0x8C,0x77,0x12,"Samsung"}, {0x5C,0x0A,0x5B,"Samsung"},
  {0x60,0x60,0x1F,"DJI"}, {0xE4,0x7A,0x2C,"DJI"}, {0x26,0x37,0x12,"DJI"},
  {0x00,0x16,0x6F,"Intel"}, {0x3C,0xA9,0xF4,"Intel"},
  {0x54,0xE1,0xAD,"Lenovo"}, {0x00,0x1E,0x65,"Lenovo"},
  {0xEC,0xFA,0xBC,"Philips"}, {0xB4,0x2E,0x99,"Sonos"},
  {0xD8,0x15,0x0D,"Mercury"}, {0xC8,0x3A,0x35,"Tenda"}, {0x00,0x0F,0xE2,"H3C"}
};
static const int OUI_COUNT = sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]);

static const char* apVendor(const uint8_t* mac) {
  if (mac[0] & 0x02) return "Private";
  for (int i = 0; i < OUI_COUNT; i++) {
    if (mac[0] == OUI_TABLE[i].b0 && mac[1] == OUI_TABLE[i].b1 && mac[2] == OUI_TABLE[i].b2) {
      return OUI_TABLE[i].name;
    }
  }
  return "Unknown";
}

// 8 色鲜艳高对比调色板，区分不同信道/AP 的频谱曲线
static const uint16_t AP_PALETTE[] = {
  0x07FF, // Electric Cyan
  0xFD20, // Vivid Orange
  0xF81F, // Magenta Pink
  0x07E0, // Bright Emerald Green
  0xFFE0, // Golden Yellow
  0x9CDF, // Sky Blue
  0xFC08, // Coral Red
  0xAFE5, // Lime Neon
};
static const int PALETTE_COUNT = sizeof(AP_PALETTE) / sizeof(AP_PALETTE[0]);

static inline uint16_t apColor(int idx) {
  return AP_PALETTE[idx % PALETTE_COUNT];
}

// 半亮暗色（用于未选中的 AP 曲线，保留色彩但压暗）
static inline uint16_t dimColor(uint16_t c) {
  return ((c & 0xF800) >> 1 & 0x7800) | ((c & 0x07E0) >> 1 & 0x03E0) | ((c & 0x001F) >> 1);
}

// 快速估算 2.4GHz 中心频率
static inline int chanFreq(int ch) {
  return (ch == 14) ? 2484 : (2407 + ch * 5);
}

static const char* authName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_ENTERPRISE:      return "WPA2-E";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI";
    case WIFI_AUTH_WPA3_ENT_192:    return "WPA3-E";
    default:                        return "?";
  }
}

static const char* cipherName(wifi_cipher_type_t c) {
  switch (c) {
    case WIFI_CIPHER_TYPE_NONE:      return "none";
    case WIFI_CIPHER_TYPE_WEP40:
    case WIFI_CIPHER_TYPE_WEP104:    return "WEP";
    case WIFI_CIPHER_TYPE_TKIP:      return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP:      return "CCMP(AES)";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP/CCMP";
    default:                         return "-";
  }
}

static const char* phyStr(const ChanAp& a) {
  static char s[16];
  int n = 0;
  s[0] = '\0';
  if (a.phyB)  { n += snprintf(s + n, sizeof(s) - n, "b"); }
  if (a.phyG)  { n += snprintf(s + n, sizeof(s) - n, "%sg", n ? "/" : ""); }
  if (a.phyN)  { n += snprintf(s + n, sizeof(s) - n, "%sn", n ? "/" : ""); }
  if (a.phyLR) { n += snprintf(s + n, sizeof(s) - n, "%sLR", n ? "/" : ""); }
  return n > 0 ? s : "?";
}

bool wifiChanAutoMode() { return autoMode; }
bool wifiChanScanning() { return chanScanning; }

struct WiFiScanHelper : public WiFiScanClass, public WiFiGenericClass {
  static void resetScanBits() {
    clearStatusBits(WIFI_SCANNING_BIT | WIFI_SCAN_DONE_BIT);
  }
  static void setScanTimeout(uint32_t ms) {
    _scanTimeout = ms;
  }
};

static void stopScanInternal() {
  if (WiFiGenericClass::getStatusBits() & WIFI_SCANNING_BIT) {
    esp_wifi_scan_stop();
    delay(30);
  }
  WiFiScanHelper::resetScanBits();
  WiFi.scanDelete();
  chanScanning = false;
}

void wifiChanExit() {
  stopScanInternal();
  WiFi.setAutoReconnect(true);   // wifiChanScan 关掉的；进来时已连着网的话下面不会走 bootWifiStart
  chanCount = 0; chanSel = 0;
  haveSelectedBssid = false;
  autoMode = false;
  emptyScanRetries = 0;
  wifiChanView = WIFI_CHAN_VIEW_SPECTRUM;
  WiFi.setAutoReconnect(true);
  if (hotspotRestoreAfterScan()) return;
  if (WiFi.status() != WL_CONNECTED) {
    bootWifiStart();
  }
}

void wifiChanScan() {
  if (chanScanning) return;

  esp_wifi_set_promiscuous(false);

  // 1. 关闭 STA 自动重连，避免后台重连任务在扫描期间抢占射频或打乱底层状态机
  WiFi.setAutoReconnect(false);

  // 2. 若当前未连上网，断开未完成的重连；绝不能调 WiFi.mode(WIFI_OFF)，否则反复关开射频引发竞态与卡顿
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
  }

  // 3. 确保切换到目标扫描模式（热点开着时 AP+STA 共存）
  const wifi_mode_t scanMode = hotspotScanMode();
  if (WiFi.getMode() != scanMode) {
    WiFi.mode(scanMode);
    uint32_t t0 = millis();
    while (WiFi.getMode() != scanMode && (millis() - t0 < 300)) delay(10);
    WiFiGenericClass::waitStatusBits(STA_STARTED_BIT, 300);
    delay(20);
  }

  // 4. 清理残留在途扫描及标志位，消除幽灵 WIFI_SCAN_RUNNING 假死
  stopScanInternal();

  // 5. 启动异步扫描：包含隐藏 SSID、Active 主动探测、每信道 120ms
  // 注意：Arduino-esp32 内部硬编码 _scanTimeout = max_ms_per_chan * 20 (若110ms则只有2200ms)，
  // 导致底层实际耗时稍超过2.2s时被 scanComplete() 内部误判为超时而返回 -2 (FAILED)！
  // 故必须手动将 _scanTimeout 放宽至 7000ms，彻底消除假超时误杀！
  int16_t res = WiFi.scanNetworks(true, true, false, 120);
  WiFiScanHelper::setScanTimeout(7000);
  Serial.printf("[wifichan] scanNetworks res=%d\n", res);

  if (res == WIFI_SCAN_FAILED) {
    // 第一次失败：极短延时重试一次
    delay(80);
    stopScanInternal();
    res = WiFi.scanNetworks(true, true, false, 120);
    WiFiScanHelper::setScanTimeout(7000);
    Serial.printf("[wifichan] retry res=%d\n", res);
  }

  if (res != WIFI_SCAN_FAILED) {
    chanScanning = true;
    scanStartMs = millis();
  } else {
    chanScanning = false;
    nextScanMs = millis() + 2000;
  }
}

static void parseScanResults(int n) {
  chanCount = 0;
  for (int i = 0; i < n && chanCount < CHAN_MAX; i++) {
    const wifi_ap_record_t* r = (const wifi_ap_record_t*)WiFi.getScanInfoByIndex(i);
    if (!r) continue;
    ChanAp& a = chanList[chanCount++];
    strncpy(a.ssid, (const char*)r->ssid, sizeof(a.ssid) - 1);
    a.ssid[sizeof(a.ssid) - 1] = '\0';
    a.channel = r->primary;
    a.rssi = r->rssi;
    a.auth = r->authmode;
    a.secure = (r->authmode != WIFI_AUTH_OPEN);
    a.cipher = r->pairwise_cipher;
    a.second = r->second;
    memcpy(a.bssid, r->bssid, 6);
    a.phyB = r->phy_11b; a.phyG = r->phy_11g; a.phyN = r->phy_11n; a.phyLR = r->phy_lr;
    a.wps = r->wps;
    a.cc[0] = r->country.cc[0]; a.cc[1] = r->country.cc[1]; a.cc[2] = 0;
  }
  WiFi.scanDelete();

  // 恢复之前选中的 BSSID，防止列表跳动
  if (haveSelectedBssid) {
    bool found = false;
    for (int i = 0; i < chanCount; i++) {
      if (memcmp(chanList[i].bssid, selectedBssid, 6) == 0) {
        chanSel = i;
        found = true;
        break;
      }
    }
    if (!found && chanSel >= chanCount) chanSel = (chanCount > 0) ? (chanCount - 1) : 0;
  } else {
    chanSel = 0;
  }
  if (chanCount > 0) {
    memcpy(selectedBssid, chanList[chanSel].bssid, 6);
    haveSelectedBssid = true;
  }
}

void wifiChanUpdate() {
  uint32_t now = millis();
  if (chanScanning) {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      if (now - scanStartMs > CHAN_SCAN_TIMEOUT_MS) {
        Serial.printf("[wifichan] scan timeout after %ums\n", (unsigned)(now - scanStartMs));
        stopScanInternal();
        nextScanMs = now + (chanCount == 0 ? 2500 : CHAN_SCAN_INTERVAL_MS);
        dirty = true;
      } else {
        static uint32_t lastSpinMs = 0;
        if (now - lastSpinMs >= 120) {
          lastSpinMs = now;
          dirty = true;
        }
      }
      return;
    }
    chanScanning = false;
    Serial.printf("[wifichan] scan finished n=%d in %ums\n", n, (unsigned)(now - scanStartMs));
    if (n > 0) {
      parseScanResults(n);
      emptyScanRetries = 0;
      nextScanMs = now + CHAN_SCAN_INTERVAL_MS;
    } else {
      stopScanInternal();
      if (emptyScanRetries < 2) emptyScanRetries++;
      nextScanMs = now + (chanCount == 0 ? 2500 : CHAN_SCAN_INTERVAL_MS);
    }
    dirty = true;
    return;
  }

  const bool shouldAutoScan = autoMode || (chanCount == 0 && emptyScanRetries < 2);
  if (shouldAutoScan) {
    static int lastRemainSec = -1;
    int remainSec = max(0, (int)((nextScanMs - now + 999) / 1000));
    if (remainSec != lastRemainSec) {
      lastRemainSec = remainSec;
      dirty = true;
    }
    if ((int32_t)(now - nextScanMs) >= 0) {
      wifiChanScan();
      dirty = true;
    }
  }
}

void wifiChanToggleAuto() {
  autoMode = !autoMode;
  if (autoMode) {
    nextScanMs = millis();
    if (!chanScanning) wifiChanScan();
  }
}

void wifiChanKey(char k) {
  if (k == 'r' || k == 'R') {
    emptyScanRetries = 0;
    if (chanScanning) {
      stopScanInternal();
    }
    wifiChanScan();
    dirty = true;
  } else if (k == 'a' || k == 'A') {
    wifiChanToggleAuto();
    dirty = true;
  } else if (k == ' ' || k == '\t' || k == 'v' || k == 'V' || k == 'm' || k == 'M') {
    wifiChanView = (wifiChanView + 1) % WIFI_CHAN_VIEW_COUNT;
    dirty = true;
  } else if (chanCount > 0 && (k == ';' || k == ',')) {
    chanSel = (chanSel - 1 + chanCount) % chanCount;
    dirty = true;
  } else if (chanCount > 0 && (k == '.' || k == '/')) {
    chanSel = (chanSel + 1) % chanCount;
    dirty = true;
  } else if (chanCount > 0 && k == '\n') {
    screen = SCREEN_WIFI_CHAN_DETAIL;
    dirty = true;
  }
}

// RSSI -> 曲线高度归一化（0..1）：-30dBm 顶格，-95dBm 及以下贴底
static inline float rssiToHeight(int rssi) {
  const int RSSI_MAX = -30, RSSI_MIN = -95;
  int r = constrain(rssi, RSSI_MIN, RSSI_MAX);
  return (float)(r - RSSI_MIN) / (RSSI_MAX - RSSI_MIN);
}

// 2.4GHz 全频段信道统计与评分结构体
struct ChanStats {
  int count[15];       // 1..14 信道 AP 数量
  int maxRssi[15];     // 1..14 信道最强信号
  int congestion[15];  // 1..14 拥挤度得分 (0..100)
  int penalty[15];     // 未截断的原始分；拥挤的环境里 1/6/11 常常一起顶到 100，选最优必须比这个
  int stars[15];       // 1..5 星级评级
  int bestCh;          // 推荐最优主信道 (1, 6, 11)
};

static void computeChanStats(ChanStats& s) {
  for (int c = 1; c <= 14; c++) {
    s.count[c] = 0;
    s.maxRssi[c] = -100;
  }
  for (int i = 0; i < chanCount; i++) {
    int c = chanList[i].channel;
    if (c >= 1 && c <= 14) {
      s.count[c]++;
      if (chanList[i].rssi > s.maxRssi[c]) s.maxRssi[c] = chanList[i].rssi;
    }
  }
  for (int c = 1; c <= 14; c++) {
    int penalty = 0;
    penalty += s.count[c] * 24;
    if (s.maxRssi[c] > -100) {
      penalty += max(0, s.maxRssi[c] + 90) / 2;
    }
    for (int d = 1; d <= 2; d++) {
      int weight = (d == 1) ? 12 : 6;
      if (c - d >= 1) penalty += s.count[c - d] * weight;
      if (c + d <= 14) penalty += s.count[c + d] * weight;
    }
    s.penalty[c] = penalty;
    s.congestion[c] = constrain(penalty, 0, 100);
    if (s.congestion[c] < 12) s.stars[c] = 5;
    else if (s.congestion[c] < 32) s.stars[c] = 4;
    else if (s.congestion[c] < 58) s.stars[c] = 3;
    else if (s.congestion[c] < 82) s.stars[c] = 2;
    else s.stars[c] = 1;
  }

  int best = 1;
  int minPenalty = s.penalty[1];
  if (s.penalty[6] < minPenalty) { minPenalty = s.penalty[6]; best = 6; }
  if (s.penalty[11] < minPenalty) { minPenalty = s.penalty[11]; best = 11; }
  s.bestCh = best;
}

// 连续、平滑且真实的 RF 频谱山丘曲线（带 OFDM 扁顶平滑滤波与 40MHz 区分）
static void drawApSpectrum(int chartX, int baseY, int chartH, int chW,
                           const ChanAp& a, uint16_t col, bool isSelected) {
  float h = rssiToHeight(a.rssi);
  int peakX = chartX + (a.channel - 1) * chW + chW / 2;
  int halfW = (int)(2.2f * chW); // 20MHz 标准半宽（~35px）
  int flatW = (a.phyB && !a.phyG && !a.phyN) ? 0 : (int)(halfW * 0.40f);

  // 40MHz 频宽与中心频点偏移适配
  if (a.second == WIFI_SECOND_CHAN_ABOVE) {
    peakX += chW * 2;
    halfW = (int)(4.2f * chW);
    flatW = (int)(halfW * 0.60f);
  } else if (a.second == WIFI_SECOND_CHAN_BELOW) {
    peakX -= chW * 2;
    halfW = (int)(4.2f * chW);
    flatW = (int)(halfW * 0.60f);
  }

  int x0 = max(chartX, peakX - halfW);
  int x1 = min(chartX + 14 * chW, peakX + halfW);

  auto calcY = [&](int x) -> int {
    int dx = abs(x - peakX);
    if (dx >= halfW) return baseY;
    float k = 0.0f;
    if (dx <= flatW) {
      k = 1.0f;
    } else {
      float t = (float)(dx - flatW) / (float)(halfW - flatW);
      k = 1.0f - t * t;
      if (k < 0.0f) k = 0.0f;
    }
    return baseY - (int)(k * h * chartH);
  };

  // 1. 若为选中 AP，绘制科技感垂直微网纹扫描填充
  if (isSelected) {
    uint16_t fillCol = dimColor(col);
    for (int x = x0; x <= x1; x += 2) {
      int y = calcY(x);
      if (y < baseY) {
        cv.drawFastVLine(x, y + 1, baseY - y - 1, fillCol);
      }
    }
  }

  // 2. 连续平滑轮廓线（逐像素连续绘制，消除折线锯齿）
  int prevX = x0, prevY = calcY(x0);
  for (int x = x0 + 1; x <= x1; x++) {
    int y = calcY(x);
    cv.drawLine(prevX, prevY, x, y, col);
    if (isSelected) {
      cv.drawLine(prevX, prevY - 1, x, y - 1, TFT_WHITE); // 选中项双像素高光外缘
    }
    prevX = x;
    prevY = y;
  }

  // 3. 选中项在波峰上方浮现 HUD 瞄准标靶与数据胶囊
  if (isSelected) {
    int peakY = calcY(peakX);
    int badgeY = max(33, peakY - 12);

    char badge[20];
    if (a.second != WIFI_SECOND_CHAN_NONE) {
      snprintf(badge, sizeof(badge), "Ch%d[40M] %d", a.channel, a.rssi);
    } else {
      snprintf(badge, sizeof(badge), "Ch%d %ddBm", a.channel, a.rssi);
    }
    int bw = strlen(badge) * 6 + 6;
    int bx = constrain(peakX - bw / 2, chartX + 2, chartX + 14 * chW - bw - 2);

    // 细线连接波峰与标签
    if (badgeY + 10 < peakY) {
      cv.drawFastVLine(peakX, badgeY + 10, peakY - (badgeY + 10), 0x07FF);
      cv.drawPixel(peakX, peakY, TFT_WHITE);
    }

    cv.fillRoundRect(bx, badgeY, bw, 10, 2, 0x0821);
    cv.drawRoundRect(bx, badgeY, bw, 10, 2, col);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(TFT_WHITE, 0x0821);
    cv.drawString(badge, bx + bw / 2, badgeY + 5);
  }
}

// 视图 0：RF 频谱山丘曲线 (RF Spectrum Analyzer)
static void drawWifiChanSpectrum(const ChanStats& stats) {
  const uint32_t now = millis();

  // 若尚未扫描到 AP，展示科技感待机卡片
  if (chanCount == 0 || !chanList) {
    const int bx = SW / 2 - 86, by = 46, bw = 172, bh = 32;
    cv.fillRoundRect(bx, by, bw, bh, 4, 0x0821);
    cv.drawRoundRect(bx, by, bw, bh, 4, 0x18C3);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    if (chanScanning) {
      cv.setTextColor(TFT_YELLOW, 0x0821);
      cv.drawString("SCANNING 2.4GHz SPECTRUM...", SW / 2, by + 10);
      cv.setTextColor(0x07FF, 0x0821);
      cv.drawString("Detecting 802.11 beacons", SW / 2, by + 22);
    } else {
      cv.setTextColor(TFT_WHITE, 0x0821);
      if (emptyScanRetries < 2) {
        cv.drawString("INITIALIZING SCAN...", SW / 2, by + 10);
        cv.setTextColor(0x8410, 0x0821);
        cv.drawString("Auto retrying / Press 'R'", SW / 2, by + 22);
      } else {
        cv.drawString("NO AP DETECTED", SW / 2, by + 10);
        cv.setTextColor(0x8410, 0x0821);
        cv.drawString("Press 'R' to scan / 'A' auto", SW / 2, by + 22);
      }
    }
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(bottom_left); cv.setTextSize(1);
    cv.drawString("R:SCAN  A:AUTO  SPC:MODE  `:MENU", 4, SH - 2);
    return;
  }

  if (chanSel >= chanCount) chanSel = chanCount - 1;
  ChanAp& sel = chanList[chanSel];
  memcpy(selectedBssid, sel.bssid, 6);
  haveSelectedBssid = true;

  // ---- 顶部选中 AP 赛博胶囊栏 (y = 15..28, 高 14) ----
  const int cx = 4, cy = 15, cw = SW - 8, ch = 14;
  cv.fillRoundRect(cx, cy, cw, ch, 3, 0x0821);
  cv.drawRoundRect(cx, cy, cw, ch, 3, 0x18C3);

  // 左侧：调色板色块 + AP 索引 + SSID
  cv.fillRect(cx + 4, cy + 2, 4, 10, apColor(chanSel));
  cv.setTextDatum(top_left); cv.setTextSize(1);
  char idxStr[16]; snprintf(idxStr, sizeof(idxStr), "#%02d", chanSel + 1);
  cv.setTextColor(0x632C, 0x0821);
  cv.drawString(idxStr, cx + 11, cy + 3);

  const char* ssidDisp = sel.ssid[0] ? sel.ssid : "(hidden)";
  cv.setTextColor(TFT_WHITE, 0x0821);
  cv.drawString(trunc(ssidDisp, 12), cx + 33, cy + 3);

  // 中间：安全认证胶囊徽章
  if (!sel.secure) {
    cv.fillRoundRect(cx + 108, cy + 2, 26, 10, 2, 0x3800);
    cv.setTextColor(0xF800, 0x3800);
    cv.setTextDatum(middle_center);
    cv.drawString("OPN", cx + 121, cy + 7);
  } else if (sel.auth == WIFI_AUTH_WPA3_PSK || sel.auth == WIFI_AUTH_WPA2_WPA3_PSK) {
    cv.fillRoundRect(cx + 108, cy + 2, 28, 10, 2, 0x2014);
    cv.setTextColor(0xF81F, 0x2014);
    cv.setTextDatum(middle_center);
    cv.drawString("WPA3", cx + 122, cy + 7);
  } else {
    cv.fillRoundRect(cx + 108, cy + 2, 28, 10, 2, 0x0188);
    cv.setTextColor(0x07FF, 0x0188);
    cv.setTextDatum(middle_center);
    cv.drawString("WPA2", cx + 122, cy + 7);
  }

  // 右侧：信道 + RSSI 信号徽章
  cv.setTextDatum(top_right);
  char rinfo[24];
  const char* w40 = (sel.second != WIFI_SECOND_CHAN_NONE) ? "[40M]" : "";
  snprintf(rinfo, sizeof(rinfo), "Ch%-2d%s", sel.channel, w40);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString(rinfo, cx + cw - 52, cy + 3);

  uint16_t rssiCol = (sel.rssi > -65) ? 0x07E0 : (sel.rssi > -75) ? 0xFFE0 : 0xFD20;
  char rssiStr[12]; snprintf(rssiStr, sizeof(rssiStr), "%ddBm", sel.rssi);
  cv.setTextColor(rssiCol, 0x0821);
  cv.drawString(rssiStr, cx + cw - 4, cy + 3);

  // ---- RF 频谱分析仪器面板 (y = 30..106, 高 77) ----
  const int CH_COUNT = 14;
  const int chartX = 8, chartY = 32, chartW = 224, chartH = 65;
  const int chW = chartW / CH_COUNT;   // 224 / 14 = 16 整除
  const int baseY = chartY + chartH;   // 97

  // 背景卡片：纯黑黑夜质感，配 0x18C3 边框，对比度拉满
  cv.fillRoundRect(4, 30, SW - 8, 77, 3, TFT_BLACK);
  cv.drawRoundRect(4, 30, SW - 8, 77, 3, 0x18C3);

  // dBm 参考基准横虚线 (-40, -60, -80 dBm)
  static const int DBM_MARKS[] = { -40, -60, -80 };
  for (int i = 0; i < 3; i++) {
    float h = rssiToHeight(DBM_MARKS[i]);
    int my = baseY - (int)(h * chartH);
    for (int dx = chartX; dx < chartX + chartW - 20; dx += 4) {
      cv.drawFastHLine(dx, my, 2, 0x10A2);
    }
    char dbmStr[6]; snprintf(dbmStr, sizeof(dbmStr), "%d", DBM_MARKS[i]);
    cv.setTextDatum(middle_right); cv.setTextSize(1);
    cv.setTextColor(0x4208, TFT_BLACK);
    cv.drawString(dbmStr, chartX + chartW - 2, my);
  }

  // 1/6/11 核心非重叠信道垂直导引柱
  static const struct { int ch; uint16_t col; } PRIME_GUIDES[] = {
    { 1, 0x0A24 }, { 6, 0x1824 }, { 11, 0x08A4 }
  };
  for (int p = 0; p < 3; p++) {
    int mx = chartX + (PRIME_GUIDES[p].ch - 1) * chW + chW / 2;
    cv.drawFastVLine(mx, chartY, chartH, PRIME_GUIDES[p].col);
  }

  // 扫描中的动态雷达扫频光柱
  if (chanScanning) {
    int sweepX = chartX + (int)((now / 8) % chartW);
    cv.drawFastVLine(sweepX, chartY, chartH, 0x07FF);
  }

  // 信道基准底线与刻度标注
  cv.drawFastHLine(chartX, baseY, chartW, 0x2965);
  for (int c = 1; c <= 14; c++) {
    int mx = chartX + (c - 1) * chW + chW / 2;
    if (c == 1 || c == 6 || c == 11) {
      cv.drawFastVLine(mx, baseY, 3, 0x07E0);
      char b[3]; snprintf(b, sizeof(b), "%d", c);
      cv.setTextDatum(top_center); cv.setTextSize(1);
      cv.setTextColor((c == 1) ? 0x07E0 : (c == 6) ? 0xFDA0 : 0x07FF, TFT_BLACK);
      cv.drawString(b, mx, baseY + 2);
    } else {
      cv.drawFastVLine(mx, baseY, 2, 0x2124);
      char b[3]; snprintf(b, sizeof(b), "%d", c);
      cv.setTextDatum(top_center); cv.setTextSize(1);
      cv.setTextColor(0x632C, TFT_BLACK);
      cv.drawString(b, mx, baseY + 3);
    }
  }

  // 1. 先画未选中的 AP 曲线（带专属暗色调，保留多 AP 频谱辨析力）
  for (int i = 0; i < chanCount; i++) {
    if (i == chanSel) continue;
    uint16_t c = dimColor(apColor(i));
    drawApSpectrum(chartX, baseY, chartH, chW, chanList[i], c, false);
  }

  // 2. 最后画选中的 AP 曲线（双线高光外缘 + 激光网纹扫描填充）
  drawApSpectrum(chartX, baseY, chartH, chW, sel, apColor(chanSel), true);

  // ---- 智能信道干扰推荐与互调统计底栏 (y = 109..122, 高 14) ----
  const int by = 109, bh = 14;
  cv.fillRoundRect(cx, by, cw, bh, 3, 0x0821);
  cv.drawRoundRect(cx, by, cw, bh, 3, 0x18C3);

  // 最优信道绿色徽章
  cv.fillRoundRect(cx + 3, by + 2, 60, 10, 2, 0x0320);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  char bestStr[16]; snprintf(bestStr, sizeof(bestStr), "BEST: CH %d", stats.bestCh);
  cv.setTextColor(0x07E0, 0x0320);
  cv.drawString(bestStr, cx + 33, by + 7);

  // 核心信道 AP 密度
  cv.setTextDatum(middle_left);
  int tx = cx + 68;
  auto drawChCount = [&](int ch, int cnt) {
    char cb[8]; snprintf(cb, sizeof(cb), "CH%d:", ch);
    cv.setTextColor(0x8410, 0x0821);
    cv.drawString(cb, tx, by + 7);
    tx += strlen(cb) * 6 + 1;
    char nb[6]; snprintf(nb, sizeof(nb), "%d", cnt);
    uint16_t nc = (cnt == 0) ? 0x07E0 : (cnt <= 2) ? 0xFDA0 : 0xF800;
    cv.setTextColor(nc, 0x0821);
    cv.drawString(nb, tx, by + 7);
    tx += strlen(nb) * 6 + 5;
  };
  drawChCount(1, stats.count[1]);
  drawChCount(6, stats.count[6]);
  drawChCount(11, stats.count[11]);

  // 视图标识
  cv.setTextDatum(middle_right);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("[1:SPEC]", cx + cw - 4, by + 7);

  // ---- 底部按键指引 (y = 125..134) ----
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.drawString(";.:AP SPC:MODE R:SCAN A:AUTO RET:INFO", 4, SH - 2);
}

// 视图 1：信道拥挤度与星级评级矩阵 (Channel Rating & Congestion)
static void drawWifiChanRating(const ChanStats& stats) {
  const int cx = 4, cw = SW - 8;

  // ---- 顶部副标题条 (y = 15..26, 高 12) ----
  cv.fillRoundRect(cx, 15, cw, 12, 2, 0x0821);
  cv.drawRoundRect(cx, 15, cw, 12, 2, 0x18C3);
  cv.setTextDatum(middle_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("CH CONGESTION", cx + 5, 21);

  cv.setTextDatum(middle_right);
  char recBuf[24]; snprintf(recBuf, sizeof(recBuf), "BEST: CH %d", stats.bestCh);
  cv.setTextColor(0x07E0, 0x0821);
  cv.drawString(recBuf, cx + cw - 5, 21);

  // ---- 三大核心非重叠信道卡片 (y = 29..66, 高 38) ----
  static const struct { int ch; const char* freq; } PRIMES[] = {
    { 1, "2412M" }, { 6, "2437M" }, { 11, "2462M" }
  };
  const int cardW = 74, cardH = 38;
  for (int p = 0; p < 3; p++) {
    int px = cx + p * (cardW + 5);
    int ch = PRIMES[p].ch;
    bool isBest = (ch == stats.bestCh);

    cv.fillRoundRect(px, 29, cardW, cardH, 3, 0x0821);
    cv.drawRoundRect(px, 29, cardW, cardH, 3, isBest ? 0x07E0 : 0x18C3);

    // 行 1：信道名 + 频率
    cv.setTextDatum(top_left); cv.setTextSize(1);
    char chHeader[12]; snprintf(chHeader, sizeof(chHeader), "CH %d", ch);
    cv.setTextColor(isBest ? 0x07E0 : TFT_WHITE, 0x0821);
    cv.drawString(chHeader, px + 4, 32);

    cv.setTextDatum(top_right);
    cv.setTextColor(0x8410, 0x0821);
    cv.drawString(PRIMES[p].freq, px + cardW - 4, 32);

    // 行 2：星级指示
    int stars = stats.stars[ch];
    uint16_t starCol = (stars >= 4) ? 0x07E0 : (stars == 3) ? 0xFDA0 : 0xF800;
    cv.setTextDatum(top_left);
    char starBuf[8];
    for (int s = 0; s < 5; s++) starBuf[s] = (s < stars) ? '*' : '-';
    starBuf[5] = 0;
    cv.setTextColor(starCol, 0x0821);
    cv.drawString(starBuf, px + 4, 43);

    if (isBest) {
      cv.fillRoundRect(px + cardW - 28, 42, 24, 9, 2, 0x0320);
      cv.setTextDatum(middle_center);
      cv.setTextColor(0x07E0, 0x0320);
      cv.drawString("BEST", px + cardW - 16, 46);
    }

    // 行 3：AP 数量 + 状态标签
    cv.setTextDatum(top_left);
    char apBuf[12]; snprintf(apBuf, sizeof(apBuf), "%d APs", stats.count[ch]);
    cv.setTextColor(TFT_WHITE, 0x0821);
    cv.drawString(apBuf, px + 4, 54);

    const char* tag = (stars >= 5) ? "CLEAN" : (stars >= 3) ? "FAIR" : "BUSY";
    cv.setTextDatum(top_right);
    cv.setTextColor(starCol, 0x0821);
    cv.drawString(tag, px + cardW - 4, 54);
  }

  // ---- 14 信道全频段拥挤度直方图 (y = 69..122, 高 54) ----
  const int hy = 69, hh = 54;
  cv.fillRoundRect(cx, hy, cw, hh, 3, 0x0821);
  cv.drawRoundRect(cx, hy, cw, hh, 3, 0x18C3);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("SPECTRUM DENSITY (1-14)", cx + 6, hy + 3);

  cv.setTextDatum(top_right);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("[2:RATE]", cx + cw - 5, hy + 3);

  // 14 列直方图
  const int baseY = hy + hh - 12; // y = 111
  const int chW = 16;
  for (int c = 1; c <= 14; c++) {
    int colX = cx + 4 + (c - 1) * chW + 2;

    // 柱顶 AP 计数
    cv.setTextDatum(bottom_center); cv.setTextSize(1);
    if (stats.count[c] > 0) {
      char cntStr[16]; snprintf(cntStr, sizeof(cntStr), "%d", stats.count[c]);
      cv.setTextColor(TFT_WHITE, 0x0821);
      cv.drawString(cntStr, colX + 5, hy + 21);
    } else {
      cv.setTextColor(0x3186, 0x0821);
      cv.drawString("-", colX + 5, hy + 21);
    }

    // 分段电平柱（最多 7 段，每段高 2px，间隔 1px）
    int segs = stats.congestion[c] * 7 / 100;
    if (stats.count[c] > 0 && segs == 0) segs = 1;

    for (int s = 0; s < 7; s++) {
      int sy = baseY - 2 - s * 3;
      uint16_t segCol = (s < 3) ? 0x07E0 : (s < 5) ? 0xFDA0 : 0xF800;
      if (s < segs) {
        cv.fillRect(colX, sy, 10, 2, segCol);
      } else {
        cv.fillRect(colX, sy, 10, 1, 0x1082); // 微弱虚线点阵背景
      }
    }

    // 底部信道编号
    cv.setTextDatum(top_center); cv.setTextSize(1);
    char numStr[4]; snprintf(numStr, sizeof(numStr), "%d", c);
    uint16_t numCol = (c == 1 || c == 6 || c == 11) ? 0x07E0 : 0x632C;
    cv.setTextColor(numCol, 0x0821);
    cv.drawString(numStr, colX + 5, baseY + 2);
  }

  // ---- 底部按键指引 (y = 125..134) ----
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.drawString("SPC:MODE R:SCAN A:AUTO RET:INFO `:MENU", 4, SH - 2);
}

// 视图 2：AP 热点情报清单 (AP Feed Table)
static void drawWifiChanList(const ChanStats& stats) {
  const int cx = 4, cw = SW - 8;

  // ---- 顶部副标题条 (y = 15..26, 高 12) ----
  cv.fillRoundRect(cx, 15, cw, 12, 2, 0x0821);
  cv.drawRoundRect(cx, 15, cw, 12, 2, 0x18C3);
  cv.setTextDatum(middle_left); cv.setTextSize(1);
  char titleBuf[32]; snprintf(titleBuf, sizeof(titleBuf), "AP FEED (%d DETECTED)", chanCount);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString(titleBuf, cx + 5, 21);

  cv.setTextDatum(middle_right);
  char pageBuf[32]; snprintf(pageBuf, sizeof(pageBuf), "SEL %d/%d", chanSel + 1, max(1, chanCount));
  cv.setTextColor(TFT_WHITE, 0x0821);
  cv.drawString(pageBuf, cx + cw - 5, 21);

  // ---- 数据表格卡片 (y = 28..122, 高 95) ----
  const int ty = 28, th = 95;
  cv.fillRoundRect(cx, ty, cw, th, 3, 0x0821);
  cv.drawRoundRect(cx, ty, cw, th, 3, 0x18C3);

  // 表头
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x8410, 0x0821);
  cv.drawString("#", cx + 7, ty + 3);
  cv.drawString("CH", cx + 24, ty + 3);
  cv.drawString("SSID", cx + 46, ty + 3);
  cv.drawString("RSSI", cx + 100, ty + 3);
  cv.drawString("SEC", cx + 154, ty + 3);
  cv.drawString("VENDOR", cx + 184, ty + 3);
  cv.drawFastHLine(cx + 4, ty + 12, cw - 8, 0x18C3);

  // 4 个可见数据行
  int topRow = (chanSel / 4) * 4;
  for (int r = 0; r < 4; r++) {
    int idx = topRow + r;
    int rowY = ty + 15 + r * 17;
    if (idx >= chanCount) break;

    const ChanAp& a = chanList[idx];
    bool isSelected = (idx == chanSel);

    if (isSelected) {
      cv.fillRoundRect(cx + 3, rowY - 2, cw - 6, 16, 2, 0x1082);
      cv.drawRoundRect(cx + 3, rowY - 2, cw - 6, 16, 2, 0x07FF);
    }

    // 列 1：调色板微色块 + 序号
    cv.fillRect(cx + 5, rowY + 1, 3, 9, apColor(idx));
    char ib[4]; snprintf(ib, sizeof(ib), "%02d", idx + 1);
    cv.setTextColor(isSelected ? TFT_WHITE : 0x632C, isSelected ? 0x1082 : 0x0821);
    cv.drawString(ib, cx + 9, rowY + 1);

    // 列 2：信道
    char cb[6]; snprintf(cb, sizeof(cb), "c%-2d", a.channel);
    cv.setTextColor(0x07FF, isSelected ? 0x1082 : 0x0821);
    cv.drawString(cb, cx + 24, rowY + 1);

    // 列 3：SSID
    const char* sname = a.ssid[0] ? a.ssid : "(hidden)";
    cv.setTextColor(isSelected ? TFT_WHITE : TFT_LIGHTGREY, isSelected ? 0x1082 : 0x0821);
    cv.drawString(trunc(sname, 8), cx + 46, rowY + 1);

    // 列 4：RSSI + 迷你信号条
    uint16_t rc = (a.rssi > -65) ? 0x07E0 : (a.rssi > -75) ? 0xFFE0 : 0xFD20;
    char rb[8]; snprintf(rb, sizeof(rb), "%d", a.rssi);
    cv.setTextColor(rc, isSelected ? 0x1082 : 0x0821);
    cv.drawString(rb, cx + 100, rowY + 1);
    drawSignal(cx + 132, rowY + 7, a.rssi, rc);

    // 列 5：安全类型
    const char* secTag = !a.secure ? "OPN" : (a.auth == WIFI_AUTH_WPA3_PSK) ? "W3" : "W2";
    uint16_t sc = !a.secure ? 0xF800 : (a.auth == WIFI_AUTH_WPA3_PSK) ? 0xF81F : 0x07FF;
    cv.setTextColor(sc, isSelected ? 0x1082 : 0x0821);
    cv.drawString(secTag, cx + 154, rowY + 1);

    // 列 6：厂商反查
    const char* vendor = apVendor(a.bssid);
    cv.setTextColor(0xFDA0, isSelected ? 0x1082 : 0x0821);
    cv.drawString(trunc(vendor, 6), cx + 184, rowY + 1);
  }

  // 滚动与底部指示
  cv.setTextDatum(middle_left); cv.setTextSize(1);
  cv.setTextColor(0x632C, 0x0821);
  cv.drawString("[; .] Scroll APs", cx + 7, ty + th - 6);

  cv.setTextDatum(middle_right);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("[3:LIST]", cx + cw - 5, ty + th - 6);

  // ---- 底部按键指引 (y = 125..134) ----
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.drawString(";.:SEL SPC:MODE R:SCAN RET:DETAIL", 4, SH - 2);
}

// 主绘图入口：依据当前视图模式绘制
void drawWifiChan() {
  cv.fillScreen(TFT_BLACK);
  const uint32_t now = millis();

  // 顶栏状态指示
  char stBuf[16];
  uint16_t stCol = 0;
  if (chanScanning) {
    static const char* SPINS[] = { "SCAN-", "SCAN\\", "SCAN|", "SCAN/" };
    snprintf(stBuf, sizeof(stBuf), "%s", SPINS[(now / 120) % 4]);
    stCol = TFT_YELLOW;
  } else if (autoMode) {
    int remainSec = max(0, (int)((nextScanMs - now + 999) / 1000));
    snprintf(stBuf, sizeof(stBuf), "AUTO %ds", remainSec);
    stCol = ACCENT;
  } else if (chanCount > 0) {
    snprintf(stBuf, sizeof(stBuf), "%d APs", chanCount);
    stCol = 0x07E0;
  } else if (emptyScanRetries < 2) {
    snprintf(stBuf, sizeof(stBuf), "SCAN...");
    stCol = TFT_YELLOW;
  } else {
    snprintf(stBuf, sizeof(stBuf), "READY");
    stCol = ICON_DIM;
  }
  drawPageHeader("Wi-Fi Chan", stBuf, stCol);

  ChanStats stats;
  computeChanStats(stats);

  switch (wifiChanView) {
    case WIFI_CHAN_VIEW_RATING:
      drawWifiChanRating(stats);
      break;
    case WIFI_CHAN_VIEW_LIST:
      drawWifiChanList(stats);
      break;
    case WIFI_CHAN_VIEW_SPECTRUM:
    default:
      drawWifiChanSpectrum(stats);
      break;
  }
}

// 选中 AP 的完整详情页 (AP Detail HUD)
void drawWifiChanDetail() {
  cv.fillScreen(TFT_BLACK);

  if (chanCount == 0 || !chanList) {
    drawPageHeader("AP Detail");
    cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString("No networks", SW / 2, SH / 2);
    return;
  }
  if (chanSel >= chanCount) chanSel = chanCount - 1;
  ChanAp& a = chanList[chanSel];

  char idxBuf[16];
  snprintf(idxBuf, sizeof(idxBuf), "%d/%d", chanSel + 1, chanCount);
  drawPageHeader("AP Detail", idxBuf, apColor(chanSel));

  const int cx = 4, cw = SW - 8;

  // ---- 顶部身份卡片 (y = 14..45, 高 31) ----
  const int tpy = 14, tph = 31;
  cv.fillRoundRect(cx, tpy, cw, tph, 3, 0x0821);
  cv.drawRoundRect(cx, tpy, cw, tph, 3, 0x18C3);

  // 第一行：颜色块 + SSID + 加密徽章
  cv.fillRect(cx + 6, tpy + 4, 4, 10, apColor(chanSel));
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(TFT_WHITE, 0x0821);
  cv.drawString(trunc(a.ssid[0] ? a.ssid : "(Hidden Network)", 18), cx + 14, tpy + 4);

  cv.setTextDatum(top_right);
  if (!a.secure) {
    cv.setTextColor(0xF800, 0x0821);
    cv.drawString("[OPEN NETWORK]", cx + cw - 6, tpy + 4);
  } else if (a.auth == WIFI_AUTH_WPA3_PSK || a.auth == WIFI_AUTH_WPA2_WPA3_PSK) {
    cv.setTextColor(0xF81F, 0x0821);
    cv.drawString("[WPA3-SAE]", cx + cw - 6, tpy + 4);
  } else {
    cv.setTextColor(0x07FF, 0x0821);
    cv.drawString("[WPA2-PSK]", cx + cw - 6, tpy + 4);
  }

  // 第二行：MAC 地址 + 厂商反查标签
  char macStr[20];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           a.bssid[0], a.bssid[1], a.bssid[2], a.bssid[3], a.bssid[4], a.bssid[5]);
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString(macStr, cx + 6, tpy + 17);

  const char* vendor = apVendor(a.bssid);
  char vtag[20]; snprintf(vtag, sizeof(vtag), "[%s]", vendor);
  cv.setTextDatum(top_right); cv.setTextSize(1);
  cv.setTextColor(0xFDA0, 0x0821);
  cv.drawString(vtag, cx + cw - 6, tpy + 17);

  // ---- 中部信道频点 mini 态势条 (y = 47..61, 高 15) ----
  const int mpy = 46, mph = 16;
  cv.fillRoundRect(cx, mpy, cw, mph, 2, 0x0821);
  cv.drawRoundRect(cx, mpy, cw, mph, 2, 0x18C3);

  // 14 信道频段标尺
  const int trackX = cx + 8, trackW = cw - 16;
  const int chStep = trackW / 14;
  cv.drawFastHLine(trackX, mpy + 12, trackW, 0x2124);

  // 刻度线
  for (int c = 1; c <= 14; c++) {
    int mx = trackX + (c - 1) * chStep + chStep / 2;
    cv.drawFastVLine(mx, mpy + 11, 2, (c == 1 || c == 6 || c == 11) ? 0x07E0 : 0x3186);
  }

  // 当前信道覆盖范围高亮带
  int targetX = trackX + (a.channel - 1) * chStep + chStep / 2;
  int spanW = (a.second != WIFI_SECOND_CHAN_NONE) ? (chStep * 4) : (chStep * 2);
  int x0 = max(trackX, targetX - spanW / 2);
  int x1 = min(trackX + trackW, targetX + spanW / 2);
  cv.fillRect(x0, mpy + 10, x1 - x0, 3, apColor(chanSel));
  cv.drawFastVLine(targetX, mpy + 8, 5, TFT_WHITE);

  char freqBadge[40];
  const char* bwStr = (a.second == WIFI_SECOND_CHAN_ABOVE) ? "40M [+Above]"
                    : (a.second == WIFI_SECOND_CHAN_BELOW) ? "40M [-Below]" : "20MHz Span";
  snprintf(freqBadge, sizeof(freqBadge), "Ch %d (%d MHz) - %s", a.channel, chanFreq(a.channel), bwStr);
  cv.setTextDatum(top_center); cv.setTextSize(1);
  cv.setTextColor(TFT_WHITE, 0x0821);
  cv.drawString(freqBadge, SW / 2, mpy + 2);

  // ---- 下部双联卡片 (y = 64..122, 高 58) ----
  const int c1w = 114, c2w = 114;
  const int bpy = 64, bph = 58;

  // 卡片 1 (左)：无线射频参数 (RADIO SPECS)
  cv.fillRoundRect(cx, bpy, c1w, bph, 3, 0x0821);
  cv.drawRoundRect(cx, bpy, c1w, bph, 3, 0x18C3);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("RADIO SPECS", cx + 5, bpy + 3);
  cv.drawFastHLine(cx + 4, bpy + 13, c1w - 8, 0x1082);

  int y1 = bpy + 15; const int lh = 8;
  char b[32];
  snprintf(b, sizeof(b), "Ch %d (%dM)", a.channel, chanFreq(a.channel));
  cv.setTextColor(TFT_WHITE, 0x0821); cv.drawString(b, cx + 5, y1); y1 += lh;

  const char* bw = (a.second != WIFI_SECOND_CHAN_NONE) ? "Band: 40 MHz" : "Band: 20 MHz";
  cv.setTextColor(0xFDA0, 0x0821); cv.drawString(bw, cx + 5, y1); y1 += lh;

  snprintf(b, sizeof(b), "PHY: 802.11%s", phyStr(a));
  cv.setTextColor(TFT_LIGHTGREY, 0x0821); cv.drawString(b, cx + 5, y1); y1 += lh;

  const char* gen = a.phyN ? "Gen: Wi-Fi 4 (11n)" : a.phyG ? "Gen: Wi-Fi 3 (11g)" : "Gen: Legacy 11b";
  uint16_t genCol = a.phyN ? 0x07E0 : a.phyG ? 0x07FF : 0x8410;
  cv.setTextColor(genCol, 0x0821); cv.drawString(gen, cx + 5, y1); y1 += lh;

  bool ccOk = (a.cc[0] >= 'A' && a.cc[0] <= 'Z');
  snprintf(b, sizeof(b), "Reg: %s", ccOk ? a.cc : "World Domain");
  cv.setTextColor(0x8410, 0x0821); cv.drawString(b, cx + 5, y1);

  // 卡片 2 (右)：信号与安全加密 (SIGNAL & SEC)
  const int c2x = 122;
  cv.fillRoundRect(c2x, bpy, c2w, bph, 3, 0x0821);
  cv.drawRoundRect(c2x, bpy, c2w, bph, 3, 0x18C3);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x07E0, 0x0821);
  cv.drawString("SIGNAL & SEC", c2x + 5, bpy + 4);
  cv.drawFastHLine(c2x + 4, bpy + 14, c2w - 8, 0x1082);

  int y2 = bpy + 17;
  int quality = constrain(2 * (a.rssi + 100), 0, 100);
  snprintf(b, sizeof(b), "%ddBm (%d%%)", a.rssi, quality);
  uint16_t rssiCol = (a.rssi > -65) ? 0x07E0 : (a.rssi > -78) ? TFT_YELLOW : 0xFD20;
  cv.setTextColor(rssiCol, 0x0821);
  cv.drawString(b, c2x + 5, y2);
  drawSignal(c2x + c2w - 20, y2 + 6, a.rssi, rssiCol);
  y2 += lh;

  cv.setTextColor(TFT_LIGHTGREY, 0x0821);
  snprintf(b, sizeof(b), "%s", authName(a.auth));
  cv.drawString(b, c2x + 5, y2); y2 += lh;

  snprintf(b, sizeof(b), "%s", cipherName(a.cipher));
  cv.setTextColor(0x8410, 0x0821);
  cv.drawString(b, c2x + 5, y2); y2 += lh;

  snprintf(b, sizeof(b), "WPS: %s", a.wps ? "Supported" : "Disabled");
  cv.setTextColor(a.wps ? 0xFDA0 : 0x632C, 0x0821);
  cv.drawString(b, c2x + 5, y2);

  // ---- 底部按键提示 (y = 125..134) ----
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.drawString("[; .] PREV/NEXT AP   [`] BACK", 4, SH - 2);
}
