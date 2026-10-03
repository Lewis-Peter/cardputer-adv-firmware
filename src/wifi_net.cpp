#include "wifi_net.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <ctime>
#include "icons.h"
#include "ui_common.h"
#include "config.h"
#include "hotspot.h"
#include "ram_profile.h"
#include "gbk_table.h"
#include "geoloc.h"
#include "keyboard_adv.h"

static const char* NTP_1 = CFG_NTP_1;
static const char* NTP_2 = CFG_NTP_2;
static const char* NTP_3 = CFG_NTP_3;
static const uint32_t NTP_RESYNC_MS = 6UL * 60UL * 60UL * 1000UL;   // 6 hours
static uint32_t lastNtpSyncMs = 0;
static uint32_t lastNtpAttemptMs = 0;

// 连接握手日志用共用的终端日志组件（termLog*，见 ui_common）
static const char* authShort(int m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "open"; case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA"; case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2"; case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3"; default: return "?";
  }
}
// WiFi 事件回调（在 WiFi 任务上下文触发）：只写简单标量/定长缓冲，主循环再读出来拼日志，避开竞态
static volatile bool evStart, evAssoc, evGotIp, evDisc;
static volatile int  evChan, evAuth, evReason;
static char evIp[20];
static void onWifiEv(WiFiEvent_t e, WiFiEventInfo_t info) {
  switch (e) {
    case ARDUINO_EVENT_WIFI_STA_START: evStart = true; break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      evChan = info.wifi_sta_connected.channel; evAuth = info.wifi_sta_connected.authmode; evAssoc = true; break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      snprintf(evIp, sizeof(evIp), "%s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str()); evGotIp = true; break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      evReason = info.wifi_sta_disconnected.reason; evDisc = true; break;
    default: break;
  }
}

bool connectWith(const char* ssid, const char* pass, bool needNtp) {
  static bool evReg = false;
  if (!evReg) { WiFi.onEvent(onWifiEv); evReg = true; }
  evStart = evAssoc = evGotIp = evDisc = false; evIp[0] = 0;

  termLogReset();
  char b[40];
  snprintf(b, sizeof(b), "wifi> %s", ssid); termLogLine(b);
  termLogDraw(true);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  bool pStart = false, pAssoc = false, pIp = false, pDisc = false, connected = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 15000) {
    if (evStart && !pStart) { termLogLine(" . start"); pStart = true; }
    if (evAssoc && !pAssoc) { snprintf(b, sizeof(b), " . assoc ch%d %s", evChan, authShort(evAuth)); termLogLine(b); pAssoc = true; }
    if (evGotIp && !pIp)    { snprintf(b, sizeof(b), " . ip %s", evIp); termLogLine(b); pIp = true; }
    if (evDisc  && !pDisc)  { snprintf(b, sizeof(b), " . disc (reason %d)", evReason); termLogLine(b); pDisc = true; }
    if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
    termLogDraw(true);
    delay(30);
  }
  if (!connected) { termLogLine(" . timeout"); termLogDraw(false); delay(800); kbd::flushEvents(); return false; }
  snprintf(b, sizeof(b), " . rssi %ddBm", (int)WiFi.RSSI()); termLogLine(b);

  if (needNtp && !timeSynced) {
    termLogLine("ntp> sync..."); termLogDraw(true);
    configTzTime(TZ_INFO, NTP_1, NTP_2, NTP_3);
    struct tm ti; uint32_t s2 = millis();
    while (millis() - s2 < 8000) {
      if (getLocalTime(&ti, 0) && ti.tm_year + 1900 > 2020) {
        timeSynced = true; lastNtpSyncMs = millis(); lastNtpAttemptMs = lastNtpSyncMs; break;
      }
      termLogDraw(true); delay(60);
    }
    if (timeSynced) { strftime(b, sizeof(b), " . %Y-%m-%d %H:%M:%S", &ti); termLogLine(b); }
    else              termLogLine(" . ntp timeout");
  }
  termLogLine("ok"); termLogDraw(false); delay(600);
  geoInvalidate(); // 连接新网络后失效旧的定位缓存
  kbd::flushEvents();  // 连接期间（最长 15s）积压的按键全部丢弃
  return true;
}
// 开机后台连网+对时：连上就对时，区别于 connectWith() 的是拆成了非阻塞状态机，每帧推进
// 一步，不霸占 setup()，开机动画播完能立刻进菜单，WiFi/NTP 在后台悄悄跑。
// （早期版本在这儿"对完/超时就断开关 WiFi"，现在不关了——理由见下面那段策略说明。）
enum BootWifiState { BW_IDLE, BW_CONNECTING, BW_NTP, BW_DONE };
static BootWifiState bwState = BW_IDLE;
static uint32_t bwT0 = 0;

// ⚠️ 策略变更：Wi-Fi 现在是**常开 + 自动重连**，开机对完时不再关掉 radio。
// 以前关掉是为了省电、也为了躲开一个坑：记住的网不在范围内时，协议栈会每 ~2.4s
// 重试一次（NO_AP_FOUND），期间 WiFi.scanNetworks() 会因为 STA 正忙而直接失败。
// 那个坑现在由 doScan() 自己扛（扫描前先把 STA 复位一次），所以可以放心常开——
// 好处是天气/飞机/瓦片/Router 这些联网页面进去就能用，不用每次现连十几秒。
// 代价是 Wi-Fi 协议栈常驻吃掉 40~50KB 堆，这块板子没 PSRAM，所以 GNSS 地图那 48KB
// 底图缓存有可能申请不到（它自己有降级路径：先退 8bpp，再退 no zoom: low mem）。
// esp-lwip 给每个 socket 槽位配一把锁，第一次用到这个槽位时才建、永不释放
// （sockets.c alloc_socket()："one time init and never free"，每把 96B）。懒建的后果：
// LAN Scan 每个 IP 一个 ping 会话、前后会话的任务有重叠，一口气摸到七八个新槽位，
// 这些锁就落在当时刚分配的 ping 任务栈后面；栈回收后锁钉在原地，把最大连续块
// 从 ~60K 切成 ~28K（HEAPDUMP 实测：原大空闲块里每隔 ~4.4KB 一把锁）。
// 开机协议栈刚起、堆还完整时把 16 个槽位一次摸遍，锁就集中建在一起，以后谁用 socket 都不再新建。
static void lwipSockLockWarmup() {
  int fds[CONFIG_LWIP_MAX_SOCKETS];
  int n = 0;
  for (; n < CONFIG_LWIP_MAX_SOCKETS; n++) {
    fds[n] = socket(AF_INET, SOCK_DGRAM, 0);
    if (fds[n] < 0) break;
  }
  for (int i = 0; i < n; i++) close(fds[i]);
}

void bootWifiStart() {
  String s, p;
  if (!loadCreds(s, p)) { bwState = BW_DONE; return; }
  WiFi.mode(WIFI_STA);
  lwipSockLockWarmup();
  WiFi.setAutoReconnect(true);   // 掉线由协议栈自己重连，不用应用层轮询
  WiFi.setSleep(true);           // modem sleep：常开之后这个必须留着，不然待机电流很难看
  WiFi.begin(s.c_str(), p.c_str());
  bwState = BW_CONNECTING;
  bwT0 = millis();
  Serial.println("[bootwifi] connecting in background...");
}

void bootWifiUpdate() {
  switch (bwState) {
    case BW_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[bootwifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
        // setup() 里的 "wifi start" 只是踢了一脚，协议栈那一大坨是异步吃掉的：
        // 这一枪才是 WiFi 的真实账单。放在 CONNECTING->NTP 的那一次跳变里，
        // 所以掉线重连**不会**重复打点（bwState 已经不是 BW_CONNECTING 了）。
        ramMark("wifi up");
        configTzTime(TZ_INFO, NTP_1, NTP_2, NTP_3);
        bwState = BW_NTP;
        bwT0 = millis();
      } else if (millis() - bwT0 > 15000) {
        // 超时也不关radio：保持 STA 开着，让协议栈继续在后台重连
        // （比如出门时记住的网不在范围内，回到家自动就连上了）
        Serial.println("[bootwifi] connect timeout, leaving radio on to retry");
        bwState = BW_DONE;
      }
      break;
    case BW_NTP: {
      struct tm ti;
      if (getLocalTime(&ti, 0) && ti.tm_year + 1900 > 2020) {
        timeSynced = true;
        lastNtpSyncMs = millis();
        lastNtpAttemptMs = lastNtpSyncMs;
        Serial.println("[bootwifi] ntp synced");
        bwState = BW_DONE;
      } else if (millis() - bwT0 > 8000) {
        Serial.println("[bootwifi] ntp timeout");
        lastNtpAttemptMs = millis();
        bwState = BW_DONE;
      }
      break;
    }
    case BW_DONE:
      if (WiFi.status() == WL_CONNECTED &&
          millis() - lastNtpSyncMs >= NTP_RESYNC_MS &&
          millis() - lastNtpAttemptMs >= NTP_RESYNC_MS) {
        Serial.println("[bootwifi] periodic ntp sync");
        configTzTime(TZ_INFO, NTP_1, NTP_2, NTP_3);
        bwState = BW_NTP;
        bwT0 = millis();
        lastNtpAttemptMs = bwT0;
      }
      break;
    default: break;   // BW_IDLE：什么都不用做
  }
}

// 记住 / 读取上次连接成功的网络（存 NVS）
void saveCreds(const String& ssid, const String& pass) {
  saveString("wifi", "ssid", ssid);
  saveString("wifi", "pass", pass);
}
bool loadCreds(String& ssid, String& pass) {
  ssid = loadString("wifi", "ssid", "");
  pass = loadString("wifi", "pass", "");
  return ssid.length() > 0;
}

int scanCount = 0;
uint32_t lastWifiScanMs = 0;
int wifiIdx = 0;
int wifiTop = 0;
String selSSID;
String pwInput;
const int WIFI_VIS = 4;

void doScan() {
  centerMsg("scanning wifi...", TFT_YELLOW);

  // 1. 热点开着就用 AP+STA 共存模式扫描（ESP32 支持），不关热点，连着的设备不会掉线
  const wifi_mode_t scanMode = hotspotScanMode();

  // 2. 没连着网就先把 STA 彻底复位，清掉可能卡在后台重连的状态（否则 scanNetworks 会直接失败）。
  //    热点开着时不能 WIFI_OFF（会把 AP 一起关掉），只断开 STA。
  if (WiFi.status() != WL_CONNECTED) {
    if (scanMode == WIFI_AP_STA) {
      WiFi.disconnect();
    } else {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
    delay(60);
  }

  // 3. 切到扫描模式，并等待底层协议栈就绪
  WiFi.mode(scanMode);
  uint32_t t0 = millis();
  while (WiFi.getMode() != scanMode && (millis() - t0 < 300)) {
    delay(10);
  }
  WiFiGenericClass::waitStatusBits(STA_STARTED_BIT, 300);
  delay(50);

  // 4. 清除可能残留的在途扫描，再发起扫描
  if (WiFiGenericClass::getStatusBits() & WIFI_SCANNING_BIT) {
    esp_wifi_scan_stop();
    delay(30);
  }
  WiFi.scanDelete();
  scanCount = WiFi.scanNetworks();   // 返回数量，<0 表示失败

  // 5. 失败自动重试一次：若由于模式切换时序未完全生效或底层忙导致 <0，短暂等待后重试，避免直接甩给用户 "scan failed"
  if (scanCount < 0) {
    Serial.printf("[wifi] scan failed (%d), retrying...\n", scanCount);
    delay(200);
    if (WiFi.getMode() != scanMode) {
      WiFi.mode(scanMode);
      uint32_t t1 = millis();
      while (WiFi.getMode() != scanMode && (millis() - t1 < 300)) {
        delay(10);
      }
      delay(50);
    }
    if (WiFiGenericClass::getStatusBits() & WIFI_SCANNING_BIT) {
      esp_wifi_scan_stop();
      delay(30);
    }
    WiFi.scanDelete();
    scanCount = WiFi.scanNetworks();
    Serial.printf("[wifi] scan retry count=%d\n", scanCount);
  }

  wifiIdx = 0; wifiTop = 0;
  lastWifiScanMs = millis();

  // 扫描期间（2~8 秒阻塞）用户按下的所有键都积压在 TCA8418 FIFO 里，
  // 若不清空，下一帧会批量消费它们，导致立刻触发第二次扫描（闪烁根因）。
  kbd::flushEvents();
}

void wifiScanExit() {
  WiFi.scanDelete();
  hotspotRestoreAfterScan();
  scanCount = 0;
  wifiIdx = 0;
  wifiTop = 0;
  selSSID = "";
  pwInput = "";
}

void drawWifiScan() {
  cv.fillScreen(TFT_BLACK);
  drawPageHeader("Wi-Fi");

  // 当前连接状态（右上角小字）
  bool up = (WiFi.status() == WL_CONNECTED);
  cv.setTextDatum(top_right); cv.setTextSize(1);
  cv.setTextColor(up ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  cv.setFont(&fonts::efontCN_14);
  cv.drawString(up ? truncPx(ensureUtf8(WiFi.SSID()), 120) : "not connected", SW - 6, 6);
  cv.setFont(&fonts::Font0);

  if (scanCount <= 0) {
    cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString(scanCount == 0 ? "no networks - press r" : "scan failed - press r",
                  SW / 2, SH / 2);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(bottom_center);
    cv.drawString("r rescan", SW / 2, SH - 2);
    return;
  }

  const int startY = 26, rowH = 20;
  for (int p = 0; p < WIFI_VIS; p++) {
    int i = wifiTop + p;
    if (i >= scanCount) break;
    int y = startY + p * rowH;
    bool sel = (i == wifiIdx);
    if (sel) {
      cv.fillRoundRect(4, y, SW - 8, rowH - 2, 4, CARD_BG);
      cv.fillRect(4, y, 3, rowH - 2, ACCENT);
    }
    int midY = y + (rowH - 2) / 2;
    bool enc = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    if (enc) drawLock(16, midY, sel ? TFT_WHITE : TFT_LIGHTGREY);
    cv.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? CARD_BG : TFT_BLACK);
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setFont(&fonts::efontCN_14);
    cv.drawString(truncPx(ensureUtf8(WiFi.SSID(i)), SW - 60), 26, midY);
    cv.setFont(&fonts::Font0);
    drawSignal(SW - 28, midY + 5, WiFi.RSSI(i), sel ? ACCENT : TFT_LIGHTGREY);
  }
  drawScrollBar(SW - 4, startY, WIFI_VIS * rowH - 2, wifiTop, scanCount, WIFI_VIS);

  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_center); cv.setTextSize(1);
  cv.drawString("Enter connect   r rescan", SW / 2, SH - 2);
}

void drawWifiPw() {
  cv.fillScreen(TFT_BLACK);
  cv.setTextColor(ACCENT, TFT_BLACK);
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.drawString("Connect to:", 8, 8);
  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  cv.setFont(&fonts::efontCN_14);
  cv.drawString(truncPx(ensureUtf8(selSSID), SW - 16), 8, 24);
  cv.setFont(&fonts::Font0);

  // 密码输入框（明文显示，方便小屏核对）
  int boxY = 54, boxH = 26;
  cv.drawRoundRect(8, boxY, SW - 16, boxH, 4, TFT_DARKGREY);
  String shown = pwInput.length() > 24 ? ("~" + pwInput.substring(pwInput.length() - 23)) : pwInput;
  cv.setTextColor(TFT_GREEN, TFT_BLACK);
  cv.setTextDatum(middle_left); cv.setTextSize(2);
  cv.drawString(shown + "_", 14, boxY + boxH / 2);

  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_center); cv.setTextSize(1);
  cv.drawString("type pw (shift=CAP/sym)  Enter=go", SW / 2, SH - 2);
}

// Wi-Fi 看门狗：常开策略下总有 app 会把 radio 抢走（嗅探要混杂模式、热点要切 AP、
// wardrive/lanscan 退出时会 WIFI_OFF）。那些 app 一退出，这里负责把 STA 拉回来。
// allowed=false 时什么都不做——由 main.cpp 判断"当前页面是不是正拿着射频"，
// 免得在嗅探/热点跑着的时候去抢模式。
static bool keeperEnabled = true;

// 给串口的 WIFIOFF 指令用：常开策略下看门狗会在下一帧就把 radio 拉回来
// （lastKickMs 初值 0，所以第一次判断 millis()-0 > 10000 直接成立），
// 那样"手动关掉 WiFi 做干净的重连对照测试"这个调试口子就废了。挂起它，直到 WIFI 指令重新打开。
void wifiKeeperSuspend() { keeperEnabled = false; }
void wifiKeeperResume()  { keeperEnabled = true; }

void wifiKeeperUpdate(bool allowed) {
  static uint32_t lastKickMs = 0;
  if (!keeperEnabled || !allowed) return;
  if (WiFi.getMode() & WIFI_MODE_AP) return;          // 热点开着就别抢
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastKickMs < 10000) return;          // 别每帧都 begin，10s 踢一脚就够
  String s, p;
  if (!loadCreds(s, p)) return;                       // 没记住过网络，那就老实待着
  lastKickMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(true);
  WiFi.begin(s.c_str(), p.c_str());                   // 非阻塞，连上与否交给协议栈
}

// ⚠️ 这里以前掉线时会退回阻塞的 connectWith()，现在不会了。两条理由：
//  1) 它的调用方（weather/adsb/sats/github/router/geoloc/地图瓦片）全都在 loop() 上，
//     一次 connectWith() 最坏卡 15 秒，而 ADS-B 25 秒自动刷一次、Router 1.5 秒一次——
//     Wi-Fi 一掉线就变成"每隔一会儿整机僵十几秒"。
//  2) connectWith() 会用 termLog 抢屏刷握手日志，把当前 app 的画面整个盖掉再画回来，
//     表现就是"看着天气/飞机呢，屏幕突然闪一段 wifi 日志"。
// 现在掉线就直接返回 false，各页面显示自己的 "wifi not connected" 空状态，重连交给
// 常开策略下的看门狗 wifiKeeperUpdate()（10 秒踢一脚，非阻塞）——它本来就是干这个的。
bool wifiEnsureConnected() {
  return WiFi.status() == WL_CONNECTED;
}
