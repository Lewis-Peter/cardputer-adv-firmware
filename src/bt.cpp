#include "bt.h"
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEHIDDevice.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <esp_bt.h>        // esp_bt_controller_enable/disable
#include <esp_bt_main.h>   // esp_bluedroid_enable/disable
#include <esp_heap_caps.h>
#include "bctrail.h"
#define US_KEYBOARD
#include <HIDKeyboardTypes.h>
#include "ui_common.h"
#include "keyboard_adv.h"
#include "icons.h"
#include "gbk_table.h"

// BLE 的生命周期：init 一次，之后只在 enable/disable 之间切，**永不 deinit**。
//
// 为什么不能 deinit 了重建：Arduino 的 BLE 库根本没有拆除路径——BLEServer 连析构函数
// 都没声明，BLEService 也没有，BLECharacteristic::~ 的 body 是被注释掉的 free，
// BLEHIDDevice::~ 是空的。于是每走一轮 btHidSetup() 就把一整套 server+3 个 service+
// 十几个 characteristic 永久漏在堆里。实测每轮吃掉约 13KB 最大连续块
// （42996→25588→15348→8180），第 4 轮直接崩溃重启。
//
// 而 disable 不销毁任何对象，句柄和 GATT 注册全都留着，再 enable 回来就能直接用，
// 一次都不用重建 = 零泄漏。
static bool bleInited = false;      // BLEDevice::init() 这辈子只做一次
static bool bleSuspended = false;   // 已 disable（对象都还在，enable 就能回来）

// 蓝牙 ↔ 其它 app 每来回一次，btReleaseForOtherApps() 就要 deinit 一次，而 Arduino 的
// BLE 库没有拆除路径（BLEServer 连析构都没有，createServer 直接覆盖上一个），
// 于是每来回一次就永久漏掉一整套 GATT 对象树。实测最大连续块：
//     63476 → 23540 → 18420 → 17396 → 10740 → 之后卡死
// 卡死点实测在 BLEDevice::deinit() 内部（用 RTC 面包屑抓到的，见 wip 分支）。
//
// 修库的尝试见分支 wip/ble-destructor-leak-fix：内存确实能修好（收敛在 18KB 不再跌），
// 但 deinit 本身仍会偶发永久阻塞，所以没有合入。
// 在那之前，这里能做的是**别让人在卡死时莫名其妙**——提前把余量摆出来。
static int btReleaseCount = 0;

// 直接看真实的最大连续块，不是数来回次数：不同使用路径消耗速度不一样，计数会失真。
int  btAlternations() { return btReleaseCount; }

// bleInited 而非 bleSuspended：disable 一个字节都不还，所以只要 init 过又没 deinit，
// 那 ~64KB 就一直占着。挂起与否只影响射频，不影响堆。
bool btHoldsHeap() { return bleInited; }
bool btMemoryLow()    { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 20 * 1024; }
bool btMemoryCritical(){ return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 13 * 1024; }

static void btEnsureInit(bool quiet = false) {
  // 开机自动连 Wi-Fi 失败时（比如密码错/信号不好），WiFi 会一直卡在半连接的 STA
  // 状态没关掉——这时候硬开 BLE 控制器，ESP32 的 Wi-Fi/BT 共存机制很容易直接崩溃
  // 重启。先强制关掉 Wi-Fi，让 BLE controller 在干净状态下初始化。
  if (!bleInited || bleSuspended) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  if (!bleInited) {
    if (!quiet) centerMsg("starting bluetooth...", TFT_YELLOW);
    BLEDevice::init("Cardputer ADV");
    bleInited = true;
    bleSuspended = false;
    return;
  }
  if (bleSuspended) {
    if (!quiet) centerMsg("resuming bluetooth...", TFT_YELLOW);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_enable();
    bleSuspended = false;
  }
}

// =============================================================================
// 子菜单
// =============================================================================

// =============================================================================
// BLE 扫描器
// =============================================================================
const int BT_SCAN_MAX = 30;
const int BT_SCAN_VIS = 5;
BtDevice* btDevList = nullptr;
int btDevCount = 0;
int btDevIdx = 0, btDevTop = 0;

static const char* btVendorName(const String& manufHex) {
  if (manufHex.length() < 5) return nullptr;
  if (manufHex.startsWith("4C 00")) return "Apple";
  if (manufHex.startsWith("8E 05")) return "Meta/Oculus";
  if (manufHex.startsWith("06 00")) return "Microsoft";
  if (manufHex.startsWith("75 00")) return "Samsung";
  if (manufHex.startsWith("8F 03") || manufHex.startsWith("57 01")) return "Xiaomi";
  if (manufHex.startsWith("7D 02")) return "Huawei";
  if (manufHex.startsWith("D0 08")) return "DJI";
  if (manufHex.startsWith("E0 00")) return "Google";
  if (manufHex.startsWith("E5 02")) return "Espressif";
  if (manufHex.startsWith("46 00")) return "Sony";
  if (manufHex.startsWith("87 00")) return "Garmin";
  if (manufHex.startsWith("59 00")) return "Nordic";
  if (manufHex.startsWith("A8 01")) return "Amazon";
  if (manufHex.startsWith("9E 00")) return "Bose";
  return nullptr;
}

static const char* btVendorTag(const BtDevice& dev) {
  if (dev.haveOdid) return "DRON";
  const char* v = btVendorName(dev.manufHex);
  if (v) {
    if (strcmp(v, "Apple") == 0) return "AAPL";
    if (strcmp(v, "Meta/Oculus") == 0) return "META";
    if (strcmp(v, "Microsoft") == 0) return "MSFT";
    if (strcmp(v, "Samsung") == 0) return "SAMS";
    if (strcmp(v, "Xiaomi") == 0) return "XIAO";
    if (strcmp(v, "Huawei") == 0) return "HW";
    if (strcmp(v, "Google") == 0) return "GOOG";
    if (strcmp(v, "DJI") == 0) return "DJI";
    if (strcmp(v, "Espressif") == 0) return "ESP";
    if (strcmp(v, "Sony") == 0) return "SONY";
    if (strcmp(v, "Garmin") == 0) return "GRMN";
    if (strcmp(v, "Nordic") == 0) return "NRF";
    if (strcmp(v, "Amazon") == 0) return "AMZN";
    if (strcmp(v, "Bose") == 0) return "BOSE";
  }
  if (dev.haveAppearance) {
    uint16_t a = dev.appearance;
    if ((a >= 0x03C0 && a <= 0x03C4)) return "WTCH";
    if ((a >= 0x0400 && a <= 0x0403)) return "VR";
    if ((a >= 0x0380 && a <= 0x0383)) return "BAND";
    if ((a >= 0x0040 && a <= 0x0042)) return "PHON";
    if ((a >= 0x03C5 && a <= 0x03C8)) return "AUD";
  }
  return dev.addrType == 0 ? "PUB" : "RPA";
}

static uint16_t btVendorTagColor(const char* tag) {
  if (strcmp(tag, "DRON") == 0) return 0x07E0;
  if (strcmp(tag, "AAPL") == 0) return 0x07FF;
  if (strcmp(tag, "META") == 0) return 0x07FF;
  if (strcmp(tag, "XIAO") == 0) return 0xFDA0;
  if (strcmp(tag, "HW") == 0)   return 0xFDA0;
  if (strcmp(tag, "DJI") == 0)  return 0x07E0;
  if (strcmp(tag, "ESP") == 0)  return 0xFDA0;
  if (strcmp(tag, "PUB") == 0)  return 0x07FF;
  return 0x7BEF;
}

static String btDisplayTitle(const BtDevice& dev) {
  if (dev.haveOdid && dev.odid.haveBasic && dev.odid.uasId[0]) {
    return String("[Drone] ") + dev.odid.uasId;
  }
  if (dev.name.length() > 0 && dev.name != "(unnamed)") {
    return dev.name;
  }
  const char* v = btVendorName(dev.manufHex);
  if (v) {
    String shortAddr = dev.addr.length() >= 5 ? dev.addr.substring(dev.addr.length() - 5) : dev.addr;
    return String(v) + " (" + shortAddr + ")";
  }
  return String("[") + dev.addr + "]";
}

// 很多设备的名字只出现在"扫描响应"包里，紧跟在第一条广播包后面才到。
// BLEScan 默认 wantDuplicates=false，同一个地址第二次上报（往往就是带名字的
// 那条扫描响应）会被直接丢掉，导致列表里全是 "(unnamed)"。这里自己注册回调、
// 开 wantDuplicates=true，每次收到同一地址的新包就用它来补全/刷新名字。
static String hexDump(const std::string& raw, int maxBytes) {
  String out;
  int n = min((int)raw.length(), maxBytes);
  char b[4];
  for (int i = 0; i < n; i++) {
    snprintf(b, sizeof(b), "%02X", (uint8_t)raw[i]);
    out += b;
    if (i < n - 1) out += ' ';
  }
  if ((int)raw.length() > maxBytes) out += "..";
  return out;
}

class BtScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    if (!btDevList) return;
    String addr = String(d.getAddress().toString().c_str());
    int idx = -1;
    for (int i = 0; i < btDevCount; i++) if (btDevList[i].addr == addr) { idx = i; break; }
    if (idx < 0) {
      if (btDevCount >= BT_SCAN_MAX) return;
      idx = btDevCount++;
      BtDevice& nd = btDevList[idx];
      nd.addr = addr;
      nd.name = "(unnamed)";
      nd.rssi = -100;
      nd.haveTXPower = false;
      nd.haveAppearance = false;
      nd.haveService = false;
      nd.haveManuf = false;
      nd.haveSvcData = false;
      nd.haveOdid = false;
      nd.odid = OdidResult();
    }
    BtDevice& dev = btDevList[idx];
    dev.addrType = (uint8_t)d.getAddressType();
    if (d.haveName())         dev.name = String(d.getName().c_str());
    if (d.haveRSSI())         dev.rssi = d.getRSSI();
    if (d.haveTXPower())      { dev.haveTXPower = true; dev.txPower = d.getTXPower(); }
    if (d.haveAppearance())   { dev.haveAppearance = true; dev.appearance = d.getAppearance(); }
    if (d.haveServiceUUID())  { dev.haveService = true; dev.serviceUUID = String(d.getServiceUUID().toString().c_str()); }
    if (d.haveManufacturerData()) { dev.haveManuf = true; dev.manufHex = hexDump(d.getManufacturerData(), 8); }
    if (d.haveServiceData()) {
      dev.haveSvcData = true;
      dev.svcDataUUID = String(d.getServiceDataUUID().toString().c_str());
      std::string sd  = d.getServiceData();

      // Remote ID 走 BLE 时就藏在这里：UUID 0xFFFA、首字节 0x0D 是应用码，
      // 后面是 1 字节消息计数器 + 25 字节 ODID 消息。
      // ⚠️ 命中的话 hex 要多留一些——默认只 dump 6 字节，连一条消息的头都盖不住，
      // 解不出来时想拿原始字节比对（比如判断是不是国标那套）就全靠这一段。
      bool odidSvc = dev.svcDataUUID.indexOf("fffa") >= 0 &&
                     sd.length() >= 2 && (uint8_t)sd[0] == 0x0D;
      dev.svcDataHex = hexDump(sd, odidSvc ? 28 : 6);

      if (odidSvc) {
        // 跳过应用码，从计数字节开始交给解码器（它自己会试跳/不跳计数字节两种）
        OdidResult r;
        if (ridDecode((const uint8_t*)sd.data() + 1, (int)sd.length() - 1, r)) {
          // 合并而不是覆盖：一条 BLE 广播只有 25 字节，装不下 Pack，所以发送方是
          // 一条一条轮着播的。覆盖的话编号和坐标永远凑不到一起。
          odidMerge(dev.odid, r);
          dev.haveOdid = true;
        }
      }
    }
  }
};
static BtScanCallbacks btScanCallbacks;

void btScan() {
  btEnsureInit();
  cv.fillScreen(TFT_BLACK);
  drawPageHeader("BLE Scan", "scanning...", TFT_YELLOW);
  icoBt(SW / 2, SH / 2 - 10, 12, ACCENT);
  cv.drawCircle(SW / 2, SH / 2 - 10, 22, DIM_BORDER);
  cv.drawCircle(SW / 2, SH / 2 - 10, 34, DIM_BORDER);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  cv.setTextColor(TFT_YELLOW, TFT_BLACK);
  cv.drawString("Scanning 2.4GHz BLE...", SW / 2, SH / 2 + 28);
  cv.pushSprite(0, 0);

  if (!btDevList) btDevList = new BtDevice[BT_SCAN_MAX];
  btDevCount = 0;
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&btScanCallbacks, true /* wantDuplicates */);
  scan->setActiveScan(true);
  scan->start(3, false);   // 阻塞 3s；结果通过上面的回调直接填 btDevList
  scan->stop();
  scan->clearResults();

  // 扫描完成后按信号强弱由强到弱排序，近场设备优先呈现
  for (int a = 0; a < btDevCount - 1; a++) {
    for (int b = a + 1; b < btDevCount; b++) {
      if (btDevList[b].rssi > btDevList[a].rssi) {
        BtDevice tmp = btDevList[a];
        btDevList[a] = btDevList[b];
        btDevList[b] = tmp;
      }
    }
  }

  btDevIdx = 0; btDevTop = 0;
  kbd::flushEvents();   // 阻塞 3s 期间积压的按键（比如连按 r）别在返回后连发重扫
}

// 顶栏状态串按等宽字体宽度截断，使用双静态缓冲区防止同一行内多次调用冲突
static const char* clipToWidth(const char* s, int maxW, int textSize = 1) {
  static char bufs[2][64];
  static uint8_t bIdx = 0;
  bIdx ^= 1;
  char* buf = bufs[bIdx];
  if (!s) s = "";
  int chW = 6 * textSize;
  int maxChars = maxW / chW;
  if (maxChars < 1) maxChars = 1;
  if (maxChars > 63) maxChars = 63;
  int n = 0;
  while (n < maxChars && s[n]) { buf[n] = s[n]; n++; }
  buf[n] = '\0';
  return buf;
}

void drawBtScan() {
  cv.fillScreen(TFT_BLACK);
  char rightHdr[32];
  if (btDevCount > 0) {
    snprintf(rightHdr, sizeof(rightHdr), "%d/%d DEVS", btDevIdx + 1, btDevCount);
  } else {
    snprintf(rightHdr, sizeof(rightHdr), "STANDBY");
  }
  drawPageHeader("BLE Scanner", rightHdr, btDevCount > 0 ? 0x07E0 : 0x7BEF);

  if (btDevCount == 0 || !btDevList) {
    // Cyber Empty State Card
    const int cx = SW / 2;
    cv.fillRoundRect(8, 16, SW - 16, 102, 4, 0x0821);
    cv.drawRoundRect(8, 16, SW - 16, 102, 4, 0x18C3);
    icoBt(cx, 44, 12, 0x07FF);
    cv.drawCircle(cx, 44, 18, 0x1082);
    cv.drawCircle(cx, 44, 28, 0x1082);

    cv.setTextDatum(middle_center); cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextColor(TFT_WHITE, 0x0821);
    cv.drawString("[NO BLE BEACONS DETECTED]", cx, 76);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("Press 'R' to scan 2.4GHz airspace", cx, 92);

    cv.drawFastHLine(0, 121, SW, 0x1082);
    const int footY = SH - 12;
    cv.setTextDatum(top_left);
    int fx = 12;
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", fx, footY); fx += 10;
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("SCAN", fx, footY); fx += 36;
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", fx, footY); fx += 10;
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("BACK", fx, footY);
    return;
  }

  const int startY = 16, rowH = 20, cardH = 18;
  const bool showScroll = (btDevCount > BT_SCAN_VIS);
  const int cardW = SW - 16 - (showScroll ? 6 : 0);

  for (int p = 0; p < BT_SCAN_VIS; p++) {
    int i = btDevTop + p;
    if (i >= btDevCount) break;
    int y = startY + p * rowH;
    bool sel = (i == btDevIdx);
    BtDevice& dev = btDevList[i];

    uint16_t bg = sel ? 0x1124 : 0x0821;
    uint16_t bdr = sel ? 0x07FF : 0x18C3;

    cv.fillRoundRect(8, y, cardW, cardH, 3, bg);
    cv.drawRoundRect(8, y, cardW, cardH, 3, bdr);
    if (sel) {
      cv.drawFastVLine(8, y + 2, cardH - 4, 0x07FF);
    }

    int midY = y + cardH / 2;

    // 左侧厂商/类型胶囊
    const char* tag = btVendorTag(dev);
    uint16_t tagCol = btVendorTagColor(tag);
    cv.fillRoundRect(12, y + 2, 28, cardH - 4, 2, sel ? 0x0210 : 0x1082);
    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextDatum(middle_center);
    cv.setTextColor(sel ? tagCol : 0x7BEF, sel ? 0x0210 : 0x1082);
    cv.drawString(tag, 26, midY);

    // 智能标题（设备名 / 厂商+尾号 / MAC 地址）
    String title = btDisplayTitle(dev);
    cv.setTextDatum(middle_left);
    cv.setTextColor(sel ? TFT_WHITE : 0xCE79, bg);

    bool hasNonAscii = false;
    for (size_t c = 0; c < title.length(); c++) {
      if ((uint8_t)title[c] >= 0x80) { hasNonAscii = true; break; }
    }
    if (hasNonAscii) {
      cv.setFont(&fonts::efontCN_14);
      cv.drawString(truncPx(ensureUtf8(title), cardW - 88), 44, midY);
      cv.setFont(&fonts::Font0);
    } else {
      cv.setFont(&fonts::Font0);
      cv.drawString(clipToWidth(title.c_str(), cardW - 88), 44, midY);
    }

    // 右侧信号读数 + 4 格天线柱
    int rightEdge = 8 + cardW - 4;
    char rssiBuf[8];
    snprintf(rssiBuf, sizeof(rssiBuf), "%d", dev.rssi);
    uint16_t sigCol = (dev.rssi >= -65) ? 0x07E0 : ((dev.rssi >= -80) ? 0xFDA0 : 0x632C);

    // 4 格赛博天线信号柱
    int sigX = rightEdge - 15;
    for (int b = 0; b < 4; b++) {
      int bh = 3 + b * 2;
      bool act = (dev.rssi >= (-90 + b * 10));
      cv.fillRect(sigX + b * 4, midY + 4 - bh, 2, bh, act ? (sel ? 0x07FF : sigCol) : 0x1082);
    }

    cv.setTextDatum(middle_right);
    cv.setTextColor(sel ? 0x07FF : sigCol, bg);
    cv.drawString(rssiBuf, sigX - 4, midY);
  }

  // 垂直滚动条
  drawScrollBar(SW - 6, startY, BT_SCAN_VIS * rowH - 2, btDevTop, btDevCount, BT_SCAN_VIS, 0x07FF, 0x1082);

  // 底部按键提示
  cv.drawFastHLine(0, 121, SW, 0x1082);
  const int footY = SH - 12;
  cv.setTextDatum(top_left);
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  int fx = 8;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("RET", fx, footY); fx += 22;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("DETAIL", fx, footY); fx += 38;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString(";.", fx, footY); fx += 15;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("MOVE", fx, footY); fx += 28;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", fx, footY); fx += 9;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("RESCAN", fx, footY); fx += 42;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", fx, footY); fx += 9;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("MENU", fx, footY);
}

static const char* addrTypeName(uint8_t t) {
  switch (t) {
    case 0: return "public";
    case 1: return "random";
    case 2: return "rpa public";
    case 3: return "rpa random";
    default: return "?";
  }
}

void drawBtDeviceDetail() {
  cv.fillScreen(TFT_BLACK);
  if (!btDevList || btDevIdx < 0 || btDevIdx >= btDevCount) return;
  BtDevice& dev = btDevList[btDevIdx];

  String title = btDisplayTitle(dev);
  drawPageHeader(clipToWidth(title.c_str(), 120), "RADAR >", 0x07E0);

  const int cardW = SW - 16;

  // 顶部概要卡片 (y = 15..36, h = 22)
  cv.fillRoundRect(8, 15, cardW, 22, 3, 0x0821);
  cv.drawRoundRect(8, 15, cardW, 22, 3, 0x07FF);
  cv.drawFastHLine(8, 15, 6, 0x07FF); cv.drawFastVLine(8, 15, 6, 0x07FF);

  // MAC 地址
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  cv.setTextDatum(middle_left);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString(dev.addr, 13, 26);

  // 地址类型徽章
  cv.setTextDatum(middle_center);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString(addrTypeName(dev.addrType), 136, 26);

  // RSSI 数值 + 4 格天线柱
  char rBuf[16]; snprintf(rBuf, sizeof(rBuf), "%ddBm", dev.rssi);
  uint16_t sigCol = (dev.rssi >= -65) ? 0x07E0 : ((dev.rssi >= -80) ? 0xFDA0 : 0x632C);
  cv.setTextDatum(middle_right);
  cv.setTextColor(sigCol, 0x0821);
  cv.drawString(rBuf, cardW - 14, 26);

  int sigX = cardW - 10;
  for (int b = 0; b < 4; b++) {
    int bh = 3 + b * 2;
    bool act = (dev.rssi >= (-90 + b * 10));
    cv.fillRect(sigX + b * 3, 26 + 4 - bh, 2, bh, act ? sigCol : 0x1082);
  }

  // 两个遥测舱：
  // Pod 1: RADIO PROFILE & ATTRIBUTES (y = 40..77, h = 38)
  cv.fillRoundRect(8, 40, cardW, 38, 3, 0x0821);
  cv.drawRoundRect(8, 40, cardW, 38, 3, 0x18C3);

  cv.setTextDatum(top_left);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("RADIO ATTRIBUTES // BLE PROFILE", 13, 43);

  const char* vname = btVendorName(dev.manufHex);
  char row1Buf[48];
  if (vname) snprintf(row1Buf, sizeof(row1Buf), "Vendor: %s", vname);
  else if (dev.haveManuf) snprintf(row1Buf, sizeof(row1Buf), "Vendor: 0x%s", dev.manufHex.substring(0, 5).c_str());
  else snprintf(row1Buf, sizeof(row1Buf), "Vendor: n/a");
  cv.setTextColor(0xCE79, 0x0821);
  cv.drawString(row1Buf, 13, 54);

  char txBuf[24];
  if (dev.haveTXPower) snprintf(txBuf, sizeof(txBuf), "TX: %ddBm", dev.txPower);
  else snprintf(txBuf, sizeof(txBuf), "TX: n/a");
  cv.setTextDatum(top_right);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString(txBuf, 8 + cardW - 6, 54);

  cv.setTextDatum(top_left);
  char svcBuf[48];
  if (dev.haveService) snprintf(svcBuf, sizeof(svcBuf), "Svc: %s", dev.serviceUUID.c_str());
  else if (dev.haveAppearance) snprintf(svcBuf, sizeof(svcBuf), "Appear: 0x%04X", dev.appearance);
  else snprintf(svcBuf, sizeof(svcBuf), "Svc: none detected");
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString(clipToWidth(svcBuf, cardW - 12), 13, 65);

  // Pod 2: ADVERTISING DATA & ODID PAYLOAD (y = 81..118, h = 38)
  cv.fillRoundRect(8, 81, cardW, 38, 3, 0x0821);
  cv.drawRoundRect(8, 81, cardW, 38, 3, 0x18C3);

  cv.setTextDatum(top_left);
  if (dev.haveOdid) {
    cv.setTextColor(0x07E0, 0x0821);
    cv.drawString("ASTM F3411 OPEN DRONE ID", 13, 84);
    String odidStr = dev.odid.haveBasic ? dev.odid.uasId : "F3411 Broadcast";
    cv.setTextColor(TFT_WHITE, 0x0821);
    cv.drawString(String("UAS ID: ") + odidStr, 13, 95);
    char odidLoc[48];
    if (dev.odid.haveLoc) snprintf(odidLoc, sizeof(odidLoc), "Alt: %.1fm  Spd: %.1fm/s", dev.odid.height, dev.odid.speed);
    else snprintf(odidLoc, sizeof(odidLoc), "Telemetry broadcast active");
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString(odidLoc, 13, 106);
  } else {
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("ADVERTISING RAW PAYLOAD", 13, 84);
    char mfgStr[48];
    if (dev.haveManuf) snprintf(mfgStr, sizeof(mfgStr), "Mfg: %s", dev.manufHex.c_str());
    else snprintf(mfgStr, sizeof(mfgStr), "Mfg: none");
    cv.setTextColor(0xCE79, 0x0821);
    cv.drawString(clipToWidth(mfgStr, cardW - 12), 13, 95);

    char svcDatStr[48];
    if (dev.haveSvcData) snprintf(svcDatStr, sizeof(svcDatStr), "SvcDat: %s", dev.svcDataHex.c_str());
    else snprintf(svcDatStr, sizeof(svcDatStr), "SvcDat: none");
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString(clipToWidth(svcDatStr, cardW - 12), 13, 106);
  }

  // 底部按键提示
  cv.drawFastHLine(0, 121, SW, 0x1082);
  const int footY = SH - 12;
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  int fx = 8;
  cv.setTextColor(0x07E0, TFT_BLACK); cv.drawString("RET", fx, footY); fx += 22;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("TRACK IN RADAR", fx, footY); fx += 90;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", fx, footY); fx += 10;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("BACK", fx, footY);
}

// =============================================================================
// BLE HID 键盘：把设备伪装成一个标准 BLE 键盘（boot keyboard 报表描述符），
// Cardputer 打的可打印字符原样转成 HID keycode 转发给已连接的主机。
// 复用 kbd.cpp 同款布局（US 键盘），键位映射表来自 arduino-esp32 自带的
// HIDKeyboardTypes.h（跟主流 ESP32-BLE-Keyboard 库用的是同一份）。
// =============================================================================
static BLEHIDDevice* hid = nullptr;
static BLECharacteristic* inputKB = nullptr;
static BLECharacteristic* inputMedia = nullptr;
static bool hidStarted = false;
// 键盘和媒体键共用一个 HID 设备、一次配对，靠 report ID 区分。
// ⚠️ 改了报表描述符之后，之前配过对的主机可能还缓存着旧的那份（GATT caching），
// 表现是媒体键没反应——在主机上"忘记此设备"再配一次就好。
#define REPORT_ID_MEDIA 2
static volatile bool btConnected = false;   // BLE 回调里翻转、主 loop 每帧读，需 volatile 防编译器缓存

class BtServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { btConnected = true; }
  void onDisconnect(BLEServer* s) override { btConnected = false; BLEDevice::startAdvertising(); }
};
static BtServerCallbacks btServerCallbacks;

// 标准 boot keyboard 报表描述符：1 字节 modifier + 1 字节保留 + 6 字节按键码
static const uint8_t KB_REPORT_MAP[] = {
  USAGE_PAGE(1),      0x01,
  USAGE(1),           0x06,
  COLLECTION(1),      0x01,
  REPORT_ID(1),       REPORT_ID_KEYBOARD,
  USAGE_PAGE(1),      0x07,
  USAGE_MINIMUM(1),   0xE0,
  USAGE_MAXIMUM(1),   0xE7,
  LOGICAL_MINIMUM(1), 0x00,
  LOGICAL_MAXIMUM(1), 0x01,
  REPORT_SIZE(1),     0x01,
  REPORT_COUNT(1),    0x08,
  HIDINPUT(1),        0x02,           // modifier 字节
  REPORT_COUNT(1),    0x01,
  REPORT_SIZE(1),     0x08,
  HIDINPUT(1),        0x01,           // 保留字节
  REPORT_COUNT(1),    0x06,
  REPORT_SIZE(1),     0x08,
  LOGICAL_MINIMUM(1), 0x00,
  LOGICAL_MAXIMUM(1), 0x65,
  USAGE_PAGE(1),      0x07,
  USAGE_MINIMUM(1),   0x00,
  USAGE_MAXIMUM(1),   0x65,
  HIDINPUT(1),        0x00,           // 6 个按键码
  END_COLLECTION(0),

  // ---- Consumer Control：媒体键。一字节位图，一位一个功能，按下置位、松开清零 ----
  // 位序必须跟下面 btMediaSend() 里的 BtMediaKey 枚举顺序一一对应。
  USAGE_PAGE(1),      0x0C,           // Consumer
  USAGE(1),           0x01,           // Consumer Control
  COLLECTION(1),      0x01,
  REPORT_ID(1),       REPORT_ID_MEDIA,
  LOGICAL_MINIMUM(1), 0x00,
  LOGICAL_MAXIMUM(1), 0x01,
  REPORT_SIZE(1),     0x01,
  REPORT_COUNT(1),    0x07,
  USAGE(1),           0xB5,           // bit0 Scan Next Track
  USAGE(1),           0xB6,           // bit1 Scan Previous Track
  USAGE(1),           0xB7,           // bit2 Stop
  USAGE(1),           0xCD,           // bit3 Play/Pause
  USAGE(1),           0xE2,           // bit4 Mute
  USAGE(1),           0xE9,           // bit5 Volume Up
  USAGE(1),           0xEA,           // bit6 Volume Down
  HIDINPUT(1),        0x02,
  REPORT_COUNT(1),    0x01,
  HIDINPUT(1),        0x01,           // 第 8 位补齐成整字节（常量，主机忽略）
  END_COLLECTION(0)
};

static void btHidSetup() {
  if (hidStarted) return;

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(&btServerCallbacks);

  hid = new BLEHIDDevice(server);
  inputKB = hid->inputReport(REPORT_ID_KEYBOARD);
  inputMedia = hid->inputReport(REPORT_ID_MEDIA);
  // 注意：BLEHIDDevice::manufacturer(string) 这个重载假设特征值已经存在，
  // 但它从来不会自己创建——直接调用它等于往一个没初始化的指针里 setValue()，
  // 100% crash（LoadProhibited）。得先用无参版本 manufacturer() 建出特征值。
  hid->manufacturer()->setValue("Cardputer ADV");
  hid->pnp(0x02, 0xe502, 0xa111, 0x0210);
  hid->hidInfo(0x00, 0x01);
  hid->reportMap((uint8_t*)KB_REPORT_MAP, sizeof(KB_REPORT_MAP));
  hid->startServices();
  hid->setBatteryLevel(100);

  BLEAdvertising* adv = server->getAdvertising();
  adv->setAppearance(HID_KEYBOARD);
  adv->addServiceUUID(hid->hidService()->getUUID());
  adv->setScanResponse(true);

  // HID 报表特征要求加密连接才能读写（BLEHIDDevice 内部权限就是这么设的），
  // 不配这几项主机大多会配对失败或者收不到按键 notify。
  // ⚠️ 栈对象，不是 new。BLESecurity 本身不持有任何东西——三个 setter 各自直接调
  // esp_ble_gap_set_security_param()，配置进的是协议栈，对象本体用完就没用了。
  // Arduino 的示例一律写成 `new BLESecurity()` 且从不 delete，于是每次
  // btHidSetup() 漏十几个字节。数量上跟这一带那笔 ~13KB 的 deinit 库泄漏没法比
  // （见 btReleaseForOtherApps 的注释），但白漏没有任何理由。
  BLESecurity security;
  security.setAuthenticationMode(ESP_LE_AUTH_BOND);
  security.setCapability(ESP_IO_CAP_NONE);
  security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  hidStarted = true;
}

void btKeyboardStart() {
  btEnsureInit();
  btHidSetup();
  BLEDevice::startAdvertising();
}
void btKeyboardStop() {
  if (hidStarted) BLEDevice::stopAdvertising();
}

void btExit() {
  if (btDevList) {
    delete[] btDevList;
    btDevList = nullptr;
    btDevCount = 0;
  }
  if (!bleInited) return;
  btRadarStop();                       // 雷达可能还开着持续扫描，先停
  if (hidStarted) BLEDevice::stopAdvertising();


  if (bleSuspended) return;

  // 只 disable，不 deinit（原因见文件顶上 bleInited 那段）。这样 BLEServer /
  // BLEHIDDevice / 那堆 characteristic 一个都不销毁、也一个都不重建——零泄漏。
  //
  // ⚠️ 历史：这里先后是 deinit(true) → deinit(false) → 现在的 disable。
  //   deinit(true)   一个 boot 只能用一次蓝牙，第二次进去整机卡死。因为 Arduino 的
  //                  BLEDevice::deinit() 在 release_memory=true 那条分支忘了把
  //                  initialized 置回 false（BLEDevice.cpp:656-662），协议栈已经拆了
  //                  库却仍以为活着，下次 init() 整段跳过，后面对着死栈调用卡在信号量上。
  //                  何况 esp_bt_controller_mem_release() 是永久释放，改对 flag 也白搭。
  //   deinit(false)  不卡在第 2 轮了，但每轮重建一套 HID 对象漏 ~13KB，第 4 轮崩溃重启。
  //   干脆不关       WiFi 彻底起不来（esp_wifi_init 返回 257 = ESP_ERR_NO_MEM）。
  esp_bluedroid_disable();
  esp_bt_controller_disable();
  bleSuspended = true;
  btConnected = false;
  // ⚠️ hidStarted 必须保持 true，hid/inputKB/inputMedia 也不能置空——btHidSetup() 靠
  // hidStarted 判断"已经建过了，直接返回"。一旦在这儿清掉，下次进来就会重建一整套，
  // 泄漏原样回来。这几个对象跨 disable/enable 一直有效，这正是这套做法成立的前提。
}

// 彻底释放 BLE，把内存还给 WiFi。
//
// 只 disable 是不够的：实测挂起状态下堆只有 33.7KB / 最大块 19.4KB，而 WiFi 协议栈要
// ~36KB，esp_wifi_init 直接返回 ESP_ERR_NO_MEM。这块板子没 PSRAM，BLE HID 和 WiFi
// 就是不可能同时存在的——必须真的 deinit。
//
// 而 deinit 一次就要付一次库泄漏（~13KB 最大连续块，见上面那段），所以这一步要尽量晚做：
// 在蓝牙里进进出出一直是零开销的 disable/enable，只有当人真的去用别的 app 时才在这儿
// 付一次。代价从"每进一次蓝牙"降到"每次蓝牙↔其它 app 的来回"。
void btReleaseForOtherApps() {
  if (btDevList) {
    delete[] btDevList;
    btDevList = nullptr;
    btDevCount = 0;
  }
  if (!bleInited || !bleSuspended) return;

  // ⚠️ 拆除顺序必须是 IDF 规定的：bluedroid disable → bluedroid deinit → controller
  // disable → controller deinit（BLEDevice::deinit 自己也是这个顺序，BLEDevice.cpp:652-655）。
  // 但走到这里时 btExit() 已经把 **controller 也 disable 了**，于是以前直接调
  // BLEDevice::deinit(false) 的实际顺序变成：
  //     bluedroid disable → controller disable →（稍后）bluedroid deinit → controller deinit
  // 也就是在 controller 已停的状态下拆 host 栈。esp_bluedroid_deinit() 是同步的：把
  // BTC_MAIN_ACT_DEINIT 丢给 BTC 任务，然后 future_await() **无限期**等它回话
  // （IDF v4.4.7 esp_bt_main.c:203 → future.c:78 OSI_SEM_MAX_TIMEOUT）。BTC 任务那边
  // btc_deinit_bluetooth() 要依次拆 HCI/BTU 线程，每个线程 join 只等 1 秒、超时就
  // vTaskDelete 硬杀（osi/thread.c:197-201）；被硬杀的线程若恰好持着 alarm_mutex 之类
  // 的锁，后面 osi_alarm_deinit() 的 OSI_MUTEX_MAX_TIMEOUT 就永远拿不到（alarm.c:102），
  // future 永不 ready，主循环永久阻塞——跟 wip 分支面包屑"停在 deinit 前、deinit 后从未
  // 出现"、Saved PC 落在 idle 的 waiti 完全吻合。这条链是**推断**，没在机器上坐实；
  // 所以下面每一步都埋了面包屑（0xB0..0xB5），再挂一次就能直接读出是哪一步。
  //
  // 修法：先把 controller 重新 enable 回来（内存本来就占着，这一步不申请新堆；此时
  // Wi-Fi 由看门狗按 btHoldsHeap() 压着没起，不存在共存问题），让 bluedroid deinit
  // 在 controller 活着时跑，恢复成 IDF 规定的顺序。
  bcMark(0xB0);                                       // release: 开始
  esp_err_t e = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (e != ESP_OK) { bcMark(0xBE); bcMark((uint8_t)e); log_w("bt ctrl re-enable: %d", (int)e); }
  bcMark(0xB1);                                       // controller 已重新 enable
  e = esp_bluedroid_deinit();
  if (e != ESP_OK) { bcMark(0xBE); bcMark((uint8_t)e); log_w("bluedroid deinit: %d", (int)e); }
  bcMark(0xB2);                                       // bluedroid deinit 返回（嫌疑最大的一步）
  e = esp_bt_controller_disable();
  if (e != ESP_OK) { bcMark(0xBE); bcMark((uint8_t)e); log_w("bt ctrl disable: %d", (int)e); }
  bcMark(0xB3);
  e = esp_bt_controller_deinit();
  if (e != ESP_OK) { bcMark(0xBE); bcMark((uint8_t)e); log_w("bt ctrl deinit: %d", (int)e); }
  bcMark(0xB4);
  // 协议栈已经拆完，这里只为把库里的 initialized 清回 false（下次 init 才会真做）。
  // 它内部那四个 IDF 调用此刻全部立即返回 ESP_ERR_INVALID_STATE，不会再碰协议栈。
  BLEDevice::deinit(false);   // ⚠️ 必须 false，true 会让下次 init 整段跳过（见上）
  bcMark(0xB5);                                       // release: 完成
  bleInited = false;
  bleSuspended = false;
  hidStarted = false;
  btConnected = false;
  hid = nullptr;              // 底层已失效，置空，下次进蓝牙重建一套
  inputKB = nullptr;
  inputMedia = nullptr;
  btReleaseCount++;
}

void btKeyboardSendRaw(uint8_t modifier, uint8_t keycode) {
  if (!inputKB || !btConnected) return;
  uint8_t press[8] = { modifier, 0, keycode, 0, 0, 0, 0, 0 };
  inputKB->setValue(press, 8);
  inputKB->notify();
  delay(6);
  uint8_t release[8] = { 0 };
  inputKB->setValue(release, 8);
  inputKB->notify();
}

// 把可打印 ASCII 字符转成 HID keycode+modifier 发出去；用不了的（比如控制字符）直接忽略
void btKeyboardSendKey(char k) {
  if (!btConnected) return;
  uint8_t ascii = (uint8_t)k;
  if (ascii >= KEYMAP_SIZE) return;
  uint8_t usage = keymap[ascii].usage;
  if (usage == 0) return;
  btKeyboardSendRaw(keymap[ascii].modifier, usage);
}

// 键码 → 裸 HID usage。keymap 那张表覆盖 ASCII 和 F1..F12/方向/Home 这些 FUNCTION_KEY，
// 表里没有的（Esc / 前向删除 / End）用 keyboard_adv.h 里那几个 KX_* 私有码补上。
static uint8_t usageFor(uint8_t code, uint8_t& modOut) {
  switch (code) {
    case kbd::KX_ESC: return 0x29;
    case kbd::KX_DEL: return 0x4C;
    case kbd::KX_END: return 0x4D;
    case kbd::KX_BACK:                  // 本机 UI 键，永远不转发给主机
    case kbd::KX_FN_ENTER: return 0;
    default:
      if (code >= KEYMAP_SIZE) return 0;
      modOut |= keymap[code].modifier;   // '!' '?' 这类字符本身就自带 Shift
      return keymap[code].usage;
  }
}

// 给这个键起个人看的名字，画在"最后发出去的组合键"那一行
static String keyLabel(uint8_t code) {
  switch (code) {
    case '\n': return "Enter";
    case '\t': return "Tab";
    case '\b': return "Bksp";
    case ' ':  return "Space";
    case 148:  return "Right";
    case 149:  return "Left";
    case 150:  return "Down";
    case 151:  return "Up";
    case 145:  return "Home";
    case kbd::KX_ESC: return "Esc";
    case kbd::KX_DEL: return "Del";
    case kbd::KX_END: return "End";
    default:
      if (code >= 128 && code <= 139) return String("F") + (int)(code - 127);
      if (code >= 0x20 && code <= 0x7e) return String((char)code);
      return "?";
  }
}

// 粘滞 Shift 的情形下，本机读到的还是小写/未上档的字符，但主机收到的是上档字符。
// 回显要照主机那份来，否则屏幕上写着 a、对面出来个 A，看着像发错了。
static char shiftedEcho(char c, uint8_t mod) {
  if (!(mod & kbd::MOD_SHIFT)) return c;
  if (c >= 'a' && c <= 'z') return (char)(c - 32);
  static const char FROM[] = "`1234567890-=[]\\;',./";
  static const char TO[]   = "~!@#$%^&*()_+{}|:\"<>?";
  const char* p = c ? strchr(FROM, c) : nullptr;
  return p ? TO[p - FROM] : c;
}

// ---- 键盘页的显示状态：回显 + 最后一次组合键 + 计数 ----
static const size_t KB_ECHO_MAX = 72;      // 两行 × 36 字符
static String   kbEcho;
static String   kbLastChord;
static uint32_t kbSentCount = 0;
static bool     kbDropped = false;         // 有按键因为没连上被丢掉

// IME 模式。主机上开着中文输入法时，回显框那套"模拟主机那一行"的做法必然失真：
// 屏幕上是拼音、主机上是汉字，而且退格删的是输入法的候选缓冲区，本机根本无从得知
// 删掉了什么——上屏一次就彻底错位。所以这个模式下回显退化成老实的"本机键流"，
// 退格只记一个 `<` 标记，不去猜主机的文本。顺带关掉长按连发：连发退格会把整串候选
// 一口气吃光，而屏幕上显示的删除量跟实际完全对不上。
static bool kbIme = false;   // 跨进出保留（不在 ResetEcho 里清），少一次来回切

void btKeyboardResetEcho() {
  kbEcho = ""; kbLastChord = ""; kbSentCount = 0; kbDropped = false;
}
bool btKeyboardImeMode() { return kbIme; }
void btKeyboardToggleIme() { kbIme = !kbIme; kbEcho = ""; }   // 语义变了，旧回显没意义

// 键盘页的统一入口：带修饰键转发一个键，并更新屏幕上的回显。
void btKeyboardType(char k, uint8_t mods) {
  uint8_t code = (uint8_t)k;
  uint8_t mod = mods;
  uint8_t usage = usageFor(code, mod);
  if (!usage) return;

  if (!btConnected) { kbDropped = true; return; }
  btKeyboardSendRaw(mod, usage);
  kbSentCount++;
  kbDropped = false;

  // 组合键那一行：只要按了 Ctrl/Alt/Cmd 就写全名，纯打字（含 Shift）不写，免得刷屏
  if (mods & (kbd::MOD_CTRL | kbd::MOD_ALT | kbd::MOD_GUI)) {
    String s;
    if (mods & kbd::MOD_CTRL)  s += "Ctrl+";
    if (mods & kbd::MOD_ALT)   s += "Alt+";
    if (mods & kbd::MOD_GUI)   s += "Cmd+";
    if (mods & kbd::MOD_SHIFT) s += "Shift+";
    kbLastChord = s + keyLabel(code);
  } else {
    kbLastChord = keyLabel(code);
  }

  // 回显。两种语义，由 IME 模式切换（见 kbIme 那段注释）：
  // 关＝模拟主机那一行；开＝老实记本机键流，不猜主机状态。
  if (code >= 0x20 && code <= 0x7e) {
    kbEcho += shiftedEcho((char)code, mod);
  } else if (kbIme) {
    // 键流语义：只记"我按了什么"，不猜主机变成了什么
    if      (code == '\b') kbEcho += '<';    // 退格（删掉的是候选还是正文，本机不知道）
    else if (code == '\n') kbEcho += '|';    // 回车/上屏
    else if (code == '\t') kbEcho += ' ';
  } else {
    // 文本语义：退格就真的退一格，回显尽量接近主机上那一行的样子
    if      (code == '\b' && kbEcho.length()) kbEcho.remove(kbEcho.length() - 1);
    // ⚠️ 回车要清空，不能像 Tab 那样追加空格。这一框声称画的是"主机上那一行"，而按下
    //    回车之后主机那一行已经提交、新的一行是空的——继续挂着上一行就是在假装知道主机
    //    状态，正是 IME 模式刻意要避免的那种错（见 kbIme 那段）。原来写成 += ' ' 时，
    //    打 hello↵world 会显示成 "hello world"，而主机上其实是两行。
    //    代价是回车后看不到刚发出去的那一行了；要回看发了什么，右上角有累计键数、
    //    下面一行有最后发出去的组合键。IME 模式不受影响，那边 '|' 是键流记号，本来就该留。
    else if (code == '\n') kbEcho = "";
    else if (code == '\t') kbEcho += ' ';
  }
  if (kbEcho.length() > KB_ECHO_MAX) kbEcho.remove(0, kbEcho.length() - KB_ECHO_MAX);
}

bool btIsConnected() { return btConnected; }

uint8_t btKeyUsageFor(char c) {
  uint8_t ascii = (uint8_t)c;
  if (ascii >= KEYMAP_SIZE) return 0;
  return keymap[ascii].usage;
}

void drawBtKeyboard() {
  cv.fillScreen(TFT_BLACK);

  char cnt[24];
  snprintf(cnt, sizeof(cnt), "%s%lu keys", kbIme ? "IME  " : "", (unsigned long)kbSentCount);
  drawPageHeader("BLE Keyboard", cnt, kbIme ? TFT_CYAN : 0);

  cv.setTextSize(1);

  // ---- 状态行。三选一，不叠加——"advertising..." 那句很长，再往右塞一个角标必然重叠 ----
  cv.setTextDatum(top_left);
  if (btConnected) {
    cv.setTextColor(TFT_GREEN, TFT_BLACK);
    cv.drawString("connected", 6, 16);
  } else if (kbDropped) {
    // 没连上却在打字：明确说清楚按键是被丢掉了，不是发出去没生效
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.drawString("not paired - keys dropped", 6, 16);
  } else {
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("advertising - pair \"Cardputer ADV\"", 6, 16);
  }

  // ---- 回显框：主机那边这一行大概长什么样 ----
  const int boxY = 28, boxH = 26, chars = 36;
  cv.drawRect(5, boxY, SW - 10, boxH, DIM_BORDER);
  cv.setTextDatum(top_left);
  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  int len = kbEcho.length();
  int firstLen = len > chars ? len - chars : 0;   // 满了就把旧的推到上一行
  if (firstLen > 0) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString(kbEcho.substring(0, firstLen), 9, boxY + 4);
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  // 没满一行就画在第一行——固定画第二行的话，空框子里的光标会孤零零吊在底下
  String tail = kbEcho.substring(firstLen);
  cv.drawString(tail + "_", 9, boxY + (firstLen > 0 ? 15 : 4));

  // ---- 修饰键指示：按住=实心，单击锁定=描边（下一个键用完自动解开）----
  struct Chip { const char* name; uint8_t bit; };
  static const Chip CHIPS[4] = {
    {"CTRL", kbd::MOD_CTRL}, {"SHIFT", kbd::MOD_SHIFT},
    {"ALT", kbd::MOD_ALT},   {"CMD", kbd::MOD_GUI},
  };
  const uint8_t mods = kbd::modMask(), sticky = kbd::modSticky();
  const int chipY = 60, chipW = 54, chipH = 15;
  for (int i = 0; i < 4; i++) {
    int x = 5 + i * (chipW + 4);
    bool on = mods & CHIPS[i].bit, lock = sticky & CHIPS[i].bit;
    if (on && !lock) cv.fillRect(x, chipY, chipW, chipH, ACCENT);
    else             cv.drawRect(x, chipY, chipW, chipH, on ? ACCENT : DIM_BORDER);
    cv.setTextDatum(middle_center);
    cv.setTextColor(on ? (lock ? ACCENT : TFT_BLACK) : TFT_DARKGREY, on && !lock ? ACCENT : TFT_BLACK);
    cv.drawString(CHIPS[i].name, x + chipW / 2, chipY + chipH / 2);
  }

  // ---- 最后发出去的组合键 ----
  cv.setTextDatum(top_left);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString("sent:", 6, 80);
  cv.setTextColor(ACCENT, TFT_BLACK);
  cv.drawString(kbLastChord.length() ? kbLastChord : String("-"), 42, 80);

  // ---- 按键提示。裸 ` 要留给主机打反引号，所以退出挪到了 Fn+` ----
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  // ⚠️ 最后一行必须画完在 y=123 之前：Debug 条从 SH-11(=124) 起整条填黑，
  // 越界的话那行字只会剩最顶上一排像素（main.cpp drawDebugBar 里那段注释说的就是这个）。
  // 顺带把三行整体上提，middle 那块空档也一起收掉了。
  cv.drawString("Fn: tab esc  1-= F1-F12  ;./, arrows", 6, 93);
  cv.drawString("Fn: bksp del  [ ] home/end", 6, 104);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("Fn+` back  Fn+ent IME  tap mod=lock", 6, 115);
}

// =============================================================================
// 媒体遥控（HID Consumer Control）
// 跟键盘是同一个 HID 设备、同一次配对，只是走 REPORT_ID_MEDIA 这个报表。
// =============================================================================
void btMediaStart() {
  btEnsureInit();
  btHidSetup();
  BLEDevice::startAdvertising();
}

void btMediaSend(BtMediaKey k) {
  if (!inputMedia || !btConnected) return;
  uint8_t press = (uint8_t)(1u << (uint8_t)k);   // 位序 = BtMediaKey 枚举序，跟报表描述符对齐
  inputMedia->setValue(&press, 1);
  inputMedia->notify();
  delay(6);
  uint8_t release = 0;
  inputMedia->setValue(&release, 1);
  inputMedia->notify();
}

void drawBtMedia() {
  cv.fillScreen(TFT_BLACK);
  drawPageHeader("Media Remote");

  cv.setTextDatum(top_center); cv.setTextSize(1);
  if (btConnected) {
    cv.setTextColor(TFT_GREEN, TFT_BLACK);
    cv.drawString("connected", SW / 2, 24);
  } else {
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("advertising - pair \"Cardputer ADV\"", SW / 2, 24);
  }

  // 按键表：左右两列，跟机器上导航键的方位对应（; 上 . 下 , 左 / 右）
  struct Hint { const char* key; const char* what; };
  static const Hint L[] = { {",", "prev"}, {"/", "next"}, {"Enter", "play/pause"} };
  static const Hint R[] = { {";", "vol +"}, {".", "vol -"}, {"m", "mute"} };
  const int y0 = 44, rowH = 22;
  for (int i = 0; i < 3; i++) {
    int y = y0 + i * rowH;
    cv.setTextDatum(top_right); cv.setTextSize(1);
    cv.setTextColor(ACCENT, TFT_BLACK);      cv.drawString(L[i].key, 52, y);
    cv.setTextDatum(top_left);
    cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK); cv.drawString(L[i].what, 60, y);
    cv.setTextDatum(top_right);
    cv.setTextColor(ACCENT, TFT_BLACK);      cv.drawString(R[i].key, 170, y);
    cv.setTextDatum(top_left);
    cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK); cv.drawString(R[i].what, 178, y);
  }

  // s=stop 一直是支持的（见 main.cpp 的按键分发），但上面那张两列表正好排满六格，
  // 它就一直没露过面——挂到底部提示里，免得成了只有 README 才知道的隐藏键。
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString("` back    s stop", 8, SH - 4);
}

// =============================================================================
// 找物雷达：锁定一个设备持续跟它的 RSSI，画时间曲线，越近响得越急。
//
// 扫描回调跑在 BLE 协议栈自己的任务里，不是主循环。所以回调里只写两个标量
// （最新 RSSI + 时间戳），历史曲线和蜂鸣全部由主循环的 btRadarUpdate() 去做——
// 这样两边不用加锁，也不会因为回调里干重活把协议栈拖慢。
//
// ⚠️ 按 MAC 跟踪的固有限制：用随机地址(RPA)的设备（AirPods、防丢标签、手机）
// 每十几分钟换一次地址，换掉就跟丢，得回扫描列表重新选。地址类型在设备详情页看得到。
// =============================================================================
static const int  RADAR_N = 112;         // 历史点数，画图时一点占 2px
static int8_t     radarHist[RADAR_N];
static int        radarHistN = 0;
static BLEAddress radarTargetAddr("00:00:00:00:00:00");
static bool       radarActive = false;
static bool       radarBeep = true;
static String     radarName;
// 1m 处期望收到的 RSSI。⚠️ 不能直接拿广播里的 TX Power 当这个值——AD type 0x0A 报的是
// 发射功率，不是 1m 处的接收电平，两者差着一整段路径损耗。实测这台板子上就有设备报
// txpwr=0dBm、实际读数 -39dBm，直接代进去会算出三十多米。iBeacon 的经验常数是"0dBm
// 发射对应 1m 处 -59dBm"，所以这里统一按 txPower-59 折算；没报 TX Power 就当它是 0dBm。
static int8_t     radarTxRef = -59;
static volatile int      radarRssiRaw = -127;
static volatile uint32_t radarLastSeenMs = 0;
static float      radarEma = -90;
static bool       radarEmaInit = false;
static uint32_t   radarLastSampleMs = 0, radarLastBeepMs = 0;
static const uint32_t RADAR_SAMPLE_MS = 150;
static const uint32_t RADAR_LOST_MS   = 4000;

class BtRadarCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    if (d.getAddress() != radarTargetAddr) return;
    radarRssiRaw = d.getRSSI();
    radarLastSeenMs = millis();
  }
};
static BtRadarCallbacks btRadarCallbacks;

void btRadarStart(int devIndex) {
  if (!btDevList || devIndex < 0 || devIndex >= btDevCount) return;
  BtDevice& d = btDevList[devIndex];
  radarTargetAddr = BLEAddress(d.addr.c_str());
  radarName = d.name;
  radarTxRef = d.haveTXPower ? (int8_t)(d.txPower - 59) : (int8_t)-59;
  radarHistN = 0;
  radarEmaInit = false;
  radarRssiRaw = -127;
  radarLastSeenMs = 0;
  radarLastSampleMs = radarLastBeepMs = millis();

  btEnsureInit();
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&btRadarCallbacks, true /* wantDuplicates：要的就是重复包，每个包一次 RSSI */);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);              // 窗口几乎等于间隔 = 近乎连续收，RSSI 更新才够密
  scan->start(0, nullptr, false);   // duration=0 一直扫；这个重载不阻塞，主循环照常跑
  radarActive = true;
}

void btRadarStop() {
  if (!radarActive) return;
  BLEScan* scan = BLEDevice::getScan();
  scan->stop();
  scan->clearResults();
  // 回调对象是本文件的静态实例，扫描停了之后不再被调用；但下次普通扫描会重新
  // setAdvertisedDeviceCallbacks 成 btScanCallbacks，这里不用还原。
  radarActive = false;
}

void btRadarToggleBeep() { radarBeep = !radarBeep; }

static bool radarLost() {
  return radarLastSeenMs == 0 || (millis() - radarLastSeenMs) > RADAR_LOST_MS;
}

// 路径损耗反推距离。n=2.5 是室内多径的常见取值；这个数只配当"近了还是远了"的粗指示，
// 别当尺子用——人体遮挡、天线朝向、金属家具都能轻松让它差一倍。
static float radarDistM(float rssi) {
  float d = powf(10.0f, ((float)radarTxRef - rssi) / (10.0f * 2.5f));
  if (d < 0.1f) d = 0.1f;
  if (d > 99.0f) d = 99.0f;
  return d;
}

void btRadarUpdate() {
  if (!radarActive) return;
  uint32_t now = millis();

  if (now - radarLastSampleMs >= RADAR_SAMPLE_MS) {
    radarLastSampleMs = now;
    if (!radarLost()) {
      int raw = radarRssiRaw;
      if (!radarEmaInit) { radarEma = raw; radarEmaInit = true; }
      else                radarEma = radarEma * 0.7f + raw * 0.3f;
    }
    int8_t v = radarLost() ? -127 : (int8_t)lroundf(radarEma);   // -127 = 这一格没信号，画图时留空
    if (radarHistN < RADAR_N) radarHist[radarHistN++] = v;
    else { memmove(radarHist, radarHist + 1, RADAR_N - 1); radarHist[RADAR_N - 1] = v; }
  }

  // 音高提示。第一版是"越近点得越快"的盖革计数器式（35ms 短促咔哒 + 最快 80ms 一声），
  // 听着太吵也太机械。改成：音高走五声音阶、音符拉长到 90ms、最快也隔 300ms——
  // 变成一串往上爬的清脆提示音，靠**音高**而不是靠密度来表达远近，耳朵舒服得多。
  if (radarBeep && !radarLost() && radarEmaInit) {
    float t = (radarEma + 95.0f) / 50.0f;          // -95..-45 映射到 0..1
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    // C 大调五声音阶，两个八度。量化到音阶上才有"旋律感"，连续扫频听着像警报。
    static const uint16_t SCALE[] = { 523, 587, 659, 784, 880, 1047, 1175, 1319, 1568, 1760 };
    const int SCALE_N = sizeof(SCALE) / sizeof(SCALE[0]);
    int step = (int)(t * (SCALE_N - 1) + 0.5f);
    uint32_t gap = (uint32_t)(1500 - t * 1200);    // 远 1.5s 一声，近 0.3s 一声
    if (now - radarLastBeepMs >= gap) {
      radarLastBeepMs = now;
      M5.Speaker.setVolume(volVal());
      M5.Speaker.tone(SCALE[step], 90);
    }
  }
}

void drawBtRadar() {
  cv.fillScreen(TFT_BLACK);
  const bool lost = radarLost();
  const char* tName = radarName.length() ? radarName.c_str() : "(unnamed target)";
  drawPageHeader(clipToWidth(tName, 120),
                 lost ? "LOST" : (radarBeep ? "AUDIO ON" : "MUTED"),
                 lost ? 0xF800 : (radarBeep ? 0x07E0 : 0x7BEF));

  // 左侧战术声纳雷达舱 (x = 8, y = 15, w = 104, h = 103)
  const int rcx = 60, rcy = 66;
  cv.fillRoundRect(8, 15, 104, 103, 4, 0x0002);
  cv.drawRoundRect(8, 15, 104, 103, 4, lost ? 0xF800 : 0x07FF);

  // 赛博边角准星刻度
  cv.drawFastHLine(8, 15, 6, 0x07FF); cv.drawFastVLine(8, 15, 6, 0x07FF);
  cv.drawFastHLine(106, 15, 6, 0x07FF); cv.drawFastVLine(111, 15, 6, 0x07FF);
  cv.drawFastHLine(8, 117, 6, 0x07FF); cv.drawFastVLine(8, 112, 6, 0x07FF);
  cv.drawFastHLine(106, 117, 6, 0x07FF); cv.drawFastVLine(111, 112, 6, 0x07FF);

  // 雷达同心圆刻度 (10m, 3m, 1m)
  cv.drawCircle(rcx, rcy, 42, 0x18C3); // 10m
  cv.drawCircle(rcx, rcy, 28, 0x1082); // 3m
  cv.drawCircle(rcx, rcy, 14, 0x2492); // 1m
  cv.drawFastHLine(rcx - 42, rcy, 85, 0x1082);
  cv.drawFastVLine(rcx, rcy - 42, 85, 0x1082);

  // 雷达旋转扫描扇区光芒
  if (!lost) {
    float ang = (float)(millis() % 2400) * (2.0f * 3.14159f / 2400.0f);
    int sx = rcx + (int)(cosf(ang) * 42.0f);
    int sy = rcy + (int)(sinf(ang) * 42.0f);
    cv.drawLine(rcx, rcy, sx, sy, 0x07E0);

    // 拖尾光芒
    int sx1 = rcx + (int)(cosf(ang - 0.08f) * 41.0f);
    int sy1 = rcy + (int)(sinf(ang - 0.08f) * 41.0f);
    cv.drawLine(rcx, rcy, sx1, sy1, 0x0540);

    int sx2 = rcx + (int)(cosf(ang - 0.16f) * 40.0f);
    int sy2 = rcy + (int)(sinf(ang - 0.16f) * 40.0f);
    cv.drawLine(rcx, rcy, sx2, sy2, 0x0210);

    // 目标 Ping 点位置映射
    float dM = radarDistM(radarEma);
    float rDist = 10.0f;
    if (dM < 0.5f)       rDist = 8.0f;
    else if (dM < 1.0f)  rDist = 8.0f + (dM - 0.5f) * 12.0f;
    else if (dM < 3.0f)  rDist = 14.0f + (dM - 1.0f) * 7.0f;
    else if (dM < 10.0f) rDist = 28.0f + (dM - 3.0f) * 2.0f;
    else                 rDist = 41.0f;

    // 固定在第一象限方向 (-45度方位)，直观感知远近
    const float tAng = -0.785f;
    int bx = rcx + (int)(cosf(tAng) * rDist);
    int by = rcy + (int)(sinf(tAng) * rDist);

    // 脉冲波纹扩散
    int pingR = (millis() / 45) % 18;
    if (pingR > 2) cv.drawCircle(bx, by, pingR, 0x07FF);

    // 目标高亮核心点
    cv.fillCircle(bx, by, 3, 0xFFFF);
    cv.drawCircle(bx, by, 4, 0x07E0);
  } else {
    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextDatum(middle_center);
    cv.setTextColor(0xF800, 0x0002);
    cv.drawString("SIGNAL", rcx, rcy - 6);
    cv.drawString("LOST", rcx, rcy + 6);
  }

  // 右侧双 Pod 遥测仪表舱 (x = 118, y = 15, w = 114, h = 103)
  const int rw = SW - 118 - 8;

  // 上 Pod: SIGNAL INTENSITY & PROXIMITY (y = 15..61, h = 47)
  cv.fillRoundRect(118, 15, rw, 47, 3, 0x0821);
  cv.drawRoundRect(118, 15, rw, 47, 3, lost ? 0xF800 : 0x18C3);

  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  cv.setTextDatum(top_left);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("SIGNAL LEVEL", 123, 18);

  if (lost) {
    cv.setTextSize(2); cv.setTextColor(0xF800, 0x0821);
    cv.drawString("-- dBm", 123, 29);
    cv.setTextSize(1); cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("OUT OF RANGE", 123, 48);
  } else {
    char b[16];
    int emaInt = (int)lroundf(radarEma);
    uint16_t sigCol = (emaInt >= -65) ? 0x07E0 : ((emaInt >= -80) ? 0xFDA0 : 0xF800);
    snprintf(b, sizeof(b), "%d", emaInt);
    cv.setTextSize(2); cv.setTextColor(sigCol, 0x0821);
    cv.drawString(b, 123, 29);

    cv.setTextSize(1); cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("dBm", 163, 35);

    // 距离估计与临近分区徽标
    float dM = radarDistM(radarEma);
    char distBuf[24];
    if (dM < 1.0f)       snprintf(distBuf, sizeof(distBuf), "IMM: ~%.1fm", dM);
    else if (dM < 3.0f)  snprintf(distBuf, sizeof(distBuf), "NEAR: ~%.1fm", dM);
    else                 snprintf(distBuf, sizeof(distBuf), "FAR: ~%.1fm", dM);
    cv.setTextColor(dM < 1.0f ? 0x07E0 : (dM < 3.0f ? 0x07FF : 0xFDA0), 0x0821);
    cv.drawString(distBuf, 123, 48);
  }

  // 下 Pod: 60s RSSI TREND SPARKLINE (y = 65..118, h = 53)
  cv.fillRoundRect(118, 65, rw, 53, 3, 0x0821);
  cv.drawRoundRect(118, 65, rw, 53, 3, 0x18C3);

  cv.setTextDatum(top_left);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("RSSI TREND", 123, 68);
  cv.setTextDatum(top_right);
  cv.setTextColor(0x4208, 0x0821);
  cv.drawString("60S", 118 + rw - 4, 68);

  // 波形图区域 (gx = 122, gy = 80, gw = 104, gh = 32)
  const int gx = 122, gy = 80, gw = rw - 8, gh = 32;
  cv.drawFastHLine(gx, gy, gw, 0x1082);          // -40dBm 顶刻度
  cv.drawFastHLine(gx, gy + gh / 2, gw, 0x0821); // -70dBm 中刻度
  cv.drawFastHLine(gx, gy + gh, gw, 0x18C3);     // -100dBm 底轴

  int prevY = -1, prevX = -1;
  int pts = min(radarHistN, gw);
  int startIdx = radarHistN - pts;
  for (int i = 0; i < pts; i++) {
    int val = radarHist[startIdx + i];
    if (val == -127) { prevY = -1; continue; }
    float t = (val + 100.0f) / 60.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int y = gy + gh - (int)(t * gh);
    int x = gx + (i * gw) / max(1, pts - 1);
    if (prevY >= 0) {
      cv.drawLine(prevX, prevY, x, y, 0x07FF);
    } else {
      cv.drawPixel(x, y, 0x07FF);
    }
    prevX = x;
    prevY = y;
    if (i == pts - 1 && !lost) {
      cv.fillCircle(x, y, 2, 0xFFFF);
    }
  }

  // 底部按键提示
  cv.drawFastHLine(0, 121, SW, 0x1082);
  const int footY = SH - 12;
  cv.setTextDatum(top_left);
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  int fx = 8;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("M", fx, footY); fx += 10;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(radarBeep ? "MUTE" : "SOUND", fx, footY); fx += 34;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", fx, footY); fx += 10;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("RESET", fx, footY); fx += 36;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", fx, footY); fx += 10;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("BACK", fx, footY);
}

// =============================================================================
// BLE 取样（串口 BTDUMP [秒]）：屏幕一次只看得到一个设备，横扫一片时用这个。
// 每个设备把 Service Data(AD 0x16) 和 Manufacturer Data(AD 0xFF) 原样打出来。
// Remote ID 走 BLE 的话特征是：svcUUID 含 fffa、Service Data 首字节 0x0D。
// =============================================================================
void btDumpRun(int seconds) {
  btEnsureInit();
  if (!btDevList) btDevList = new BtDevice[BT_SCAN_MAX];
  btDevCount = 0;
  Serial.printf("[btdump] scanning %ds...\n", seconds);
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&btScanCallbacks, true);
  scan->setActiveScan(true);
  scan->start(seconds, false);
  scan->stop();
  scan->clearResults();

  Serial.printf("[btdump] %d devices\n", btDevCount);
  for (int i = 0; i < btDevCount; i++) {
    BtDevice& d = btDevList[i];
    bool odid = d.haveSvcData && d.svcDataUUID.indexOf("fffa") >= 0;
    Serial.printf("[btdump] %-18s %s %4d dBm %s\n",
                  d.addr.c_str(), addrTypeName(d.addrType), d.rssi,
                  trunc(d.name, 20).c_str());
    if (d.haveService)  Serial.printf("[btdump]   uuid: %s\n", d.serviceUUID.c_str());
    if (d.haveManuf)    Serial.printf("[btdump]   mfg : %s\n", d.manufHex.c_str());
    if (d.haveSvcData)  Serial.printf("[btdump]   svc : %s  data: %s%s\n",
                                      d.svcDataUUID.c_str(), d.svcDataHex.c_str(),
                                      odid ? "   <== OpenDroneID" : "");
    // 解出来的 Remote ID。命中 0xFFFA 但这里一行都没有，就说明载荷不是标准 ODID
    // ——这时候上面那段 hex 就是判断它到底是什么的唯一依据。
    if (d.haveOdid) {
      const OdidResult& r = d.odid;
      if (r.haveBasic)
        Serial.printf("[btdump]   RID id  : %s  (%s / %s)\n", r.uasId,
                      odidIdTypeName(r.idType), odidUaTypeName(r.uaType));
      if (r.haveLoc) {
        Serial.printf("[btdump]   RID pos : %.7f, %.7f", r.lat, r.lon);
        if (r.haveHeight) Serial.printf("  agl=%.1fm", r.height);
        if (r.haveAlt)    Serial.printf("  alt=%.1fm", r.altGeo);
        if (r.haveSpeed)  Serial.printf("  spd=%.1fm/s", r.speed);
        if (r.heading >= 0) Serial.printf("  hdg=%d", r.heading);
        Serial.println();
      }
      if (r.haveSys)
        Serial.printf("[btdump]   RID plt : %.7f, %.7f  (%s)\n",
                      r.pilotLat, r.pilotLon, odidPilotLocName(r.pilotLocType));
      if (r.haveSelfId)     Serial.printf("[btdump]   RID desc: %s\n", r.selfId);
      if (r.haveOperatorId) Serial.printf("[btdump]   RID oper: %s\n", r.operatorId);
    }
  }
  Serial.println("[btdump] end");
  if (!isBleScreen(screen)) {
    btExit();
    btReleaseForOtherApps();
  }
}

// =============================================================================
// BLE 扩展广播取样（串口 BTEXT [秒]）
//
// 为什么必须单独写一套：Arduino 那个 BLEScan 只扫 **legacy 广播、1M PHY**。
// 而 ASTM F3411 里 Bluetooth 那条用的是 **BT5 Long Range + 扩展广播（Coded PHY）**——
// 用 BTDUMP 扫不到不等于 BLE 上没有，只是那套 API 根本看不见。
// 这里直接调 Bluedroid 的 ext scan：1M 和 Coded 两个 PHY 一起收。
//
// ⚠️ 回调必须用 BLEDevice::setCustomGapHandler() 挂，**不能**用
// esp_ble_gap_register_callback()。后者只能注册一个，会把 BLEDevice 自己装的顶掉；
// 而 BLEDevice 的 gapEventHandler 末尾是**无条件**转发给自定义回调的
// （BLEDevice.cpp 里的 m_customGapHandler），走这条两边都收得到。
//
// 这里踩过一次，而且是自己挖的坑：原来用的就是 esp_ble_gap_register_callback，
// 靠"跑完 deinit、下次 BLEDevice::init 把回调装回来"收拾。后来 BLE 生命周期改成
// 永不 deinit（见文件顶部那段），这个前提就没了——于是跑过一次 BTEXT 之后，
// BTDUMP / 扫描 / HID 全部静默失效直到复位。实测撞到过：BTDUMP 一行不回、零报错。
// =============================================================================
#include <esp_gap_ble_api.h>

static volatile bool btExtStarted = false;
static volatile int  btExtSeen = 0, btExtOdid = 0;

static void btExtGapCb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  if (event == ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT) {
    esp_ble_gap_start_ext_scan(0, 0);      // duration=0 一直扫，靠我们自己停
    btExtStarted = true;
    return;
  }
  if (event != ESP_GAP_BLE_EXT_ADV_REPORT_EVT) return;
  const esp_ble_gap_ext_adv_reprot_t& r = param->ext_adv_report.params;
  btExtSeen++;

  // 走一遍 AD 结构，专门找 Service Data(0x16) 里的 16-bit UUID 0xFFFA
  bool odid = false;
  OdidResult rid;
  bool ridOk = false;
  for (int i = 0; i + 1 < r.adv_data_len; ) {
    uint8_t l = r.adv_data[i];
    if (l == 0 || i + 1 + l > r.adv_data_len) break;
    uint8_t t = r.adv_data[i + 1];
    // AD 结构：[len][type][UUID 小端 2 字节][应用码 0x0D][计数器][25 字节消息]
    // len 覆盖 type + UUID + 载荷，所以载荷长度是 l-3、从 i+4 开始。
    if (t == 0x16 && l >= 3 && r.adv_data[i + 2] == 0xFA && r.adv_data[i + 3] == 0xFF) {
      odid = true;
      // 跳过应用码 0x0D，从计数字节交给解码器
      if (l >= 5 && r.adv_data[i + 4] == 0x0D)
        ridOk = ridDecode(&r.adv_data[i + 5], l - 4, rid);
    }
    i += 1 + l;
  }
  if (odid) btExtOdid++;

  // PHY 编号：1=1M 2=2M 3=Coded。primary 是 Coded 就说明真的是 Long Range 广播。
  Serial.printf("[btext] %02X:%02X:%02X:%02X:%02X:%02X rssi=%d phy=%d/%d sid=%u len=%u%s\n",
                r.addr[0], r.addr[1], r.addr[2], r.addr[3], r.addr[4], r.addr[5],
                r.rssi, (int)r.primary_phy, (int)r.secondly_phy, r.sid, r.adv_data_len,
                odid ? "  <== OpenDroneID (fffa)" : "");
  Serial.print("[btext]   ");
  for (int i = 0; i < r.adv_data_len && i < 40; i++) Serial.printf("%02X ", r.adv_data[i]);
  Serial.println();

  // 这里不做跨包合并：ext scan 是一条报告一行地打，合并需要按地址建表，而 BT5 的
  // Remote ID 多数用随机地址、还会轮换。要凑齐字段看多打几行即可，原始 hex 就在上面。
  if (ridOk) {
    if (rid.haveBasic)
      Serial.printf("[btext]   RID id  : %s  (%s / %s)\n", rid.uasId,
                    odidIdTypeName(rid.idType), odidUaTypeName(rid.uaType));
    if (rid.haveLoc) {
      Serial.printf("[btext]   RID pos : %.7f, %.7f", rid.lat, rid.lon);
      if (rid.haveHeight) Serial.printf("  agl=%.1fm", rid.height);
      if (rid.haveSpeed)  Serial.printf("  spd=%.1fm/s", rid.speed);
      if (rid.heading >= 0) Serial.printf("  hdg=%d", rid.heading);
      Serial.println();
    }
    if (rid.haveSys)
      Serial.printf("[btext]   RID plt : %.7f, %.7f  (%s)\n",
                    rid.pilotLat, rid.pilotLon, odidPilotLocName(rid.pilotLocType));
    if (rid.haveSelfId)     Serial.printf("[btext]   RID desc: %s\n", rid.selfId);
    if (rid.haveOperatorId) Serial.printf("[btext]   RID oper: %s\n", rid.operatorId);
  }
}

void btExtScanRun(int seconds) {
  // ⚠️ 每次都先彻底拆掉再重建。上一轮结束时 deinit 过、下一轮 BLEDevice::init 回来之后，
  // 控制器状态不干净，实测第二次跑会 reports=0 完全收不到东西——而且没有任何报错，
  // 极容易被误读成"目标没在播"。强制走一遍干净的 init 是唯一稳的做法。
  btExit();
  btEnsureInit();                       // 借 BLEDevice 把控制器和 Bluedroid 拉起来
  btExtSeen = btExtOdid = 0;
  btExtStarted = false;

  BLEDevice::setCustomGapHandler(btExtGapCb);   // 挂在 BLEDevice 的回调之后，不顶掉它

  esp_ble_ext_scan_params_t sp = {};
  // ⚠️ 这里不能填 RANDOM：没先调 esp_ble_gap_set_rand_addr 的话控制器会报
  // "No random address yet" 然后 LE UpdateOwnType 失败，表现是这一轮 reports=0
  // 完全收不到东西——而且是间歇性的，很容易误判成"目标没在播"。
  sp.own_addr_type   = BLE_ADDR_TYPE_PUBLIC;
  sp.filter_policy   = BLE_SCAN_FILTER_ALLOW_ALL;
  sp.scan_duplicate  = BLE_SCAN_DUPLICATE_DISABLE;   // 要看重复包，别去重
  sp.cfg_mask        = (esp_ble_ext_scan_cfg_mask_t)
                       (ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK | ESP_BLE_GAP_EXT_SCAN_CFG_CODE_MASK);
  sp.uncoded_cfg     = { BLE_SCAN_TYPE_ACTIVE, 80, 80 };   // interval/window 单位 0.625ms，等于连续收
  sp.coded_cfg       = { BLE_SCAN_TYPE_ACTIVE, 80, 80 };

  Serial.printf("[btext] ext scan %ds (1M + Coded PHY)...\n", seconds);
  esp_err_t e = esp_ble_gap_set_ext_scan_params(&sp);
  if (e != ESP_OK) { Serial.printf("[btext] set_ext_scan_params failed: %d\n", (int)e); }

  uint32_t end = millis() + (uint32_t)seconds * 1000;
  while ((int32_t)(end - millis()) > 0) delay(20);

  esp_ble_gap_stop_ext_scan();
  // 把"扫描根本没起来"和"起来了但没收到"区分开——这两种情况的结论完全相反
  if (!btExtStarted) Serial.println("[btext] ⚠️ 扫描没启动（没收到 SET_EXT_SCAN_PARAMS_COMPLETE），本轮结果无效");
  Serial.printf("[btext] done. started=%d reports=%d  with fffa=%d\n",
                (int)btExtStarted, btExtSeen, btExtOdid);
  // 摘掉自定义回调，别让它继续截事件。以前这里靠 deinit 顺带复位，现在不 deinit 了，必须显式摘。
  BLEDevice::setCustomGapHandler(nullptr);
  btExit();                              // 只是挂起 BLE，对象和 GATT 注册都留着
  if (!isBleScreen(screen)) {
    btReleaseForOtherApps();
  }
}


// =============================================================================
// PC 主导模式 BLE 流（协议 §8.6）：BLE ON / BLE OFF
//
// 用 ext scan（1M + Coded PHY 一起收，legacy 广播也在内），不用 Arduino 的 BLEScan——
// 后者只看得见 legacy/1M，而且每包都要建 BLEAdvertisedDevice（堆分配）。
// 回调跑在蓝牙任务里，只把原始报告塞队列；AD 解析、ODID 解码、按地址限频、出 JSON
// 全在主循环 btStreamPump() 里做，回调里不碰 Serial 也不分配。
//
// BLE 生命周期沿用上面的规矩：init 一次、之后只 enable/disable。停流只是 btExit()（挂起），
// 不 deinit——在 PC MODE 里反复开关 BLE 零泄漏。真要用 Wi-Fi 时（SCAN WIFI / RID ON）
// 由那边先调 btReleaseForOtherApps() 付一次库泄漏。
// =============================================================================
#include <freertos/queue.h>
#include "ridapp.h"

struct BtRawAdv {
  uint8_t addr[6];
  uint8_t atype;
  int8_t  rssi;
  uint8_t phy;      // primary PHY：1=1M 3=Coded
  uint8_t len;
  uint8_t data[96]; // 名字可能被截，Remote ID 的 AD 结构在 31 字节内，够用
};

struct BtStreamEnt {
  bool used;
  uint8_t addr[6];
  uint8_t atype;
  uint32_t lastEmit;
  uint32_t lastSeen;
  uint32_t sig;       // 合并后名字/厂商/服务 UUID 的摘要：变了就立即再报一次
  OdidResult* od;     // 只有出过 ODID 的地址才分配
  char name[25];      // 同一地址的广播包和扫描响应包字段不同（名字常只在响应包里），
  uint8_t mfg[12]; int mfgLen;   // 所以按地址合并保留；不合并的话两种包交替到达会让摘要来回跳、
  uint16_t svc16;                // 每个包都当"变了"重复上报
};

static const int BTS_MAX = 32;
static const uint32_t BTS_KEEPALIVE_MS = 2000;   // 内容没变时，同一地址最快多久再报一次（带新 RSSI）
static const uint32_t BTS_ODID_MIN_MS  = 250;    // 有新 ODID 数据时的限频，跟 Wi-Fi 流一致
static bool btsOn = false;
static volatile bool btsStarted = false;         // 控制器确认扫描启动
static uint32_t btsStartMs = 0;
static QueueHandle_t btsQ = nullptr;
static BtStreamEnt* btsTab = nullptr;
static volatile uint32_t btsPkts = 0, btsDrop = 0;
static int btsDevs = 0;

bool btStreamIsActive() { return btsOn; }
int  btStreamDevCount() { return btsOn ? btsDevs : 0; }
uint32_t btStreamPktCount() { return btsPkts; }

static void btsGapCb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  if (!btsOn) return;
  if (event == ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT) {
    esp_ble_gap_start_ext_scan(0, 0);   // duration=0 一直扫，靠 BLE OFF 停
    return;
  }
  if (event == ESP_GAP_BLE_EXT_SCAN_START_COMPLETE_EVT) {
    btsStarted = (param->ext_scan_start.status == ESP_BT_STATUS_SUCCESS);
    return;
  }
  if (event != ESP_GAP_BLE_EXT_ADV_REPORT_EVT) return;
  const esp_ble_gap_ext_adv_reprot_t& r = param->ext_adv_report.params;
  btsPkts = btsPkts + 1;
  if (!btsQ) return;
  BtRawAdv a;
  memcpy(a.addr, r.addr, 6);
  a.atype = (uint8_t)r.addr_type;
  a.rssi = (int8_t)r.rssi;
  a.phy = (uint8_t)r.primary_phy;
  a.len = r.adv_data_len > sizeof(a.data) ? sizeof(a.data) : r.adv_data_len;
  memcpy(a.data, r.adv_data, a.len);
  if (xQueueSend(btsQ, &a, 0) != pdTRUE) btsDrop = btsDrop + 1;
}

// 一条广播里我们关心的 AD 字段
struct BtsAd {
  char name[25];
  uint8_t mfg[12]; int mfgLen;
  uint16_t svc16; uint8_t sd[32]; int sdLen;   // Service Data（16-bit UUID + 载荷）
};

static void btsParseAd(const uint8_t* d, int len, BtsAd& o) {
  memset(&o, 0, sizeof(o));
  for (int i = 0; i + 1 < len; ) {
    const int l = d[i];
    if (l == 0 || i + 1 + l > len) break;
    const uint8_t t = d[i + 1];
    const uint8_t* v = d + i + 2;
    const int vl = l - 1;
    if ((t == 0x08 || t == 0x09) && vl > 0) {
      int n = vl < (int)sizeof(o.name) - 1 ? vl : (int)sizeof(o.name) - 1;
      memcpy(o.name, v, n); o.name[n] = 0;
    } else if (t == 0xFF && vl > 0) {
      o.mfgLen = vl < (int)sizeof(o.mfg) ? vl : (int)sizeof(o.mfg);
      memcpy(o.mfg, v, o.mfgLen);
    } else if (t == 0x16 && vl >= 2) {
      o.svc16 = (uint16_t)(v[0] | (v[1] << 8));
      o.sdLen = (vl - 2) < (int)sizeof(o.sd) ? (vl - 2) : (int)sizeof(o.sd);
      memcpy(o.sd, v + 2, o.sdLen);
    }
    i += 1 + l;
  }
}

// 名字里的非可打印字节先替换掉，再按 JSON 转义（广播名任何人能填）
static void btsAppendStr(char* buf, int cap, int& n, const char* s) {
  for (; *s && n < cap - 8; s++) {
    const uint8_t c = (uint8_t)*s;
    if (c == '"' || c == '\\') { buf[n++] = '\\'; buf[n++] = (char)c; }
    else if (c < 0x20 || c == 0x7F) { n += snprintf(buf + n, cap - n, "\\u%04x", c); }
    else buf[n++] = (char)c;
  }
  buf[n] = 0;
}

static void btsAppendHex(char* buf, int cap, int& n, const uint8_t* b, int len) {
  for (int i = 0; i < len && n < cap - 4; i++) n += snprintf(buf + n, cap - n, "%02x", b[i]);
}

static uint32_t btsHash(const BtsAd& a) {
  uint32_t h = 2166136261u;
  for (const char* p = a.name; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  for (int i = 0; i < a.mfgLen && i < 4; i++) { h ^= a.mfg[i]; h *= 16777619u; }   // 厂商 ID + 前两字节足够区分
  h ^= a.svc16; h *= 16777619u;
  return h;
}

static BtStreamEnt* btsFind(const BtRawAdv& a, bool& isNew) {
  BtStreamEnt* free_ = nullptr; BtStreamEnt* oldest = nullptr;
  for (int i = 0; i < BTS_MAX; i++) {
    BtStreamEnt& e = btsTab[i];
    if (!e.used) { if (!free_) free_ = &e; continue; }
    if (memcmp(e.addr, a.addr, 6) == 0 && e.atype == a.atype) { isNew = false; return &e; }
    if (!oldest || (int32_t)(oldest->lastSeen - e.lastSeen) > 0) oldest = &e;
  }
  isNew = true;
  BtStreamEnt* e = free_ ? free_ : oldest;   // 满了就顶掉最久没见到的（随机地址轮换会让旧的永远不再出现）
  if (e->used && e->od) { delete e->od; }
  memset(e, 0, sizeof(*e));
  e->used = true;
  memcpy(e->addr, a.addr, 6);
  e->atype = a.atype;
  btsDevs++;
  if (btsDevs > BTS_MAX) btsDevs = BTS_MAX;
  return e;
}

static void btsHandle(const BtRawAdv& a) {
  const uint32_t now = millis();
  bool isNew = false;
  BtStreamEnt* e = btsFind(a, isNew);
  e->lastSeen = now;

  BtsAd pk;
  btsParseAd(a.data, a.len, pk);
  // 合并进该地址的已知字段：新包里有的才覆盖
  if (pk.name[0]) memcpy(e->name, pk.name, sizeof(e->name));
  if (pk.mfgLen)  { memcpy(e->mfg, pk.mfg, pk.mfgLen); e->mfgLen = pk.mfgLen; }
  if (pk.svc16)   e->svc16 = pk.svc16;
  BtsAd ad;
  memset(&ad, 0, sizeof(ad));
  memcpy(ad.name, e->name, sizeof(ad.name));
  memcpy(ad.mfg, e->mfg, sizeof(ad.mfg)); ad.mfgLen = e->mfgLen;
  ad.svc16 = e->svc16;
  if (pk.svc16) { memcpy(ad.sd, pk.sd, sizeof(ad.sd)); ad.sdLen = pk.sdLen; }   // 载荷每包都变，只带本包的

  // Remote ID：Service Data UUID 0xFFFA、首字节应用码 0x0D，后面计数字节 + 25 字节消息
  bool odidUpdated = false;
  if (pk.svc16 == 0xFFFA && pk.sdLen >= 2 && pk.sd[0] == 0x0D) {
    OdidResult r;
    if (ridDecode(pk.sd + 1, pk.sdLen - 1, r)) {
      if (!e->od) e->od = new (std::nothrow) OdidResult();
      if (e->od) { odidMerge(*e->od, r); odidUpdated = true; }   // 一条只带 25 字节，要按地址合并才凑得齐
    }
  }

  const uint32_t sig = btsHash(ad);
  bool emit;
  if (isNew || sig != e->sig) emit = true;
  else if (odidUpdated)       emit = (now - e->lastEmit) >= BTS_ODID_MIN_MS;
  else                        emit = (now - e->lastEmit) >= BTS_KEEPALIVE_MS;
  e->sig = sig;
  if (!emit) return;
  e->lastEmit = now;

  char buf[640]; int n = 0; buf[0] = 0;
  n += snprintf(buf, sizeof(buf),
                "{\"t\":\"d\",\"ts\":%lu,\"addr\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"at\":%u,\"rssi\":%d,\"phy\":%u",
                (unsigned long)now, a.addr[0], a.addr[1], a.addr[2], a.addr[3], a.addr[4], a.addr[5],
                (unsigned)a.atype, (int)a.rssi, (unsigned)a.phy);
  if (ad.name[0]) { n += snprintf(buf + n, sizeof(buf) - n, ",\"name\":\""); btsAppendStr(buf, sizeof(buf), n, ad.name); n += snprintf(buf + n, sizeof(buf) - n, "\""); }
  if (ad.mfgLen)  { n += snprintf(buf + n, sizeof(buf) - n, ",\"mfg\":\""); btsAppendHex(buf, sizeof(buf), n, ad.mfg, ad.mfgLen); n += snprintf(buf + n, sizeof(buf) - n, "\""); }
  if (ad.svc16)   {
    n += snprintf(buf + n, sizeof(buf) - n, ",\"svc\":\"%04x\"", ad.svc16);
    if (ad.sdLen) { n += snprintf(buf + n, sizeof(buf) - n, ",\"sd\":\""); btsAppendHex(buf, sizeof(buf), n, ad.sd, ad.sdLen); n += snprintf(buf + n, sizeof(buf) - n, "\""); }
  }
  if (e->od) {
    n += snprintf(buf + n, sizeof(buf) - n, ",\"rid\":{\"std\":\"%s\"", e->od->isGb ? "GB" : "ASTM");
    ridAppendOdidFields(buf, sizeof(buf), n, *e->od);
    if (n < (int)sizeof(buf) - 2) { buf[n++] = '}'; buf[n] = 0; }
  }
  if (n < (int)sizeof(buf) - 2) { buf[n++] = '}'; buf[n] = 0; }
  // 被截断就整行丢弃，绝不出不合法 JSON（跟 buildDroneJson 同一条规矩）
  if (n >= (int)sizeof(buf) - 2 || buf[n - 1] != '}') return;
  Serial.print("BLE ");
  Serial.println(buf);
}

void btStreamPump() {
  if (!btsOn) return;
  // 控制器没确认启动 → 报错收摊，避免"开了流却永远没数据"（BTEXT 踩过的静默 reports=0）
  if (!btsStarted && millis() - btsStartMs > 2000) {
    Serial.println("BLE {\"t\":\"err\",\"msg\":\"scan failed to start\"}");
    btStreamStop();
    return;
  }
  BtRawAdv a;
  for (int i = 0; i < 12 && btsQ && xQueueReceive(btsQ, &a, 0) == pdTRUE; i++) btsHandle(a);
}

void btStreamStart() {
  if (const char* why = pcmRadioConflict()) {
    Serial.printf("BLE {\"t\":\"err\",\"msg\":\"%s\"}\n", why);
    return;
  }
  if (ridStreamIsActive()) {
    Serial.println("BLE {\"t\":\"err\",\"msg\":\"busy: RID stream on\"}");
    return;
  }
  if (btsOn) btStreamStop();   // 再发一次 BLE ON = 重开（先 end 再 start），跟 RID/LORA 一致
  if (btMemoryCritical()) {
    Serial.println("BLE {\"t\":\"err\",\"msg\":\"out of memory\"}");
    return;
  }
  btsQ = xQueueCreate(24, sizeof(BtRawAdv));
  btsTab = (BtStreamEnt*)calloc(BTS_MAX, sizeof(BtStreamEnt));
  if (!btsQ || !btsTab) {
    if (btsQ) { vQueueDelete(btsQ); btsQ = nullptr; }
    if (btsTab) { free(btsTab); btsTab = nullptr; }
    Serial.println("BLE {\"t\":\"err\",\"msg\":\"out of memory\"}");
    return;
  }
  btsPkts = 0; btsDrop = 0; btsDevs = 0; btsStarted = false;

  // 跟 BTEXT 一样先挂起再 init：不然第二次开扫描会静默收不到东西（见 btExtScanRun）
  btExit();
  btEnsureInit(true);   // quiet：PC MODE 里画布已释放，不能走 centerMsg 之外的绘制，这里干脆不画
  btsOn = true;
  btsStartMs = millis();
  BLEDevice::setCustomGapHandler(btsGapCb);

  esp_ble_ext_scan_params_t sp = {};
  sp.own_addr_type   = BLE_ADDR_TYPE_PUBLIC;   // 不能填 RANDOM，见 btExtScanRun
  sp.filter_policy   = BLE_SCAN_FILTER_ALLOW_ALL;
  sp.scan_duplicate  = BLE_SCAN_DUPLICATE_DISABLE;
  sp.cfg_mask        = (esp_ble_ext_scan_cfg_mask_t)
                       (ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK | ESP_BLE_GAP_EXT_SCAN_CFG_CODE_MASK);
  sp.uncoded_cfg     = { BLE_SCAN_TYPE_ACTIVE, 80, 80 };
  sp.coded_cfg       = { BLE_SCAN_TYPE_ACTIVE, 80, 80 };
  const esp_err_t e = esp_ble_gap_set_ext_scan_params(&sp);
  if (e != ESP_OK) {
    Serial.println("BLE {\"t\":\"err\",\"msg\":\"scan failed to start\"}");
    btStreamStop();
    return;
  }
  Serial.println("BLE {\"t\":\"start\"}");
}

void btStreamStop() {
  // 幂等：没开也回 end，PC 端发 BLE OFF 总能拿到确定的答复
  if (btsOn) {
    btsOn = false;   // 回调先看到这个就不再入队
    esp_ble_gap_stop_ext_scan();
    BLEDevice::setCustomGapHandler(nullptr);
    vTaskDelay(pdMS_TO_TICKS(30));   // 等在途回调退场再释放队列
    if (btsQ) { vQueueDelete(btsQ); btsQ = nullptr; }
    if (btsTab) {
      for (int i = 0; i < BTS_MAX; i++) if (btsTab[i].od) delete btsTab[i].od;
      free(btsTab); btsTab = nullptr;
    }
    btExit();   // 只挂起，不 deinit（零泄漏）
  }
  Serial.printf("BLE {\"t\":\"end\",\"pkts\":%lu,\"drop\":%lu}\n",
                (unsigned long)btsPkts, (unsigned long)btsDrop);
}
