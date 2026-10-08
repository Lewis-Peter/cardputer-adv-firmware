#include "ridapp.h"
#include "odid.h"
#include "ui_common.h"
#include "wifi_net.h"
#include "gnss.h"
#include "rid_radar.h"
#include "rid_alert.h"
#include "rid_opid.h"
#include "led.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <cstring>
#include <cmath>
#include <stdarg.h>
#include <atomic>
#include "list_sel.h"
#include "hotspot.h"
#include "sd_files.h"
#include "chat.h"
#include "bt.h"
#include "ssh_app.h"
#include <SD.h>

// ---- SD 卡机载录制状态 ----
static bool isRecording = false;
static File ridLogFile;
static uint32_t recPackets = 0;
static uint32_t lastFlushMs = 0;
static char sdToastMsg[28] = {0};
static uint32_t sdToastUntilMs = 0;
static uint16_t sdToastColor = ACCENT;

static void setSdToast(const char* msg, uint16_t col, uint32_t durationMs = 1800) {
  snprintf(sdToastMsg, sizeof(sdToastMsg), "%s", msg);
  sdToastColor = col;
  sdToastUntilMs = millis() + durationMs;
  dirty = true;
}

static void drawSdToast(uint32_t now) {
  if (now >= sdToastUntilMs || !sdToastMsg[0]) return;
  const int tw = cv.textWidth(sdToastMsg) + 16;
  const int tx = (SW - tw) / 2;
  const int ty = SH - 24;
  cv.fillRoundRect(tx, ty, tw, 13, 3, 0x0821);
  cv.drawRoundRect(tx, ty, tw, 13, 3, sdToastColor);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  cv.setTextColor(sdToastColor, 0x0821);
  cv.drawString(sdToastMsg, SW / 2, ty + 6);
}

static void ridAppStartRecord() {
  if (isRecording) return;
  if (!sdReady()) {
    setSdToast("NO SD CARD", TFT_RED);
    return;
  }
  const bool hasDir = SD.exists("/rid") || SD.mkdir("/rid");
  char path[40];
  struct tm ti{};
  if (timeSynced && getLocalTime(&ti, 0) && ti.tm_year + 1900 > 2020) {
    if (hasDir) strftime(path, sizeof(path), "/rid/%Y%m%d_%H%M%S.ndjson", &ti);
    else        strftime(path, sizeof(path), "/rid_%Y%m%d_%H%M%S.ndjson", &ti);
  } else {
    int seq = 1;
    while (seq < 10000) {
      if (hasDir) snprintf(path, sizeof(path), "/rid/rid_%04d.ndjson", seq);
      else        snprintf(path, sizeof(path), "/rid_%04d.ndjson", seq);
      if (!SD.exists(path)) break;
      seq++;
    }
  }
  ridLogFile = SD.open(path, FILE_WRITE);
  if (!ridLogFile) {
    setSdToast("SD WRITE FAIL", TFT_RED);
    return;
  }
  isRecording = true;
  recPackets = 0;
  lastFlushMs = millis();
  char toast[28];
  const char* slash = strrchr(path, '/');
  const char* fname = slash ? (slash + 1) : path;
  snprintf(toast, sizeof(toast), "REC: %s", fname);
  setSdToast(toast, TFT_GREEN);
}

static void ridAppStopRecord() {
  if (!isRecording) return;
  if (ridLogFile) {
    ridLogFile.flush();
    ridLogFile.close();
  }
  isRecording = false;
  char toast[28];
  snprintf(toast, sizeof(toast), "SAVED (%lu pkts)", (unsigned long)recPackets);
  setSdToast(toast, ACCENT);
}

void ridAppToggleRecord() {
  if (isRecording) ridAppStopRecord();
  else ridAppStartRecord();
}

bool ridAppIsRecording() {
  return isRecording;
}

// ⚠️ 别直接写 n += snprintf(buf+n, sizeof(buf)-n, ...)。snprintf 返回的是"本该写多少"，
// 可能大于缓冲区；n 一旦超过容量，sizeof(buf)-n 会让 size_t 下溢成一个巨大的长度，
// 下一次写就直接越界了。这里把 n 夹在容量内，写满就停。
static void appendf(char* buf, int cap, int& n, const char* fmt, ...) {
  if (n < 0) n = 0;
  if (n >= cap - 1) return;
  va_list ap; va_start(ap, fmt);
  const int w = vsnprintf(buf + n, (size_t)(cap - n), fmt, ap);
  va_end(ap);
  if (w < 0) return;
  n += w;
  if (n > cap - 1) n = cap - 1;
}

// 将字符串做合法 JSON 转义（转义引号、反斜杠及控制字符），防止空中广播恶意数据破坏外层 JSON 结构
static void appendJsonStr(char* buf, int cap, int& n, const char* str) {
  if (n < 0) n = 0;
  if (!str) return;
  while (*str && n < cap - 1) {
    const char c = *str++;
    if (c == '"') {
      appendf(buf, cap, n, "\\\"");
    } else if (c == '\\') {
      appendf(buf, cap, n, "\\\\");
    } else if ((uint8_t)c < 0x20) {
      appendf(buf, cap, n, "\\u%04x", (uint8_t)c);
    } else {
      buf[n++] = c;
      buf[n] = '\0';
    }
  }
}

// 避免在绘制主循环中频繁构造临时 String 及调用 trunc() 造成堆分配与碎片
static inline void truncInPlace(char* s, int maxChars) {
  int len = (int)strlen(s);
  if (len > maxChars && maxChars > 0) {
    s[maxChars - 1] = '~';
    s[maxChars] = 0;
  }
}

// ---- 发现的无人机 ----
struct Drone {
  uint8_t  mac[6];
  OdidResult r;
  int8_t   rssi;        // 最近一包的原始值（串口输出用）
  int16_t  rssiAvgX8;   // 指数平滑 ×8（屏幕显示用）：每包 rssi 抖几 dB，直接显示数字会跳
  uint8_t  chan;
  uint32_t packets;
  uint32_t lastMs;
  uint32_t firstMs;     // 首次收到的时间，运营人 ID 的"没播"要看观察了多久
  bool injected;        // 来自串口 RIDFAKE 的样本，不是射频收到的
  volatile bool fresh;  // 有新数据还没往串口吐（见 ridStreamPump）
  uint32_t lastEmitMs;
};
static const int DRONE_MAX = 12;
static Drone* drones = nullptr;
static volatile int droneCount = 0;
static int selIdx = 0, listTop = 0;
static bool radarView = false;        // m 键：列表 / 雷达

// ---- 接近告警 ----
// a 切距离档（关/200/500/1000/2000m），b 开关声音；两项都存 NVS。只在 Drone ID 页开着时评估。
static int alertLevel = 0;
static bool alertSound = true;
static AlertState alertSt[12];
static uint32_t alertFlashUntilMs = 0;   // 事件发生后红灯闪到这个时刻
static uint32_t alertLastBeepMs = 0;
static bool alertLedOwned = false;
static bool alertAnyInside = false;
static void alertLedRelease();

// 一段时间没再收到就标成 lost（不删——人可能正想看它最后出现在哪）
static const uint32_t LOST_MS = 20000;

// ---- 跳频 / 锁频 ----
// 实测：无人机 beacon 约 5.6 包/秒，大疆及国内多数设备主要集中在信道 6（2.437GHz），其次为 1 与 11。
// 优化为加权非对称轮询序列，信道 6 占比约 50%，且自适应驻留时间（Ch6 320ms, Ch1/11 220ms, 其余 130ms）。
static const uint8_t HOP_SEQ[] = { 6, 1, 6, 11, 6, 2, 3, 6, 4, 5, 6, 7, 8, 6, 9, 10, 6, 12, 13 };
static const int HOP_COUNT = (int)(sizeof(HOP_SEQ) / sizeof(HOP_SEQ[0]));
static int hopIdx = 0;
static int curChan = 6;
static uint32_t lastHopMs = 0;
static int lockChan = 0;              // 0 = 没锁，正在跳频；>0 = 锁在该信道
static uint32_t lockUntilMs = 0;
static bool manualLock = false;       // 用户按 'C' 键手动保持信道锁定
static const uint32_t LOCK_HOLD_MS = 8000;   // 锁定后至少守这么久，避免刚锁上就被一次丢包解锁

static inline uint32_t dwellForChan(int ch) {
  if (ch == 6) return 320;             // Ch 6 黄金信道充裕驻留，确保覆盖 1~2 个完整 beacon 周期
  if (ch == 1 || ch == 11) return 220; // 常见非重叠信道
  return 130;                          // 其余信道快速巡查
}

// 只有真正进过 ridAppEnter() 才允许跳信道。串口 GOTO 能直接跳到这一页而不走 enter，
// 那时混杂模式没开、WiFi 还是正常 STA——这时候去 set_channel 会把已连接的 STA
// 信道拽着乱跑，把网搞断。
static bool running = false;

// PC 主导模式下的无屏流：跟带屏页共用同一套混杂模式 / 解码 / 合并 / 跳频（见 ridStreamStart）。
// streamMode 为 true 时 ridStreamPump 吐协议格式的 "RID {...}" 行，而不是给 rid_view.py 用的 RIDPKT。
static bool streamMode = false;
static std::atomic<uint32_t> streamPkts{0};   // 流开启以来收到的 RID 报文数（回调里自增，主循环里读）

void ridAppPreLockChannel(uint8_t chan, uint32_t holdMs) {
  if (chan < 1 || chan > 13) return;
  if (!running || manualLock) return;
  const uint32_t until = millis() + holdMs;
  if (lockChan == chan && (int32_t)(until - lockUntilMs) <= 0) return;
  lockChan = chan;
  curChan = chan;
  lockUntilMs = until;
  esp_wifi_set_channel(curChan, WIFI_SECOND_CHAN_NONE);
}

static void mergeDrone(const uint8_t* mac, const OdidResult& n, int8_t rssi, uint8_t chan,
                       bool injected = false) {
  if (!drones) return;
  int idx = -1;
  for (int i = 0; i < droneCount; i++)
    if (memcmp(drones[i].mac, mac, 6) == 0) { idx = i; break; }
  if (idx < 0) {
    idx = droneCount;
    if (idx >= DRONE_MAX) {
      // 表满了就顶掉最久没消息的那条，否则一进热闹环境就被先来的占死、
      // 后面真正在飞的反而挤不进来
      uint32_t oldest = 0; int victim = -1;
      for (int i = 0; i < DRONE_MAX; i++) {
        uint32_t age = millis() - drones[i].lastMs;
        if (age > LOST_MS && age > oldest) { oldest = age; victim = i; }
      }
      if (victim < 0) return;                 // 全都还活着，这条只能丢
      idx = victim;
    }
    // ⚠️ 驱逐或初始化槽位时必须先拉低 fresh，防主循环 ridStreamPump 并发读到重置了一半的内容
    drones[idx].fresh = false;
    std::atomic_thread_fence(std::memory_order_release);
    memcpy(drones[idx].mac, mac, 6);
    drones[idx].r = OdidResult();
    drones[idx].packets = 0;
    drones[idx].rssi = rssi;
    drones[idx].rssiAvgX8 = (int16_t)rssi * 8;
    drones[idx].chan = chan;
    drones[idx].lastMs = millis();
    drones[idx].firstMs = drones[idx].lastMs;
    drones[idx].injected = injected;
    drones[idx].lastEmitMs = 0;
    // ⚠️ 必须最后才让 droneCount 覆盖到这一条。回调跑在 WiFi 任务、绘制跑在主循环，
    // 先自增再填内容的话，主循环会读到一条没初始化的记录（编号是随机字节、rssi 乱跳）。
    if (idx == droneCount) droneCount = idx + 1;
  }
  Drone& d = drones[idx];
  d.packets++;
  if (!injected) streamPkts.fetch_add(1, std::memory_order_relaxed);
  d.rssi = rssi;
  d.rssiAvgX8 += ((int16_t)rssi * 8 - d.rssiAvgX8) / 8;   // 约 8 包的时间常数
  d.chan = chan;
  d.lastMs = millis();
  if (injected) d.injected = true;
  odidMerge(d.r, n);
  std::atomic_thread_fence(std::memory_order_release);
  d.fresh = true;   // 标记一下，真正的串口输出在主循环里做
}

// 混杂模式回调（跑在 WiFi 任务上下文）。帧解析全部交给 odid.cpp 那份共用实现，
// 这里只负责把结果并进表里——回调里别干重活，也别碰屏幕。
static void ridCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!running || !drones || type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  OdidResult r;
  bool isCand = false;
  if (ridFromMgmtFrame(p->payload, p->rx_ctrl.sig_len, r, &isCand)) {
    mergeDrone(p->payload + 10, r, p->rx_ctrl.rssi, p->rx_ctrl.channel);   // addr2 = 发送方
  } else if (isCand) {
    // SSID 以 RID- 开头（确认为 Remote ID 广播源），即便本帧 IE 微损亦预先锁定信道
    ridAppPreLockChannel(p->rx_ctrl.channel, 4000);
  }
}

// 开/关混杂模式接收 + 目标表。带屏页和 PC 流共用，两边的准备/还原动作必须是同一份：
// 一边漏了 WiFi 还原或热点恢复，退出后网就回不来了。
static bool rxStart(int fixedChan) {
  hotspotSuspend();
  if (!drones) drones = (Drone*)malloc(sizeof(Drone) * DRONE_MAX);
  if (!drones) {
    hotspotResume();
    return false;
  }
  droneCount = 0; selIdx = 0; listTop = 0;
  lockChan = 0; lockUntilMs = 0; manualLock = false;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();                 // STA 会自己跳信道，跟嗅探打架
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(ridCb);
  esp_wifi_set_promiscuous(true);
  hopIdx = 0;
  curChan = HOP_SEQ[0];
  if (fixedChan >= 1 && fixedChan <= 13) {       // 指定了信道：蹲死不跳（跟 RIDSCAN 的信道参数同义）
    curChan = fixedChan;
    manualLock = true;
    lockChan = fixedChan;
    lockUntilMs = 0xFFFFFFFF;
  }
  esp_wifi_set_channel(curChan, WIFI_SECOND_CHAN_NONE);
  lastHopMs = millis();
  running = true;
  return true;
}

static void rxStop() {
  running = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  Drone* tmp = drones;
  drones = nullptr;
  droneCount = 0;
  vTaskDelay(pdMS_TO_TICKS(20));      // 先置空指针再延时，给 Core 0 在途 ridCb 充足退场时间，防 UAF
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  bootWifiStart();                   // 跟 wsniff 一样：把 WiFi 还原成后台自动重连
  if (tmp) {
    free(tmp);
  }
  hotspotResume();
}

void ridAppEnter() {
  alertLevel = loadInt("ridalert", "lvl", 0);
  if (alertLevel < 0 || alertLevel >= ALERT_LEVEL_N) alertLevel = 0;
  alertSound = loadBool("ridalert", "snd", true);
  for (auto& a : alertSt) a = AlertState();
  alertAnyInside = false;
  if (streamMode) ridStreamStop();   // 正常走不到（PC MODE 里进不了菜单），但两边抢同一块射频，宁可多防一手
  if (!rxStart(0)) {
    screen = SCREEN_MENU;
    dirty = true;
    return;
  }
}

void ridAppExit() {
  alertLedRelease();
  alertFlashUntilMs = 0;
  if (isRecording) {
    ridAppStopRecord();
  }
  rxStop();
}

// 把有新数据的目标往串口吐一行 RIDPKT {json}，给 tools/rid_view.py 用。
// 若开启了机载 SD 录制，同步将纯 JSON 行写入 SD 卡文件。
static const uint32_t EMIT_MIN_MS = 250;

// ODID 解码结果 → JSON 字段（每个字段前带逗号，调用方负责前面的花括号和第一个字段）。
// 带屏页 / Wi-Fi 流 / BLE 流共用同一份，字段名和格式保持一致（协议 §8.5 / §8.6）。
void ridAppendOdidFields(char* buf, int cap, int& n, const OdidResult& r) {
  if (r.haveBasic) {
    appendf(buf, cap, n, ",\"id\":\"");
    appendJsonStr(buf, cap, n, r.uasId);
    appendf(buf, cap, n, "\"");
  }
  if (r.haveOperatorId) {
    appendf(buf, cap, n, ",\"reg\":\"");
    appendJsonStr(buf, cap, n, r.operatorId);
    appendf(buf, cap, n, "\"");
  }
  if (r.haveStatus)     appendf(buf, cap, n, ",\"state\":%u", (unsigned)r.opStatus);
  if (r.haveLoc)        appendf(buf, cap, n, ",\"lat\":%.7f,\"lon\":%.7f", r.lat, r.lon);
  if (r.haveHeight)     appendf(buf, cap, n, ",\"agl\":%.1f,\"aglIsGround\":%d", r.height, r.heightIsAgl ? 1 : 0);
  if (r.haveAlt)        appendf(buf, cap, n, ",\"alt\":%.1f", r.altGeo);
  if (r.haveSpeed)      appendf(buf, cap, n, ",\"spd\":%.2f", r.speed);
  if (r.heading >= 0)   appendf(buf, cap, n, ",\"hdg\":%d", r.heading);
  if (r.vspeed != 0)    appendf(buf, cap, n, ",\"vs\":%.1f", r.vspeed);
  if (r.haveSys)        appendf(buf, cap, n, ",\"plat\":%.7f,\"plon\":%.7f", r.pilotLat, r.pilotLon);
  if (r.haveAstmTime)   appendf(buf, cap, n, ",\"t_hour\":%.1f", r.astmTimeSec);       // 整点后秒数
  if (r.haveGbTime)     appendf(buf, cap, n, ",\"t_ms\":%llu", (unsigned long long)r.gbTimeMs); // Unix 毫秒
  if (r.haveAuth) {
    appendf(buf, cap, n, ",\"auth\":%u,\"authp\":%u", (unsigned)r.authType, (unsigned)r.authPages);
    if (r.authHavePage0)
      appendf(buf, cap, n, ",\"authn\":%u,\"authlen\":%u,\"autht\":%lu", (unsigned)r.authLastPage + 1,
              (unsigned)r.authLen, (unsigned long)r.authTime);
  }
}

// 一个目标的 JSON。两种外壳，字段名一致：
//   带屏页（rid_view.py 用）：RIDPKT {"ms":..,"mac":..,"fake":0|1,...,"mylat":..}
//   PC 协议（cardputer-bridge）：RID {"t":"d","ts":..,"mac":..,...}  —— 协议 v1.1 的数据行都带 ts；
//     fake 只在注入样本时出现（PC 端要能区分），我方坐标 PC 端自己有，不带。
// 运营人 ID 观察结果。串口注入的假样本只发一次包，按"包数够了"算，否则永远停在 pending 看不到效果。
static OpIdState droneOpId(const Drone& d, uint32_t now) {
  return opidCheck(d.r.haveOperatorId, d.r.operatorId, d.r.uasId, d.r.haveBasic,
                   now - d.firstMs, d.injected ? OPID_MIN_PKTS : d.packets);
}

static bool buildDroneJson(char* buf, int cap, const Drone& d, uint32_t now, bool pc) {
  const OdidResult& r = d.r;
  int n = 0;
  buf[0] = '\0';
  if (pc) {
    appendf(buf, cap, n, "{\"t\":\"d\",\"ts\":%lu,", (unsigned long)now);
  } else {
    appendf(buf, cap, n, "{\"ms\":%lu,", (unsigned long)now);
  }
  appendf(buf, cap, n,
          "\"mac\":\"%02x%02x%02x%02x%02x%02x\",\"rssi\":%d,\"ch\":%u,\"pkts\":%lu,\"std\":\"%s\"",
          d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
          (int)d.rssi, (unsigned)d.chan, (unsigned long)d.packets,
          r.isGb ? "GB" : "ASTM");
  if (pc) { if (d.injected) appendf(buf, cap, n, ",\"fake\":1"); }
  else    appendf(buf, cap, n, ",\"fake\":%d", d.injected ? 1 : 0);
  ridAppendOdidFields(buf, cap, n, r);
  const OpIdState opid = droneOpId(d, now);
  if (opid != OPID_PENDING) appendf(buf, cap, n, ",\"opid\":\"%s\"", opidName(opid));
  if (!pc && gnssHasFix()) appendf(buf, cap, n, ",\"mylat\":%.7f,\"mylon\":%.7f", gnssLat(), gnssLng());
  appendf(buf, cap, n, "}");

  // 严格校验是否被截断：n 达到容量上限或未以 '}' 正常收尾时整行丢弃，绝不出不合法 JSON
  if (n >= cap - 1 || n < 2 || buf[n - 1] != '}') {
    buf[0] = '\0';
    return false;
  }
  return true;
}

static void ridStreamPump() {
  const uint32_t now = millis();
  for (int i = 0; i < droneCount; i++) {
    Drone& d = drones[i];
    if (!d.fresh) continue;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (now - d.lastEmitMs < EMIT_MIN_MS) continue;
    d.fresh = false;
    d.lastEmitMs = now;

    char jsonBuf[576];
    if (!buildDroneJson(jsonBuf, sizeof(jsonBuf), d, now, streamMode)) {
      continue;
    }

    Serial.print(streamMode ? "RID " : "RIDPKT ");
    Serial.println(jsonBuf);

    if (!streamMode && isRecording && ridLogFile) {
      ridLogFile.println(jsonBuf);
      recPackets++;
      if (now - lastFlushMs > 2000) {
        ridLogFile.flush();
        lastFlushMs = now;
      }
    }
    return;                 // 一帧只吐一个目标，别让多目标时一次占住主循环太久
  }
}

// ---- PC 主导模式：无屏 Remote ID 流（协议见 cardputer-bridge docs/pc-mode-protocol.md §8.5）----
bool ridStreamIsActive() { return streamMode; }
int  ridStreamDroneCount() { return streamMode ? droneCount : 0; }
uint32_t ridStreamPktCount() { return streamPkts.load(std::memory_order_relaxed); }
int  ridStreamChannel() { return streamMode ? (manualLock ? curChan : 0) : 0; }

// PC 主导模式下开混杂流（RID）或 BLE 流前，检查是不是有别的页面占着射频/Wi-Fi。
// 返回 "busy: ..." 原因文本，空闲返回 nullptr。两条流共用，原因文本进协议 §8.5 / §8.6。
const char* pcmRadioConflict() {
  if (screen == SCREEN_RID || screen == SCREEN_RID_DETAIL) return "busy: Drone ID app open";
  if (screen == SCREEN_WSNIFF)   return "busy: Wi-Fi sniffer open";
  if (screen == SCREEN_WARDRIVE) return "busy: Wardrive open";
  if (screen == SCREEN_WIFI_CHAN || screen == SCREEN_WIFI_CHAN_DETAIL) return "busy: Wi-Fi channel analyzer open";
  if (isBleScreen(screen))       return "busy: Bluetooth app open";
  if (radioIsActive())           return "busy: Radio stream open";
  if (chatBusy())                return "busy: Chat active";
  if (screen == SCREEN_SSH && sshIsTerminalActive()) return "busy: SSH terminal active";
  return nullptr;
}

void ridStreamStart(int fixedChan) {
  if (const char* why = pcmRadioConflict()) {
    Serial.printf("RID {\"t\":\"err\",\"msg\":\"%s\"}\n", why);
    return;
  }
  if (btStreamIsActive()) {
    Serial.println("RID {\"t\":\"err\",\"msg\":\"busy: BLE stream on\"}");
    return;
  }
  // BLE 流停掉后只是挂起、堆还占着；Wi-Fi 要起来必须先彻底释放（付一次库泄漏，见 bt.cpp 顶部）
  if (btHoldsHeap()) btReleaseForOtherApps();

  // 已经在流了再发一次 RID ON = 换参数重开（先发 end 再发新的 start），跟 LORA SNIFF 一致
  if (streamMode) ridStreamStop();
  streamPkts.store(0, std::memory_order_relaxed);
  if (!rxStart(fixedChan)) {
    streamMode = false;
    Serial.println("RID {\"t\":\"err\",\"msg\":\"out of memory\"}");
    return;
  }
  streamMode = true;
  Serial.printf("RID {\"t\":\"start\",\"chan\":%d}\n", ridStreamChannel());
}

void ridStreamStop() {
  // 幂等：本来就没开也回 end，PC 端发 RID OFF 总能拿到确定的答复
  if (streamMode) {
    streamMode = false;
    rxStop();
  }
  Serial.println("RID {\"t\":\"end\"}");
}

static void alertBeep(uint8_t ev) {
  if (!alertSound || volVal() == 0) return;
  M5.Speaker.setVolume(volVal());
  if (ev & ALERT_EMERGENCY) {                    // 紧急：三声高音
    M5.Speaker.tone(2637, 110);
    M5.Speaker.tone(2637, 110, -1, false);
    M5.Speaker.tone(2637, 110, -1, false);
  } else {                                       // 进入告警圈：上行两声
    M5.Speaker.tone(1568, 120);
    M5.Speaker.tone(2093, 180, -1, false);
  }
}

static void alertLedRelease() {
  if (alertLedOwned) { ledSetOverride(false); alertLedOwned = false; }
}

static void alertUpdate(uint32_t now) {
  const bool haveMe = gnssHasFix();
  const double myLat = haveMe ? gnssLat() : 0, myLon = haveMe ? gnssLng() : 0;
  uint8_t ev = 0;
  bool inside = false;
  const int n = droneCount < 12 ? droneCount : 12;
  for (int i = 0; i < n; i++) {
    const Drone& d = drones[i];
    const bool lost = (now - d.lastMs) > LOST_MS;
    float distM = -1;
    if (haveMe && d.r.haveLoc) distM = gcKm(myLat, myLon, d.r.lat, d.r.lon) * 1000.0f;
    ev |= alertEval(alertSt[i], distM, alertLevel, d.r.haveStatus && d.r.opStatus == 3, lost);
    if (alertSt[i].inside) inside = true;
  }
  alertAnyInside = inside;
  if (ev) {
    alertFlashUntilMs = now + 3000;
    alertLastBeepMs = now;
    alertBeep(ev);
  } else if (inside && now - alertLastBeepMs >= 8000) {   // 还在圈内：隔一会儿轻提醒一下
    alertLastBeepMs = now;
    if (alertSound && volVal()) { M5.Speaker.setVolume(volVal()); M5.Speaker.tone(1568, 70); }
  }
  // 红灯闪：只在没人接管 LED 时才抢，退出时务必还回去
  if ((int32_t)(now - alertFlashUntilMs) < 0) {
    if (!alertLedOwned && !ledOverrideActive()) { ledSetOverride(true); alertLedOwned = true; }
    if (alertLedOwned) { const bool on = (now / 150) % 2 == 0; ledShowRGB(on ? 255 : 0, 0, 0); }
  } else alertLedRelease();
}

void ridAppUpdate() {
  if (!running || !drones) return;
  ridStreamPump();
  const uint32_t now = millis();
  alertUpdate(now);

  if (manualLock) return;   // 手动锁频模式保持

  // 有活着的目标就锁它的信道；优先锁最新收到报文的活跃目标，都丢了才回去跳频
  int liveChan = 0;
  uint32_t freshestMs = 0;
  for (int i = 0; i < droneCount; i++) {
    const uint8_t ch = drones[i].chan;
    if (now - drones[i].lastMs < LOST_MS && ch >= 1 && ch <= 13) {
      if (drones[i].lastMs >= freshestMs) {
        freshestMs = drones[i].lastMs;
        liveChan = ch;
      }
    }
  }

  if (liveChan) {
    if (lockChan != liveChan) {
      lockChan = liveChan;
      curChan = liveChan;
      esp_wifi_set_channel(curChan, WIFI_SECOND_CHAN_NONE);
    }
    lockUntilMs = now + LOCK_HOLD_MS;
    return;
  }
  if (lockChan && (int32_t)(now - lockUntilMs) < 0) return;     // 候选锁或刚丢失时守一会儿，避免频繁颠簸
  lockChan = 0;

  const uint32_t dwell = dwellForChan(curChan);
  if (now - lastHopMs > dwell) {
    hopIdx = (hopIdx + 1) % HOP_COUNT;
    curChan = HOP_SEQ[hopIdx];
    esp_wifi_set_channel(curChan, WIFI_SECOND_CHAN_NONE);
    lastHopMs = now;
  }
}

void ridAppKey(char k) {
  if (k == 's' || k == 'S') {
    ridAppToggleRecord();
    dirty = true;
    return;
  }
  if (k == ';' || k == ',') {
    if (droneCount > 0) { listMove(selIdx, listTop, droneCount, 3, -1); dirty = true; }
  }
  else if (k == '.' || k == '/') {
    if (droneCount > 0) { listMove(selIdx, listTop, droneCount, 3, +1); dirty = true; }
  }
  else if (k == 'r' || k == 'R') {
    droneCount = 0; selIdx = 0; listTop = 0; dirty = true;
    for (auto& a : alertSt) a = AlertState();
  }
  else if (k == 'a' || k == 'A') {
    alertLevel = (alertLevel + 1) % ALERT_LEVEL_N;
    saveInt("ridalert", "lvl", alertLevel);
    for (auto& a : alertSt) a = AlertState();     // 换档后让已在圈内的重新判一次
    dirty = true;
  }
  else if (k == 'b' || k == 'B') {
    alertSound = !alertSound;
    saveBool("ridalert", "snd", alertSound);
    if (alertSound && volVal()) { M5.Speaker.setVolume(volVal()); M5.Speaker.tone(1568, 80); }   // 开的时候响一声确认
    dirty = true;
  }
  else if (k == 'm' || k == 'M') {
    radarView = !radarView; dirty = true;
  }
  else if (k == 'c' || k == 'C') {
    // 切换手动锁定 / 动态轮询
    manualLock = !manualLock;
    if (manualLock) {
      lockChan = curChan;
      lockUntilMs = 0xFFFFFFFF;
    } else {
      lockChan = 0;
      lockUntilMs = 0;
    }
    dirty = true;
  }
  const int VIS = 3;
  listClampScroll(selIdx, listTop, droneCount, VIS);
}

// 从我到目标的方位角（正北顺时针）
static float bearingTo(double lat1, double lon1, double lat2, double lon2) {
  const double D = M_PI / 180.0;
  double dLon = (lon2 - lon1) * D;
  double y = sin(dLon) * cos(lat2 * D);
  double x = cos(lat1 * D) * sin(lat2 * D) - sin(lat1 * D) * cos(lat2 * D) * cos(dLon);
  double b = atan2(y, x) / D;
  return (float)(b < 0 ? b + 360.0 : b);
}

// 梯形 4 级信号条
static int8_t rssiShown(const Drone& d) {
  return (int8_t)((d.rssiAvgX8 + (d.rssiAvgX8 < 0 ? -4 : 4)) / 8);   // 四舍五入
}

static void drawRssiBars(int x, int y, int8_t rssi) {
  int level = 0;
  if (rssi >= -65)      level = 4;
  else if (rssi >= -75) level = 3;
  else if (rssi >= -85) level = 2;
  else if (rssi > -105) level = 1;

  const uint16_t colActive = (level >= 3) ? TFT_GREEN : (level == 2 ? TFT_YELLOW : TFT_RED);
  const uint16_t colDim = 0x2104; // 暗灰底柱
  for (int b = 0; b < 4; b++) {
    int bh = 3 + b * 2;
    int bx = x + b * 3;
    int by = y + (9 - bh);
    cv.fillRect(bx, by, 2, bh, (b < level) ? colActive : colDim);
  }
}

// 状态胶囊徽标（AIR 绿底 / GND 灰底 / EMG 亮红 / LOST 暗灰 / FAKE 洋红）
static void drawStatusBadge(int x, int y, const OdidResult& r, bool lost, bool injected) {
  const char* text = "?";
  uint16_t bg = 0x18C3, fg = TFT_DARKGREY;
  if (injected) {
    text = "FAKE"; bg = 0x8010; fg = TFT_MAGENTA;
  } else if (lost) {
    text = "LOST"; bg = 0x18C3; fg = TFT_DARKGREY;
  } else if (r.haveStatus) {
    switch (r.opStatus) {
      case 1: text = "GND"; bg = 0x2104; fg = TFT_LIGHTGREY; break;
      case 2: text = "AIR"; bg = 0x0320; fg = TFT_GREEN;     break;
      case 3: text = "EMG"; bg = 0x8800; fg = TFT_RED;       break;
      case 4: case 5: text = "FAIL"; bg = 0x8A00; fg = TFT_ORANGE; break;
      default: text = "?"; bg = 0x18C3; fg = TFT_DARKGREY; break;
    }
  }
  const int w = 26, h = 10;
  cv.fillRoundRect(x, y, w, h, 2, bg);
  cv.setTextDatum(middle_center);
  cv.setTextSize(1);
  cv.setTextColor(fg, bg);
  cv.drawString(text, x + w / 2, y + h / 2);
}

// 厂商暗青胶囊徽标
static void drawVendorBadge(int x, int y, const char* vendor) {
  if (!vendor || !vendor[0]) return;
  const int w = 24, h = 10;
  cv.fillRoundRect(x, y, w, h, 2, 0x01E8); // 深暗青底
  cv.setTextDatum(middle_center);
  cv.setTextSize(1);
  cv.setTextColor(TFT_CYAN, 0x01E8);
  cv.drawString(vendor, x + w / 2, y + h / 2);
}

// 雷达视图：北向上，我在圆心，圆周 = 当前量程；选中的机体高亮，右侧给出它的读数。
static void drawAlertSettings(int x, int y) {
  char b[24];
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(alertLevel ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
  if (alertLevel) snprintf(b, sizeof(b), "a ALR %um", (unsigned)ALERT_RANGE_M[alertLevel]);
  else            snprintf(b, sizeof(b), "a ALR off");
  cv.drawString(b, x, y);
  int alrW = cv.textWidth(b);
  cv.setTextColor(alertSound ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
  cv.drawString(alertSound ? "b SND on" : "b SND off", x + alrW + 6, y);
}

static void drawRidRadar(uint32_t now, bool haveMe, double myLat, double myLon) {
  const int cx = 68, cy = 72, R = 50;
  if (!haveMe) {
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("Radar needs a GNSS fix", SW / 2, 54);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("Switch to list or wait for satellites", SW / 2, 70);
    drawAlertSettings(SW / 2 - 50, 92);

    cv.setTextDatum(bottom_left);
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("M", 6, SH - 2);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("list", 14, SH - 2);
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("A", 54, SH - 2);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("alr", 62, SH - 2);
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("B", 94, SH - 2);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("snd", 102, SH - 2);
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 136, SH - 2);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("back", 144, SH - 2);
    return;
  }
  // 量程：能装下所有有位置目标的最小一档
  float maxD = 0;
  int noPos = 0;
  for (int i = 0; i < droneCount; i++) {
    if (!drones[i].r.haveLoc) { noPos++; continue; }
    RadarPt q = radarRel(myLat, myLon, drones[i].r.lat, drones[i].r.lon);
    if (q.distM > maxD) maxD = q.distM;
  }
  const float range = radarPickRange(maxD);

  cv.drawCircle(cx, cy, R, 0x2945);
  cv.drawCircle(cx, cy, R / 2, 0x1082);
  cv.drawFastHLine(cx - R, cy, 2 * R + 1, 0x1082);
  cv.drawFastVLine(cx, cy - R, 2 * R + 1, 0x1082);
  cv.setTextDatum(middle_center); cv.setTextColor(0x8410, TFT_BLACK);
  cv.drawString("N", cx, cy - R + 6);
  cv.fillCircle(cx, cy, 2, TFT_WHITE);

  for (int i = 0; i < droneCount; i++) {
    const Drone& d = drones[i];
    if (!d.r.haveLoc) continue;
    const bool lost = (now - d.lastMs) > LOST_MS;
    const bool sel = (i == selIdx);
    int dx, dy; bool clipped;
    radarToPx(radarRel(myLat, myLon, d.r.lat, d.r.lon), range, R, dx, dy, clipped);
    const int x = cx + dx, y = cy + dy;
    const uint16_t col = lost ? 0x4208 : sel ? TFT_YELLOW : (d.r.opStatus == 3 ? TFT_RED : ACCENT);
    if (sel && d.r.haveSys && !lost) {   // 选中机的飞手/起飞点：空心方框
      int px, py; bool pc;
      radarToPx(radarRel(myLat, myLon, d.r.pilotLat, d.r.pilotLon), range, R, px, py, pc);
      cv.drawRect(cx + px - 2, cy + py - 2, 5, 5, TFT_GREEN);
    }
    if (d.r.heading >= 0 && !lost) {      // 航向短线
      const float a = d.r.heading * (float)(M_PI / 180.0);
      cv.drawLine(x, y, x + (int)lroundf(sinf(a) * 9), y - (int)lroundf(cosf(a) * 9), col);
    }
    if (clipped) cv.drawCircle(x, y, 3, col); else cv.fillCircle(x, y, sel ? 3 : 2, col);
    char num[4]; snprintf(num, sizeof(num), "%d", i + 1);
    cv.setTextDatum(top_left); cv.setTextColor(col, TFT_BLACK);
    cv.drawString(num, x + 4, y - 9);
  }

  // 右侧读数
  const int px0 = 130;
  char b[40];
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x8410, TFT_BLACK);
  if (range >= 1000) snprintf(b, sizeof(b), "RNG %.0fkm", range / 1000);
  else               snprintf(b, sizeof(b), "RNG %.0fm", range);
  cv.drawString(b, px0, 18);
  if (alertAnyInside) {
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.drawString("!ALERT", px0 + 64, 18);
  }
  if (selIdx >= 0 && selIdx < droneCount) {
    const Drone& d = drones[selIdx];
    const OdidResult& r = d.r;
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    snprintf(b, sizeof(b), "#%d %s", selIdx + 1, r.haveBasic ? r.uasId : "(no id)");
    truncInPlace(b, 16);
    cv.drawString(b, px0, 31);
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    if (r.haveLoc) {
      const float km = gcKm(myLat, myLon, r.lat, r.lon);
      const float br = bearingTo(myLat, myLon, r.lat, r.lon);
      if (km < 1.0f) snprintf(b, sizeof(b), "%dm %s", (int)(km * 1000), cardOf(br));
      else           snprintf(b, sizeof(b), "%.1fkm %s", km, cardOf(br));
    } else snprintf(b, sizeof(b), "no position");
    cv.drawString(b, px0, 44);
    cv.setTextColor(0x05E8, TFT_BLACK);
    int n = 0; b[0] = 0;
    if (r.haveHeight)   appendf(b, sizeof(b), n, "H %.0fm ", r.height);
    else if (r.haveAlt) appendf(b, sizeof(b), n, "Alt %.0fm ", r.altGeo);
    if (r.haveSpeed && r.speed > 0.2f) appendf(b, sizeof(b), n, "%.1fm/s", r.speed);
    cv.drawString(b, px0, 57);
    if (r.heading >= 0) { snprintf(b, sizeof(b), "HDG %d", r.heading); cv.drawString(b, px0, 70); }
    snprintf(b, sizeof(b), "%ddB", rssiShown(d));
    cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    cv.drawString(b, px0, 83);
    drawRssiBars(px0 + 38, 83, rssiShown(d));
  }
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (noPos) { snprintf(b, sizeof(b), "%d w/o pos", noPos); cv.drawString(b, px0, 96); }
  drawAlertSettings(px0, 109);

  // 底部快捷键提示
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("M", 6, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("list", 14, SH - 2);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString(";", 46, SH - 2);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString(".", 53, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("sel", 61, SH - 2);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Ent", 89, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("info", 109, SH - 2);
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 141, SH - 2);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("back", 148, SH - 2);
}

void drawRidApp() {
  cv.fillScreen(TFT_BLACK);
  const uint32_t now = millis();

  bool anyFake = false;
  if (drones) {
    for (int i = 0; i < droneCount; i++) if (drones[i].injected) anyFake = true;
  }

  char hdr[32];
  char chanStr[16];
  if (manualLock)     snprintf(chanStr, sizeof(chanStr), "ch%d HOLD", curChan);
  else if (lockChan)  snprintf(chanStr, sizeof(chanStr), "ch%d LOCK", lockChan);
  else                snprintf(chanStr, sizeof(chanStr), "ch%d SCAN", curChan);

  if (isRecording) {
    const bool blink = (now / 500) % 2 == 0;
    snprintf(hdr, sizeof(hdr), "%s REC %lup", chanStr, (unsigned long)recPackets);
    drawPageHeader(anyFake ? "Drone ID [SIM]" : "Drone ID", hdr, blink ? TFT_RED : 0x8800);
    if (blink) {
      const int tw = cv.textWidth(hdr);
      cv.fillCircle(SW - tw - 12, 6, 2, TFT_RED);
    }
  } else {
    if (alertAnyInside) {
      drawPageHeader(anyFake ? "RID [SIM]" : "Drone ID", chanStr, (manualLock || lockChan) ? ACCENT : 0);
      const int rtw = cv.textWidth(chanStr);
      const int bw = 38, bh = 10;
      const int bx = SW - 6 - rtw - bw - 4;
      const bool blink = ((now / 400) % 2) == 0;
      const uint16_t bBg = blink ? TFT_RED : 0x8800;
      cv.fillRoundRect(bx, 2, bw, bh, 2, bBg);
      cv.setTextDatum(middle_center); cv.setTextSize(1);
      cv.setTextColor(TFT_WHITE, bBg);
      cv.drawString("ALERT", bx + bw / 2, 2 + bh / 2);
    } else {
      drawPageHeader(anyFake ? "Drone ID [SIM]" : "Drone ID", chanStr, (manualLock || lockChan) ? ACCENT : 0);
    }
  }

  if (droneCount == 0 || !drones) {
    // 极客风空状态雷达扫描盘与说明
    const int cx = SW / 2, cy = 54;
    cv.drawCircle(cx, cy, 36, 0x1082);
    cv.drawCircle(cx, cy, 20, 0x1082);
    cv.drawFastHLine(cx - 42, cy, 84, 0x1082);
    cv.drawFastVLine(cx, cy - 40, 80, 0x1082);
    const float ang = ((now / 7) % 360) * (float)(M_PI / 180.0);
    cv.drawLine(cx, cy, cx + (int)(cosf(ang) * 36), cy + (int)(sinf(ang) * 36), ACCENT);

    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    cv.drawString("Listening for Remote ID (2.4GHz)...", cx, 100);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("Ch 6 / 1 / 11 weighted   [C] lock chan", cx, 112);

    cv.setTextDatum(bottom_left);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("S rec   C lock   r reset   ` back", 4, SH - 2);
    if (hotspotIsSuspended()) {
      cv.setFont(&fonts::efontCN_12); cv.setTextColor(TFT_YELLOW, TFT_BLACK);
      cv.drawString("热点已暂停", SW - 65, SH - 2); cv.setFont(&fonts::Font0);
    }
    drawSdToast(now);
    return;
  }

  const bool haveMe = gnssHasFix();
  const double myLat = haveMe ? gnssLat() : 0, myLon = haveMe ? gnssLng() : 0;

  if (radarView) {
    drawRidRadar(now, haveMe, myLat, myLon);
    drawSdToast(now);
    return;
  }

  const int startY = 16;
  const int rowH = 34, cardH = 31, VIS = 3;
  for (int p = 0; p < VIS; p++) {
    const int i = listTop + p;
    if (i >= droneCount) break;
    const Drone& d = drones[i];
    const bool lost = (now - d.lastMs) > LOST_MS;
    const int y = startY + p * rowH;
    const bool sel = (i == selIdx);

    const uint16_t cardBg = sel ? CARD_BG : 0x0841;
    if (sel) {
      cv.fillRoundRect(3, y, SW - 6, cardH, 3, CARD_BG);
      cv.fillRect(3, y + 2, 3, cardH - 4, ACCENT);
    } else {
      cv.fillRoundRect(3, y, SW - 6, cardH, 3, 0x0841);
      cv.drawRoundRect(3, y, SW - 6, cardH, 3, 0x18C3);
    }

    // 第一行：厂商徽标 + 机身序列号 + 状态徽标
    const char* vname = odidVendorName(d.r.uasId);
    int textX = 7;
    if (vname[0]) {
      drawVendorBadge(7, y + 3, vname);
      textX = 34;
    }
    const char* id = d.r.haveBasic ? d.r.uasId : (d.r.haveOperatorId ? d.r.operatorId : "(listening...)");
    char idBuf[28];
    snprintf(idBuf, sizeof(idBuf), "%s", id);
    truncInPlace(idBuf, vname[0] ? 20 : 25);
    cv.setTextDatum(top_left); cv.setTextSize(1);
    cv.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, cardBg);
    cv.drawString(idBuf, textX, y + 4);
    drawStatusBadge(SW - 31, y + 3, d.r, lost, d.injected);

    // 第二行：距离/方位 + 高度/速度 + 信号梯形条 & dBm
    char sub[48]; int n = 0;
    if (haveMe && d.r.haveLoc) {
      const float km = gcKm(myLat, myLon, d.r.lat, d.r.lon);
      const float br = bearingTo(myLat, myLon, d.r.lat, d.r.lon);
      if (km < 1.0f) appendf(sub, sizeof(sub), n, "%dm %s ", (int)(km * 1000), cardOf(br));
      else           appendf(sub, sizeof(sub), n, "%.1fkm %s ", km, cardOf(br));
    }
    if (d.r.haveHeight)   appendf(sub, sizeof(sub), n, "%s %.0fm ", d.r.heightIsAgl ? "AGL" : "H", d.r.height);
    else if (d.r.haveAlt) appendf(sub, sizeof(sub), n, "Alt %.0fm ", d.r.altGeo);
    if (d.r.haveSpeed && d.r.speed > 0.2f) appendf(sub, sizeof(sub), n, "%.1fm/s ", d.r.speed);
    if (n == 0) appendf(sub, sizeof(sub), n, "%lupkt  %lus ago", (unsigned long)d.packets, (unsigned long)((now - d.lastMs) / 1000));

    truncInPlace(sub, 24);
    cv.setTextDatum(top_left); cv.setTextSize(1);
    cv.setTextColor(sel ? ACCENT : 0x05E8, cardBg);
    cv.drawString(sub, 7, y + 18);

    drawRssiBars(SW - 49, y + 18, rssiShown(d));
    char rssiStr[10]; snprintf(rssiStr, sizeof(rssiStr), "%ddB", rssiShown(d));
    cv.setTextDatum(top_right);
    cv.setTextColor(sel ? TFT_WHITE : TFT_DARKGREY, cardBg);
    cv.drawString(rssiStr, SW - 6, y + 18);
  }

  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString("Ent info M radar S rec C lock ` back", 4, SH - 2);
  drawSdToast(now);
}

void drawRidAppDetail() {
  cv.fillScreen(TFT_BLACK);
  if (selIdx < 0 || selIdx >= droneCount || !drones) { drawRidApp(); return; }
  const Drone& d = drones[selIdx];
  const OdidResult& r = d.r;
  const uint32_t now = millis();
  const bool lost = (now - d.lastMs) > LOST_MS;

  char hdr[32];
  if (isRecording) {
    const bool blink = (now / 500) % 2 == 0;
    snprintf(hdr, sizeof(hdr), "%ddB REC %lup", rssiShown(d), (unsigned long)recPackets);
    drawPageHeader(d.injected ? "Drone (DEMO)" : (r.isGb ? "Drone GB 42590" : "Drone ASTM"),
                   hdr, blink ? TFT_RED : 0x8800);
    if (blink) {
      const int tw = cv.textWidth(hdr);
      cv.fillCircle(SW - tw - 12, 6, 2, TFT_RED);
    }
  } else {
    snprintf(hdr, sizeof(hdr), "%ddBm  ch%u", rssiShown(d), d.chan);
    drawPageHeader(d.injected ? "Drone (DEMO)" : (r.isGb ? "Drone GB 42590" : "Drone ASTM"),
                   hdr, d.injected ? TFT_MAGENTA : 0);
  }

  char b[60];

  // 卡片 1: 飞行遥测态势 (y = 15..68, 高 53)
  cv.fillRoundRect(3, 15, SW - 6, 53, 3, 0x0841);
  cv.drawRoundRect(3, 15, SW - 6, 53, 3, 0x18C3);

  const char* vendor = odidVendorName(r.uasId);
  int vx = 7;
  if (vendor[0]) {
    drawVendorBadge(7, 18, vendor);
    vx = 34;
  }
  char uasBuf[32];
  snprintf(uasBuf, sizeof(uasBuf), "%s", r.haveBasic ? r.uasId : "(no id yet)");
  truncInPlace(uasBuf, vendor[0] ? 24 : 29);
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(ACCENT, 0x0841);
  cv.drawString(uasBuf, vx, 19);

  cv.setTextColor(TFT_LIGHTGREY, 0x0841);
  if (r.haveLoc) snprintf(b, sizeof(b), "Pos : %.6f, %.6f", r.lat, r.lon);
  else           snprintf(b, sizeof(b), "Pos : no valid GNSS fix");
  cv.drawString(b, 7, 30);

  if (gnssHasFix() && r.haveLoc) {
    const double myLat = gnssLat(), myLon = gnssLng();
    const float km = gcKm(myLat, myLon, r.lat, r.lon);
    const float br = bearingTo(myLat, myLon, r.lat, r.lon);
    int n = 0;
    if (km < 1.0f) appendf(b, sizeof(b), n, "From: %dm %s", (int)(km * 1000), cardOf(br));
    else           appendf(b, sizeof(b), n, "From: %.1fkm %s", km, cardOf(br));
    if (r.haveHeight)   appendf(b, sizeof(b), n, "  %s %.0fm", r.heightIsAgl ? "AGL" : "H", r.height);
    else if (r.haveAlt) appendf(b, sizeof(b), n, "  Alt %.0fm", r.altGeo);
    cv.setTextColor(ACCENT, 0x0841);
  } else {
    int n = 0; b[0] = 0;
    if (r.haveHeight) appendf(b, sizeof(b), n, "%s %.1fm  ", r.heightIsAgl ? "AGL" : "H", r.height);
    if (r.haveAlt)    appendf(b, sizeof(b), n, "Alt %.1fm", r.altGeo);
    if (n == 0) appendf(b, sizeof(b), n, "Alt : no height broadcast");
    cv.setTextColor(TFT_LIGHTGREY, 0x0841);
  }
  cv.drawString(b, 7, 41);

  int dn = 0; b[0] = 0;
  if (r.haveSpeed)   appendf(b, sizeof(b), dn, "Spd %.1fm/s  ", r.speed);
  if (r.heading >= 0) appendf(b, sizeof(b), dn, "Hdg %d %s  ", r.heading, cardOf(r.heading));
  if (r.haveAlt && gnssHasFix()) appendf(b, sizeof(b), dn, "Alt %.0fm", r.altGeo);
  else if (r.vspeed != 0)        appendf(b, sizeof(b), dn, "vs %.1f", r.vspeed);
  if (dn == 0) appendf(b, sizeof(b), dn, "Dyn : stationary/hovering");
  truncInPlace(b, 37);
  cv.setTextColor(TFT_LIGHTGREY, 0x0841);
  cv.drawString(b, 7, 52);

  // 卡片 2: 系统合规与运营人审计 (y = 71..121, 高 50)
  cv.fillRoundRect(3, 71, SW - 6, 50, 3, 0x0841);
  cv.drawRoundRect(3, 71, SW - 6, 50, 3, 0x18C3);

  {
    const OpIdState op = droneOpId(d, now);
    const char* stdName = r.isGb ? (r.gbVersion ? "GB V2.0" : "GB 42590") : "ASTM F3411";
    cv.setTextDatum(top_right);
    cv.setTextColor(TFT_LIGHTGREY, 0x0841);
    cv.drawString(stdName, SW - 7, 74);
    cv.setTextDatum(top_left);
    if (op == OPID_NONE) {                 // 观察够久仍没播：橙色提示
      cv.setTextColor(TFT_ORANGE, 0x0841);
      snprintf(b, sizeof(b), "Reg : not broadcast");
    } else if (op == OPID_SUSPECT) {       // 播了但像占位符：黄色，值后加问号
      cv.setTextColor(TFT_YELLOW, 0x0841);
      snprintf(b, sizeof(b), "Reg : %s ?", r.operatorId);
    } else {
      cv.setTextColor(TFT_LIGHTGREY, 0x0841);
      snprintf(b, sizeof(b), "Reg : %s", r.haveOperatorId ? r.operatorId : "--");
    }
    truncInPlace(b, 22);
    cv.drawString(b, 7, 74);
    cv.setTextColor(TFT_LIGHTGREY, 0x0841);
  }

  if (r.haveSys) snprintf(b, sizeof(b), "Pilot: %.6f, %.6f", r.pilotLat, r.pilotLon);
  else           snprintf(b, sizeof(b), "Pilot: no remote station pos");
  cv.drawString(b, 7, 85);

  cv.setTextColor(TFT_DARKGREY, 0x0841);
  cv.drawString("State:", 7, 96);
  drawStatusBadge(44, 96, r, lost, d.injected);

  drawRssiBars(SW - 88, 97, rssiShown(d));
  snprintf(b, sizeof(b), "%lupkts  %lus", (unsigned long)d.packets, (unsigned long)((now - d.lastMs) / 1000));
  cv.setTextDatum(top_right);
  cv.setTextColor(TFT_DARKGREY, 0x0841);
  cv.drawString(b, SW - 7, 96);

  cv.setTextDatum(top_left);
  if (r.haveSelfId) {
    snprintf(b, sizeof(b), "Desc: %s", r.selfId);
  } else if (r.haveAuth) {
    // 已收页数 / 总页数（页 0 没收到时总数未知）
    const int got = __builtin_popcount(r.authPages);
    if (r.authHavePage0) snprintf(b, sizeof(b), "Auth: %s %d/%d  %uB", odidAuthTypeName(r.authType),
                                  got, r.authLastPage + 1, (unsigned)r.authLen);
    else                 snprintf(b, sizeof(b), "Auth: %s %d/?", odidAuthTypeName(r.authType), got);
  } else if (r.haveUaClass) {
    snprintf(b, sizeof(b), "Class: %s  Coord: %s", gbClassName(r.uaClass), gbCoordSysName(r.coordSys));
  } else {
    snprintf(b, sizeof(b), "MAC : %02X:%02X:%02X:%02X:%02X:%02X",
             d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
  }
  truncInPlace(b, 37);
  cv.setTextColor(ICON_DIM, 0x0841);
  cv.drawString(b, 7, 107);

  cv.setTextDatum(bottom_left);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString("S rec   ` back", 4, SH - 2);
  if (hotspotIsSuspended()) {
    cv.setFont(&fonts::efontCN_12); cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("热点已暂停", SW - 65, SH - 2); cv.setFont(&fonts::Font0);
  }
  drawSdToast(now);
}

void ridAppInjectSample() {
  // 注入的条目全部打上 injected 标记，界面上会用洋红 FAKE 徽标 + 顶部横幅标出来
  // 2026-08-08 实测抓到的两帧，原始字节和现场情况见 tools/odidtest/samples_gb.md
  static const uint8_t A[] = {
    0x0B,0xFF,0x20,0x48,0xFF,0xFF,0xFE,
    '1','5','8','1','F','A','6','Q','C','2','5','A','H','0','0','C','2','E','V','W',
    '0','7','3','8','7','4','1','3',
    0x00,0x01,0x01, 0xBB,0xE7,0x31,0x46, 0x3E,0x4A,0x56,0x17, 0xD0,0x07,
    0x2A,0xE1,0x31,0x46, 0x15,0x51,0x56,0x17, 0xFF,0xFF, 0x00,0x00, 0x50,0x46, 0x00,
    0xF8,0x07, 0x13,0x08, 0x02, 0x00, 0x0B,0x04,0x03, 0xA0,0x32,0xED,0xDF, 0x9F, 0x01,0x03,
  };
  static const uint8_t B[] = {
    0xA0,0xFF,0x20,0x48,0xFF,0xFF,0xFE,
    '1','5','8','1','F','A','6','Q','C','2','5','A','H','0','0','C','2','X','Y','Z',
    '0','7','3','8','7','4','9','9',
    0x00,0x01,0x01, 0xE5,0xE0,0x31,0x46, 0x93,0x4B,0x56,0x17, 0xD0,0x07,
    0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF, 0x00,0x00, 0x50,0x46, 0x80,
    0x00,0x00, 0x12,0x08, 0x03, 0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00, 0x00,0x00,
  };
  const uint8_t macA[6] = {0x8C,0x1E,0xD9,0x7E,0x39,0xFB};
  const uint8_t macB[6] = {0x8C,0x1E,0xD9,0x7E,0x39,0xAA};
  OdidResult r;
  if (ridDecode(A, sizeof(A), r)) mergeDrone(macA, r, -64, 6, true);
  r = OdidResult();
  if (ridDecode(B, sizeof(B), r)) mergeDrone(macB, r, -88, 6, true);
}
