#include "wsniff.h"
#include "ui_common.h"
#include "wifi_net.h"
#include "odid.h"
#include "hotspot.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>

// ---- 抓到的探测请求（源 MAC + 它在找的 SSID）----
// ---- 抓到的探测请求（源 MAC + 它在找的 SSID + 信道 + 信号强度）----
struct ProbeRec {
  uint8_t mac[6];
  char ssid[33];
  int8_t rssi;
  uint8_t chan;
};
static const int PROBE_N = 16;
static ProbeRec probeLog[PROBE_N];
static volatile int probeHead = 0, probeCount = 0;

// 分类计数（回调里累加）
static volatile uint32_t cBeacon, cProbe, cDeauth, cData, cTotal;

static int curChan = 1;
static bool hopLock = false; // false = 自动逐信道轮询, true = 锁定当前信道
static uint32_t lastHop = 0;
static uint32_t lastRateCalc = 0;
static uint32_t lastTotalForRate = 0;
static uint32_t packetRate = 0; // 每秒抓包速率 (fps)

static void resetStats() {
  cBeacon = cProbe = cDeauth = cData = cTotal = 0;
  probeHead = probeCount = 0;
  lastTotalForRate = 0;
  packetRate = 0;
}

// 混杂模式回调（在 WiFi 任务上下文）：解析 802.11 帧头，分类；探测请求还抠出 SSID
static void sniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type == WIFI_PKT_MISC) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* fr = p->payload;
  int len = p->rx_ctrl.sig_len;
  if (len < 10) return;
  cTotal++;

  uint8_t fc = fr[0];
  uint8_t ftype = (fc >> 2) & 0x3;      // 0=管理 1=控制 2=数据
  uint8_t fsub  = (fc >> 4) & 0xF;

  if (ftype == 0) {                     // 管理帧
    if (fsub == 8) cBeacon++;                                   // beacon
    else if (fsub == 12 || fsub == 10) { cDeauth++; }           // deauth / disassoc
    else if (fsub == 4 && len >= 26) {                          // probe request
      cProbe++;
      const uint8_t* body = fr + 24;                            // 24 字节 MAC 头之后是 tagged params
      if (body[0] == 0x00) {                                    // SSID 元素(tag 0)
        int slen = body[1];
        if (slen > 32) slen = 32;
        if (slen > 0 && 26 + slen <= len) {                     // 只记有名字的定向探测（省得一堆 (any)）
          int idx = probeHead % PROBE_N;
          memcpy(probeLog[idx].mac, fr + 10, 6);                // addr2 = 发送方(客户端) MAC
          memcpy(probeLog[idx].ssid, body + 2, slen);
          probeLog[idx].ssid[slen] = 0;
          probeLog[idx].rssi = p->rx_ctrl.rssi;
          probeLog[idx].chan = (uint8_t)curChan;
          probeHead++;
          if (probeCount < PROBE_N) probeCount++;
        }
      }
    }
  } else if (ftype == 2) {
    cData++;
  }
}

void wsniffEnter() {
  hotspotSuspend();
  resetStats();
  hopLock = false;
  lastRateCalc = millis();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();                    // 别让 STA 后台连——它会自己跳信道，干扰嗅探
  esp_wifi_set_promiscuous(false);
  // 显式重置过滤掩码为嗅探所需，防止串口 RFSCAN 遗留 FCSFAIL 等掩码
  wifi_promiscuous_filter_t f = {};
  f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&f);
  wifi_promiscuous_filter_t cf = {};
  esp_wifi_set_promiscuous_ctrl_filter(&cf);
  esp_wifi_set_promiscuous_rx_cb(sniffCb);
  esp_wifi_set_promiscuous(true);
  curChan = 1;
  esp_wifi_set_channel(curChan, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();
}

void wsniffExit() {
  esp_wifi_set_promiscuous(false);
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);   // 先彻底清理掉混杂模式+乱跳的信道状态
  bootWifiStart();   // 再用跟开机一样的后台异步流程重连回记住的网络，不然嗅探完WiFi就一直断着
  hotspotResume();
}

void wsniffUpdate() {
  uint32_t now = millis();
  if (!hopLock && now - lastHop > 250) {       // 逐信道跳(1..13)扫全 2.4G 频段
    curChan = curChan % 13 + 1;
    esp_wifi_set_channel(curChan, WIFI_SECOND_CHAN_NONE);
    lastHop = now;
  }
  if (now - lastRateCalc >= 500) {
    uint32_t dt = now - lastRateCalc;
    uint32_t df = cTotal - lastTotalForRate;
    packetRate = (df * 1000) / dt;
    lastTotalForRate = cTotal;
    lastRateCalc = now;
  }
}

void wsniffKey(int k) {
  if (k == 'r' || k == 'R') {
    resetStats();
  } else if (k == 'h' || k == 'H' || k == ' ') {
    hopLock = !hopLock;
  } else if (k == ';' || k == ',') {
    hopLock = true;
    curChan = (curChan <= 1) ? 13 : (curChan - 1);
    esp_wifi_set_channel(curChan, WIFI_SECOND_CHAN_NONE);
    lastHop = millis();
  } else if (k == '.' || k == '/') {
    hopLock = true;
    curChan = (curChan >= 13) ? 1 : (curChan + 1);
    esp_wifi_set_channel(curChan, WIFI_SECOND_CHAN_NONE);
    lastHop = millis();
  }
}

static void rPod(int cx, int y, int cw, int ch, const char* lab, const char* val, uint16_t vc, uint16_t lc = 0x7BEF) {
  cv.fillRoundRect(cx, y, cw, ch, 3, 0x0821);
  cv.drawRoundRect(cx, y, cw, ch, 3, 0x18C3);
  cv.setTextDatum(top_center); cv.setTextSize(1);
  cv.setTextColor(lc, 0x0821);
  cv.drawString(lab, cx + cw / 2, y + 3);
  cv.setTextColor(vc, 0x0821);
  cv.drawString(val, cx + cw / 2, y + 15);
}

void drawWsniff() {
  cv.fillScreen(TFT_BLACK);

  // 顶栏状态装配
  char rightBuf[32];
  if (hopLock) {
    snprintf(rightBuf, sizeof(rightBuf), "CH%02d [HOLD] %up/s", curChan, (unsigned)packetRate);
  } else {
    snprintf(rightBuf, sizeof(rightBuf), "HOP CH%-2d %up/s", curChan, (unsigned)packetRate);
  }
  drawPageHeader("Sniffer", rightBuf, hopLock ? 0xFBE0 : 0x07FF);

  char b[48];

  // 1. 四联赛博遥测指标舱 (y = 15..41, h = 26)
  snprintf(b, sizeof(b), "%lu", (unsigned long)cBeacon);
  rPod(4, 15, 56, 26, "BEACON", b, TFT_WHITE, 0x07FF);

  snprintf(b, sizeof(b), "%lu", (unsigned long)cProbe);
  rPod(63, 15, 56, 26, "PROBE", b, 0xFDA0, 0x07FF);

  snprintf(b, sizeof(b), "%lu", (unsigned long)cData);
  rPod(122, 15, 56, 26, "DATA", b, 0x8CD2, 0x07FF);

  if (cDeauth == 0) {
    rPod(181, 15, 55, 26, "DEAUTH", "0", 0x4208, 0x632C);
  } else {
    snprintf(b, sizeof(b), "!%lu", (unsigned long)cDeauth);
    rPod(181, 15, 55, 26, "DEAUTH", b, 0xF800, 0xF800);
  }

  // 2. 空域安全告警横幅 / 频谱状态条 (y = 44..55, h = 12)
  const int warnY = 44, warnH = 12;
  if (cDeauth > 0) {
    cv.fillRoundRect(4, warnY, SW - 8, warnH, 2, 0x3800);
    cv.drawRoundRect(4, warnY, SW - 8, warnH, 2, 0x7800);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(0xF800, 0x3800);
    snprintf(b, sizeof(b), "[!] THREAT: %lu DEAUTH DETECTED", (unsigned long)cDeauth);
    cv.drawString(b, SW / 2, warnY + warnH / 2);
  } else {
    cv.fillRoundRect(4, warnY, SW - 8, warnH, 2, 0x0821);
    cv.drawRoundRect(4, warnY, SW - 8, warnH, 2, 0x18C3);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(0x632C, 0x0821);
    if (hopLock) {
      snprintf(b, sizeof(b), "PASSIVE RECON: CH%02d | %lu PKTS", curChan, (unsigned long)cTotal);
    } else {
      snprintf(b, sizeof(b), "PASSIVE RECON: CH1-13 | %lu PKTS", (unsigned long)cTotal);
    }
    cv.drawString(b, SW / 2, warnY + warnH / 2);
  }

  // 3. 探测请求雷达捕获卡片 (y = 59..121, h = 63)
  const int feedY = 59, feedH = 63;
  cv.fillRoundRect(4, feedY, SW - 8, feedH, 3, 0x0821);
  cv.drawRoundRect(4, feedY, SW - 8, feedH, 3, 0x18C3);
  cv.fillRect(4, feedY, 2, feedH, 0x07FF);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("PROBE REQ (CLIENT -> SSID):", 10, feedY + 3);

  cv.setTextDatum(top_right);
  cv.setTextColor(0x8410, 0x0821);
  snprintf(b, sizeof(b), "%d FOUND", probeCount);
  cv.drawString(b, SW - 10, feedY + 3);

  cv.drawFastHLine(6, feedY + 13, SW - 12, 0x18C3);

  // 逐条渲染捕获到的客户端定向探测
  if (probeCount == 0) {
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(0x4208, 0x0821);
    cv.drawString("Listening for active client probes...", SW / 2, feedY + 37);
  } else {
    int maxRows = 4;
    int count = (int)probeCount;
    for (int i = 0; i < min(count, maxRows); i++) {
      int idx = (probeHead - 1 - i + PROBE_N * 2) % PROBE_N;
      const ProbeRec& r = probeLog[idx];
      int rowY = feedY + 16 + i * 11;

      // 客户端 MAC 后缀
      cv.setTextDatum(top_left); cv.setTextSize(1);
      cv.setTextColor(0x8CD2, 0x0821);
      snprintf(b, sizeof(b), "%02x:%02x:%02x", r.mac[3], r.mac[4], r.mac[5]);
      cv.drawString(b, 10, rowY);

      // 信道
      cv.setTextColor(0x07FF, 0x0821);
      snprintf(b, sizeof(b), "c%02d", r.chan);
      cv.drawString(b, 62, rowY);

      // 寻找的 SSID 名称
      cv.setTextColor(TFT_WHITE, 0x0821);
      cv.drawString(trunc(String(r.ssid), 16), 84, rowY);

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
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("H", 4, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(hopLock ? " hop" : " hold", 10, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("; .", 46, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" ch", 60, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", 84, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" rst", 90, footY);

  if (hotspotIsSuspended()) {
    cv.setFont(&fonts::efontCN_12); cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("热点已暂停", 122, footY); cv.setFont(&fonts::Font0);
  }

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 192, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" back", 198, footY);
}

// =============================================================================
// Remote ID 取样（串口 RIDSCAN [秒]）
//
// 两件事一起做：
//   1) 解码标准 ODID（FA:0B:BC + OUI type 0x0D）——解析器在 odid.cpp，Wi-Fi 和 BLE 共用
//   2) 仍然把所有 vendor IE 的原始字节打出来。DJI 各机型的实现不统一，解不出来的时候
//      得能看见真机到底播了什么，才知道下一步该补哪种布局
//
// 认得的两个 OUI：
//   FA:0B:BC  ASTM F3411 / ASD-STAN 标准 Remote ID（oui_type 应为 0x0D）
//   26:37:12  DJI 私有 DroneID（subcmd 0x10=遥测+位置，0x11=用户填写信息）
// 其余 OUI 也统计出现次数——万一 DJI 换了新的，不至于因为没在白名单里就看不见。
//
// ⚠️ ESP32-S3 只有 2.4GHz。无人机图传跑在 5.8 的话这里物理上收不到，
//    这不是代码问题，得在 App 里把图传锁到 2.4GHz。
// =============================================================================
struct RidHit { uint8_t oui[3]; uint8_t ouiType; uint32_t count; uint8_t chan; int8_t rssi;
                uint8_t src[6]; uint8_t body[24]; uint8_t bodyLen; };
static const int RID_MAX = 32;   // 12 太小，实测在普通住宅里就撑满了，会把后来的条目丢掉

struct NanHit { uint8_t oui[3]; uint8_t ouiType; uint32_t count; uint8_t chan; int8_t rssi;
                uint8_t src[6]; uint8_t body[32]; uint8_t bodyLen; };
static const int NAN_MAX = 8;

struct ApRec { char ssid[33]; uint8_t bssid[6]; uint8_t chan; int8_t rssi; };
static const int AP_MAX = 24;

struct RidDrone { uint8_t src[6]; OdidResult r; uint32_t count; int8_t rssi; uint8_t chan; };
static const int RID_DRONE_MAX = 6;

struct RidScanContext {
  RidHit ridHits[RID_MAX];
  NanHit ridNan[NAN_MAX];
  ApRec ridAps[AP_MAX];
  RidDrone ridDrones[RID_DRONE_MAX];
  uint32_t ridSubCount[16];
  uint8_t ridOdidRaw[100];
};
static RidScanContext* ridScanCtx = nullptr;
#define ridHits (ridScanCtx->ridHits)
#define ridNan (ridScanCtx->ridNan)
#define ridAps (ridScanCtx->ridAps)
#define ridDrones (ridScanCtx->ridDrones)
#define ridSubCount (ridScanCtx->ridSubCount)
#define ridOdidRaw (ridScanCtx->ridOdidRaw)

static volatile int ridHitCount = 0;
static volatile uint32_t ridBeacons = 0, ridMgmt = 0;
static volatile int ridNanCount = 0;
static volatile int ridApCount = 0;
static volatile int ridDroneCount = 0;
static volatile int ridOdidRawLen = 0;

// 按发送方 MAC 找到条目，把新解出的字段并进去（合并规则见 odid.h 的 odidMerge——
// BLE 那边同样要合并，所以那段逻辑放在 odid.cpp 里两边共用）。
static void ridMergeDrone(const uint8_t* src, const OdidResult& n, int8_t rssi, uint8_t chan) {
  int idx = -1;
  for (int i = 0; i < ridDroneCount; i++)
    if (memcmp(ridDrones[i].src, src, 6) == 0) { idx = i; break; }
  if (idx < 0) {
    if (ridDroneCount >= RID_DRONE_MAX) return;
    idx = ridDroneCount++;
    memcpy(ridDrones[idx].src, src, 6);
    ridDrones[idx].r = OdidResult();
    ridDrones[idx].count = 0;
  }
  RidDrone& d = ridDrones[idx];
  d.count++;
  d.rssi = rssi;
  d.chan = chan;
  odidMerge(d.r, n);
}

// vendor 载荷是不是标准 Remote ID：OUI 之外还要校验后面那一字节的 OUI type。
// （原来只比 OUI 三字节就打 ASTM 标签，会把同 OUI 的其它类型也误标）
static inline bool ridIsOdid(const uint8_t* v) {
  return v[0] == ODID_OUI_0 && v[1] == ODID_OUI_1 && v[2] == ODID_OUI_2 && v[3] == ODID_OUI_TYPE;
}

static void ridCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT || !ridScanCtx) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* fr = p->payload;
  int len = p->rx_ctrl.sig_len;
  if (len < 36) return;
  uint8_t fsub = (fr[0] >> 4) & 0xF;
  ridMgmt++;
  ridSubCount[fsub]++;

  // 解码统一走 odid.cpp 那份共用实现（beacon vendor IE / NAN 两条路都在里面）。
  // 下面那些代码只负责"看见了哪些 OUI / 哪些 AP"这类取样统计。
  {
    OdidResult r;
    if (ridFromMgmtFrame(fr, len, r))
      ridMergeDrone(fr + 10, r, p->rx_ctrl.rssi, p->rx_ctrl.channel);
  }

  // Action 帧(13)：NAN 的 Service Discovery Frame 走这条。帧体没有 beacon 那 12 字节
  // 固定参数，24 字节 MAC 头之后直接是 category(1) + action(1)，公共动作里的
  // Vendor Specific 是 category=0x04 action=0x09，再跟 3 字节 OUI + 1 字节 OUI type。
  //   50:6F:9A type 0x13 = Wi-Fi Alliance NAN
  //   FA:0B:BC type 0x0D = ASTM F3411 直接塞在 action 帧里的情形
  if (fsub == 13) {
    if (len < 24 + 6) return;
    const uint8_t* b = fr + 24;
    if (!(b[0] == 0x04 && b[1] == 0x09)) return;      // 只看公共动作里的 vendor specific
    const uint8_t* v = b + 2;
    int idx = -1;
    for (int i = 0; i < ridNanCount; i++)
      if (memcmp(ridNan[i].oui, v, 3) == 0 && ridNan[i].ouiType == v[3]) { idx = i; break; }
    if (idx < 0 && ridNanCount < NAN_MAX) {
      idx = ridNanCount++;
      memcpy(ridNan[idx].oui, v, 3);
      ridNan[idx].ouiType = v[3];
      ridNan[idx].count = 0;
    }
    if (idx >= 0) {
      NanHit& h = ridNan[idx];
      h.count++;
      h.chan = p->rx_ctrl.channel;
      h.rssi = p->rx_ctrl.rssi;
      memcpy(h.src, fr + 10, 6);
      int n = len - (24 + 6); if (n > (int)sizeof(h.body)) n = sizeof(h.body);
      if (n < 0) n = 0;
      memcpy(h.body, v + 4, n);
      h.bodyLen = (uint8_t)n;
    }
    return;   // 解码统一在函数开头做（ridFromMgmtFrame）
  }

  // beacon(8) 和 probe response(5) 都可能带 vendor IE，两个都看
  if (fsub != 8 && fsub != 5) return;
  ridBeacons++;

  // 24 字节 MAC 头 + 12 字节固定参数(timestamp/interval/capability) 之后才是 tagged params
  int off = 36;
  // 第一个 tag 一定是 SSID(0)，先把 AP 记下来
  if (off + 2 <= len && fr[off] == 0x00) {
    int slen = fr[off + 1];
    if (slen <= 32 && off + 2 + slen <= len) {
      int ai = -1;
      for (int i = 0; i < ridApCount; i++) if (memcmp(ridAps[i].bssid, fr + 10, 6) == 0) { ai = i; break; }
      if (ai < 0 && ridApCount < AP_MAX) {
        ai = ridApCount++;
        memcpy(ridAps[ai].bssid, fr + 10, 6);
      }
      if (ai >= 0) {
        memcpy(ridAps[ai].ssid, fr + off + 2, slen);
        ridAps[ai].ssid[slen] = 0;
        ridAps[ai].chan = p->rx_ctrl.channel;
        ridAps[ai].rssi = p->rx_ctrl.rssi;
      }
    }
  }
  while (off + 2 <= len) {
    uint8_t tag = fr[off], tlen = fr[off + 1];
    if (off + 2 + tlen > len) break;
    if (tag == 221 && tlen >= 4) {                       // vendor specific
      const uint8_t* v = fr + off + 2;
      int idx = -1;
      for (int i = 0; i < ridHitCount; i++)
        if (memcmp(ridHits[i].oui, v, 3) == 0 && ridHits[i].ouiType == v[3]) { idx = i; break; }
      if (idx < 0 && ridHitCount < RID_MAX) {
        idx = ridHitCount++;
        memcpy(ridHits[idx].oui, v, 3);
        ridHits[idx].ouiType = v[3];
        ridHits[idx].count = 0;
      }
      if (idx >= 0) {
        RidHit& h = ridHits[idx];
        h.count++;
        h.chan = p->rx_ctrl.channel;
        h.rssi = p->rx_ctrl.rssi;
        memcpy(h.src, fr + 10, 6);                        // addr2 = 发送方
        int n = tlen - 4; if (n > (int)sizeof(h.body)) n = sizeof(h.body);
        if (n < 0) n = 0;
        memcpy(h.body, v + 4, n);
        h.bodyLen = (uint8_t)n;
      }
      // 命中标准 OUI 就单独留一份长样本（解码本身在函数开头统一做了）。
      // ridHits.body 只有 24 字节，而国标载荷的位置字段排在 38 字节开外，截断了没法比对。
      if (ridIsOdid(v)) {
        int n = tlen - 4;
        if (n > (int)sizeof(ridOdidRaw)) n = (int)sizeof(ridOdidRaw);
        if (n > 0) { memcpy(ridOdidRaw, v + 4, n); ridOdidRawLen = n; }
      }
    }
    off += 2 + tlen;
  }
}

static void ridPrintOne(const RidHit& h) {
  // 标 ASTM 要连 OUI type 一起对上：同一个 OUI 下还可能有别的 type，只比三字节会误标
  const bool astm = (h.oui[0] == ODID_OUI_0 && h.oui[1] == ODID_OUI_1 &&
                     h.oui[2] == ODID_OUI_2 && h.ouiType == ODID_OUI_TYPE);
  const bool dji  = (h.oui[0] == 0x26 && h.oui[1] == 0x37 && h.oui[2] == 0x12);
  const char* tag = astm ? "  <== ASTM F3411 Remote ID" : dji ? "  <== DJI DroneID" : "";
  Serial.printf("[rid] OUI %02X:%02X:%02X type=%02X  x%lu  ch=%u rssi=%d  src=%02X:%02X:%02X:%02X:%02X:%02X%s\n",
                h.oui[0], h.oui[1], h.oui[2], h.ouiType, (unsigned long)h.count,
                h.chan, h.rssi, h.src[0], h.src[1], h.src[2], h.src[3], h.src[4], h.src[5], tag);
  Serial.print("[rid]   body:");
  for (int i = 0; i < h.bodyLen; i++) Serial.printf(" %02X", h.body[i]);
  Serial.println();
}

void ridScanRun(int seconds, int fixedChan) {
  if (!ridScanCtx) ridScanCtx = new RidScanContext();
  if (!ridScanCtx) return;

  ridHitCount = 0; ridBeacons = ridMgmt = 0; ridApCount = 0; ridNanCount = 0; ridDroneCount = 0; ridOdidRawLen = 0;
  for (int i = 0; i < 16; i++) ridSubCount[i] = 0;
  if (fixedChan > 0)
    Serial.printf("[rid] scanning %ds, parked on ch%d (2.4GHz only)\n", seconds, fixedChan);
  else
    Serial.printf("[rid] scanning %ds, hopping ch1-13 (2.4GHz only)\n", seconds);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(ridCb);
  esp_wifi_set_promiscuous(true);

  uint32_t end = millis() + (uint32_t)seconds * 1000;
  int ch = fixedChan > 0 ? fixedChan : 1;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  uint32_t hop = millis();
  while ((int32_t)(end - millis()) > 0) {
    // 固定信道模式是为 NAN 准备的：NAN 只在 ch6，而且发现窗口 ~524ms 才开一次，
    // 跳频时每个信道才停 300ms，很容易整场都错过——蹲死才算数。
    if (fixedChan <= 0 && millis() - hop > 300) {   // beacon 通常 100ms 一发，300ms 够撞上几次
      ch = ch % 13 + 1;
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      hop = millis();
    }
    delay(10);
  }

  esp_wifi_set_promiscuous(false);
  Serial.printf("[rid] done. mgmt=%lu beacon/proberesp=%lu  distinct vendor IE=%d\n",
                (unsigned long)ridMgmt, (unsigned long)ridBeacons, ridHitCount);
  if (ridHitCount == 0) Serial.println("[rid] no vendor IE at all — 附近没有 2.4G beacon？");
  for (int i = 0; i < ridHitCount; i++) ridPrintOne(ridHits[i]);
  if (ridHitCount >= RID_MAX) Serial.println("[rid] ⚠️ vendor IE 表满了，可能有条目被丢");
  // 子类型直方图：先确认"到底收没收到 Action 帧"，再谈里面有没有 NAN
  Serial.print("[rid] mgmt subtypes:");
  for (int i = 0; i < 16; i++) if (ridSubCount[i]) Serial.printf(" %d=%lu", i, (unsigned long)ridSubCount[i]);
  Serial.println();
  Serial.printf("[rid] --- %d NAN/action vendor frames ---\n", ridNanCount);
  for (int i = 0; i < ridNanCount; i++) {
    const NanHit& h = ridNan[i];
    const bool nan  = (h.oui[0] == 0x50 && h.oui[1] == 0x6F && h.oui[2] == 0x9A && h.ouiType == 0x13);
    const bool astm = (h.oui[0] == ODID_OUI_0 && h.oui[1] == ODID_OUI_1 &&
                       h.oui[2] == ODID_OUI_2 && h.ouiType == ODID_OUI_TYPE);
    Serial.printf("[rid] ACT OUI %02X:%02X:%02X type=%02X x%lu ch=%u rssi=%d src=%02X:%02X:%02X:%02X:%02X:%02X%s\n",
                  h.oui[0], h.oui[1], h.oui[2], h.ouiType, (unsigned long)h.count, h.chan, h.rssi,
                  h.src[0], h.src[1], h.src[2], h.src[3], h.src[4], h.src[5],
                  nan ? "  <== Wi-Fi NAN" : astm ? "  <== ASTM F3411" : "");
    Serial.print("[rid]   body:");
    for (int j = 0; j < h.bodyLen; j++) Serial.printf(" %02X", h.body[j]);
    Serial.println();
  }
  // ---- 解码结果。上面全是原始字节，这一段才是人能直接读的 ----
  Serial.printf("[rid] === %d decoded Remote ID ===\n", ridDroneCount);
  if (ridDroneCount == 0 && ridHitCount > 0)
    Serial.println("[rid] 收到了 vendor IE 但没有一条解得出来。ASTM ODID 和国标 GB 两套都试过了，"
                   "多半是附近没有正在飞的无人机——机器落地后通常就停播了。"
                   "下面那段 FA:0B:BC 原始载荷可以拿来判断是不是又出了新格式");
  for (int i = 0; i < ridDroneCount; i++) {
    const RidDrone& d = ridDrones[i];
    Serial.printf("[rid] %02X:%02X:%02X:%02X:%02X:%02X  x%lu  ch=%u %d dBm\n",
                  d.src[0], d.src[1], d.src[2], d.src[3], d.src[4], d.src[5],
                  (unsigned long)d.count, d.chan, d.rssi);
    if (d.r.haveBasic) {
      if (d.r.isGb)   // 国标：编号就是唯一产品识别码，机型走国标自己那套分类
        Serial.printf("[rid]   id   : %s  (GB / %s)\n",
                      d.r.uasId, d.r.haveUaClass ? gbClassName(d.r.uaClass) : "?");
      else
        Serial.printf("[rid]   id   : %s  (%s / %s)\n",
                      d.r.uasId, odidIdTypeName(d.r.idType), odidUaTypeName(d.r.uaType));
    }
    if (d.r.haveStatus)
      Serial.printf("[rid]   state: %s%s\n", gbStatusName(d.r.opStatus),
                    d.r.haveCoordSys ? (d.r.coordSys == 0 ? "   coord=WGS-84" : "   coord=非WGS84") : "");
    if (d.r.haveLoc) {
      Serial.printf("[rid]   pos  : %.7f, %.7f", d.r.lat, d.r.lon);
      if (d.r.haveHeight) Serial.printf("  agl=%.1fm", d.r.height);   // 离地高度，抬头找它时看这个
      if (d.r.haveAlt)   Serial.printf("  alt=%.1fm", d.r.altGeo);
      if (d.r.haveSpeed) Serial.printf("  spd=%.1fm/s", d.r.speed);
      if (d.r.vspeed != 0) Serial.printf("  vs=%+.1fm/s", d.r.vspeed);
      if (d.r.heading >= 0) Serial.printf("  hdg=%d", d.r.heading);
      Serial.println();
    }
    if (d.r.haveSys)
      Serial.printf("[rid]   pilot: %.7f, %.7f  (%s)\n",
                    d.r.pilotLat, d.r.pilotLon, odidPilotLocName(d.r.pilotLocType));
    if (d.r.haveSelfId)     Serial.printf("[rid]   desc : %s\n", d.r.selfId);
    if (d.r.haveOperatorId) Serial.printf("[rid]   oper : %s\n", d.r.operatorId);
  }
  if (ridDroneCount >= RID_DRONE_MAX) Serial.println("[rid] ⚠️ 解码表满了，可能有机器没列出");

  // 标准 ODID 解不出来时，这段原始字节是判断"它到底是什么格式"的唯一依据
  if (ridOdidRawLen > 0) {
    Serial.printf("[rid] --- FA:0B:BC 原始载荷 %d 字节 ---\n", ridOdidRawLen);
    Serial.print("[rid] ");
    for (int i = 0; i < ridOdidRawLen; i++) {
      Serial.printf("%02X ", ridOdidRaw[i]);
      if ((i + 1) % 24 == 0) { Serial.print("\n[rid] "); }
    }
    Serial.println();
  }

  Serial.printf("[rid] --- %d APs seen on 2.4GHz ---\n", ridApCount);
  for (int i = 0; i < ridApCount; i++) {
    const ApRec& a = ridAps[i];
    Serial.printf("[rid] ch%-3u %4d dBm  %02X:%02X:%02X:%02X:%02X:%02X  %s\n",
                  a.chan, a.rssi, a.bssid[0], a.bssid[1], a.bssid[2],
                  a.bssid[3], a.bssid[4], a.bssid[5], a.ssid);
  }

  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  bootWifiStart();                        // 跟 wsniffExit 一样：扫完把 WiFi 还原回后台重连

  delete ridScanCtx;
  ridScanCtx = nullptr;
}

// =============================================================================
// 2.4G 802.11 活动普查（串口 RFSCAN [秒]）
//
// ⚠️ 先说清楚它**不能**做什么，因为这正是它当初被写出来的动机、而那个动机被实测否掉了：
//
// 原本想拿它探测大疆 O4 图传。O4 是私有调制不是 802.11，ESP32 解不了，但当时以为
// "能量还在，接收机会被顶起来、按 Wi-Fi 解失败，从而留下 WIFI_PKT_MISC 或 rx_state
// 非 0 的痕迹"。2026-08-09 现场做了对照实验（图传开着测一轮、无人机彻底断电再测一轮）：
//   **两轮、13 个信道，misc 和 err 全是 0。**
// 也就是说这条路子一点信号都没产生——不是"没测到 O4"，是探测手段本身不工作。
//
// 原因：混杂模式回调只在射频**认出 802.11 前导码之后**才触发。O4 的前导跟 Wi-Fi 完全
// 不同，芯片根本不认为"这里有个包开始了"，连失败事件都不会产生。所以 WIFI_PKT_MISC
// 不是"能量事件"，**ESP32 的混杂模式当不了能量探测器**。真要看 O4 得上 SDR。
//
// 那它现在还剩什么用：逐信道的 802.11 活动普查——管理帧/数据帧计数、峰值 RSSI、噪声底。
// 找空信道、看哪个信道最吵、确认某个 AP 在哪，这些还是好用的。
// =============================================================================
static volatile uint32_t rfMgmt[14], rfMisc[14], rfErr[14], rfData[14];
static volatile int8_t   rfPeak[14], rfNoise[14];
static volatile int      rfCurChan = 1;

static void rfCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  int ch = rfCurChan;
  if (ch < 1 || ch > 13) return;
  if      (type == WIFI_PKT_MISC) rfMisc[ch]++;
  else if (type == WIFI_PKT_DATA) rfData[ch]++;
  else                            rfMgmt[ch]++;
  if (p->rx_ctrl.rx_state != 0) rfErr[ch]++;
  const int8_t r = (int8_t)p->rx_ctrl.rssi;
  if (r > rfPeak[ch]) rfPeak[ch] = r;
  rfNoise[ch] = (int8_t)p->rx_ctrl.noise_floor;
}

void rfScanRun(int seconds) {
  for (int i = 0; i < 14; i++) { rfMgmt[i]=rfMisc[i]=rfErr[i]=rfData[i]=0; rfPeak[i]=-128; rfNoise[i]=0; }
  Serial.printf("[rf] 2.4G 802.11 活动普查 %ds（逐信道计数 + 峰值 RSSI + 噪声底）\n", seconds);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(false);
  // 关键：默认过滤器不放行 MISC，非 802.11 的能量事件就全看不见了
  wifi_promiscuous_filter_t f = {};
  f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA |
                  WIFI_PROMIS_FILTER_MASK_CTRL | WIFI_PROMIS_FILTER_MASK_MISC |
                  WIFI_PROMIS_FILTER_MASK_FCSFAIL;
  esp_wifi_set_promiscuous_filter(&f);
  wifi_promiscuous_filter_t cf = {};
  cf.filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL;
  esp_wifi_set_promiscuous_ctrl_filter(&cf);
  esp_wifi_set_promiscuous_rx_cb(rfCb);
  esp_wifi_set_promiscuous(true);

  // 每个信道均分时间，来回扫几轮，避免只赶上某一瞬间
  const int rounds = 3;
  const int dwell = (seconds * 1000) / (13 * rounds);
  for (int r = 0; r < rounds; r++) {
    for (int ch = 1; ch <= 13; ch++) {
      rfCurChan = ch;
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      delay(dwell > 5 ? dwell : 5);
    }
  }
  esp_wifi_set_promiscuous(false);
  // 恢复默认过滤掩码，避免影响后续混杂模式应用
  wifi_promiscuous_filter_t f_rst = {};
  f_rst.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&f_rst);
  wifi_promiscuous_filter_t cf_rst = {};
  esp_wifi_set_promiscuous_ctrl_filter(&cf_rst);

  Serial.println("[rf] ch |  mgmt  data  misc   err | peak dBm | noise");
  for (int ch = 1; ch <= 13; ch++) {
    Serial.printf("[rf] %2d | %5lu %5lu %5lu %5lu | %7d  | %5d\n", ch,
                  (unsigned long)rfMgmt[ch], (unsigned long)rfData[ch],
                  (unsigned long)rfMisc[ch], (unsigned long)rfErr[ch],
                  rfPeak[ch] == -128 ? 0 : rfPeak[ch], rfNoise[ch]);
  }
  Serial.println("[rf] misc/err 实测恒为 0：混杂模式只在认出 802.11 前导后才回调，");
  Serial.println("[rf] 非 Wi-Fi 的能量(O4 图传/蓝牙/微波炉)压根不会触发——这里看不到，要 SDR");

  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  bootWifiStart();
}
