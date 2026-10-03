#include "pcmode.h"
#include "gnss.h"
#include "imu.h"
#include "lora.h"
#include "ridapp.h"
#include "bt.h"
#include <WiFi.h>
#include <M5Unified.h>

// 状态快照，用于判断内容是否变化（变化时才重画屏幕，避免闪烁与无谓 SPI 刷新）
struct PcModeDisplayState {
  bool cdcConnected;
  bool gnssActive;
  int gnssHz;
  bool imuActive;
  int imuHz;
  bool loraActive;
  float loraMhz;
  uint32_t loraPkts;
  bool ridActive;
  int ridDrones;
  uint32_t ridPkts;
  bool bleActive;
  int bleDevs;
  uint32_t blePkts;
  int wifiAps;
  bool wifiConnected;
};

// 带背景色 + 固定宽度填充的绘制：新字符串盖住旧的，不用先清黑再画，局部刷新就不会闪。
static void pcmDraw(const char* txt, int x, int y, int pad) {
  M5.Display.setTextPadding(pad);
  M5.Display.drawString(txt, x, y);
  M5.Display.setTextPadding(0);
}

static int lastWifiScanAps = -1;
static uint32_t pcmodeLastDrawMs = 0;
static bool pcmodeFirstDraw = true;
static const PcModeDisplayState kInitDrawnState = {false, false, 0, false, 0, false, 0.0f, 0, false, 0, 0, false, 0, 0, -1, false};
static PcModeDisplayState lastDrawnState = kInitDrawnState;

void pcmodeRecordWifiScan(int count) {
  lastWifiScanAps = count;
}

int pcmodeGetLastWifiScan() {
  return lastWifiScanAps;
}

static PcModeDisplayState pcmodeCurrentState() {
  PcModeDisplayState s;
  s.cdcConnected = (bool)Serial;
  s.gnssActive = gnssStreamIsActive();
  s.gnssHz = gnssStreamGetHz();
  s.imuActive = imuStreamIsActive();
  s.imuHz = imuStreamGetHz();
  s.loraActive = loraSniffIsActive();
  s.loraMhz = loraSniffGetMhz();
  s.loraPkts = loraSniffGetPktCount();
  s.ridActive = ridStreamIsActive();
  s.ridDrones = ridStreamDroneCount();
  s.ridPkts = ridStreamPktCount();
  s.bleActive = btStreamIsActive();
  s.bleDevs = btStreamDevCount();
  s.blePkts = btStreamPktCount();
  s.wifiAps = lastWifiScanAps;
  s.wifiConnected = (WiFi.status() == WL_CONNECTED);
  return s;
}

static bool pcmodeStateChanged(const PcModeDisplayState& a, const PcModeDisplayState& b) {
  return a.cdcConnected != b.cdcConnected ||
         a.gnssActive != b.gnssActive ||
         a.gnssHz != b.gnssHz ||
         a.imuActive != b.imuActive ||
         a.imuHz != b.imuHz ||
         a.loraActive != b.loraActive ||
         a.loraMhz != b.loraMhz ||
         a.loraPkts != b.loraPkts ||
         a.ridActive != b.ridActive ||
         a.ridDrones != b.ridDrones ||
         a.ridPkts != b.ridPkts ||
         a.bleActive != b.bleActive ||
         a.bleDevs != b.bleDevs ||
         a.blePkts != b.blePkts ||
         a.wifiAps != b.wifiAps ||
         a.wifiConnected != b.wifiConnected;
}

// mode 字段：bridge 重启或 USB 重插后只能靠 CAPS 探测，不带模式的话它分不清设备是不是还在 PC MODE 里
void pcmodePrintCaps() {
  Serial.print(screen == SCREEN_PCMODE ? "PCM {\"t\":\"caps\",\"mode\":\"pc\"," : "PCM {\"t\":\"caps\",\"mode\":\"normal\",");
  Serial.println("\"caps\":[{\"id\":\"wifi\",\"verbs\":[\"SCAN WIFI\"],\"stream\":false},{\"id\":\"gnss\",\"verbs\":[\"GNSS ON\",\"GNSS OFF\"],\"stream\":true},{\"id\":\"imu\",\"verbs\":[\"IMU ON\",\"IMU OFF\"],\"stream\":true},{\"id\":\"lora\",\"verbs\":[\"LORA SNIFF\",\"LORA OFF\"],\"stream\":true},{\"id\":\"rid\",\"verbs\":[\"RID ON\",\"RID OFF\"],\"stream\":true},{\"id\":\"ble\",\"verbs\":[\"BLE ON\",\"BLE OFF\"],\"stream\":true}]}");
}

// 记录从哪儿进入 PC MODE（如从 Settings 进入则退出时回 Settings，否则回主菜单）
static Screen pcmodeReturnScreen = SCREEN_MENU;

// 协议规格：cardputer-bridge 仓库 docs/pc-mode-protocol.md（两端以那份为准，本仓库不另存副本）
void pcmodePrintHello() {
  Serial.println("PCM {\"t\":\"enter\"}");
  Serial.printf("PCM {\"t\":\"hello\",\"proto\":1,\"proto_minor\":1,\"fw\":\"Slacker v4 (" __DATE__ " " __TIME__ ")\",\"caps\":[\"wifi\",\"gnss\",\"imu\",\"lora\",\"rid\",\"ble\"]}\n");
}

void pcmodeEnter() {
  if (screen == SCREEN_PCMODE) {
    // 已在 PC MODE：重发握手 hello 即可，不重复释放画布与重置状态
    pcmodePrintHello();
    return;
  }

  // 记录来源屏幕，退出时按来源返回（Settings 进回 Settings，串口/主菜单进回主菜单）
  pcmodeReturnScreen = (screen == SCREEN_SETTINGS) ? SCREEN_SETTINGS : SCREEN_MENU;

  cleanupApp(screen);
  screen = SCREEN_PCMODE;

  // 释放 64.8KB cv 画布，留出连续内存给 TLS / 传感器流
  canvasRelease();

  // 重置状态屏刷新状态
  pcmodeFirstDraw = true;
  pcmodeLastDrawMs = 0;
  lastDrawnState = kInitDrawnState;
  dirty = true;

  pcmodePrintHello();
}

void pcmodeExit() {
  if (screen != SCREEN_PCMODE) return;

  // 关掉 PC MODE 期间开启的能力
  if (gnssStreamIsActive()) {
    gnssStreamSet(false);
  }
  if (imuStreamIsActive()) {
    imuStreamSet(false);
  }
  if (loraSniffIsActive()) {
    loraSniffStop();
  }
  if (ridStreamIsActive()) {
    ridStreamStop();   // 发 end 并把混杂模式/WiFi 还原，跟 GNSS/IMU/LoRa 一样
  }
  if (btStreamIsActive()) {
    btStreamStop();    // 发 end 并挂起 BLE
  }

  // 恢复全局全屏画布
  canvasRestore();

  // 协议退出通知
  Serial.println("PCM {\"t\":\"exit\"}");

  // 回到进入前的界面（从 Settings 进回 Settings，其余回主菜单）
  screen = pcmodeReturnScreen;
  pcmodeReturnScreen = SCREEN_MENU;
  dirty = true;
}

void pcmodeUpdate() {
  if (screen != SCREEN_PCMODE) return;
  uint32_t now = millis();
  // 最多 1Hz 检查一次状态变化
  if (now - pcmodeLastDrawMs < 1000) return;

  PcModeDisplayState cur = pcmodeCurrentState();
  if (pcmodeFirstDraw || pcmodeStateChanged(cur, lastDrawnState)) {
    dirty = true;
  }
}

void drawPcMode() {
  uint32_t now = millis();
  PcModeDisplayState cur = pcmodeCurrentState();

  // 严格限制 ≤1Hz 刷新，且只有在内容变了或首次绘制时才重画，避免闪烁
  if (!pcmodeFirstDraw && (now - pcmodeLastDrawMs < 1000 || !pcmodeStateChanged(cur, lastDrawnState))) {
    return;
  }
  // 整屏背景/标题/卡片只在首次绘制时画；之后只覆盖会变的文字，避免每秒整屏清黑闪一下
  const bool full = pcmodeFirstDraw;
  pcmodeLastDrawMs = now;
  lastDrawnState = cur;
  pcmodeFirstDraw = false;

  // 直连 M5.Display，绝对不分配任何 sprite
  const int cardX = 8, cardY = 23, cardW = SW - 16;
  if (full) {
    M5.Display.fillScreen(TFT_BLACK);

    // 1. 顶部标题栏
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(top_left);
    M5.Display.fillRoundRect(8, 6, 4, 12, 1, ACCENT);
    M5.Display.setTextColor(ACCENT, TFT_BLACK);
    M5.Display.drawString("PC MODE", 16, 7);

    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(0x07FF, TFT_BLACK);
    M5.Display.drawString("[ BRIDGE ]", SW - 8, 7);

    M5.Display.drawFastHLine(8, 20, SW - 16, DIM_BORDER);

    // 2. 信息面板卡片（5 行布局，GNSS/IMU/LoRa 同时开启时也能完整排布）
    const int cardH = 94;
    M5.Display.fillRoundRect(cardX, cardY, cardW, cardH, 3, 0x0821);
    M5.Display.drawRoundRect(cardX, cardY, cardW, cardH, 3, 0x18C3);
  }
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(top_left);  // 底部提示上一轮用的是 bottom_center

  // Row 1: 当前活动传感器流 (ACTIVE)
  int y = cardY + 5;
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(0x9CD3, 0x0821);
  pcmDraw("ACTIVE CAP :", cardX + 8, y, 76);

  char capBuf[32];
  uint16_t capCol = TFT_LIGHTGREY;
  if (cur.gnssActive || cur.imuActive || cur.ridActive || cur.bleActive) {
    // 开着几路就列几路；两路以上用短写(H)，一行 26 字符放得下 "GNSS 5H | IMU 50H | RID"
    const int nAct = (int)cur.gnssActive + (int)cur.imuActive + (int)cur.ridActive + (int)cur.bleActive;
    int w = 0;
    capBuf[0] = 0;
    auto add = [&](const char* txt) {
      if (w >= (int)sizeof(capBuf) - 1) return;
      w += snprintf(capBuf + w, sizeof(capBuf) - w, "%s%s", w ? " | " : "", txt);
    };
    char part[16];
    if (cur.gnssActive) { snprintf(part, sizeof(part), nAct > 1 ? "GNSS %dH" : "GNSS %dHz", cur.gnssHz); add(part); }
    if (cur.imuActive)  { snprintf(part, sizeof(part), nAct > 1 ? "IMU %dH"  : "IMU %dHz",  cur.imuHz);  add(part); }
    if (cur.ridActive)  add("RID");
    if (cur.bleActive)  add("BLE");
    capCol = TFT_GREEN;
  } else if (cur.wifiAps >= 0) {
    snprintf(capBuf, sizeof(capBuf), "WIFI %d APs", cur.wifiAps);
    capCol = 0x07FF;
  } else {
    snprintf(capBuf, sizeof(capBuf), "IDLE");
    capCol = TFT_DARKGREY;
  }
  M5.Display.setTextColor(capCol, 0x0821);
  pcmDraw(capBuf, cardX + 84, y, cardW - 90);

  // Row 2: LoRa 嗅探状态 (频率 + 收包数)
  y += 16;
  M5.Display.setTextColor(0x9CD3, 0x0821);
  pcmDraw("LORA SNIFF :", cardX + 8, y, 76);
  if (cur.loraActive) {
    snprintf(capBuf, sizeof(capBuf), "%.3fM #%lu", cur.loraMhz, (unsigned long)cur.loraPkts);
    M5.Display.setTextColor(0x07FF, 0x0821);
    pcmDraw(capBuf, cardX + 84, y, cardW - 90);
  } else {
    M5.Display.setTextColor(TFT_DARKGREY, 0x0821);
    pcmDraw("OFF", cardX + 84, y, cardW - 90);
  }

  // Row 3: 串口连接状态 (USB SERIAL)
  y += 16;
  M5.Display.setTextColor(0x9CD3, 0x0821);
  pcmDraw("USB CDC    :", cardX + 8, y, 76);
  if (cur.cdcConnected) {
    M5.Display.setTextColor(TFT_GREEN, 0x0821);
    pcmDraw("CONNECTED", cardX + 84, y, cardW - 90);
  } else {
    M5.Display.setTextColor(TFT_ORANGE, 0x0821);
    pcmDraw("WAITING", cardX + 84, y, cardW - 90);
  }

  // Row 4: WiFi 状态 (WIFI STA)
  y += 16;
  M5.Display.setTextColor(0x9CD3, 0x0821);
  pcmDraw("WIFI STA   :", cardX + 8, y, 76);
  if (cur.wifiConnected) {
    M5.Display.setTextColor(TFT_GREEN, 0x0821);
    String ssid = WiFi.SSID();
    if (ssid.length() > 14) ssid = ssid.substring(0, 13) + "~";
    pcmDraw(ssid.c_str(), cardX + 84, y, cardW - 90);
  } else {
    M5.Display.setTextColor(TFT_DARKGREY, 0x0821);
    pcmDraw("DOWN", cardX + 84, y, cardW - 90);
  }

  // Row 5: 辅助状态行（RID 流开着显示看到几架，其次是最近 WiFi 扫描结果，否则提示已释放显存量）
  y += 16;
  if (cur.ridActive) {
    M5.Display.setTextColor(0x9CD3, 0x0821);
    pcmDraw("DRONE ID   :", cardX + 8, y, 76);
    const int rc = ridStreamChannel();
    if (rc) snprintf(capBuf, sizeof(capBuf), "%d UAS #%lu ch%d", cur.ridDrones, (unsigned long)cur.ridPkts, rc);
    else    snprintf(capBuf, sizeof(capBuf), "%d UAS #%lu hop", cur.ridDrones, (unsigned long)cur.ridPkts);
    M5.Display.setTextColor(0x07FF, 0x0821);
    pcmDraw(capBuf, cardX + 84, y, cardW - 90);
  } else if (cur.bleActive) {
    M5.Display.setTextColor(0x9CD3, 0x0821);
    pcmDraw("BLE SCAN   :", cardX + 8, y, 76);
    snprintf(capBuf, sizeof(capBuf), "%d dev #%lu", cur.bleDevs, (unsigned long)cur.blePkts);
    M5.Display.setTextColor(0x07FF, 0x0821);
    pcmDraw(capBuf, cardX + 84, y, cardW - 90);
  } else if ((cur.gnssActive || cur.imuActive || cur.loraActive) && cur.wifiAps >= 0) {
    M5.Display.setTextColor(0x9CD3, 0x0821);
    pcmDraw("LAST SCAN  :", cardX + 8, y, 76);
    snprintf(capBuf, sizeof(capBuf), "%d APs", cur.wifiAps);
    M5.Display.setTextColor(0x07FF, 0x0821);
    pcmDraw(capBuf, cardX + 84, y, cardW - 90);
  } else {
    M5.Display.setTextColor(0x632C, 0x0821);
    pcmDraw("MEM SAVED  : 64.8 KB (cv free)", cardX + 8, y, cardW - 14);
  }

  // 3. 底部退出与指令提示
  M5.Display.setTextDatum(bottom_center);
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.drawString("` / MENU exit   |   CAPS query", SW / 2, SH - 3);
}
