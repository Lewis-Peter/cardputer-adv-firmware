#include "lora.h"
#include <RadioLib.h>
#include <SPI.h>
#include "icons.h"
#include "ui_common.h"
#include "sd_files.h"   // 蹲守日志写 SD（SD 和 LoRa 共用同一条 SPI，靠 NSS 区分）
#include "gnss.h"       // 日志里带上定位，才分得清哪一段是在哪儿测的
#include <ctime>
#include <cmath>

// ---- SX1262 引脚（M5 Cap LoRa-1262 官方 pinmap）----
// SCK40/MISO39/MOSI14 就是 sd_files.cpp 里 SD 卡用的那条总线，这里不用再声明/初始化。
static const int LORA_NSS = 5, LORA_BUSY = 6, LORA_DIO1 = 4, LORA_RST = 3;
static const uint8_t LORA_IOEXP_ADDR = 0x43;   // 与 GNSS 共用；P0=1 打开 RF 天线开关
// M5 的 SX1262 模组带 TCXO(1.6V)。若上电 init 返回 -706/-20（TCXO 相关）说明其实是无源晶振，
// 把这里改成 0.0f 再试。
static const float LORA_TCXO = 1.6f;

// LoRa 的 SCK/MISO/MOSI(40/39/14) 跟 sd_files.cpp 里 SD 卡用的完全一样——Cap 连接器和
// SD 卡槽本来就接在同一条物理 SPI 总线上，只是各自的片选不同（SD=GPIO12，LoRa=GPIO5）。
// ESP32-S3 只有两个可用的通用 SPI 硬件外设（FSPI/SPI2、HSPI/SPI3），FSPI 被 SD 占了，
// HSPI 被屏幕占了（M5GFX 里 Cardputer/CardputerADV 用 SPI3_HOST）——所以不能再开第三条
// 总线，必须复用 sd_files.cpp 里已经 begin() 好的全局 `SPI`，靠 NSS 引脚区分设备。
// 之前用独立 SPIClass(FSPI) 跟 SD 撞车，退出时 end() 会把 SD 的总线状态弄坏；
// 换成 SPIClass(HSPI) 又跟屏幕撞车，退出时 end() 直接把显示总线拆了，导致画面卡死。
static SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, SPI);

// ---- 预设：LISTEN 模式要收谁，参数就得跟谁完全一致 ----
// 频率来自 Meshtastic 各区默认 LongFast 槽位：EU868=869.525、US915=906.875。
// LongFast = BW250 / SF11 / CR4:5 / sync 0x2b / preamble 16。
struct LoraPreset { const char* name; float freq; float bw; uint8_t sf; uint8_t cr; uint8_t sync; uint16_t preamble; };
static const LoraPreset PRESETS[] = {
  {"EU868 Mesh", 869.525f, 250.0f, 11, 5, 0x2b, 16},
  {"US915 Mesh", 906.875f, 250.0f, 11, 5, 0x2b, 16},
  {"868 SF7",    868.0f,   125.0f, 7,  5, 0x12, 8},
  {"915 SF7",    915.0f,   125.0f, 7,  5, 0x12, 8},
  // CN470（国内 Meshtastic：470-510MHz LongFast）。默认槽位按信道名 hash 定，160 个信道里
  // 具体哪个不确定，这里给 3 个候选槽（hash 的三种可能值）挨个试。⚠️ 这块 Cap 天线/滤波是
  // 868-923 的，调到 470 严重失配、灵敏度很差，只能碰运气收很近的强信号（配 AUTO 更有戏）。
  {"CN470 a",    474.875f, 250.0f, 11, 5, 0x2b, 16},
  {"CN470 b",    486.875f, 250.0f, 11, 5, 0x2b, 16},
  {"CN470 c",    500.875f, 250.0f, 11, 5, 0x2b, 16},
};
static const int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);
static int presetIdx = 0;
// LISTEN 实际收听的频率：平时等于当前预设的频率，但可以在 SCAN 页用 Enter
// "锁定"到扫到的峰值频点上——SF/BW/CR/sync 还是用当前预设的，只微调频率，
// 用来应付同一种 LongFast 参数但信道槽位/频点跟预设默认值对不上的情况。
static float curFreq = 0;

// ---- Meshtastic 官方全部 modem preset（只有 BW/SF/CR 不同，sync/preamble 全网统一）----
// 数据来自 meshtastic.org/docs/overview/radio-settings。sync word 0x2b、preamble 16
// 是 Meshtastic 私有网络固定用的，跟选哪个 preset 无关。
struct ModemPreset { const char* name; float bw; uint8_t sf; uint8_t cr; };
static const ModemPreset MODEM_PRESETS[] = {
  {"ShortTurbo",   500.0f, 7,  5},
  {"ShortFast",    250.0f, 7,  5},
  {"ShortSlow",    250.0f, 8,  5},
  {"MediumFast",   250.0f, 9,  5},
  {"MediumSlow",   250.0f, 10, 5},
  {"LongFast",     250.0f, 11, 5},
  {"LongTurbo",    500.0f, 11, 8},
  {"LongModerate", 125.0f, 11, 8},
  {"LongSlow",     125.0f, 12, 8},
};
static const int MODEM_COUNT = sizeof(MODEM_PRESETS) / sizeof(MODEM_PRESETS[0]);
static const uint8_t MESH_SYNC = 0x2b;
static const uint16_t MESH_PREAMBLE = 16;

// LISTEN 实际用的调制参数：平时等于当前预设的 BW/SF/CR，AUTO 模式蹲到包之后
// 会把它改成蹲到的那组参数（频率仍是 curFreq，不受影响）。
struct ActiveModem { const char* name; float bw; uint8_t sf, cr, sync; uint16_t preamble; };
static ActiveModem activeModem;

// ---- AUTO：在 curFreq 上按 MODEM_PRESETS 逐个试，蹲到合法包就停在那组参数上 ----
static int autoIdx = 0;
static uint32_t autoDwellStartMs = 0;
static const uint32_t AUTO_DWELL_MS = 1500;   // 每组参数蹲这么久没等到包就换下一组
static bool autoFound = false;                // 上一轮 AUTO 是否蹲中了（供 UI 提示用）

// ---- 频段扫描（RSSI sweep）----
// 两个频段：ISM（LoRa 本行）和 VHF（探测 AIS 用，见下）。
//
// 为什么能扫到 162MHz：SX1262 芯片本身支持 150-960MHz，RadioLib 的范围检查就是
// CHECK_RANGE(freq, 150, 960)；镜像校准没有匹配的预设频段时会自动退回
// calibrateImageRejection(freq±4)，所以调过去是合法的。
// ⚠️ 但 Cap LoRa-1262 这块板子的天线/匹配网络是按 868-923MHz 设计的，调到 162MHz
// 失配极其严重（900M 的 1/4 波长天线 8cm，162M 要 46cm）。这个模式的定位是**测量**：
// 看看在天津港这种船台密集的地方，AIS 那两个频点的能量能不能穿过失配冒出底噪。
// 能看出来再谈解调，看不出来就别写解帧了。想提高命中率就在 SMA 上接一根 46cm 的线。
//
// AIS：161.975MHz(AIS1/ch87B) 和 162.025MHz(AIS2/ch88B)，9600bps GMSK，25kHz 信道。
static const float ISM_LO = 863.0f, ISM_HI = 928.0f;   // 覆盖 EU868 + US915 子频段
static const float VHF_LO = 161.5f, VHF_HI = 162.5f;   // 两个 AIS 信道 + 左右各留半兆当底噪参照
static const float AIS1 = 161.975f, AIS2 = 162.025f;
static bool vhfBand = false;
static inline float scanLo() { return vhfBand ? VHF_LO : ISM_LO; }
static inline float scanHi() { return vhfBand ? VHF_HI : ISM_HI; }
static const int NBINS = 44;
static float rssiBins[NBINS];      // 当前一轮各频点 RSSI（dBm）
static float peakBins[NBINS];      // 最大保持（看瞬时峰）
static int scanPos = 0;
static const float RSSI_FLOOR = -125.0f, RSSI_CEIL = -30.0f;

// ---- VHF AIS 自动蹲守（SCAN 页 + VHF 频段下按 A）----
// 扫频模式每个频点只驻留 4ms、一轮 44 格约 1 秒，AIS 那种 26.67ms 的突发基本靠撞运气
// （A1 上的观测占空比不到 0.5%）。蹲守模式干脆不扫了，只在四个频点之间轮流长驻，
// 占空比提到 7% 上下：
//   A1/A2 = 两个 AIS 信道      C1/C2 = 紧挨着的空频点，当对照组
//
// 判据：d = max(A1,A2) - max(C1,C2)
// 两边都是"两个频点各取 48 次采样的最大值，再二取一取大"，统计量完全同构——这一点是
// 整个判定成立的前提。之前那版栽在这上面：参照点走了 EMA（方差被抹平、收敛到均值），
// 信道却用每轮原始值（保留全部抖动）；再加上 max(A1,A2) 是二取一、ref 只有一取一，
// 白送 2~3dB 的系统偏置。两条叠起来，固定 6dB 门限在纯噪声里必然会响。
//
// 有了同构，纯噪声下 d 就关于 0 对称，于是**负尾巴天生就是对照组**，不用另外花槽位：
//   d > bias+band 记一次 up，d < bias-band 记一次 dn。
// 真有突发时 up 远多于 dn；两边差不多就说明在吃噪声。
//
// ⚠ 这里从头到尾**没有解调**。up 是"AIS 那两个频点这一轮比对照点亮了一下"的次数，
// 不是收到的 AIS 报文数——一条报文也没解出来过，当前代码也解不出来。
// 要真解 AIS 得走 SX1262 的 continuous receive 拿原始比特，再在软件里做
// NRZI 解码 + HDLC 去位填充 + CRC-16，RadioLib 的包引擎做不了这些。
//
// 偏置(bias)和抖动(spread)必须分开统计，这是第二个坑：
//   bias   = d 的慢平均      → AIS 信道相对空频点的**持续**抬升，本身就是主证据
//   spread = |d-bias| 慢平均 → 抖动尺度；band = max(6dB, 4×spread)
// 早先拿 EMA|d| 直接当噪声尺度，一旦信道上确实持续有能量（bias 长期 +10dB），
// 这个偏置就把门限自己顶到 20dB 以上，越有信号越不报警，正好反了。
//
// ⚠️ 对照频点不能写死，也不能挑"最安静的那格"。写死会踩中干扰（实测 162.30 比周围高
// 10dB）；而从十几格里取最小值是选择偏置，会把参照压低几 dB、人为制造正的 bias。
// 正确做法是开蹲前扫一遍这 1MHz，左右两侧各取**中位数**那格——既躲开离群干扰，又不偏心。
static const int   VHF_CAL_N   = 16;      // 挑对照点时每格的采样数（比正式蹲守少，图快）
static const uint32_t VHF_WARMUP = 60;    // 头这么多轮只学噪声、不报警（约 12 秒）
static const int   VHF_GUARD   = 3;       // 对照点离 A1/A2 至少隔这么多格，别蹭到信道边上
static const int   VHF_DWELL_N = 48;      // 每槽采样次数，约 15ms
static const float VHF_HIT_DB = 6.0f;     // 门限下限，再安静也不低于这个值
static bool     vhfWatch = false;
static bool     vhfCal = false;           // 开蹲头一秒：扫频挑对照点
static float    vhfCtrl[2] = { 161.70f, 162.30f };   // 只是初值，校准完会被覆盖
static uint8_t  vhfSlot = 0;              // 0=A1  1=A2  2=C1  3=C2
static float    vhfMax[4] = { RSSI_FLOOR, RSSI_FLOOR, RSSI_FLOOR, RSSI_FLOOR };
static float    vhfMargin = 0, vhfBestMargin = 0;
static float    vhfBias = 0, vhfSpread = 0, vhfThr = VHF_HIT_DB;  // d 慢平均 / 抖动 / 门限
static uint32_t vhfWatchStart = 0, vhfLastHit = 0;
static uint32_t vhfHits = 0, vhfNulls = 0, vhfRounds = 0;

// ---- 蹲守日志（SD 卡）----
// 为什么非记不可：屏幕上那几个数（up/dn/avg）全是**相对当前现场学出来的门限**算的，
// 换个地方门限就换了一套，两地的计数根本不同刻度，没法比。要回答"在家收得比海边多吗"
// 这类问题，只能比绝对电平——所以这里每 5 秒把四个频点的原始 dBm 连同定位一起落盘。
// 每按一次 A（或 R 重置）开一个新文件，蹲守结束/切频段/退出 app 时关掉。
static File     vhfLogFile;
static bool     vhfLogOk = false;
static char     vhfLogPath[48] = "";
static uint32_t vhfLogLast = 0;
static const uint32_t VHF_LOG_MS = 5000;

static void vhfLogClose() {
  if (vhfLogOk) { vhfLogFile.close(); vhfLogOk = false; }
  vhfLogPath[0] = '\0';
}

static void vhfLogOpen() {
  vhfLogClose();
  if (!sdMounted) { Serial.println("[vhf] no SD, not logging"); return; }
  if (!SD.exists("/ais")) SD.mkdir("/ais");
  // 有对时就用时间当文件名（现场好认），没有就退回开机秒数，至少不会互相覆盖
  struct tm ti;
  if (timeSynced && getLocalTime(&ti, 0)) {
    snprintf(vhfLogPath, sizeof(vhfLogPath), "/ais/%04d%02d%02d_%02d%02d%02d.csv",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec);
  } else {
    snprintf(vhfLogPath, sizeof(vhfLogPath), "/ais/run%lu.csv", (unsigned long)(millis() / 1000));
  }
  vhfLogFile = SD.open(vhfLogPath, FILE_WRITE);
  vhfLogOk = (bool)vhfLogFile;
  if (!vhfLogOk) { Serial.printf("[vhf] log open failed: %s\n", vhfLogPath); vhfLogPath[0] = '\0'; return; }
  // a1/a2 = 两个 AIS 信道的原始 dBm；c1/c2 = 对照频点。ev: -=定期采样 UP/DN=越界事件
  vhfLogFile.println("# cardputer_adv AIS energy log (RSSI only, no demodulation)");
  vhfLogFile.println("t_s,round,a1,a2,c1,c2,d,bias,spread,thr,ev,lat,lon,alt");
  vhfLogFile.flush();
  Serial.printf("[vhf] logging to %s\n", vhfLogPath);
}

static void vhfLogRow(const char* ev, float d) {
  if (!vhfLogOk) return;
  // flush 每行都做：这东西是拿去野外跑的，直接拔电/没电关机是常态，攒在缓冲里等于没记。
  // 代价是偶尔几十毫秒的写延迟，会让那一轮的驻留稍微错开——对 5 秒一行的采样无所谓。
  vhfLogFile.printf("%lu,%lu,%.0f,%.0f,%.0f,%.0f,%+.1f,%+.1f,%.1f,%.1f,%s,",
                    (unsigned long)((millis() - vhfWatchStart) / 1000),
                    (unsigned long)vhfRounds,
                    vhfMax[0], vhfMax[1], vhfMax[2], vhfMax[3],
                    d, vhfBias, vhfSpread, vhfThr, ev);
  if (gnssHasFix()) vhfLogFile.printf("%.6f,%.6f,%.0f\n", gnssLat(), gnssLng(), gnssAlt());
  else              vhfLogFile.println(",,");
  vhfLogFile.flush();
  vhfLogLast = millis();
}

static void vhfWatchReset() {
  // 清掉扫频留下的柱子：蹲守只碰 4 个频点，其余 40 格再挂着旧数据会看着像还在扫
  for (int i = 0; i < NBINS; i++) { rssiBins[i] = RSSI_FLOOR; peakBins[i] = RSSI_FLOOR; }
  vhfCal = true; scanPos = 0;             // 先扫一遍挑对照点，扫完才开始正式判定
  vhfSlot = 0;
  for (int i = 0; i < 4; i++) vhfMax[i] = RSSI_FLOOR;
  vhfMargin = vhfBestMargin = 0;
  vhfBias = 0; vhfSpread = 0; vhfThr = VHF_HIT_DB;
  vhfHits = vhfNulls = vhfRounds = 0;
  vhfLastHit = 0;
  vhfWatchStart = millis();
  vhfLogLast = 0;
  vhfLogOpen();
}

// ---- 抓包日志 ----
// Meshtastic 帧头（16字节，未加密，即使 payload 是密文也能读出来）：
// to(4) from(4) id(4) flags(1) channel(1) next_hop(1) relay_node(1)，全部小端。
// flags: bit0-2=hop_limit  bit3=want_ack  bit4=via_mqtt  bit5-7=hop_start（据 Meshtastic RadioInterface.h）
struct PktRec {
  uint16_t len; int16_t rssi; int8_t snr; bool crcBad; char hex[18];
  bool hasHeader; uint32_t to, from, id;
  uint8_t hopLimit, hopStart, channelHash; bool wantAck, viaMqtt;
};
static const int LOG_N = 6;
static PktRec pktLog[LOG_N];
static int pktHead = 0, pktCount = 0;
static uint32_t pktTotal = 0;
static uint32_t lastPktMs = 0;

enum LoraMode { LORA_SCAN, LORA_LISTEN, LORA_AUTO, LORA_CHAT };
static LoraMode mode = LORA_SCAN;
static bool loraReady = false;
static int loraErr = 0;

// ---- CHAT：自家设备专用的简单收发协议，不是 Meshtastic 兼容(不加密/不走protobuf)。
// 复用当前预设的频率/BW/SF/CR/sync(通常还是 LongFast 那组参数)，纯粹省事——不打算跟
// 真实 Meshtastic 网络互通，用固定的 magic 前缀区分"是不是我们自己发的"，收到不匹配的
// (真实Meshtastic流量/噪声/别的LoRa设备)直接丢，不当聊天消息处理。
// ⚠️ 没做占空比限制，频繁发送请留意当地无线电频谱法规里对这个频段的占空比要求。
static const uint16_t CHAT_MAGIC = 0xC5A1;
static const uint32_t CHAT_MIN_INTERVAL_MS = 3000;   // 软性限速：太频繁发送会被晾一下
struct ChatMsg { bool mine; uint8_t nodeId; String text; uint32_t ms; };
static const int CHAT_LOG_N = 6;
static ChatMsg chatLog[CHAT_LOG_N];
static int chatHead = 0, chatCount = 0;
static String composeBuf;
static uint32_t lastSendMs = 0;
static uint8_t chatNodeId = 0;
static uint8_t chatSeq = 0;
static const char* CHAT_LOG_PATH = "/lorachat.log";

static void loraChatEnsureNodeId() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;
  uint32_t saved = loadUInt("lora", "nodeid", 0xFFFFFFFFu);
  if (saved > 0xFF) {                        // 没存过：随机生成一个、跨重启保持稳定
    saved = (uint32_t)random(0, 256);
    saveUInt("lora", "nodeid", saved);
  }
  chatNodeId = (uint8_t)saved;
}

static void chatLogPush(bool mine, uint8_t nodeId, const String& text) {
  ChatMsg& m = chatLog[chatHead];
  m.mine = mine; m.nodeId = nodeId; m.text = text; m.ms = millis();
  chatHead = (chatHead + 1) % CHAT_LOG_N;
  if (chatCount < CHAT_LOG_N) chatCount++;
  sdAppend(CHAT_LOG_PATH, nowStamp() + "  " + (mine ? "me" : (String("!") + String(nodeId, HEX))) + "  " + text);
}

// ---- PC 主导模式：LoRa 嗅探流状态 ----
static bool sniffActive = false;
static LoraSniffConfig sniffCfg = {0};
static uint32_t sniffPktCount = 0;

static volatile bool rxFlag = false;
static void IRAM_ATTR onDio1() { rxFlag = true; }

// 打开 Cap 的 RF 天线开关（IO 扩展 P0=输出、驱动使能、拉高）——复用 GNSS 那套 I2C 序列
static void loraRfSwitchOn() {
  M5.In_I2C.bitOn(LORA_IOEXP_ADDR, 0x03, 0x01, 100000);   // P0 方向=输出
  M5.In_I2C.bitOff(LORA_IOEXP_ADDR, 0x07, 0x01, 100000);  // 清 Hi-Z=驱动使能
  M5.In_I2C.bitOn(LORA_IOEXP_ADDR, 0x05, 0x01, 100000);   // 输出=1
}

// 把电台配置成 activeModem 并开始接收（频率用 curFreq）
static void loraStartListen() {
  radio.standby();
  radio.setFrequency(curFreq);
  radio.setBandwidth(activeModem.bw);
  radio.setSpreadingFactor(activeModem.sf);
  radio.setCodingRate(activeModem.cr);
  radio.setSyncWord(activeModem.sync);
  radio.setPreambleLength(activeModem.preamble);
  rxFlag = false;
  radio.startReceive();
}

// AUTO 模式：把电台配置成 MODEM_PRESETS[autoIdx]，频率仍是 curFreq 不变
static void loraStartListenAuto() {
  const ModemPreset& m = MODEM_PRESETS[autoIdx];
  radio.standby();
  radio.setFrequency(curFreq);
  radio.setBandwidth(m.bw);
  radio.setSpreadingFactor(m.sf);
  radio.setCodingRate(m.cr);
  radio.setSyncWord(MESH_SYNC);
  radio.setPreambleLength(MESH_PREAMBLE);
  rxFlag = false;
  radio.startReceive();
  autoDwellStartMs = millis();
}

// 发一条聊天消息：先standby再transmit（阻塞到发完，SF11这种慢参数几百ms很正常，
// 跟别处WiFi扫描的阻塞是一个道理），发完立刻切回接收，不然就聋了收不到回复/回信。
// 返回是否真的发出去了——限速窗口里没发成的，调用方要把输入框内容留着（原来是不管发没发成
// 都直接清空 composeBuf，用户打了一半的话在"wait Ns to send"期间按回车就凭空消失了）。
static bool loraSendText(const String& text) {
  if (!loraReady) return false;
  uint32_t now = millis();
  if (now - lastSendMs < CHAT_MIN_INTERVAL_MS) return false;   // 软性限速，还没到点就先不发
  lastSendMs = now;

  uint8_t buf[5 + 64];
  size_t n = text.length(); if (n > 64) n = 64;
  buf[0] = (uint8_t)(CHAT_MAGIC >> 8);
  buf[1] = (uint8_t)CHAT_MAGIC;
  buf[2] = chatNodeId;
  buf[3] = chatSeq++;
  buf[4] = (uint8_t)n;
  memcpy(buf + 5, text.c_str(), n);

  radio.standby();
  radio.transmit(buf, 5 + n);
  radio.startReceive();
  chatLogPush(true, chatNodeId, text);
  return true;
}

// 找 SCAN 扫到的峰值频点（峰值保持，跟频谱图里高亮的那根柱子是同一个）——
// 用来把 LISTEN 锁到实际热闹的频率上
static float loraScanPeakFreq() {
  int pk = 0;
  for (int i = 1; i < NBINS; i++) if (peakBins[i] > peakBins[pk]) pk = i;
  return scanLo() + (scanHi() - scanLo()) * pk / (NBINS - 1);
}

void loraEnter() {
  // 进入 LoRa 前台应用前如果嗅探开着，先停掉嗅探并向串口发出 end 事件（避免电台与 SPI 冲突）
  if (sniffActive) {
    loraSniffStop();
  }
  vhfBand = false;   // 每次进来都回到正常频段，VHF 只是个临时探测模式
  loraChatEnsureNodeId();

  // 快探测：若没有插 Cap 模组，I2C 0x43 (IO 扩展芯片) 不响应。
  // 若直接调 radio.begin()，RadioLib 会在 SX126x::findChip() 里做 10 次超时重试（共耗时 ~10.5 秒），
  // 导致整个系统主循环阻塞冻结。通过先探测 0x43，未插模组时可在 1ms 内快速判定并返回。
  if (!M5.In_I2C.scanID(LORA_IOEXP_ADDR)) {
    loraReady = false;
    loraErr = RADIOLIB_ERR_CHIP_NOT_FOUND;
    for (int i = 0; i < NBINS; i++) { rssiBins[i] = RSSI_FLOOR; peakBins[i] = RSSI_FLOOR; }
    scanPos = 0;
    pktHead = pktCount = 0; pktTotal = 0;
    chatHead = chatCount = 0; composeBuf = "";
    autoFound = false;
    vhfWatch = false; vhfLogClose();
    mode = LORA_SCAN;
    return;
  }

  loraRfSwitchOn();
  const LoraPreset& p = PRESETS[presetIdx];
  curFreq = p.freq;
  activeModem = { p.name, p.bw, p.sf, p.cr, p.sync, p.preamble };
  loraErr = radio.begin(p.freq, p.bw, p.sf, p.cr, p.sync, 10, p.preamble, LORA_TCXO);
  loraReady = (loraErr == RADIOLIB_ERR_NONE);
  if (loraReady) radio.setDio1Action(onDio1);

  for (int i = 0; i < NBINS; i++) { rssiBins[i] = RSSI_FLOOR; peakBins[i] = RSSI_FLOOR; }
  scanPos = 0;
  pktHead = pktCount = 0; pktTotal = 0;
  chatHead = chatCount = 0; composeBuf = "";
  autoFound = false;
  vhfWatch = false; vhfLogClose();
  mode = LORA_SCAN;
}

// 切换扫描频段：VHF 用 FSK + 窄接收带宽，ISM 回到当前 LoRa 预设。
// rxBw 取 23.4kHz —— 正好一个 AIS 信道的宽度；比 LoRa 那 250kHz 带宽底噪低十几 dB，
// 在失配这么严重的前提下，这十几 dB 是有没有机会看到东西的关键。
static void loraApplyScanBand() {
  if (!loraReady) return;
  radio.standby();
  if (vhfBand) {
    // br=9.6kbps / freqDev=2.4kHz 就是 AIS 的调制参数；这里只读 RSSI，参数主要影响接收带宽
    loraErr = radio.beginFSK(AIS1, 9.6f, 2.4f, 23.4f, 10, 16, LORA_TCXO);
  } else {
    const LoraPreset& p = PRESETS[presetIdx];
    loraErr = radio.begin(p.freq, p.bw, p.sf, p.cr, p.sync, 10, p.preamble, LORA_TCXO);
  }
  loraReady = (loraErr == RADIOLIB_ERR_NONE);
  for (int i = 0; i < NBINS; i++) { rssiBins[i] = RSSI_FLOOR; peakBins[i] = RSSI_FLOOR; }
  scanPos = 0;
}

void loraExit() {
  vhfWatch = false; vhfLogClose();   // 退 app 时必须把日志文件关掉，否则最后一段丢缓冲
  if (loraReady) { radio.clearDio1Action(); radio.sleep(); }
  // 不能碰全局 SPI 总线（SD 卡还在用），退出时只要把 NSS 拉高、取消片选就行
  pinMode(LORA_NSS, OUTPUT); digitalWrite(LORA_NSS, HIGH);
}

// ---- PC 主导模式：LoRa 嗅探控制与数据流实现 ----

bool loraSniffIsActive() {
  return sniffActive;
}

float loraSniffGetMhz() {
  return sniffCfg.mhz;
}

uint32_t loraSniffGetPktCount() {
  return sniffPktCount;
}

void loraSniffStop() {
  if (sniffActive) {
    sniffActive = false;
    char endBuf[32];
    loraSniffFormatEnd(endBuf, sizeof(endBuf));
    Serial.println(endBuf);
    radio.clearDio1Action();
    radio.sleep();
    pinMode(LORA_NSS, OUTPUT);
    digitalWrite(LORA_NSS, HIGH);
  } else {
    // 嗅探未开启时调用 LORA OFF，仍回显 end 保证幂等与 PC 状态确认
    char endBuf[32];
    loraSniffFormatEnd(endBuf, sizeof(endBuf));
    Serial.println(endBuf);
  }
}

bool loraSniffStart(const LoraSniffConfig& cfg) {
  // 1. 若当前正处于 LoRa 或 LoRa Chat 前台应用中，避免电台外设冲突，报错 busy
  if (screen == SCREEN_LORA) {
    char errBuf[64];
    loraSniffFormatErr(errBuf, sizeof(errBuf), "busy: LoRa app open");
    Serial.println(errBuf);
    return false;
  }

  // 2. 已经在嗅探时再发一次 LORA SNIFF ... = 换参数重开（先发 end 再发新的 start）
  if (sniffActive) {
    loraSniffStop();
  }

  // 3. 快探测：若没有插 Cap 模组，I2C 0x43 不响应，在 1ms 内快速判定并返回
  // 必须保证不会把 SD 的 SPI 状态弄坏（NSS 拉高、取消片选）
  if (!M5.In_I2C.scanID(LORA_IOEXP_ADDR)) {
    char errBuf[64];
    loraSniffFormatErr(errBuf, sizeof(errBuf), "no radio");
    Serial.println(errBuf);
    pinMode(LORA_NSS, OUTPUT);
    digitalWrite(LORA_NSS, HIGH);
    return false;
  }

  // 4. 打开 RF 天线开关并初始化 SX1262 射频芯片
  loraRfSwitchOn();
  radio.standby();
  int st = radio.begin(cfg.mhz, cfg.bw, cfg.sf, cfg.cr, cfg.sync, 10, cfg.preamble, LORA_TCXO);
  if (st != RADIOLIB_ERR_NONE) {
    // 只有真找不到芯片才说 "no radio"；其它错误（参数被 RadioLib 拒、TCXO 等）照实报码，
    // 否则 PC 端会把"配置不对"当成"模块没插"去排查。
    char errBuf[64], msg[40];
    if (st == RADIOLIB_ERR_CHIP_NOT_FOUND) snprintf(msg, sizeof(msg), "no radio");
    else snprintf(msg, sizeof(msg), "radio init failed: %d", st);
    loraSniffFormatErr(errBuf, sizeof(errBuf), msg);
    Serial.println(errBuf);
    radio.clearDio1Action();
    radio.sleep();
    pinMode(LORA_NSS, OUTPUT);
    digitalWrite(LORA_NSS, HIGH);
    return false;
  }

  // 5. 挂载 DIO1 中断并启动接收
  radio.setDio1Action(onDio1);
  rxFlag = false;
  radio.startReceive();

  // 6. 更新嗅探状态并输出 start JSON
  sniffActive = true;
  sniffCfg = cfg;
  sniffPktCount = 0;

  char startBuf[128];
  loraSniffFormatStart(startBuf, sizeof(startBuf), cfg);
  Serial.println(startBuf);
  return true;
}

static void loraSniffOutputPacket(uint32_t ts, size_t len, float rssi, float snr, long fe, bool crcOk, const uint8_t* payload, size_t payloadLen) {
  // 1. 栈上定长缓冲格式化数据包报头（零堆分配）
  char hdr[128];
  loraSniffFormatPacketHeader(hdr, sizeof(hdr), ts, len, rssi, snr, fe, crcOk);
  Serial.print(hdr);

  // 2. 静态小缓冲区按分段输出十六进制 hex（避免整行 510 字节在栈上爆栈或堆 malloc）
  static const char hexChars[] = "0123456789abcdef";
  char chunk[65];
  for (size_t i = 0; i < payloadLen; ) {
    size_t n = payloadLen - i;
    if (n > 32) n = 32;
    for (size_t j = 0; j < n; j++) {
      uint8_t b = payload[i + j];
      chunk[j * 2]     = hexChars[(b >> 4) & 0x0F];
      chunk[j * 2 + 1] = hexChars[b & 0x0F];
    }
    chunk[n * 2] = '\0';
    Serial.print(chunk);
    i += n;
  }

  // 3. 闭合 JSON 字符串并换行
  Serial.println("\"}");
}

void loraSniffTick() {
  if (!sniffActive) return;
  if (rxFlag) {
    rxFlag = false;
    // 只有头错误（HEADER_ERR 且没 HEADER_VALID）时芯片根本没收下载荷：RadioLib 的 readData 照样
    // 返回 CRC_MISMATCH，但 getPacketLength / 缓冲区里还是**上一个包**的内容——直接上报就成了一个
    // 跟上一包一模一样、crc=false 的"坏包"，PC 端会把它当成重传/碰撞。这种中断清掉重新收就行。
    // 真正的 CRC 错（CRC_ERR）载荷是收下来了的，照规格照常上报。
    uint32_t irq = radio.getIrqFlags();
    if ((irq & RADIOLIB_SX126X_IRQ_HEADER_ERR) && !(irq & RADIOLIB_SX126X_IRQ_HEADER_VALID) &&
        !(irq & RADIOLIB_SX126X_IRQ_CRC_ERR)) {
      radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
      radio.startReceive();
      return;
    }
    size_t len = radio.getPacketLength();
    static uint8_t sniffPktBuf[256];
    size_t n = len < sizeof(sniffPktBuf) ? len : sizeof(sniffPktBuf);
    int st = radio.readData(sniffPktBuf, n);
    bool crcBad = (st == RADIOLIB_ERR_CRC_MISMATCH);
    if (st == RADIOLIB_ERR_NONE || crcBad) {
      uint32_t now = millis();
      float rssi = radio.getRSSI();
      float snr = radio.getSNR();
      long fe = (long)lroundf(radio.getFrequencyError());
      bool crcOk = (st == RADIOLIB_ERR_NONE);
      sniffPktCount++;
      loraSniffOutputPacket(now, len, rssi, snr, fe, crcOk, sniffPktBuf, n);
    }
    radio.startReceive();
  }
}


static uint32_t rdLE32(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void loraPushPkt(uint16_t len, int16_t rssi, int8_t snr, bool crcBad, const uint8_t* buf, size_t n) {
  PktRec& r = pktLog[pktHead];
  r.len = len; r.rssi = rssi; r.snr = snr; r.crcBad = crcBad;
  int w = 0; size_t show = n < 5 ? n : 5;
  for (size_t i = 0; i < show && w < (int)sizeof(r.hex) - 3; i++)
    w += snprintf(r.hex + w, sizeof(r.hex) - w, "%02x", buf[i]);
  r.hex[w] = 0;

  r.hasHeader = (!crcBad && n >= 16);
  if (r.hasHeader) {
    r.to = rdLE32(buf + 0);
    r.from = rdLE32(buf + 4);
    r.id = rdLE32(buf + 8);
    uint8_t flags = buf[12];
    r.hopLimit = flags & 0x07;
    r.wantAck = (flags & 0x08) != 0;
    r.hopStart = (flags >> 5) & 0x07;
    r.viaMqtt = (flags & 0x10) != 0;
    r.channelHash = buf[13];
  }

  // 顺便把整包吐到 USB 串口，给 Mac 端脚本看（前缀 LORAPKT 便于过滤，不跟其它日志混）。
  // 屏幕只留 5 字节 hex，这里给全量 payload + 解析出的 Meshtastic 帧头。
  Serial.printf("LORAPKT {\"ms\":%lu,\"freq\":%.3f,\"bw\":%.0f,\"sf\":%u,\"cr\":%u,"
                "\"rssi\":%d,\"snr\":%d,\"crc\":%d,\"len\":%u",
                (unsigned long)millis(), curFreq, activeModem.bw, activeModem.sf, activeModem.cr,
                (int)rssi, (int)snr, crcBad ? 1 : 0, (unsigned)len);
  if (r.hasHeader) {
    Serial.printf(",\"to\":\"%08x\",\"from\":\"%08x\",\"id\":\"%08x\","
                  "\"hop\":%u,\"hopStart\":%u,\"ch\":%u,\"ack\":%d,\"mqtt\":%d",
                  r.to, r.from, r.id, r.hopLimit, r.hopStart, r.channelHash,
                  r.wantAck ? 1 : 0, r.viaMqtt ? 1 : 0);
  }
  Serial.print(",\"hex\":\"");
  for (size_t i = 0; i < n; i++) Serial.printf("%02x", buf[i]);
  Serial.println("\"}");

  pktHead = (pktHead + 1) % LOG_N;
  if (pktCount < LOG_N) pktCount++;
  pktTotal++; lastPktMs = millis();
}

// ---- VHF 非蹲守（纯扫频）读数：AIS 信道相对底噪的余量 ----
// ⚠️ 两边必须是同一个统计量，这跟蹲守模式栽过的是同一个坑。旧版拿"信道相邻 3 格的
// 最大值"去减"全带 44 格的中位数"：max-of-3 对 median 天然偏高好几 dB，报出来的余量
// 是虚的，而且底噪里还混进了 AIS 信道自己。
// 现在底噪也走 max-of-3：对每个候选格算它相邻 3 格的最大值，再取这些值的中位数。
// 候选格排除 AIS 两信道及保护带（别把信号算进底噪），也排除首末格（那两格只有 2 个邻居，
// 凑不齐 max-of-3，混进来又是一处不对称）。
static int vhfBinOf(float f) {
  return (int)lroundf((f - scanLo()) / (scanHi() - scanLo()) * (NBINS - 1));
}

static float vhfPeak3(int i) {
  float best = RSSI_FLOOR;
  for (int d = -1; d <= 1; d++) {
    int j = i + d;
    if (j >= 0 && j < NBINS && peakBins[j] > best) best = peakBins[j];
  }
  return best;
}

static void vhfProbeStats(float& a1, float& a2, float& floorDb) {
  const int b1 = vhfBinOf(AIS1), b2 = vhfBinOf(AIS2);
  a1 = vhfPeak3(b1);
  a2 = vhfPeak3(b2);

  float m[NBINS];
  int n = 0;
  for (int i = 1; i < NBINS - 1; i++) {
    if (i >= b1 - VHF_GUARD && i <= b2 + VHF_GUARD) continue;
    m[n++] = vhfPeak3(i);
  }
  if (n == 0) { floorDb = RSSI_FLOOR; return; }
  for (int a = 1; a < n; a++) {                    // 插入排序取中位数
    float v = m[a]; int j = a;
    while (j > 0 && m[j - 1] > v) { m[j] = m[j - 1]; j--; }
    m[j] = v;
  }
  floorDb = m[n / 2];
}

// 在 f 上驻留采样 n 次，返回最大值——AIS 是突发，要的就是这段时间里的峰。
// 三个槽都走这一个函数，统计量才统一（见 VHF_DWELL_N 那段注释）。
static float vhfDwell(float f, int n) {
  radio.setFrequency(f);
  radio.startReceive();
  delayMicroseconds(2500);        // 等 PLL 锁定 + AGC 稳定
  radio.getRSSI(false);           // 丢弃第一次读数（整定残留，偏高）
  float mx = RSSI_FLOOR;
  for (int i = 0; i < n; i++) {
    float r = radio.getRSSI(false);
    if (r > mx) mx = r;
    delayMicroseconds(300);
  }
  return mx;
}

void loraUpdate() {
  if (!loraReady) return;

  if (mode == LORA_SCAN && vhfBand && vhfWatch) {
    if (vhfCal) {
      // 校准：把整个 1MHz 扫一遍，用和正式蹲守同一个 vhfDwell()，只是采样少一点。
      for (int n = 0; n < 2 && scanPos < NBINS; n++) {
        float f = scanLo() + (scanHi() - scanLo()) * scanPos / (NBINS - 1);
        float v = vhfDwell(f, VHF_CAL_N);
        rssiBins[scanPos] = peakBins[scanPos] = v;
        scanPos++;
      }
      if (scanPos >= NBINS) {
        // A1/A2 各自的格子，左右两侧分别挑一个最安静的当对照
        auto binOf = [](float f) {
          return (int)lroundf((f - scanLo()) / (scanHi() - scanLo()) * (NBINS - 1));
        };
        int b1 = binOf(AIS1), b2 = binOf(AIS2);
        // ⚠️ 取"最安静的格子"是错的：从十几格里挑最小值本身就是选择偏置，会把参照压低
        // 几 dB，人为制造正的 d。取中位数那格——既躲开 162.30 那种干扰离群值，又不偏心。
        auto medianBin = [](int from, int to) {          // 闭区间，返回中位数所在的格号
          int idx[NBINS], n = 0;
          for (int i = from; i <= to && i < NBINS; i++) if (i >= 0) idx[n++] = i;
          if (n == 0) return -1;
          for (int a = 1; a < n; a++) {                  // 按 RSSI 插入排序
            int v = idx[a], j = a;
            while (j > 0 && rssiBins[idx[j - 1]] > rssiBins[v]) { idx[j] = idx[j - 1]; j--; }
            idx[j] = v;
          }
          return idx[n / 2];
        };
        int lo = medianBin(0, b1 - VHF_GUARD);
        int hi = medianBin(b2 + VHF_GUARD, NBINS - 1);
        if (lo >= 0) vhfCtrl[0] = scanLo() + (scanHi() - scanLo()) * lo / (NBINS - 1);
        if (hi >= 0) vhfCtrl[1] = scanLo() + (scanHi() - scanLo()) * hi / (NBINS - 1);
        Serial.printf("[vhf] ctrl picked: %.3f (%.0f dBm) / %.3f (%.0f dBm)\n",
                      vhfCtrl[0], lo >= 0 ? rssiBins[lo] : 0.0f,
                      vhfCtrl[1], hi >= 0 ? rssiBins[hi] : 0.0f);
        // 对照点是每次开蹲现挑的，不记下来的话事后没法解释两次跑的差异
        // （尤其在海边：161.5-161.925 整段都是海事岸台下行，"空频点"很可能并不空）
        if (vhfLogOk) {
          vhfLogFile.printf("# ctrl %.3f %.0fdBm / %.3f %.0fdBm\n",
                            vhfCtrl[0], lo >= 0 ? rssiBins[lo] : 0.0f,
                            vhfCtrl[1], hi >= 0 ? rssiBins[hi] : 0.0f);
          vhfLogFile.flush();
        }
        // 扫描留下的柱子清掉，正式蹲守只碰 4 个频点
        for (int i = 0; i < NBINS; i++) rssiBins[i] = peakBins[i] = RSSI_FLOOR;
        vhfCal = false;
        vhfWatchStart = millis();          // 计时从正式蹲守算起，不含这一秒校准
      }
      dirty = true;
      return;
    }

    // 蹲守：一帧驻留一个槽，A1 → A2 → C1 → C2 轮着来，走完一轮才判一次
    const float SLOT_F[4] = { AIS1, AIS2, vhfCtrl[0], vhfCtrl[1] };
    float f = SLOT_F[vhfSlot];
    float v = vhfDwell(f, VHF_DWELL_N);
    vhfMax[vhfSlot] = v;

    // 顺手喂进频谱图对应的格子，蹲守时图上那四根柱子照样是活的
    int bi = (int)lroundf((f - scanLo()) / (scanHi() - scanLo()) * (NBINS - 1));
    if (bi >= 0 && bi < NBINS) {
      rssiBins[bi] = v;
      if (v > peakBins[bi]) peakBins[bi] = v;
    }

    if (vhfSlot == 3) {                       // 四个槽都采完 = 一轮结束，可以判了
      float ch = max(vhfMax[0], vhfMax[1]);   // AIS 两信道
      float ct = max(vhfMax[2], vhfMax[3]);   // 对照两频点：同样是"二取一取大"
      float d  = ch - ct;
      vhfMargin = d;
      vhfRounds++;
      if (d > vhfBestMargin) vhfBestMargin = d;

      // 偏置和抖动必须分开统计。之前拿 EMA|d| 当噪声尺度，一旦 d 长期不在 0 附近
      // （信道上确实持续有能量），持续偏置就把门限自己顶上去，越有信号越不报——反了。
      //   vhfBias = d 的慢平均      → 持续抬升，这本身就是"有没有信号"的主证据
      //   vhfSpread = |d-bias| 慢平均 → 抖动尺度，突发要压过它才算一次命中
      // ⚠️ 头 VHF_WARMUP 轮只学不报。慢平均收敛要几十轮，这期间门限还没成形，
      // 判定出来的命中纯属噪声——实测两次 HIT #1 都落在开蹲头 20 秒内就是这么来的。
      // 预热期用快一点的 α 让它先收敛，之后再切回慢的（慢 = 低占空比突发拉不动）。
      const bool warm = (vhfRounds <= VHF_WARMUP);
      const float aB = warm ? 0.10f : 0.02f;
      const float aS = warm ? 0.15f : 0.05f;
      vhfBias   = (vhfRounds == 1) ? d : vhfBias * (1.0f - aB) + d * aB;
      float dev = fabsf(d - vhfBias);
      vhfSpread = (vhfRounds == 1) ? 0.0f : vhfSpread * (1.0f - aS) + dev * aS;
      float band = max(VHF_HIT_DB, 4.0f * vhfSpread);
      vhfThr = vhfBias + band;

      // 定期采样：不管预热与否都记，预热那一段的绝对电平同样是有效数据
      // （门限没成形不影响 a1/a2/c1/c2 本身）。事件行另记，两者不重复。
      bool logged = false;
      if (vhfLogOk && millis() - vhfLogLast >= VHF_LOG_MS) { vhfLogRow("-", d); logged = true; }

      if (warm) {
        // 预热期不计数，也不响
      } else if (d > vhfThr) {
        vhfHits++;
        vhfLastHit = millis();
        M5.Speaker.setVolume(volVal());
        M5.Speaker.tone(2600, 90);            // 撞到了就"嘀"一声，不用一直盯着屏幕
        Serial.printf("[vhf] UP #%lu  A1=%.0f A2=%.0f C1=%.0f C2=%.0f  d=%+.1f thr=%.1f  t=%lus\n",
                      (unsigned long)vhfHits, vhfMax[0], vhfMax[1], vhfMax[2], vhfMax[3],
                      d, vhfThr, (unsigned long)((millis() - vhfWatchStart) / 1000));
        if (!logged) vhfLogRow("UP", d);
      } else if (d < vhfBias - band) {
        // 对照组命中：能量跑到空频点那边去了，只可能是噪声起伏。不响，只记数。
        vhfNulls++;
        Serial.printf("[vhf] DN #%lu  A1=%.0f A2=%.0f C1=%.0f C2=%.0f  d=%+.1f thr=%.1f  t=%lus\n",
                      (unsigned long)vhfNulls, vhfMax[0], vhfMax[1], vhfMax[2], vhfMax[3],
                      d, vhfThr, (unsigned long)((millis() - vhfWatchStart) / 1000));
        if (!logged) vhfLogRow("DN", d);
      }
      dirty = true;
    }
    vhfSlot = (vhfSlot + 1) % 4;
    return;                                   // 蹲守时不跑扫频，两者抢的是同一个 PLL
  }

  if (mode == LORA_SCAN && vhfBand) {
    // VHF 探测：每 2 秒把 AIS 两个信道相对底噪的余量打一行，方便记录/对比
    // （屏幕那行会被 debug 条盖住，而且这种测量本来就该有可复制的日志）
    static uint32_t lastVhfLog = 0;
    if (millis() - lastVhfLog > 2000) {
      lastVhfLog = millis();
      float a1, a2, fl;
      vhfProbeStats(a1, a2, fl);
      Serial.printf("[vhf] A1=%.0f A2=%.0f floor=%.0f  margin=%.0f dB\n",
                    a1, a2, fl, max(a1, a2) - fl);
    }
  }

  if (mode == LORA_SCAN) {
    for (int n = 0; n < 2; n++) {                       // 每帧少扫几个，换取更充分的整定
      float f = scanLo() + (scanHi() - scanLo()) * scanPos / (NBINS - 1);
      radio.setFrequency(f);
      radio.startReceive();
      delayMicroseconds(2500);                          // 等 PLL 锁定 + AGC 稳定（1.5ms 太短会虚高）
      radio.getRSSI(false);                             // 丢弃第一次读数（整定残留，偏高）
      float s[5];                                        // 采样 5 次取中值，滤掉整定瞬态/偶发突发
      for (int j = 0; j < 5; j++) { s[j] = radio.getRSSI(false); delayMicroseconds(300); }
      for (int a = 0; a < 5; a++)
        for (int b2 = a + 1; b2 < 5; b2++)
          if (s[b2] < s[a]) { float t = s[a]; s[a] = s[b2]; s[b2] = t; }
      float med = s[2];
      rssiBins[scanPos] = med;
      if (med > peakBins[scanPos]) peakBins[scanPos] = med;
      else peakBins[scanPos] = max(RSSI_FLOOR, peakBins[scanPos] - 0.5f);  // 慢衰减，陈旧假峰会褪去，不低于底噪线
      scanPos = (scanPos + 1) % NBINS;
    }
  } else if (mode == LORA_LISTEN) {
    if (rxFlag) {
      rxFlag = false;
      size_t len = radio.getPacketLength();
      uint8_t buf[64];
      size_t n = len < sizeof(buf) ? len : sizeof(buf);
      int st = radio.readData(buf, n);
      bool crcBad = (st == RADIOLIB_ERR_CRC_MISMATCH);
      if (st == RADIOLIB_ERR_NONE || crcBad)
        loraPushPkt(len, (int16_t)radio.getRSSI(), (int8_t)radio.getSNR(), crcBad, buf, n);
      radio.startReceive();
    }
  } else if (mode == LORA_CHAT) {
    if (rxFlag) {
      rxFlag = false;
      size_t len = radio.getPacketLength();
      uint8_t buf[69];
      size_t n = len < sizeof(buf) ? len : sizeof(buf);
      int st = radio.readData(buf, n);
      if (st == RADIOLIB_ERR_NONE && n >= 5) {
        uint16_t magic = ((uint16_t)buf[0] << 8) | buf[1];
        uint8_t fromId = buf[2];
        uint8_t plen = buf[4];
        if (magic == CHAT_MAGIC && plen <= n - 5 && fromId != chatNodeId) {   // 过滤掉不是我们协议/自己回声的包
          String text; text.reserve(plen);
          for (uint8_t i = 0; i < plen; i++) text += (char)buf[5 + i];
          chatLogPush(false, fromId, text);
        }
        // 不匹配的（真实Meshtastic流量/别的噪声）直接丢，不当聊天消息
      }
      radio.startReceive();
    }
  } else {   // LORA_AUTO：逐个试 MODEM_PRESETS，收到合法包（哪怕 CRC 错）就说明 SF/BW 蹲对了
    if (rxFlag) {
      rxFlag = false;
      size_t len = radio.getPacketLength();
      uint8_t buf[64];
      size_t n = len < sizeof(buf) ? len : sizeof(buf);
      int st = radio.readData(buf, n);
      bool crcBad = (st == RADIOLIB_ERR_CRC_MISMATCH);
      if (st == RADIOLIB_ERR_NONE || crcBad) {
        const ModemPreset& m = MODEM_PRESETS[autoIdx];
        activeModem = { m.name, m.bw, m.sf, m.cr, MESH_SYNC, MESH_PREAMBLE };
        loraPushPkt(len, (int16_t)radio.getRSSI(), (int8_t)radio.getSNR(), crcBad, buf, n);
        autoFound = true;
        mode = LORA_LISTEN;
        loraStartListen();
        return;
      }
      radio.startReceive();
    } else if (millis() - autoDwellStartMs > AUTO_DWELL_MS) {   // 这组没蹲到，换下一组参数
      autoIdx = (autoIdx + 1) % MODEM_COUNT;
      loraStartListenAuto();
      dirty = true;
    }
  }
}

// 换成手动预设（离开 AUTO 检测到的参数，回到 PRESETS 列表里选的那组 BW/SF/CR）
static void loraApplyPreset() {
  const LoraPreset& p = PRESETS[presetIdx];
  curFreq = p.freq;
  activeModem = { p.name, p.bw, p.sf, p.cr, p.sync, p.preamble };
}

void loraKey(int k) {
  if (mode == LORA_CHAT) {               // 聊天模式：几乎所有键都当打字，跟别的模式的导航键分开处理
    if (k == '\n') {
      if (composeBuf.length() == 0) {      // 空行按 Enter：退出聊天回 LISTEN，不发空消息
        loraApplyPreset(); mode = LORA_LISTEN; if (loraReady) loraStartListen();
      } else if (loraSendText(composeBuf)) {
        composeBuf = "";                     // 发出去了才清空；限速窗口里没发成就原样留着让用户再按一次
      }
      dirty = true;
    } else if (k == '\b') {
      if (composeBuf.length()) composeBuf.remove(composeBuf.length() - 1);
      dirty = true;
    } else if (k >= 0x20 && k <= 0x7e && composeBuf.length() < 64) {
      composeBuf += (char)k;
      dirty = true;
    }
    return;
  }
  if (k == ' ') {                        // 空格键：在各模式间快速循环 (SCAN -> LISTEN -> AUTO -> CHAT)
    if (mode == LORA_SCAN) {
      vhfWatch = false; vhfLogClose();
      loraApplyPreset();
      mode = LORA_LISTEN;
      if (loraReady) loraStartListen();
    } else if (mode == LORA_LISTEN) {
      if (loraReady) {
        autoIdx = 0; autoFound = false;
        mode = LORA_AUTO;
        loraStartListenAuto();
      }
    } else if (mode == LORA_AUTO) {
      loraApplyPreset();
      mode = LORA_CHAT;
      if (loraReady) loraStartListen();
      composeBuf = "";
    }
    dirty = true;
    return;
  }
  if (k == ';') {                        // ↑ 上翻到 SCAN 页
    if (mode != LORA_SCAN) {
      vhfWatch = false; vhfLogClose();
      mode = LORA_SCAN;
      if (loraReady) radio.standby();
      dirty = true;
    }
  } else if (k == '.') {                 // ↓ 下翻到 LISTEN 页（用预设默认频率/参数，不锁频）
    if (mode != LORA_LISTEN) {
      vhfWatch = false; vhfLogClose();
      loraApplyPreset();
      mode = LORA_LISTEN; if (loraReady) loraStartListen();
      dirty = true;
    }
  } else if (k == ',') {                 // ← 上一个预设（AUTO 时只换频率，继续蹲，参数不变）
    presetIdx = (presetIdx - 1 + PRESET_COUNT) % PRESET_COUNT;
    if (mode == LORA_AUTO) { curFreq = PRESETS[presetIdx].freq; if (loraReady) loraStartListenAuto(); }
    else { loraApplyPreset(); if (mode == LORA_LISTEN && loraReady) loraStartListen(); }
    dirty = true;
  } else if (k == '/') {                 // → 下一个预设
    presetIdx = (presetIdx + 1) % PRESET_COUNT;
    if (mode == LORA_AUTO) { curFreq = PRESETS[presetIdx].freq; if (loraReady) loraStartListenAuto(); }
    else { loraApplyPreset(); if (mode == LORA_LISTEN && loraReady) loraStartListen(); }
    dirty = true;
  } else if ((k == 'v' || k == 'V') && mode == LORA_SCAN) {
    vhfBand = !vhfBand;               // ISM <-> VHF(AIS 探测)
    vhfWatch = false; vhfLogClose();  // 换频段就别继续蹲了，ISM 上这套判定没意义
    loraApplyScanBand();
    dirty = true;
  } else if (k == 'a' && mode == LORA_SCAN && vhfBand) {
    // VHF 下 a 是"AIS 蹲守"开关。这里必须挡在下面那条 LoRa AUTO 前面——
    // AUTO 试的是 SF/BW 这些 LoRa 调制参数，在 FSK 的 VHF 频段上毫无意义。
    vhfWatch = !vhfWatch;
    if (vhfWatch) vhfWatchReset();
    else { scanPos = 0; vhfLogClose(); }   // 退出蹲守，扫频从头开始
    dirty = true;
  } else if (k == '\n' && mode == LORA_SCAN && !vhfBand) {   // Enter：把 LISTEN 锁到扫到的峰值频点上
    if (loraReady) {
      curFreq = loraScanPeakFreq();
      mode = LORA_LISTEN;
      loraStartListen();
      dirty = true;
    }
  } else if (k == 'a') {                 // AUTO：在 curFreq 上轮流试各种 SF/BW，蹲中就停
    if (mode == LORA_AUTO) {             // 再按一次=放弃自动探测，回到手动预设
      loraApplyPreset();
      mode = LORA_LISTEN;
      if (loraReady) loraStartListen();
    } else if (loraReady) {
      autoIdx = 0; autoFound = false;
      mode = LORA_AUTO;
      loraStartListenAuto();
    }
    dirty = true;
  } else if (k == 'r' || k == 'R') {     // 清：峰值保持 / 抓包日志 / 蹲守统计
    for (int i = 0; i < NBINS; i++) peakBins[i] = RSSI_FLOOR;
    pktHead = pktCount = 0; pktTotal = 0;
    if (vhfWatch) vhfWatchReset();
    dirty = true;
  } else if (k == 'c' || k == 'C') {     // 进聊天模式：用当前预设频率/参数收发我们自家的私有帧
    loraApplyPreset();
    mode = LORA_CHAT;
    if (loraReady) loraStartListen();
    composeBuf = "";
    dirty = true;
  }
}

// RSSI(dBm) → 柱高比例 0..1
static float rssiNorm(float r) {
  float t = (r - RSSI_FLOOR) / (RSSI_CEIL - RSSI_FLOOR);
  return t < 0 ? 0 : (t > 1 ? 1 : t);
}

static int freqToX(float f, int gx, int gw) {
  float t = (f - scanLo()) / (scanHi() - scanLo());
  if (t < 0) t = 0; else if (t > 1) t = 1;
  return gx + (int)(t * gw);
}

static void drawScanView() {
  const int gx = 6, gw = SW - 12; // 228
  const int scopeY = 28, scopeH = 76; // y: 28..104
  const int gy0 = 34, gh = 57, baseY = gy0 + gh; // baseY = 91

  // Top sub-banner outside scope (y = 18..27)
  char hdr[64];
  cv.setTextDatum(top_left); cv.setTextSize(1);
  if (vhfBand && vhfWatch && vhfCal) {
    snprintf(hdr, sizeof(hdr), "PICKING CONTROL FREQS  %d%%", scanPos * 100 / NBINS);
    cv.setTextColor(TFT_YELLOW, 0x0000);
  } else if (vhfBand && vhfWatch && vhfRounds <= VHF_WARMUP) {
    snprintf(hdr, sizeof(hdr), "WARMING UP  %lu/%lu ROUNDS",
             (unsigned long)vhfRounds, (unsigned long)VHF_WARMUP);
    cv.setTextColor(TFT_YELLOW, 0x0000);
  } else if (vhfBand && vhfWatch) {
    uint32_t sec = (millis() - vhfWatchStart) / 1000;
    snprintf(hdr, sizeof(hdr), "AIS ENERGY  %lu:%02lu  %lur %s",
             (unsigned long)(sec / 60), (unsigned long)(sec % 60), (unsigned long)vhfRounds,
             vhfLogOk ? "SD" : "NOLOG");
    cv.setTextColor(vhfLogOk ? 0x07E0 : 0xFDA0, 0x0000);
  } else {
    snprintf(hdr, sizeof(hdr), "%s", vhfBand ? "VHF 161.5-162.5  AIS PROBE" : "ISM 863-928 MHz  LORA SCOPE");
    cv.setTextColor(vhfBand ? 0xFDA0 : 0x07FF, 0x0000);
  }
  cv.drawString(hdr, 6, 18);

  cv.setTextDatum(top_right);
  cv.setTextColor(0x8410, 0x0000);
  cv.drawString(vhfBand ? (vhfWatch ? "A=STOP" : "A=WATCH") : "V=BAND", SW - 6, 18);

  // 1. Spectrum Scope Card Frame (y: 28..104)
  cv.fillRoundRect(4, scopeY, SW - 8, scopeH, 3, 0x0000);
  cv.drawRoundRect(4, scopeY, SW - 8, scopeH, 3, 0x18C3);

  // 2. Calibrated dBm Grid & Labels (-60, -80, -100 dBm)
  const float gridDbm[] = { -60.0f, -80.0f, -100.0f };
  for (int gi = 0; gi < 3; gi++) {
    int gy = baseY - (int)(rssiNorm(gridDbm[gi]) * gh);
    if (gy > gy0 + 2 && gy < baseY) {
      for (int xi = gx; xi < gx + gw; xi += 4) cv.drawPixel(xi, gy, 0x1082);
      cv.setTextDatum(top_left); cv.setTextColor(0x4208, 0x0000);
      char glbl[8]; snprintf(glbl, sizeof(glbl), "%d", (int)gridDbm[gi]);
      cv.drawString(glbl, gx + 2, gy - 4);
    }
  }

  // 3. Known Channel Guides (Beacons)
  struct Mark { float f; const char* tag; uint16_t col; };
  const Mark ISM_MARKS[] = { {869.525f, "EU", 0x07FF}, {906.875f, "US", 0xFDA0} };
  const Mark VHF_MARKS[] = { {AIS1, "A1", 0x07FF}, {AIS2, "A2", 0x07FF} };
  const Mark* MARKS = vhfBand ? VHF_MARKS : ISM_MARKS;
  for (int mi = 0; mi < 2; mi++) {
    const Mark& mk = MARKS[mi];
    int mx = freqToX(mk.f, gx, gw);
    for (int y = gy0 + 1; y < baseY; y += 3) cv.drawPixel(mx, y, 0x2145);
    cv.setTextColor(mk.col, 0x0000); cv.setTextDatum(bottom_center);
    cv.drawString(mk.tag, mx, gy0 - 1);
  }
  if (vhfBand && vhfWatch && !vhfCal) {
    int cx1 = freqToX(vhfCtrl[0], gx, gw);
    int cx2 = freqToX(vhfCtrl[1], gx, gw);
    for (int y = gy0 + 1; y < baseY; y += 3) {
      cv.drawPixel(cx1, y, 0x18C3);
      cv.drawPixel(cx2, y, 0x18C3);
    }
    cv.setTextColor(0x8410, 0x0000); cv.setTextDatum(bottom_center);
    cv.drawString("C1", cx1, gy0 - 1);
    cv.drawString("C2", cx2, gy0 - 1);
  }

  // Baseline
  cv.drawFastHLine(gx, baseY, gw, 0x2145);
  float bw = gw / (float)NBINS;

  // Peak calculation
  int pk = 0;
  for (int i = 1; i < NBINS; i++) if (peakBins[i] > peakBins[pk]) pk = i;

  // 4. Spectrum Columns
  for (int i = 0; i < NBINS; i++) {
    int x = gx + (int)(i * bw);
    int w = (int)bw - 1; if (w < 2) w = 2;
    int h = (int)(rssiNorm(rssiBins[i]) * gh);
    int ph = (int)(rssiNorm(peakBins[i]) * gh);
    bool isPeak = (i == pk && peakBins[pk] > RSSI_FLOOR + 8);

    if (isPeak) {
      if (h > 0) cv.fillRect(x, baseY - h, w, h, 0x07E0);
      if (ph > 0) {
        cv.fillRect(x - 1, baseY - ph - 1, w + 2, 2, 0xFFFF);
        cv.drawPixel(x + w / 2, baseY - ph - 2, 0x07E0);
      }
    } else {
      if (h > 0) {
        for (int sy = baseY - h; sy < baseY; sy += 2) {
          uint16_t c = (sy < gy0 + 16) ? 0xFDA0 : (sy < gy0 + 34 ? 0x07FF : 0x19B3);
          cv.drawFastHLine(x, sy, w, c);
        }
      }
      if (ph > 0) cv.drawFastHLine(x, baseY - ph, w, 0x8410);
    }
  }

  // 5. Frequency Scale (bottom of scope)
  const float ISM_TICKS[] = { 863, 880, 900, 915, 928 };
  const float VHF_TICKS[] = { 161.5f, 161.75f, 162.0f, 162.25f, 162.5f };
  const float* TICKS = vhfBand ? VHF_TICKS : ISM_TICKS;
  char b[64];
  cv.setTextColor(0x632C, 0x0000); cv.setTextSize(1);
  for (int i = 0; i < 5; i++) {
    int tx = freqToX(TICKS[i], gx, gw);
    cv.drawFastVLine(tx, baseY + 1, 2, 0x632C);
    cv.setTextDatum(i == 0 ? top_left : (i == 4 ? top_right : top_center));
    if (vhfBand) snprintf(b, sizeof(b), "%.2f", TICKS[i]);
    else         snprintf(b, sizeof(b), "%d", (int)TICKS[i]);
    cv.drawString(b, tx, baseY + 3);
  }

  // 6. Bottom Status & Telemetry Pill (y: 106..132)
  bool hitFlash = false;
  if (vhfBand && vhfWatch) {
    hitFlash = (vhfLastHit && millis() - vhfLastHit < 1500);
  }

  cv.fillRoundRect(4, 106, SW - 8, 26, 3, hitFlash ? 0x03E0 : 0x0821);
  cv.drawRoundRect(4, 106, SW - 8, 26, 3, hitFlash ? 0x07E0 : 0x18C3);

  if (vhfBand && vhfWatch) {
    cv.setTextDatum(top_left);
    snprintf(b, sizeof(b), "AVG %+d  d: %+d dB", (int)vhfBias, (int)vhfMargin);
    cv.setTextColor(vhfMargin >= vhfThr ? 0x07E0 : 0x07FF, hitFlash ? 0x03E0 : 0x0821);
    cv.drawString(b, 8, 109);

    cv.setTextDatum(top_right);
    snprintf(b, sizeof(b), "UP:%lu  DN:%lu", (unsigned long)vhfHits, (unsigned long)vhfNulls);
    cv.setTextColor(0x07E0, hitFlash ? 0x03E0 : 0x0821);
    cv.drawString(b, SW - 8, 109);

    cv.setTextDatum(top_left);
    snprintf(b, sizeof(b), "THR %.1f dB  PK %+d dB", vhfThr, (int)vhfBestMargin);
    cv.setTextColor(0x8410, hitFlash ? 0x03E0 : 0x0821);
    cv.drawString(b, 8, 119);

    cv.setTextDatum(top_right);
    cv.setTextColor(vhfLogOk ? 0x07E0 : 0xFDA0, hitFlash ? 0x03E0 : 0x0821);
    cv.drawString(vhfLogOk ? "[A]STOP (SD)" : "[A]STOP (NOLOG)", SW - 8, 119);
  } else if (vhfBand) {
    float a1, a2, floorDb;
    vhfProbeStats(a1, a2, floorDb);
    float best = max(a1, a2) - floorDb;

    cv.setTextDatum(top_left);
    snprintf(b, sizeof(b), "A1 %.0f  A2 %.0f  FL %.0f", a1, a2, floorDb);
    cv.setTextColor(0x07FF, 0x0821);
    cv.drawString(b, 8, 109);

    cv.setTextDatum(top_right);
    snprintf(b, sizeof(b), "(%+ddB)", (int)best);
    cv.setTextColor(best >= 6.0f ? 0x07E0 : 0x8410, 0x0821);
    cv.drawString(b, SW - 8, 109);

    cv.setTextDatum(top_left);
    cv.setTextColor(0x8410, 0x0821);
    cv.drawString("AIS 161.5-162.5", 8, 119);

    cv.setTextDatum(top_right);
    cv.setTextColor(0xFDA0, 0x0821);
    cv.drawString("[A]WATCH  [V]ISM", SW - 8, 119);
  } else {
    cv.setTextDatum(top_left);
    if (peakBins[pk] > RSSI_FLOOR + 8) {
      float pf = scanLo() + (scanHi() - scanLo()) * pk / (NBINS - 1);
      snprintf(b, sizeof(b), "PEAK: %.1fMHz  %ddBm", pf, (int)peakBins[pk]);
      cv.setTextColor(0x07E0, 0x0821);
      cv.drawString(b, 8, 109);

      cv.setTextDatum(top_right);
      cv.setTextColor(0x07FF, 0x0821);
      cv.drawString("[RET=LOCK]", SW - 8, 109);
    } else {
      cv.setTextColor(0x8410, 0x0821);
      cv.drawString("PEAK: NO ACTIVE SIGNAL", 8, 109);

      cv.setTextDatum(top_right);
      cv.setTextColor(0x4208, 0x0821);
      cv.drawString("[IDLE]", SW - 8, 109);
    }

    cv.setTextDatum(top_left);
    cv.setTextColor(0x8410, 0x0821);
    cv.drawString("BAND: ISM", 8, 119);

    cv.setTextDatum(top_right);
    cv.setTextColor(0x8410, 0x0821);
    cv.drawString("[V]VHF [A]AUTO [C]CHAT", SW - 8, 119);
  }
}

static void drawListenView() {
  // 1. Top 4-Pod Telemetry Matrix (y: 19..41, h = 22)
  // Pod 0: PRESET
  cv.fillRoundRect(4, 19, 55, 22, 2, 0x0821); cv.drawRoundRect(4, 19, 55, 22, 2, 0x18C3);
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x8410, 0x0821); cv.drawString("PRESET", 7, 21);
  cv.setTextColor(0x07FF, 0x0821); cv.drawString(trunc(PRESETS[presetIdx].name, 7), 7, 31);

  // Pod 1: FREQ
  bool freqLocked = fabsf(curFreq - PRESETS[presetIdx].freq) > 0.01f;
  cv.fillRoundRect(62, 19, 55, 22, 2, 0x0821); cv.drawRoundRect(62, 19, 55, 22, 2, 0x18C3);
  cv.setTextColor(0x8410, 0x0821); cv.drawString(freqLocked ? "LOCKED" : "FREQ", 65, 21);
  char fstr[16]; snprintf(fstr, sizeof(fstr), "%.1fM%s", curFreq, freqLocked ? "*" : "");
  cv.setTextColor(0xFDA0, 0x0821); cv.drawString(fstr, 65, 31);

  // Pod 2: MODEM
  cv.fillRoundRect(120, 19, 55, 22, 2, 0x0821); cv.drawRoundRect(120, 19, 55, 22, 2, 0x18C3);
  cv.setTextColor(0x8410, 0x0821); cv.drawString("MODEM", 123, 21);
  char mstr[16]; snprintf(mstr, sizeof(mstr), "SF%d/B%.0f", activeModem.sf, activeModem.bw);
  cv.setTextColor(0x07E0, 0x0821); cv.drawString(mstr, 123, 31);

  // Pod 3: PACKETS
  cv.fillRoundRect(178, 19, 58, 22, 2, 0x0821); cv.drawRoundRect(178, 19, 58, 22, 2, 0x18C3);
  cv.setTextColor(0x8410, 0x0821); cv.drawString("PACKETS", 181, 21);
  char pstr[16]; snprintf(pstr, sizeof(pstr), "%lu", (unsigned long)pktTotal);
  cv.setTextColor(0xFFFF, 0x0821); cv.drawString(pstr, 181, 31);

  // 2. Decoded Packet Feed Card (y: 44..119, h = 76)
  cv.fillRoundRect(4, 44, SW - 8, 76, 3, 0x0821); cv.drawRoundRect(4, 44, SW - 8, 76, 3, 0x18C3);

  // Card Header Banner
  cv.setTextDatum(top_left); cv.setTextColor(0x8410, 0x0821);
  cv.drawString("MESHTASTIC / LORA FEED", 8, 47);
  cv.setTextDatum(top_right);
  if (pktCount > 0) {
    char t[32]; snprintf(t, sizeof(t), "%lus ago", (unsigned long)((millis() - lastPktMs) / 1000));
    cv.setTextColor(0x07E0, 0x0821); cv.drawString(t, SW - 8, 47);
  } else {
    cv.setTextColor(0x4208, 0x0821); cv.drawString("STANDBY", SW - 8, 47);
  }
  cv.drawFastHLine(8, 57, SW - 16, 0x18C3);

  if (pktCount == 0) {
    // Cyber Standby Radar Graphic
    int rcx = 36, rcy = 88;
    cv.drawCircle(rcx, rcy, 18, 0x18C3);
    cv.drawCircle(rcx, rcy, 9, 0x1082);
    cv.drawFastHLine(rcx - 22, rcy, 45, 0x18C3);
    cv.drawFastVLine(rcx, rcy - 22, 45, 0x18C3);
    cv.fillCircle(rcx + 7, rcy - 6, 2, 0x07FF);

    cv.setTextDatum(top_left);
    cv.setTextColor(0x07FF, 0x0821); cv.drawString("RECEIVER LISTENING...", 66, 64);
    char linfo[48]; snprintf(linfo, sizeof(linfo), "Target: %.1f MHz (%s)", curFreq, trunc(PRESETS[presetIdx].name, 7).c_str());
    cv.setTextColor(0x8410, 0x0821); cv.drawString(linfo, 66, 76);
    cv.setTextColor(0x4208, 0x0821); cv.drawString("Waiting for incoming frames", 66, 88);
    cv.setTextColor(0x4208, 0x0821); cv.drawString("Auto-decodes node, hops, RSSI", 66, 100);
  } else {
    int rows = min(pktCount, 3);
    int py = 60;
    for (int i = 0; i < rows; i++) {
      int idx = (pktHead - 1 - i + LOG_N * 2) % LOG_N;
      PktRec& r = pktLog[idx];
      uint16_t rowBg = (i == 0) ? 0x0C42 : 0x0618;
      cv.fillRoundRect(6, py, SW - 12, 18, 2, rowBg);
      cv.drawRoundRect(6, py, SW - 12, 18, 2, (i == 0) ? 0x2145 : 0x1082);

      cv.setTextDatum(top_left); cv.setTextSize(1);
      if (r.hasHeader) {
        char toStr[10];
        if (r.to == 0xFFFFFFFFu) snprintf(toStr, sizeof(toStr), "ALL");
        else snprintf(toStr, sizeof(toStr), "!%04lx", (unsigned long)(r.to & 0xFFFF));
        char fromStr[10]; snprintf(fromStr, sizeof(fromStr), "!%04lx", (unsigned long)(r.from & 0xFFFF));

        char line1[64];
        snprintf(line1, sizeof(line1), "%s->%s ch%02x H%d/%d%s",
                 fromStr, toStr, r.channelHash, r.hopLimit, r.hopStart, r.viaMqtt ? "M" : "");
        cv.setTextColor(r.crcBad ? 0xF800 : 0xFFFF, rowBg);
        cv.drawString(line1, 10, py + 5);

        uint16_t sigCol = r.rssi > -90 ? 0x07E0 : (r.rssi > -105 ? 0xFDA0 : 0xF800);
        char sig[20]; snprintf(sig, sizeof(sig), "%ddBm", (int)r.rssi);
        cv.setTextDatum(top_right);
        cv.setTextColor(sigCol, rowBg);
        cv.drawString(sig, SW - 10, py + 5);
      } else {
        char rawLine[64];
        snprintf(rawLine, sizeof(rawLine), "RAW %dB %s%s", r.len, r.hex, r.crcBad ? " !CRC" : "");
        cv.setTextColor(r.crcBad ? 0xF800 : 0xFDA0, rowBg);
        cv.drawString(rawLine, 10, py + 5);

        char sig[20]; snprintf(sig, sizeof(sig), "%ddB", (int)r.rssi);
        cv.setTextDatum(top_right);
        cv.setTextColor(0x8410, rowBg);
        cv.drawString(sig, SW - 10, py + 5);
      }
      py += 19;
    }
  }

  // 3. Bottom Navigation Pill (y: 122..134)
  cv.fillRoundRect(4, 122, SW - 8, 12, 2, 0x0000);
  cv.setTextDatum(middle_center); cv.setTextColor(0x8410, 0x0000);
  cv.drawString("[,/]PRESET  [A]AUTO  [C]CHAT  [;]SCAN", SW / 2, 128);
}

static void drawAutoView() {
  // 1. Top Frequency & Comb Banner (y: 19..32, h = 14)
  cv.fillRoundRect(4, 19, SW - 8, 14, 2, 0x0821);
  cv.drawRoundRect(4, 19, SW - 8, 14, 2, 0x18C3);

  char fbuf[32]; snprintf(fbuf, sizeof(fbuf), "SWEEP FREQ: %.3f MHz", curFreq);
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821); cv.drawString(fbuf, 8, 21);

  char cbuf[24]; snprintf(cbuf, sizeof(cbuf), "COMB %02d/%02d", autoIdx + 1, MODEM_COUNT);
  cv.setTextDatum(top_right); cv.setTextColor(0xFDA0, 0x0821); cv.drawString(cbuf, SW - 8, 21);

  // 2. Center Modem Card (y: 36..98, h = 63)
  cv.fillRoundRect(4, 36, SW - 8, 63, 4, 0x0821);
  cv.drawRoundRect(4, 36, SW - 8, 63, 4, 0x18C3);

  const ModemPreset& m = MODEM_PRESETS[autoIdx];
  char title[48]; snprintf(title, sizeof(title), "--- [ %s ] ---", m.name);
  cv.setTextDatum(top_center); cv.setTextColor(0x07E0, 0x0821); cv.drawString(title, SW / 2, 39);

  // 3 Parameter Pods (y: 51..78, h = 28)
  // Pod 0: BANDWIDTH
  cv.fillRoundRect(8, 51, 72, 28, 2, 0x10A2); cv.drawRoundRect(8, 51, 72, 28, 2, 0x2145);
  cv.setTextDatum(top_left); cv.setTextColor(0x8410, 0x10A2); cv.drawString("BANDWIDTH", 12, 53);
  char bws[16]; snprintf(bws, sizeof(bws), "%.0f kHz", m.bw);
  cv.setTextColor(0x07FF, 0x10A2); cv.drawString(bws, 12, 65);

  // Pod 1: SPREAD FACTOR
  cv.fillRoundRect(84, 51, 72, 28, 2, 0x10A2); cv.drawRoundRect(84, 51, 72, 28, 2, 0x2145);
  cv.setTextDatum(top_left); cv.setTextColor(0x8410, 0x10A2); cv.drawString("SPREAD FACT", 88, 53);
  char sfs[16]; snprintf(sfs, sizeof(sfs), "SF %d", m.sf);
  cv.setTextColor(0xFDA0, 0x10A2); cv.drawString(sfs, 88, 65);

  // Pod 2: CODING RATE
  cv.fillRoundRect(160, 51, 72, 28, 2, 0x10A2); cv.drawRoundRect(160, 51, 72, 28, 2, 0x2145);
  cv.setTextDatum(top_left); cv.setTextColor(0x8410, 0x10A2); cv.drawString("CODING RATE", 164, 53);
  char crs[16]; snprintf(crs, sizeof(crs), "4/%d", m.cr);
  cv.setTextColor(0x07E0, 0x10A2); cv.drawString(crs, 164, 65);

  cv.setTextDatum(top_center); cv.setTextColor(0x4208, 0x0821);
  cv.drawString("SYNC 0x2B (Meshtastic)  PREAMBLE 16", SW / 2, 85);

  // 3. Segmented Dwell Progress Bar (y: 102..117, h = 15)
  cv.fillRoundRect(4, 102, SW - 8, 15, 2, 0x0821);
  cv.drawRoundRect(4, 102, SW - 8, 15, 2, 0x18C3);

  uint32_t elapsed = millis() - autoDwellStartMs;
  if (elapsed > AUTO_DWELL_MS) elapsed = AUTO_DWELL_MS;
  uint32_t left = AUTO_DWELL_MS - elapsed;

  int numBlocks = 17;
  int activeBlocks = (int)((float)elapsed / AUTO_DWELL_MS * numBlocks);
  for (int b = 0; b < numBlocks; b++) {
    int bx = 8 + b * 10;
    uint16_t bcol = (b < activeBlocks) ? 0x07FF : 0x10A2;
    cv.fillRoundRect(bx, 105, 8, 9, 1, bcol);
  }

  char tstr[16]; snprintf(tstr, sizeof(tstr), "%.1fs", left / 1000.0f);
  cv.setTextDatum(top_right); cv.setTextColor(0xFDA0, 0x0821);
  cv.drawString(tstr, SW - 8, 105);

  // 4. Bottom Navigation Hint (y: 122..134)
  cv.fillRoundRect(4, 122, SW - 8, 12, 2, 0x0000);
  cv.setTextDatum(middle_center); cv.setTextColor(0x8410, 0x0000);
  cv.drawString("[A]HOLD  *LOCKS ON PKT*  [;]SCAN", SW / 2, 128);
}

static void drawLoraChatView() {
  // 1. Identity & Channel Sub-Banner (y: 19..32, h = 14)
  cv.fillRoundRect(4, 19, SW - 8, 14, 2, 0x0821);
  cv.drawRoundRect(4, 19, SW - 8, 14, 2, 0x18C3);

  char nbuf[24]; snprintf(nbuf, sizeof(nbuf), "NODE: !%02X", chatNodeId);
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x07E0, 0x0821); cv.drawString(nbuf, 8, 21);

  char fbuf[24]; snprintf(fbuf, sizeof(fbuf), "%.3f MHz", curFreq);
  cv.setTextDatum(top_center); cv.setTextColor(0x07FF, 0x0821); cv.drawString(fbuf, SW / 2, 21);

  cv.setTextDatum(top_right); cv.setTextColor(0x8410, 0x0821); cv.drawString("MAGIC: C5A1", SW - 8, 21);

  // 2. Message Feed Area (y: 35..104, h = 70)
  cv.fillRoundRect(4, 35, SW - 8, 70, 3, 0x0000);
  cv.drawRoundRect(4, 35, SW - 8, 70, 3, 0x18C3);

  if (chatCount == 0) {
    cv.setTextDatum(top_center);
    cv.setTextColor(0x07FF, 0x0000); cv.drawString("SECURE P2P LORA TERMINAL", SW / 2, 50);
    cv.setTextColor(0x8410, 0x0000); cv.drawString("No messages on current channel", SW / 2, 64);
    cv.setTextColor(0x4208, 0x0000); cv.drawString("Type message below to broadcast", SW / 2, 78);
  } else {
    int showCount = min(chatCount, 4);
    int my = 39;
    for (int i = 0; i < showCount; i++) {
      int idx = (chatHead - 1 - i + CHAT_LOG_N * 2) % CHAT_LOG_N;
      ChatMsg& m = chatLog[idx];
      cv.setTextDatum(top_left); cv.setTextSize(1);
      if (m.mine) {
        cv.fillRoundRect(8, my, 26, 12, 2, 0x0200);
        cv.drawRoundRect(8, my, 26, 12, 2, 0x07E0);
        cv.setTextColor(0x07E0, 0x0200); cv.drawString("ME", 13, my + 2);
        cv.setTextColor(0xFFFF, 0x0000); cv.drawString(trunc(m.text, 31), 38, my + 2);
      } else {
        cv.fillRoundRect(8, my, 26, 12, 2, 0x0010);
        cv.drawRoundRect(8, my, 26, 12, 2, 0x07FF);
        char tag[12]; snprintf(tag, sizeof(tag), "!%02X", m.nodeId);
        cv.setTextColor(0x07FF, 0x0010); cv.drawString(tag, 9, my + 2);
        cv.setTextColor(0xFDA0, 0x0000); cv.drawString(trunc(m.text, 31), 38, my + 2);
      }
      if (i < showCount - 1) cv.drawFastHLine(8, my + 14, SW - 16, 0x1082);
      my += 16;
    }
  }

  // 3. Compose Command Line Bar (y: 106..121, h = 16)
  cv.fillRoundRect(4, 106, SW - 8, 16, 2, 0x0821);
  cv.drawRoundRect(4, 106, SW - 8, 16, 2, 0x07FF);

  cv.setTextDatum(top_left); cv.setTextColor(0x07FF, 0x0821); cv.drawString(">", 8, 109);
  String displayMsg = composeBuf + (((millis() / 400) % 2) ? "_" : " ");
  cv.setTextColor(0xFFFF, 0x0821); cv.drawString(trunc(displayMsg, 32), 18, 109);

  char cnt[16]; snprintf(cnt, sizeof(cnt), "%d/64", (int)composeBuf.length());
  cv.setTextDatum(top_right); cv.setTextColor(0x8410, 0x0821); cv.drawString(cnt, SW - 8, 109);

  // 4. Bottom Status & Cooldown Pill (y: 123..134)
  uint32_t sinceLast = millis() - lastSendMs;
  cv.fillRoundRect(4, 123, SW - 8, 11, 2, 0x0000);
  cv.setTextDatum(middle_center);
  if (sinceLast < CHAT_MIN_INTERVAL_MS) {
    char cd[48]; snprintf(cd, sizeof(cd), "RATE LIMIT: WAIT %lus TO SEND",
                          (unsigned long)((CHAT_MIN_INTERVAL_MS - sinceLast) / 1000 + 1));
    cv.setTextColor(0xFDA0, 0x0000); cv.drawString(cd, SW / 2, 128);
  } else {
    cv.setTextColor(0x8410, 0x0000); cv.drawString("[RET]SEND  [EMPTY]EXIT  [DEL]BACK", SW / 2, 128);
  }
}

void drawLora() {
  cv.fillScreen(TFT_BLACK);
  drawPageHeader("LoRa");

  // Mode badge in header
  const char* modeLabel = (mode == LORA_SCAN && vhfBand && vhfWatch) ? "AIS"
                        : (mode == LORA_SCAN && vhfBand) ? "VHF"
                        : mode == LORA_SCAN ? "SCAN" : mode == LORA_AUTO ? "AUTO"
                        : mode == LORA_CHAT ? "CHAT" : "LISTEN";
  uint16_t modeColor = (mode == LORA_SCAN && vhfBand && vhfWatch) ? 0x07E0
                     : (mode == LORA_SCAN && vhfBand) ? 0xFDA0
                     : mode == LORA_SCAN ? 0x07FF : mode == LORA_AUTO ? 0xFDA0
                     : mode == LORA_CHAT ? 0x07E0 : 0x07FF;

  cv.fillRoundRect(SW - 52, 2, 48, 12, 2, 0x10A2);
  cv.drawRoundRect(SW - 52, 2, 48, 12, 2, modeColor);
  cv.setTextColor(modeColor, 0x10A2);
  cv.setTextDatum(middle_center);
  cv.setTextSize(1);
  cv.drawString(modeLabel, SW - 28, 8);

  if (!loraReady) {
    // Cyber Hardware Fault Card
    cv.fillRoundRect(10, 24, SW - 20, 98, 4, 0x1800);
    cv.drawRoundRect(10, 24, SW - 20, 98, 4, 0xF800);
    cv.drawFastHLine(10, 24, 8, 0xFFFF);
    cv.drawFastVLine(10, 24, 8, 0xFFFF);
    cv.drawFastHLine(SW - 18, 24, 8, 0xFFFF);
    cv.drawFastVLine(SW - 11, 24, 8, 0xFFFF);

    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.setTextColor(0xF800, 0x1800);
    cv.drawString("[ SX1262 HARDWARE FAULT ]", SW / 2, 30);

    char errStr[48];
    snprintf(errStr, sizeof(errStr), "RadioLib Status Code: %d", loraErr);
    cv.setTextColor(0xFDA0, 0x1800);
    cv.drawString(errStr, SW / 2, 46);

    cv.setTextDatum(top_left);
    cv.setTextColor(0xFFFF, 0x1800);
    cv.drawString("* Check Cap LoRa-1262 header seating", 16, 62);
    cv.drawString("* Verify I2C 0x43 (P0 Antenna Switch)", 16, 74);
    cv.drawString("* SPI Pin 5 (NSS) shared with SD Card", 16, 86);

    cv.setTextDatum(middle_center);
    cv.setTextColor(0x8410, 0x1800);
    cv.drawString("Press 'R' to retry initialization", SW / 2, 108);
    return;
  }

  if (mode == LORA_SCAN) drawScanView();
  else if (mode == LORA_AUTO) drawAutoView();
  else if (mode == LORA_CHAT) drawLoraChatView();
  else drawListenView();
}
