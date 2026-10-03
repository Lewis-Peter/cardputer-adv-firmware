#include "hotspot.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "dhcpserver/dhcpserver.h"   // dhcp_search_ip_on_mac：按 MAC 查 DHCP 租约表拿到分配的 IP
#include "ui_common.h"
#include "config.h"

static const char* HOTSPOT_SSID = CFG_HOTSPOT_SSID;
static const char* DEFAULT_PW = CFG_HOTSPOT_DEFAULT_PW;

struct HotspotClient {
  uint8_t mac[6];
  String ip;
};

// 连上的设备只知道 MAC（esp_wifi_ap_get_sta_list），IP 得再查一次 DHCP 租约表才有
static int listClients(HotspotClient* out, int maxOut) {
  wifi_sta_list_t staList;
  if (esp_wifi_ap_get_sta_list(&staList) != ESP_OK) return 0;
  int n = 0;
  for (int i = 0; i < staList.num && n < maxOut; i++) {
    memcpy(out[n].mac, staList.sta[i].mac, 6);
    ip4_addr_t ip;
    if (dhcp_search_ip_on_mac(staList.sta[i].mac, &ip)) {
      out[n].ip = IPAddress(ip.addr).toString();
    } else {
      out[n].ip = "negotiating...";
    }
    n++;
  }
  return n;
}

static bool apActive = false;
static bool apSuspended = false;
String hotspotPwInput;

static String loadPw() {
  return loadString("hotspot", "pw", DEFAULT_PW);
}
static void savePw(const String& pw) {
  saveString("hotspot", "pw", pw);
}

static void startAp() {
  WiFi.mode(WIFI_AP);   // 纯 AP，不顺带连外网——应急场景本来就是没有别的 WiFi 可用
  WiFi.softAP(HOTSPOT_SSID, loadPw().c_str());
  apActive = true;
}
static void stopAp() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  apActive = false;
}

// 别的 app 切 WiFi 模式(STA/OFF)会悄悄把 SoftAP 干掉，但 apActive 还留着 true。
// 查询/绘制前校对一次真实模式，自愈这个陈旧标志，免得 UI 显示 ON 其实早没了。
static void syncApState() { if (apActive && !(WiFi.getMode() & WIFI_MODE_AP)) apActive = false; }

bool hotspotIsActive() {
  syncApState();
  return apActive || (WiFi.getMode() & WIFI_MODE_AP);
}

void hotspotStop() {
  apSuspended = false;
  syncApState();
  if (apActive || (WiFi.getMode() & WIFI_MODE_AP)) {
    stopAp();
  }
}

wifi_mode_t hotspotScanMode() {
  syncApState();
  return apActive ? WIFI_AP_STA : WIFI_STA;
}

bool hotspotRestoreAfterScan() {
  syncApState();
  if (!apActive) return false;
  if (WiFi.status() != WL_CONNECTED) WiFi.mode(WIFI_AP);
  return true;
}

bool hotspotSuspend() {
  syncApState();
  if (apSuspended) return true;
  if (apActive || (WiFi.getMode() & WIFI_MODE_AP)) {
    apSuspended = true;
    stopAp();
    return true;
  }
  return false;
}

void hotspotResume() {
  if (apSuspended) {
    apSuspended = false;
    startAp();
  }
}

bool hotspotIsSuspended() {
  return apSuspended;
}

void hotspotToggle() {
  apSuspended = false;
  syncApState();
  if (apActive) stopAp(); else startAp();
}

void hotspotEnsureOn() {
  syncApState();
  if (!apActive) startAp();
}

String hotspotPassword() { return loadPw(); }
void hotspotSetPassword(const String& pw) {
  savePw(pw);
  if (apActive) { stopAp(); startAp(); }   // 立即用新密码重开，不用再手动关一次开一次
}

void drawHotspot() {
  syncApState();   // 进来先校对：AP 可能已被别的 app 切模式弄死了
  cv.fillScreen(TFT_BLACK);

  drawPageHeader("Hotspot", apActive ? "ON AIR" : "STANDBY", apActive ? 0x07E0 : 0x7BEF);

  // 1. 上方 AP 状态与凭据卡 (y = 16..53, h = 38)
  const int cardX = 4, cardW = SW - 8;
  const int topY = 16, topH = 38;
  uint16_t bdrColor = apActive ? 0x07E0 : 0x18C3;
  cv.fillRoundRect(cardX, topY, cardW, topH, 3, 0x0821);
  cv.drawRoundRect(cardX, topY, cardW, topH, 3, bdrColor);
  cv.fillRect(cardX, topY + 4, 3, topH - 8, apActive ? 0x07E0 : 0x4208);

  // 第一行：SSID 与状态徽标
  cv.setTextSize(1);
  cv.setTextDatum(top_left);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("SSID:", cardX + 8, topY + 5);
  cv.setTextColor(TFT_WHITE, 0x0821);
  cv.drawString(HOTSPOT_SSID, cardX + 42, topY + 5);

  // 右侧状态胶囊
  const int badgeW = 54, badgeH = 12;
  const int badgeX = cardX + cardW - badgeW - 6, badgeY = topY + 4;
  if (apActive) {
    cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x0280);
    cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x04A0);
    cv.setTextDatum(middle_center);
    cv.setTextColor(0x07E0, 0x0280);
    cv.drawString("ON AIR", badgeX + badgeW / 2, badgeY + badgeH / 2);
  } else {
    cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x18C3);
    cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x3186);
    cv.setTextDatum(middle_center);
    cv.setTextColor(0x7BEF, 0x18C3);
    cv.drawString("DISABLED", badgeX + badgeW / 2, badgeY + badgeH / 2);
  }

  // 第二行：密码与网关 IP / 频段
  cv.setTextDatum(top_left);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("PASS:", cardX + 8, topY + 21);
  cv.setTextColor(0xFDA0, 0x0821);
  cv.drawString(loadPw(), cardX + 42, topY + 21);

  cv.setTextDatum(top_right);
  if (apActive) {
    cv.setTextColor(0x07E0, 0x0821);
    char ipBuf[32];
    snprintf(ipBuf, sizeof(ipBuf), "IP %s", WiFi.softAPIP().toString().c_str());
    cv.drawString(ipBuf, cardX + cardW - 8, topY + 21);
  } else {
    cv.setTextColor(0x632C, 0x0821);
    cv.drawString("CH01 // 2.4GHz", cardX + cardW - 8, topY + 21);
  }

  // 2. 下方已连入客户端列表 / DHCP 租约卡片 (y = 57..120, h = 64)
  const int botY = 57, botH = 64;
  cv.fillRoundRect(cardX, botY, cardW, botH, 3, 0x0821);
  cv.drawRoundRect(cardX, botY, cardW, botH, 3, 0x18C3);
  cv.fillRect(cardX, botY + 4, 3, botH - 8, 0x07FF);

  HotspotClient clients[4];
  int clientCount = apActive ? listClients(clients, 4) : 0;

  // 标头行
  cv.setTextDatum(top_left);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("CONNECTED CLIENTS (DHCP):", cardX + 8, botY + 4);

  cv.setTextDatum(top_right);
  if (apActive) {
    char cntBuf[24];
    snprintf(cntBuf, sizeof(cntBuf), "%d ACTIVE", clientCount);
    cv.setTextColor(clientCount > 0 ? 0x07E0 : 0x632C, 0x0821);
    cv.drawString(cntBuf, cardX + cardW - 8, botY + 4);
  } else {
    cv.setTextColor(0x4208, 0x0821);
    cv.drawString("RADIO OFF", cardX + cardW - 8, botY + 4);
  }

  cv.drawFastHLine(cardX + 6, botY + 14, cardW - 12, 0x18C3);

  // 内容区
  if (!apActive) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(0x632C, 0x0821);
    cv.drawString("SoftAP is currently stopped", SW / 2, botY + 31);
    cv.setTextColor(0x07FF, 0x0821);
    cv.drawString("Press [Enter] to turn ON hotspot", SW / 2, botY + 46);
  } else if (clientCount == 0) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(0x632C, 0x0821);
    cv.drawString("No clients connected yet", SW / 2, botY + 31);
    cv.setTextColor(0x4208, 0x0821);
    cv.drawString("Scan QR [Q] or search SSID to connect", SW / 2, botY + 46);
  } else {
    for (int i = 0; i < clientCount; i++) {
      int rowY = botY + 18 + i * 11;
      cv.setTextDatum(top_left);

      // 设备序号
      cv.setTextColor(0x07FF, 0x0821);
      char idxBuf[8];
      snprintf(idxBuf, sizeof(idxBuf), "#%d", i + 1);
      cv.drawString(idxBuf, cardX + 8, rowY);

      // IP 地址
      cv.setTextColor(TFT_WHITE, 0x0821);
      cv.drawString(clients[i].ip, cardX + 26, rowY);

      // MAC 后缀
      char macBuf[16];
      snprintf(macBuf, sizeof(macBuf), "[%02X:%02X:%02X]", clients[i].mac[3], clients[i].mac[4], clients[i].mac[5]);
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString(macBuf, cardX + 118, rowY);

      // 状态胶囊
      cv.setTextDatum(top_right);
      cv.setTextColor(0x07E0, 0x0821);
      cv.drawString("ONLINE", cardX + cardW - 8, rowY);
    }
  }

  // 3. 底部高对比度彩色快捷键 (y = 125..134)
  const int footY = 125;
  cv.setTextDatum(middle_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 4, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(apActive ? " stop" : " start", 32, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("P", 74, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" pass", 80, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Q", 120, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" qr code", 126, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 188, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" back", 194, footY);
}

// WIFI: 串里 \ ; , : " 这几个字符有语法含义，出现在 SSID/密码里必须转义，
// 否则密码里带个分号就会被手机解析成字段分隔符，扫出来连不上还查不出原因。
static String qrEscape(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

void drawHotspotQr() {
  cv.fillScreen(TFT_BLACK);

  // 安卓/iOS 相机都认的标准 Wi-Fi 入网串
  String payload = String("WIFI:T:WPA;S:") + qrEscape(HOTSPOT_SSID) +
                   ";P:" + qrEscape(loadPw()) + ";;";

  // 尺寸严格保持与原版相同（29 模块 × 4px = 116px），保证手机秒扫
  const int qmod   = 29 * 4;
  const int boxH   = (debugOn ? SH - 12 : SH - 2) - 2;
  const int boxW   = qmod + 20;
  const int boxX   = 2, boxY = 2;
  cv.fillRect(boxX, boxY, boxW, boxH, TFT_WHITE);
  cv.qrcode(payload.c_str(), boxX + (boxW - qmod) / 2, boxY + (boxH - qmod) / 2,
            qmod, 1, false);

  // 右侧科技感信息面板卡片
  const int tx = boxX + boxW + 4;
  const int tw = SW - tx - 3;
  cv.fillRoundRect(tx, 2, tw, boxH, 3, 0x0821);
  cv.drawRoundRect(tx, 2, tw, boxH, 3, 0x18C3);
  cv.fillRect(tx, 6, 2, boxH - 12, 0x07FF);

  int curY = 6;
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("WIFI DIRECT", tx + 6, curY);
  curY += 12;

  cv.drawFastHLine(tx + 4, curY, tw - 8, 0x18C3);
  curY += 5;

  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("SSID:", tx + 6, curY);
  curY += 10;
  cv.setTextColor(TFT_WHITE, 0x0821);
  cv.drawString(truncPx(HOTSPOT_SSID, tw - 8), tx + 6, curY);
  curY += 14;

  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("PASSWORD:", tx + 6, curY);
  curY += 10;
  cv.setTextColor(0xFDA0, 0x0821);
  cv.drawString(truncPx(loadPw(), tw - 8), tx + 6, curY);
  curY += 14;

  // 状态胶囊
  if (apActive) {
    cv.fillRoundRect(tx + 6, curY, tw - 12, 11, 2, 0x0280);
    cv.drawRoundRect(tx + 6, curY, tw - 12, 11, 2, 0x04A0);
    cv.setTextDatum(middle_center);
    cv.setTextColor(0x07E0, 0x0280);
    cv.drawString("AP ONLINE", tx + tw / 2, curY + 5);
  } else {
    cv.fillRoundRect(tx + 6, curY, tw - 12, 11, 2, 0x3800);
    cv.drawRoundRect(tx + 6, curY, tw - 12, 11, 2, 0x7800);
    cv.setTextDatum(middle_center);
    cv.setTextColor(0xF800, 0x3800);
    cv.drawString("AP OFF!", tx + tw / 2, curY + 5);
  }

  cv.setTextDatum(bottom_center);
  cv.setTextColor(0x632C, 0x0821);
  cv.drawString("Scan to join", tx + tw / 2, boxH - 2);
}

void drawHotspotPw() {
  cv.fillScreen(TFT_BLACK);
  drawPageHeader("Hotspot Config", "EDIT PASSWORD", 0x07FF);

  const int cardX = 6, cardW = SW - 12;

  // 1. 密码输入终端卡片 (y = 16..58, h = 42)
  const int inY = 16, inH = 42;
  cv.fillRoundRect(cardX, inY, cardW, inH, 3, 0x0821);
  cv.drawRoundRect(cardX, inY, cardW, inH, 3, 0x07FF);
  cv.fillRect(cardX, inY + 4, 3, inH - 8, 0x07FF);

  cv.setTextSize(1);
  cv.setTextDatum(top_left);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("NEW WPA2 PASSPHRASE:", cardX + 8, inY + 5);

  cv.setTextDatum(top_right);
  int len = hotspotPwInput.length();
  char lenBuf[24];
  snprintf(lenBuf, sizeof(lenBuf), "%d/32", len);
  cv.setTextColor(len >= 8 ? 0x07E0 : 0xF800, 0x0821);
  cv.drawString(lenBuf, cardX + cardW - 8, inY + 5);

  // 输入框文字与闪烁光标
  cv.setTextDatum(middle_left); cv.setTextSize(2);
  bool blink = (millis() / 400) % 2 == 0;
  String disp = hotspotPwInput + (blink ? "_" : " ");
  cv.setTextColor(TFT_WHITE, 0x0821);
  cv.drawString(disp, cardX + 10, inY + 26);

  // 2. 规范与提示卡片 (y = 62..120, h = 58)
  const int tipY = 62, tipH = 58;
  cv.fillRoundRect(cardX, tipY, cardW, tipH, 3, 0x0821);
  cv.drawRoundRect(cardX, tipY, cardW, tipH, 3, 0x18C3);
  cv.fillRect(cardX, tipY + 4, 3, tipH - 8, 0xFDA0);

  cv.setTextSize(1);
  cv.setTextDatum(top_left);
  cv.setTextColor(0xFDA0, 0x0821);
  cv.drawString("CONFIG SPECIFICATION //", cardX + 8, tipY + 5);

  cv.setTextColor(0xCE79, 0x0821);
  cv.drawString("* WPA2-PSK requires at least 8 chars", cardX + 8, tipY + 18);

  char curBuf[48];
  snprintf(curBuf, sizeof(curBuf), "* Current: %s", loadPw().c_str());
  cv.drawString(truncPx(curBuf, cardW - 16), cardX + 8, tipY + 30);

  cv.setTextColor(0x632C, 0x0821);
  cv.drawString("* Hotspot restarts with new key immediately", cardX + 8, tipY + 42);

  // 3. 底部快捷键提示
  const int footY = 125;
  cv.setTextDatum(middle_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 4, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" save", 32, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("BS", 74, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" delete", 86, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 188, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" cancel", 194, footY);
}
