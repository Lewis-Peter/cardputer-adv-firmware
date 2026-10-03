#include "netprobe.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <lwip/sockets.h>
#include <cstring>
#include "ui_common.h"
#include "config.h"
#include "sd_files.h"

// =============================================================================
// 统一模式与调度框架 (Probe 模式 / DNS 模式共用)
// =============================================================================
static NetProbeMode currentMode = NETPROBE_MODE_PROBE;

// 自动巡检开关与节拍（两种模式共用节拍与状态指示）
static bool autoMode = false;
static uint32_t nextAutoMs = 0;
static const uint32_t AUTO_INTERVAL_MS = 5UL * 60 * 1000;   // 5分钟一轮

// 通用字段编辑器（Probe 模式改 vpn-node，DNS 模式改 target host/port）
enum EditMode { ED_OFF, ED_HOST, ED_PORT };
static EditMode editMode = ED_OFF;
static String editBuf;

// 前向声明
static void startProbeRound();
static void startDnsRun();

// =============================================================================
// [1] Probe 模式：多目标连通性、防火墙穿透与 Captive Portal 检测
// =============================================================================
enum ProbeType { PT_DNS, PT_TCP, PT_HTTP };
struct Probe { const char* label; ProbeType type; const char* host; uint16_t port; const char* path; };

// TCP/HTTP 目标用裸 IP，不走探针自己的 DNS——避免路由器毒 DNS 干扰这一层的判断，
// 保证测出来的 OPEN/BLOCKED 只反映"路由/端口"这一层，不跟 DNS 结果混在一起。
// 前面几个都是公共基础设施(Google/百度/阿里云/Cloudflare)，对谁都一样，可以放心写死；
// "vpn-node"是私人代理出口，因人而异，做成可配置项（存NVS，见下方 loadVpnCfg），不是 const。
static Probe PROBES[] = {
  {"DNS google.com",     PT_DNS,  "www.google.com",  0,   nullptr},
  {"DNS baidu.com",      PT_DNS,  "www.baidu.com",   0,   nullptr},
  {"TCP real-goog:443",  PT_TCP,  "142.251.152.4",   443, nullptr},   // 真实Google IP，测路由级封锁
  {"TCP real-goog:80",   PT_TCP,  "142.251.152.4",   80,  nullptr},   // 同一目标只换端口——隔离"端口"这一个变量
  {"TCP " CFG_PROBE_BASELINE_NAME ":443", PT_TCP, CFG_PROBE_BASELINE_IP, 443, nullptr},   // 基线：应该总是通
  {"TCP cloudflare:443", PT_TCP,  "1.1.1.1",         443, nullptr},   // Cloudflare 443
  {"TCP cloudflare:80",  PT_TCP,  "1.1.1.1",         80,  nullptr},   // Cloudflare 80
  {"TCP vpn-node",       PT_TCP,  CFG_PROBE_VPN_HOST, CFG_PROBE_VPN_PORT, nullptr},   // 默认值，loadVpnCfg() 会用NVS覆盖
  {"HTTP gstatic 204",   PT_HTTP, "www.gstatic.com", 80,  "/generate_204"},  // 抓门户跳转
};
static const int PROBE_COUNT = sizeof(PROBES) / sizeof(PROBES[0]);

// 可配置那一条的下标：按 label 现找，不写死数字
static const char* VPN_PROBE_LABEL = "TCP vpn-node";
static int vpnProbeIdx() {
  static int idx = -1;
  if (idx < 0) {
    idx = 0;
    for (int i = 0; i < PROBE_COUNT; i++) {
      if (strcmp(PROBES[i].label, VPN_PROBE_LABEL) == 0) { idx = i; break; }
    }
  }
  return idx;
}

static char vpnHostBuf[48];
static bool vpnCfgLoaded = false;
static void loadVpnCfg() {
  if (vpnCfgLoaded) return;
  vpnCfgLoaded = true;
  Probe& vp = PROBES[vpnProbeIdx()];
  String h = loadString("netprobe", "vpnhost", vp.host);
  uint16_t p = (uint16_t)loadUInt("netprobe", "vpnport", vp.port);
  snprintf(vpnHostBuf, sizeof(vpnHostBuf), "%s", h.c_str());
  vp.host = vpnHostBuf;
  vp.port = p ? p : 443;
}
static void saveVpnCfg() {
  saveString("netprobe", "vpnhost", vpnHostBuf);
  saveUInt("netprobe", "vpnport", PROBES[vpnProbeIdx()].port);
}

enum ProbeStatus {
  STAT_WAIT,      // 等待运行
  STAT_PROBING,   // 正在测试
  STAT_OK,        // 连通正常 / 204正常
  STAT_FAIL,      // 阻断 / 解析失败
  STAT_WARN,      // 投毒 / 强制门户跳转
};

struct ProbeItemResult {
  ProbeStatus status = STAT_WAIT;
  uint16_t latency = 0;      // ms
  char detail[36] = {0};     // 解析出的IP、HTTP状态码或跳转目标
};

static ProbeItemResult probeResults[PROBE_COUNT];
static String   probeResultLine[PROBE_COUNT];
static uint16_t probeResultColor[PROBE_COUNT];
static int  probeIdx = 0;
static bool probing  = false;
static int  probeRoundNum = 0;
static const char* LOG_PATH_PROBE = "/netprobe.log";

// AS32934 (Meta/Facebook) 常见CIDR段前缀匹配
struct CidrRange { uint8_t a, b, c, d, bits; };
static const CidrRange META_RANGES[] = {
  {31, 13, 24, 0, 21}, {31, 13, 64, 0, 18}, {66, 220, 144, 0, 20}, {69, 63, 176, 0, 20},
  {69, 171, 224, 0, 19}, {74, 119, 76, 0, 22}, {102, 132, 96, 0, 20}, {103, 4, 96, 0, 22},
  {157, 240, 0, 0, 16}, {173, 252, 64, 0, 18}, {179, 60, 192, 0, 22}, {185, 60, 216, 0, 22},
  {204, 15, 20, 0, 22},
};
static const int META_RANGE_COUNT = sizeof(META_RANGES) / sizeof(META_RANGES[0]);

static bool inCidr(const IPAddress& ip, const CidrRange& r) {
  uint32_t ipv  = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3];
  uint32_t netv = ((uint32_t)r.a << 24) | ((uint32_t)r.b << 16) | ((uint32_t)r.c << 8) | r.d;
  uint32_t mask = r.bits == 0 ? 0 : (0xFFFFFFFFu << (32 - r.bits));
  return (ipv & mask) == (netv & mask);
}
static bool isKnownPoisonIp(const IPAddress& ip) {
  for (int i = 0; i < META_RANGE_COUNT; i++) {
    if (inCidr(ip, META_RANGES[i])) return true;
  }
  return false;
}

enum OverallVerdict {
  VERDICT_IDLE,
  VERDICT_PROBING,
  VERDICT_UNRESTRICTED,
  VERDICT_CAPTIVE_PORTAL,
  VERDICT_DNS_POISONED,
  VERDICT_FIREWALL_BLOCK,
  VERDICT_WAN_DISCONNECTED,
};

static OverallVerdict getVerdict() {
  if (probing) return VERDICT_PROBING;

  // 尚未启动探测
  if (probeResults[0].status == STAT_WAIT) return VERDICT_IDLE;

  // 1. 强制门户跳转检测 (Captive Portal)
  if (probeResults[8].status == STAT_WARN) return VERDICT_CAPTIVE_PORTAL;

  // 2. DNS 污染/劫持检测 (命中已知投毒 IP)
  if (probeResults[0].status == STAT_WARN || probeResults[1].status == STAT_WARN) return VERDICT_DNS_POISONED;

  // 3. WAN 完全断网 (DNS 失败且国内基线 223.5.5.5:443 亦不可达)
  if (probeResults[0].status == STAT_FAIL && probeResults[1].status == STAT_FAIL && probeResults[4].status == STAT_FAIL) {
    return VERDICT_WAN_DISCONNECTED;
  }

  // 4. 防火墙端口/IP 过滤 (基线 Aliyun 正常，但 Google/CF 被握手丢弃)
  if (probeResults[2].status == STAT_FAIL || probeResults[3].status == STAT_FAIL ||
      probeResults[5].status == STAT_FAIL || probeResults[6].status == STAT_FAIL) {
    return VERDICT_FIREWALL_BLOCK;
  }

  // 5. 任何其它单项失败
  for (int i = 0; i < PROBE_COUNT; i++) {
    if (probeResults[i].status == STAT_FAIL) return VERDICT_FIREWALL_BLOCK;
  }

  return VERDICT_UNRESTRICTED;
}

// 串口临时探针
void netprobeAdhoc(const String& targetRaw) {
  String target = targetRaw;
  target.trim();
  if (target.length() == 0) {
    Serial.println("[probe] usage: PROBE host[:port]  (port默认443)");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[probe] wifi not connected");
    return;
  }

  String host = target;
  uint16_t port = 443;
  int colonIdx = target.lastIndexOf(':');
  if (colonIdx > 0) {
    host = target.substring(0, colonIdx);
    uint16_t p = (uint16_t)target.substring(colonIdx + 1).toInt();
    if (p != 0) port = p;
  }

  IPAddress ip;
  bool isLiteralIp = ip.fromString(host);
  if (!isLiteralIp) {
    Serial.printf("[probe] resolving %s ...\n", host.c_str());
    if (!WiFi.hostByName(host.c_str(), ip)) {
      Serial.printf("[probe] %s: DNS resolve failed\n", host.c_str());
      return;
    }
    Serial.printf("[probe] %s -> %s%s\n", host.c_str(), ip.toString().c_str(),
                  isKnownPoisonIp(ip) ? "  (命中已知投毒IP段)" : "");
  }

  uint32_t t0 = millis();
  WiFiClient c;
  bool ok = c.connect(ip, port, 1500);
  uint32_t ms = millis() - t0;
  c.stop();
  Serial.printf("[probe] TCP %s:%u -> %s %ums\n", ip.toString().c_str(), port, ok ? "OPEN" : "BLOCKED", ms);
}

static void runOneProbe(int i) {
  const Probe& p = PROBES[i];
  uint32_t t0 = millis();
  switch (p.type) {
    case PT_DNS: {
      IPAddress ip;
      bool ok = WiFi.hostByName(p.host, ip);
      uint32_t ms = millis() - t0;
      probeResults[i].latency = (uint16_t)ms;
      if (!ok) {
        probeResults[i].status = STAT_FAIL;
        snprintf(probeResults[i].detail, sizeof(probeResults[i].detail), "resolve fail");
        probeResultLine[i] = String(p.label) + ": resolve fail";
        probeResultColor[i] = TFT_DARKGREY;
      } else if (isKnownPoisonIp(ip)) {
        probeResults[i].status = STAT_WARN;
        snprintf(probeResults[i].detail, sizeof(probeResults[i].detail), "%s", ip.toString().c_str());
        probeResultLine[i] = String(p.label) + ": " + ip.toString() + " POISON";
        probeResultColor[i] = TFT_RED;
      } else {
        probeResults[i].status = STAT_OK;
        snprintf(probeResults[i].detail, sizeof(probeResults[i].detail), "%s", ip.toString().c_str());
        probeResultLine[i] = String(p.label) + ": " + ip.toString();
        probeResultColor[i] = TFT_GREEN;
      }
      break;
    }
    case PT_TCP: {
      WiFiClient c;
      IPAddress ip;
      bool isIp = ip.fromString(p.host);
      if (!isIp) {
        // 支持配置域名目标：现场解析
        if (!WiFi.hostByName(p.host, ip)) {
          probeResults[i].status = STAT_FAIL;
          probeResults[i].latency = 0;
          snprintf(probeResults[i].detail, sizeof(probeResults[i].detail), "DNS fail");
          probeResultLine[i] = String(p.label) + ": DNS fail";
          probeResultColor[i] = TFT_DARKGREY;
          break;
        }
      }
      // 超时从 3000ms 优化至 1500ms，显著消除静默丢包卡顿
      bool ok = c.connect(ip, p.port, 1500);
      uint32_t ms = millis() - t0;
      c.stop();
      probeResults[i].status = ok ? STAT_OK : STAT_FAIL;
      probeResults[i].latency = (uint16_t)ms;
      snprintf(probeResults[i].detail, sizeof(probeResults[i].detail), "%s", ok ? "OPEN" : "BLOCKED");
      probeResultLine[i] = String(p.label) + ": " + (ok ? "OPEN " : "BLOCKED ") + String(ms) + "ms";
      probeResultColor[i] = ok ? TFT_GREEN : TFT_RED;
      break;
    }
    case PT_HTTP: {
      WiFiClient c;
      // ⚠️ WiFiClient::setTimeout() 的单位是**秒**（它覆盖了 Stream 的毫秒版），setTimeout(1500)
      // 等于把 readStringUntil/readBytes 的超时设成 1500s=25 分钟，对端不说话就卡死主循环。
      // 要毫秒精度就显式调 Stream 那一版（socket 层的 SO_RCVTIMEO 由 connect() 按 1500ms 设好）。
      c.Stream::setTimeout(1500);
      if (!c.connect(p.host, p.port, 1500)) {
        probeResults[i].status = STAT_FAIL;
        probeResults[i].latency = (uint16_t)(millis() - t0);
        snprintf(probeResults[i].detail, sizeof(probeResults[i].detail), "connect fail");
        probeResultLine[i] = String(p.label) + ": connect fail";
        probeResultColor[i] = TFT_RED;
        break;
      }
      c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", p.path, p.host);
      String status = c.readStringUntil('\n');
      String loc = "";
      String line;
      while (c.connected() && (millis() - t0 < 3000) && (line = c.readStringUntil('\n')).length() > 1) {
        if (strncasecmp(line.c_str(), "Location:", 9) == 0) loc = line.substring(9);
      }
      c.stop();
      uint32_t ms = millis() - t0;
      probeResults[i].latency = (uint16_t)ms;
      status.trim(); loc.trim();
      if (status.startsWith("HTTP/1.1 ")) status = status.substring(9);
      else if (status.startsWith("HTTP/1.0 ")) status = status.substring(9);
      bool redirected = loc.length() > 0;
      if (redirected) {
        probeResults[i].status = STAT_WARN;
        String target = loc;
        if (target.startsWith("http://")) target = target.substring(7);
        else if (target.startsWith("https://")) target = target.substring(8);
        int slash = target.indexOf('/');
        if (slash > 0) target = target.substring(0, slash);
        target.trim();
        snprintf(probeResults[i].detail, sizeof(probeResults[i].detail), "%s", target.c_str());
        probeResultColor[i] = TFT_YELLOW;
      } else if (status.startsWith("204")) {
        probeResults[i].status = STAT_OK;
        snprintf(probeResults[i].detail, sizeof(probeResults[i].detail), "204 No Content");
        probeResultColor[i] = TFT_GREEN;
      } else {
        probeResults[i].status = STAT_WARN;
        snprintf(probeResults[i].detail, sizeof(probeResults[i].detail), "%s", status.c_str());
        probeResultColor[i] = TFT_YELLOW;
      }
      probeResultLine[i] = String(p.label) + ": " + status + (redirected ? (" -> " + loc) : "");
      break;
    }
  }
  Serial.printf("[netprobe] %s\n", probeResultLine[i].c_str());
}

static void logProbeRoundToSd() {
  String stamp = nowStamp();
  sdAppend(LOG_PATH_PROBE, String("--- round #") + probeRoundNum + " @ " + stamp + " ---");
  for (int i = 0; i < PROBE_COUNT; i++) sdAppend(LOG_PATH_PROBE, probeResultLine[i]);
}

static void startProbeRound() {
  probeIdx = 0;
  probing = (WiFi.status() == WL_CONNECTED);
  probeRoundNum++;
  Serial.printf("[netprobe] round #%d start, wifi=%s\n", probeRoundNum, probing ? "connected" : "NOT connected");
  for (int i = 0; i < PROBE_COUNT; i++) {
    probeResults[i].status = STAT_WAIT;
    probeResults[i].latency = 0;
    probeResults[i].detail[0] = '\0';
    probeResultLine[i] = String(PROBES[i].label) + ": ...";
    probeResultColor[i] = TFT_DARKGREY;
  }
}

// =============================================================================
// [2] DNS 模式：DNS 压力基准与 10 大 RFC 畸形用例模糊测试
// =============================================================================
// 目标地址存 NVS ("dnsfuzz" 命名空间)，默认优先选用当前网络网关/DNS
static IPAddress DNSMASQ_IP(192, 168, 100, 1);
static uint16_t  DNSMASQ_PORT = 53;
static bool dnsTargetLoaded = false;

static void loadDnsTarget() {
  if (dnsTargetLoaded) return;
  dnsTargetLoaded = true;

  String defHost = "192.168.1.1";
  IPAddress dns = WiFi.dnsIP();
  if (dns != IPAddress((uint32_t)0)) defHost = dns.toString();
  else {
    IPAddress gw = WiFi.gatewayIP();
    if (gw != IPAddress((uint32_t)0)) defHost = gw.toString();
  }

  String h = loadString("dnsfuzz", "host", defHost);
  uint16_t p = (uint16_t)loadUInt("dnsfuzz", "port", 53);
  DNSMASQ_IP.fromString(h);
  DNSMASQ_PORT = p ? p : 53;
}

static void saveDnsTarget() {
  saveString("dnsfuzz", "host", DNSMASQ_IP.toString());
  saveUInt("dnsfuzz", "port", DNSMASQ_PORT);
}

static const char* LOG_PATH_DNS = "/dnsfuzz.log";

static uint16_t nextDnsId() { static uint16_t id = 0x5000; return id++; }

static size_t putDnsHeader(uint8_t* b, uint16_t id, uint16_t flags, uint16_t qd, uint16_t an, uint16_t ns, uint16_t ar) {
  b[0] = id >> 8;    b[1] = id;
  b[2] = flags >> 8; b[3] = flags;
  b[4] = qd >> 8;    b[5] = qd;
  b[6] = an >> 8;    b[7] = an;
  b[8] = ns >> 8;    b[9] = ns;
  b[10] = ar >> 8;   b[11] = ar;
  return 12;
}

// 1. 压缩指针自引用死循环 (RFC1035经典自环)
static size_t caseCompressionLoop(uint8_t* b) {
  size_t n = putDnsHeader(b, nextDnsId(), 0x0100, 1, 0, 0, 0);
  b[n++] = 0xC0; b[n++] = 0x0C;
  b[n++] = 0x00; b[n++] = 0x01;
  b[n++] = 0x00; b[n++] = 0x01;
  return n;
}

// 2. label 声明长度63但提前截断 (测试越界读)
static size_t caseTruncatedLabel(uint8_t* b) {
  size_t n = putDnsHeader(b, nextDnsId(), 0x0100, 1, 0, 0, 0);
  b[n++] = 0x3F;
  memcpy(b + n, "ABCDE", 5); n += 5;
  b[n++] = 0x00; b[n++] = 0x01;
  b[n++] = 0x00; b[n++] = 0x01;
  return n;
}

// 3. 超长 name 溢出 255 字节上限 (5段 x 63 = 320字节)
static size_t caseOversizedName(uint8_t* b) {
  size_t n = putDnsHeader(b, nextDnsId(), 0x0100, 1, 0, 0, 0);
  for (int i = 0; i < 5; i++) {
    b[n++] = 63;
    memset(b + n, 'A' + i, 63); n += 63;
  }
  b[n++] = 0x00;
  b[n++] = 0x00; b[n++] = 0x01;
  b[n++] = 0x00; b[n++] = 0x01;
  return n;
}

// 4. label 长度前缀包含非法保留位 (top 2 bits = 01)
static size_t caseReservedLabelBits(uint8_t* b) {
  size_t n = putDnsHeader(b, nextDnsId(), 0x0100, 1, 0, 0, 0);
  b[n++] = 0x41; b[n++] = 0x41;
  b[n++] = 0x00;
  b[n++] = 0x00; b[n++] = 0x01;
  b[n++] = 0x00; b[n++] = 0x01;
  return n;
}

// 5. 伪造 EDNS0 OPT 记录 (声明 65535 字节附加段)
static size_t caseFakeEdnsOpt(uint8_t* b) {
  size_t n = putDnsHeader(b, nextDnsId(), 0x0100, 0, 0, 0, 1);
  b[n++] = 0x00;
  b[n++] = 0x00; b[n++] = 0x29;
  b[n++] = 0xFF; b[n++] = 0xFF;
  b[n++] = 0x00;
  b[n++] = 0x00;
  b[n++] = 0x00; b[n++] = 0x00;
  b[n++] = 0x00; b[n++] = 0x64;
  return n;
}

// 6. QDCOUNT=2 多问题解析边界测试
static size_t caseMultiQuestion(uint8_t* b) {
  size_t n = putDnsHeader(b, nextDnsId(), 0x0100, 2, 0, 0, 0);
  const char* names[2] = {"a", "bb"};
  for (int i = 0; i < 2; i++) {
    size_t len = strlen(names[i]);
    b[n++] = (uint8_t)len;
    memcpy(b + n, names[i], len); n += len;
    b[n++] = 0x00;
    b[n++] = 0x00; b[n++] = 0x01;
    b[n++] = 0x00; b[n++] = 0x01;
  }
  return n;
}

// 7. 6 字节残缺头部短包测试
static size_t caseTruncatedHeader(uint8_t* b) {
  memset(b, 0, 6);
  b[0] = 0x77; b[1] = 0x77;
  return 6;
}

// 8. 0 字节空 UDP 负载测试
static size_t caseEmptyPacket(uint8_t* b) { (void)b; return 0; }

// 9. TCP-53 标准查询连通性基线
static size_t caseTcpNormalQuery(uint8_t* b) {
  size_t n = putDnsHeader(b, nextDnsId(), 0x0100, 1, 0, 0, 0);
  const char* labels[2] = {"baidu", "com"};
  for (int i = 0; i < 2; i++) {
    size_t len = strlen(labels[i]);
    b[n++] = (uint8_t)len;
    memcpy(b + n, labels[i], len); n += len;
  }
  b[n++] = 0x00;
  b[n++] = 0x00; b[n++] = 0x01;
  b[n++] = 0x00; b[n++] = 0x01;
  return n;
}

// 10. TCP-53 长度前缀虚报与不匹配测试
static size_t caseTcpLengthMismatch(uint8_t* b) {
  size_t n = putDnsHeader(b, nextDnsId(), 0x0100, 1, 0, 0, 0);
  b[n++] = 0x03; memcpy(b + n, "www", 3); n += 3;
  b[n++] = 0x00;
  b[n++] = 0x00; b[n++] = 0x01;
  b[n++] = 0x00; b[n++] = 0x01;
  return n;
}

struct FuzzCase {
  const char* label;
  const char* shortLabel;
  size_t (*build)(uint8_t*);
  bool viaTcp;
};

static const FuzzCase CASES[] = {
  {"loop-ptr",         "loop-ptr",    caseCompressionLoop,    false},
  {"trunc-label",      "trunc-lbl",   caseTruncatedLabel,     false},
  {"oversized-name",   "oversize",    caseOversizedName,      false},
  {"reserved-bits",    "resvd-bit",   caseReservedLabelBits,  false},
  {"fake-edns-opt",    "fake-edns",   caseFakeEdnsOpt,        false},
  {"multi-question",   "multi-q",     caseMultiQuestion,      false},
  {"trunc-header",     "trunc-hdr",   caseTruncatedHeader,    false},
  {"empty-packet",     "empty-pkt",   caseEmptyPacket,        false},
  {"tcp-normal",       "tcp-norm",    caseTcpNormalQuery,     true},
  {"tcp-len-mismatch", "tcp-mism",    caseTcpLengthMismatch,  true},
};
static const int CASE_COUNT = sizeof(CASES) / sizeof(CASES[0]);

enum FuzzState {
  FZ_WAIT,
  FZ_RUNNING,
  FZ_ALIVE,     // 存活探针应答成功
  FZ_DEAD,      // 存活探针超时未响应
};

struct CaseResult {
  FuzzState state = FZ_WAIT;
  uint16_t rtt = 0;       // 存活探针往返毫秒数
  bool directReply = false; // 畸形包自身是否有直接应答 (FORMERR 等)
};

static CaseResult dnsResults[CASE_COUNT];
static char       dnsResultLine[CASE_COUNT][32];
static uint16_t   dnsResultColor[CASE_COUNT];
static int  dnsCaseIdx = 0;
static bool dnsRunning = false;
static bool dnsHalted  = false;
static int  dnsHaltedCaseIdx = -1;
static bool dnsStartPending = false;

// 发一次标准正常查询并计算 RTT 毫秒；若超时返回 0
static uint16_t controlCheckRtt(WiFiUDP& udp) {
  udp.flush();
  while (udp.parsePacket() > 0) udp.flush(); // 清理历史残留响应

  uint16_t reqId = nextDnsId();
  uint8_t buf[64];
  size_t n = putDnsHeader(buf, reqId, 0x0100, 1, 0, 0, 0);
  const char* labels[2] = {"baidu", "com"};
  for (int i = 0; i < 2; i++) {
    size_t len = strlen(labels[i]);
    buf[n++] = (uint8_t)len;
    memcpy(buf + n, labels[i], len); n += len;
  }
  buf[n++] = 0x00;
  buf[n++] = 0x00; buf[n++] = 0x01;
  buf[n++] = 0x00; buf[n++] = 0x01;

  udp.beginPacket(DNSMASQ_IP, DNSMASQ_PORT);
  udp.write(buf, n);
  udp.endPacket();

  uint32_t t0 = millis();
  while (millis() - t0 < 1500) {
    if (udp.parsePacket() > 0) {
      uint32_t dt = millis() - t0;
      uint16_t respId = 0;
      if (udp.available() >= 2) {
        respId = ((uint16_t)udp.read() << 8) | (uint16_t)udp.read();
      }
      udp.flush();
      if (respId == reqId) {
        return dt == 0 ? 1 : (uint16_t)dt;
      }
    }
    delay(5);
  }
  return 0;
}

static void sendTcpCase(int i) {
  WiFiClient tcp;
  // 同 runOneProbe 的 PT_HTTP：WiFiClient::setTimeout 是秒，1500 会让 readBytes 最长卡 25 分钟
  // （tcp-len-mismatch 这条用例恰好就是故意让对端少回数据）
  tcp.Stream::setTimeout(1500);
  uint32_t ct0 = millis();
  if (!tcp.connect(DNSMASQ_IP, DNSMASQ_PORT, 1500)) {
    Serial.printf("[dnsfuzz] sent case '%s' TCP connect FAILED (%ums)\n",
                  CASES[i].label, (unsigned)(millis() - ct0));
    return;
  }
  uint8_t buf[600];
  size_t n = CASES[i].build(buf);
  bool lenMismatch = (strcmp(CASES[i].label, "tcp-len-mismatch") == 0);
  uint16_t declaredLen = lenMismatch ? (uint16_t)(n + 400) : (uint16_t)n;

  uint8_t lenPrefix[2] = { (uint8_t)(declaredLen >> 8), (uint8_t)declaredLen };
  tcp.write(lenPrefix, 2);
  if (n > 0) tcp.write(buf, n);

  uint32_t t0 = millis();
  while (millis() - t0 < 800 && tcp.connected()) {
    if (tcp.available() >= 2) {
      uint8_t rlen[2]; tcp.read(rlen, 2);
      uint16_t rn = ((uint16_t)rlen[0] << 8) | rlen[1];
      uint8_t rbuf[64];
      int got = tcp.readBytes(rbuf, min((size_t)rn, sizeof(rbuf)));
      dnsResults[i].directReply = (got > 0);
      break;
    }
    delay(5);
  }
  // 设置 SO_LINGER 立即丢弃未决数据断开，促使 TCP 连接尽快回收
  struct linger sl;
  sl.l_onoff = 1;
  sl.l_linger = 0;
  tcp.setSocketOption(SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
  tcp.stop();
}

static void runDnsCase(int i) {
  WiFiUDP udp;
  udp.begin(0);

  if (CASES[i].viaTcp) {
    sendTcpCase(i);
  } else {
    uint8_t buf[600];
    size_t n = CASES[i].build(buf);

    udp.beginPacket(DNSMASQ_IP, DNSMASQ_PORT);
    if (n > 0) udp.write(buf, n);
    udp.endPacket();

    uint32_t t0 = millis();
    while (millis() - t0 < 300) {
      int psize = udp.parsePacket();
      if (psize > 0) {
        dnsResults[i].directReply = true;
        udp.flush(); // 释放接收缓冲，避免后续探针误阻塞
        break;
      }
      delay(5);
    }
  }

  uint16_t rtt = controlCheckRtt(udp);
  udp.stop();
  bool alive = (rtt > 0);

  dnsResults[i].state = alive ? FZ_ALIVE : FZ_DEAD;
  dnsResults[i].rtt = rtt;

  if (alive) {
    snprintf(dnsResultLine[i], sizeof(dnsResultLine[i]), "%s: %ums", CASES[i].label, (unsigned)rtt);
  } else {
    snprintf(dnsResultLine[i], sizeof(dnsResultLine[i]), "%s: NO RESP!", CASES[i].label);
  }
  dnsResultColor[i] = alive ? TFT_GREEN : TFT_RED;
  sdAppend(LOG_PATH_DNS, nowStamp() + "  " + dnsResultLine[i]);

  Serial.printf("[dnsfuzz] case #%d '%s' -> %s (%ums)\n",
                i + 1, CASES[i].label, alive ? "SURVIVED" : "DEAD", rtt);

  if (!alive) {
    dnsHalted = true;
    dnsHaltedCaseIdx = i;
    dnsRunning = false;
    Serial.printf("[dnsfuzz] HALTED after '%s' - target stopped responding, manual Enter to continue\n", CASES[i].label);
    sdAppend(LOG_PATH_DNS, nowStamp() + "  *** HALTED after " + CASES[i].label + " ***");
  }
}

static void startDnsRun() {
  dnsCaseIdx = 0;
  dnsHalted = false;
  dnsHaltedCaseIdx = -1;
  dnsRunning = (WiFi.status() == WL_CONNECTED);
  dnsStartPending = dnsRunning;
  Serial.printf("[dnsfuzz] start target=%s:%u, wifi=%s\n",
                DNSMASQ_IP.toString().c_str(), DNSMASQ_PORT,
                dnsRunning ? "connected" : "NOT connected");
  sdAppend(LOG_PATH_DNS, "--- run @ " + nowStamp() + " target=" + DNSMASQ_IP.toString() + " ---");
  for (int i = 0; i < CASE_COUNT; i++) {
    dnsResults[i].state = FZ_WAIT;
    dnsResults[i].rtt = 0;
    dnsResults[i].directReply = false;
    snprintf(dnsResultLine[i], sizeof(dnsResultLine[i]), "%s: ...", CASES[i].label);
    dnsResultColor[i] = TFT_DARKGREY;
  }
}

// =============================================================================
// [3] 模式切换与生命周期管理
// =============================================================================
static void switchMode(NetProbeMode newMode) {
  if (currentMode == newMode) return;
  // 彻底清理当前运行状态，避免不同模式之间的异步残留
  probing = false;
  dnsRunning = false;
  dnsStartPending = false;
  dnsHalted = false;
  dnsHaltedCaseIdx = -1;
  editMode = ED_OFF;
  // auto 只属于 Probe 模式（原 DnsFuzz 没有 auto）：切模式一律关掉，
  // 免得带着 Probe 的 auto 状态切过去，无人值守地每 5 分钟 fuzz 一遍全屋共用的 DNS。
  autoMode = false;

  currentMode = newMode;
  if (currentMode == NETPROBE_MODE_PROBE) {
    startProbeRound();
  } else {
    startDnsRun();
  }
}

void netprobeEnter(NetProbeMode mode) {
  loadVpnCfg();
  loadDnsTarget();
  editMode = ED_OFF;
  autoMode = false;
  currentMode = mode;
  if (currentMode == NETPROBE_MODE_PROBE) {
    startProbeRound();
  } else {
    startDnsRun();
  }
}

void netprobeExit() {
  probing = false;
  dnsRunning = false;
  dnsStartPending = false;
  dnsHalted = false;
  dnsHaltedCaseIdx = -1;
  editMode = ED_OFF;
  autoMode = false;
}

void netprobeUpdate() {
  // 如果进入时 WiFi 尚未连上，等 WiFi 握手成功后自动启动当前模式的首轮巡检
  if (WiFi.status() == WL_CONNECTED) {
    if (currentMode == NETPROBE_MODE_PROBE && !probing && probeResults[0].status == STAT_WAIT) {
      startProbeRound();
      return;
    } else if (currentMode == NETPROBE_MODE_DNS && !dnsRunning && dnsResults[0].state == FZ_WAIT && !dnsHalted) {
      startDnsRun();
      return;
    }
  }

  // Probe 模式单步步进
  if (currentMode == NETPROBE_MODE_PROBE) {
    if (probing) {
      if (probeIdx >= PROBE_COUNT) {
        probing = false;
        if (autoMode) { logProbeRoundToSd(); nextAutoMs = millis() + AUTO_INTERVAL_MS; }
      } else {
        runOneProbe(probeIdx);
        probeIdx++;
      }
      return;
    }
    if (autoMode && (int32_t)(millis() - nextAutoMs) >= 0) {
      startProbeRound();
      return;
    }
  }
  // DNS 模式单步步进
  else {
    if (dnsRunning) {
      if (dnsStartPending) { dnsStartPending = false; return; }
      if (dnsCaseIdx >= CASE_COUNT) {
        dnsRunning = false;
        int aliveCount = 0;
        for (int i = 0; i < CASE_COUNT; i++) if (dnsResults[i].state == FZ_ALIVE) aliveCount++;
        Serial.printf("[dnsfuzz] done: %d/%d alive\n", aliveCount, CASE_COUNT);
        sdAppend(LOG_PATH_DNS, nowStamp() + "  done: " + aliveCount + "/" + CASE_COUNT + " alive");
        return;
      }
      dnsResults[dnsCaseIdx].state = FZ_RUNNING;
      runDnsCase(dnsCaseIdx);
      dnsCaseIdx++;
      return;
    }
  }
}

// =============================================================================
// [4] 按键交互
// =============================================================================
bool netprobeKey(char k) {
  // 编辑模式按键处理
  if (editMode != ED_OFF) {
    if (k == '`') { editMode = ED_OFF; }
    else if (k == '\b') { if (editBuf.length()) editBuf.remove(editBuf.length() - 1); }
    else if (k == '\n') {
      if (currentMode == NETPROBE_MODE_PROBE) {
        if (editMode == ED_HOST) {
          editBuf.trim();
          if (editBuf.length() > 0 && editBuf.length() < sizeof(vpnHostBuf)) {
            snprintf(vpnHostBuf, sizeof(vpnHostBuf), "%s", editBuf.c_str());
            PROBES[vpnProbeIdx()].host = vpnHostBuf;
            editMode = ED_PORT; editBuf = String(PROBES[vpnProbeIdx()].port);
          }
        } else {
          long p = editBuf.toInt();
          if (p > 0 && p <= 65535) PROBES[vpnProbeIdx()].port = (uint16_t)p;
          saveVpnCfg();
          editMode = ED_OFF;
        }
      } else { // NETPROBE_MODE_DNS
        if (editMode == ED_HOST) {
          IPAddress ip;
          if (ip.fromString(editBuf)) {
            DNSMASQ_IP = ip;
            editMode = ED_PORT;
            editBuf = String(DNSMASQ_PORT);
          }
        } else {
          long p = editBuf.toInt();
          if (p > 0 && p <= 65535) DNSMASQ_PORT = (uint16_t)p;
          saveDnsTarget();
          editMode = ED_OFF;
        }
      }
    }
    else if (k >= 0x20 && k <= 0x7e && editBuf.length() < 40) editBuf += k;
    return false;
  }

  // 退出至主菜单
  if (k == '`') return true;

  // 模式切换快捷键: M / Tab
  if (k == 'm' || k == 'M' || k == '\t') {
    switchMode(currentMode == NETPROBE_MODE_PROBE ? NETPROBE_MODE_DNS : NETPROBE_MODE_PROBE);
    return false;
  }

  // 自动巡检开关
  // 只在 Probe 模式有效；DNS 模式下跟其它不认识的键一样直接忽略
  if ((k == 'a' || k == 'A') && currentMode == NETPROBE_MODE_PROBE) {
    autoMode = !autoMode;
    Serial.printf("[netprobe] autoMode=%d (mode=%s)\n", autoMode, currentMode == NETPROBE_MODE_PROBE ? "PROBE" : "DNS");
    if (autoMode) nextAutoMs = millis() + AUTO_INTERVAL_MS;
    return false;
  }

  // Probe 模式特有快捷键
  if (currentMode == NETPROBE_MODE_PROBE) {
    if (k == 'e' || k == 'E') { editMode = ED_HOST; editBuf = PROBES[vpnProbeIdx()].host; return false; }
    if (k == '\n') {
      startProbeRound();
      return false;
    }
  }
  // DNS 模式特有快捷键
  else {
    if (dnsRunning) return false;

    // G 键一键对准网关/默认 DNS
    if (k == 'g' || k == 'G') {
      IPAddress gw = WiFi.dnsIP();
      if (gw == IPAddress((uint32_t)0)) gw = WiFi.gatewayIP();
      if (gw != IPAddress((uint32_t)0)) {
        DNSMASQ_IP = gw;
        DNSMASQ_PORT = 53;
        saveDnsTarget();
        startDnsRun();
        return false;
      }
    }

    if (k == 'e' || k == 'E') { editMode = ED_HOST; editBuf = DNSMASQ_IP.toString(); return false; }
    if (k == '\n') {
      if (dnsHalted) { dnsHalted = false; dnsRunning = true; }
      else           startDnsRun();
      return false;
    }
  }

  return false;
}

// =============================================================================
// [5] 画面渲染
// =============================================================================
void drawNetprobe() {
  cv.fillScreen(TFT_BLACK);

  // 顶栏状态装配
  char rightBuf[32];
  uint16_t rightCol = 0x8410;

  if (currentMode == NETPROBE_MODE_PROBE) {
    if (WiFi.status() != WL_CONNECTED) {
      snprintf(rightBuf, sizeof(rightBuf), "DISCONNECTED");
      rightCol = TFT_RED;
    } else if (probing) {
      snprintf(rightBuf, sizeof(rightBuf), "PROBING %d/%d", probeIdx, PROBE_COUNT);
      rightCol = 0x07FF;
    } else if (autoMode) {
      int32_t left = (int32_t)(nextAutoMs - millis());
      uint32_t remain = left > 0 ? (uint32_t)left / 1000 : 0;
      snprintf(rightBuf, sizeof(rightBuf), "AUTO %us %s", remain, sdReady() ? "[SD]" : "[NO SD]");
      rightCol = TFT_YELLOW;
    } else if (probeResults[0].status == STAT_WAIT) {
      snprintf(rightBuf, sizeof(rightBuf), "READY %s", sdReady() ? "[SD]" : "[NO SD]");
      rightCol = 0x8410;
    } else {
      int passCount = 0;
      for (int i = 0; i < PROBE_COUNT; i++) {
        if (probeResults[i].status == STAT_OK) passCount++;
      }
      snprintf(rightBuf, sizeof(rightBuf), "PASS %d/%d %s", passCount, PROBE_COUNT, sdReady() ? "[SD]" : "[NO SD]");
      rightCol = (passCount == PROBE_COUNT) ? 0x07E0 : (passCount > 0 ? 0xFBE0 : TFT_RED);
    }
    drawPageHeader("NetProbe", rightBuf, rightCol);
  } else { // NETPROBE_MODE_DNS
    if (WiFi.status() != WL_CONNECTED) {
      snprintf(rightBuf, sizeof(rightBuf), "DISCONNECTED");
      rightCol = TFT_RED;
    } else if (dnsRunning) {
      snprintf(rightBuf, sizeof(rightBuf), "FUZZING %d/%d", dnsCaseIdx, CASE_COUNT);
      rightCol = 0x07FF;
    } else if (dnsHalted) {
      snprintf(rightBuf, sizeof(rightBuf), "HALTED #%d", dnsHaltedCaseIdx + 1);
      rightCol = TFT_RED;
    } else if (dnsCaseIdx >= CASE_COUNT) {
      int aliveCount = 0;
      for (int i = 0; i < CASE_COUNT; i++) if (dnsResults[i].state == FZ_ALIVE) aliveCount++;
      snprintf(rightBuf, sizeof(rightBuf), "PASS %d/%d %s", aliveCount, CASE_COUNT, sdReady() ? "[SD]" : "[NO SD]");
      rightCol = (aliveCount == CASE_COUNT) ? 0x07E0 : TFT_RED;
    } else {
      snprintf(rightBuf, sizeof(rightBuf), "READY %s", sdReady() ? "[SD]" : "[NO SD]");
      rightCol = 0x8410;
    }
    drawPageHeader("NetProbe:DNS", rightBuf, rightCol);
  }

  cv.setTextSize(1);

  if (editMode != ED_OFF) {
    if (currentMode == NETPROBE_MODE_PROBE) {
      drawFieldEditor(editMode == ED_HOST ? "vpn-node host:" : "vpn-node port:", editBuf);
    } else {
      drawFieldEditor(editMode == ED_HOST ? "target host:" : "target port:", editBuf);
    }
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    drawNeedWifi(26);
    return;
  }

  // ---------------------------------------------------------------------------
  // [A] Probe 模式画面内容
  // ---------------------------------------------------------------------------
  if (currentMode == NETPROBE_MODE_PROBE) {
    // 1. DNS 完整性与投毒卡片 (y = 15..40, h = 26)
    const int dnsY = 15, dnsH = 26;
    cv.fillRoundRect(4, dnsY, SW - 8, dnsH, 2, 0x0821);
    cv.drawRoundRect(4, dnsY, SW - 8, dnsH, 2, 0x18E3);
    cv.fillRect(4, dnsY, 2, dnsH, 0x05BF);

    auto drawDnsItem = [](int pIdx, const char* name, int y) {
      cv.setTextDatum(top_left);
      cv.setTextColor(0xCE79, 0x0821);
      cv.drawString(name, 10, y);

      const ProbeItemResult& r = probeResults[pIdx];
      if (r.status == STAT_WAIT) {
        cv.setTextColor(0x4208, 0x0821);
        cv.drawString("--", 80, y);
      } else if (r.status == STAT_PROBING) {
        cv.setTextColor(0x07FF, 0x0821);
        cv.drawString("resolving...", 80, y);
      } else {
        cv.setTextColor(r.status == STAT_WARN ? 0xF980 : (r.status == STAT_OK ? TFT_WHITE : 0x7BEF), 0x0821);
        cv.drawString(r.detail, 80, y);
      }

      if (r.status == STAT_OK) {
        const int bw = 32, bh = 9, bx = SW - 10 - bw, by = y - 1;
        cv.fillRoundRect(bx, by, bw, bh, 2, 0x0280);
        cv.drawRoundRect(bx, by, bw, bh, 2, 0x04A0);
        cv.setTextDatum(middle_center);
        cv.setTextColor(0x07E0, 0x0280);
        cv.drawString("OK", bx + bw / 2, by + bh / 2);
      } else if (r.status == STAT_WARN) {
        const int bw = 46, bh = 9, bx = SW - 10 - bw, by = y - 1;
        cv.fillRoundRect(bx, by, bw, bh, 2, 0x4000);
        cv.drawRoundRect(bx, by, bw, bh, 2, 0x8000);
        cv.setTextDatum(middle_center);
        cv.setTextColor(0xF800, 0x4000);
        cv.drawString("POISON", bx + bw / 2, by + bh / 2);
      } else if (r.status == STAT_FAIL) {
        const int bw = 32, bh = 9, bx = SW - 10 - bw, by = y - 1;
        cv.fillRoundRect(bx, by, bw, bh, 2, 0x2104);
        cv.drawRoundRect(bx, by, bw, bh, 2, 0x4208);
        cv.setTextDatum(middle_center);
        cv.setTextColor(0x7BEF, 0x2104);
        cv.drawString("FAIL", bx + bw / 2, by + bh / 2);
      }
    };

    drawDnsItem(0, "google.com", 17);
    drawDnsItem(1, "baidu.com",  28);

    // 2. TCP 端口与出站防火墙穿透矩阵 (y = 43..85, h = 43)
    const int tcpY = 43, tcpH = 43;
    cv.fillRoundRect(4, tcpY, SW - 8, tcpH, 2, 0x0821);
    cv.drawRoundRect(4, tcpY, SW - 8, tcpH, 2, 0x18E3);
    cv.fillRect(4, tcpY, 2, tcpH, 0x355F);

    auto drawTcpItem = [](int pIdx, const char* name, int x, int y, int colW) {
      cv.setTextDatum(top_left);
      cv.setTextColor(0xCE79, 0x0821);
      cv.drawString(name, x, y);

      cv.setTextDatum(top_right);
      const ProbeItemResult& r = probeResults[pIdx];
      if (r.status == STAT_WAIT) {
        cv.setTextColor(0x4208, 0x0821);
        cv.drawString("--", x + colW, y);
      } else if (r.status == STAT_PROBING) {
        cv.setTextColor(0x07FF, 0x0821);
        cv.drawString("...", x + colW, y);
      } else if (r.status == STAT_OK) {
        uint16_t c = (r.latency < 50) ? 0x07E0 : (r.latency < 150 ? 0x87E0 : 0xFBE0);
        cv.setTextColor(c, 0x0821);
        char b[16];
        snprintf(b, sizeof(b), "%ums", r.latency);
        cv.drawString(b, x + colW, y);
      } else {
        cv.setTextColor(0xF980, 0x0821);
        cv.drawString("BLOCKED", x + colW, y);
      }
    };

    drawTcpItem(2, "Goog:443", 10, 45, 104);
    drawTcpItem(5, "CF  :443", 124, 45, 104);

    drawTcpItem(3, "Goog:80 ", 10, 54, 104);
    drawTcpItem(6, "CF  :80 ", 124, 54, 104);

    drawTcpItem(4, "Ali :443", 10, 63, 104);
    char vpnLabel[16];
    snprintf(vpnLabel, sizeof(vpnLabel), "VPN :%u", PROBES[vpnProbeIdx()].port);
    drawTcpItem(7, vpnLabel, 124, 63, 104);

    // 分隔线与实测 VPN 目标配置展示
    cv.drawFastHLine(8, 73, SW - 16, 0x18E3);
    cv.setTextDatum(top_left);
    cv.setTextColor(0x632C, 0x0821);
    cv.drawString("VPN:", 10, 75);
    cv.setTextColor(0x07FF, 0x0821);
    char vpnTargetBuf[32];
    snprintf(vpnTargetBuf, sizeof(vpnTargetBuf), "%s:%u", PROBES[vpnProbeIdx()].host, PROBES[vpnProbeIdx()].port);
    cv.drawString(trunc(vpnTargetBuf, 22), 36, 75);

    cv.setTextDatum(top_right);
    cv.setTextColor(0x8410, 0x0821);
    cv.drawString("E=cfg", SW - 10, 75);

    // 3. Captive Portal 强制门户/HTTP 204 卡片 (y = 88..104, h = 17)
    const int portY = 88, portH = 17;
    cv.fillRoundRect(4, portY, SW - 8, portH, 2, 0x0821);
    cv.drawRoundRect(4, portY, SW - 8, portH, 2, 0x18E3);
    cv.fillRect(4, portY, 2, portH, 0xFBE0);

    cv.setTextDatum(top_left);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("Portal:", 10, 92);

    const ProbeItemResult& pr = probeResults[8];
    if (pr.status == STAT_WAIT) {
      cv.setTextColor(0x4208, 0x0821);
      cv.drawString("gstatic 204", 58, 92);
      cv.setTextDatum(top_right);
      cv.drawString("--", SW - 10, 92);
    } else if (pr.status == STAT_PROBING) {
      cv.setTextColor(0x07FF, 0x0821);
      cv.drawString("checking 204...", 58, 92);
    } else if (pr.status == STAT_OK) {
      cv.setTextColor(0xCE79, 0x0821);
      cv.drawString("gstatic 204", 58, 92);
      const int bw = 82, bh = 13, bx = SW - 8 - bw, by = 90;
      cv.fillRoundRect(bx, by, bw, bh, 2, 0x0280);
      cv.drawRoundRect(bx, by, bw, bh, 2, 0x04A0);
      cv.setTextDatum(middle_center);
      cv.setTextColor(0x07E0, 0x0280);
      cv.drawString("204 NO PORTAL", bx + bw / 2, by + bh / 2);
    } else if (pr.status == STAT_WARN) {
      cv.setTextColor(TFT_YELLOW, 0x0821);
      cv.drawString(trunc(pr.detail, 12), 58, 92);
      const int bw = 92, bh = 13, bx = SW - 8 - bw, by = 90;
      cv.fillRoundRect(bx, by, bw, bh, 2, 0x39E0);
      cv.drawRoundRect(bx, by, bw, bh, 2, 0x8400);
      cv.setTextDatum(middle_center);
      cv.setTextColor(TFT_YELLOW, 0x39E0);
      cv.drawString("PORTAL AUTH REQ", bx + bw / 2, by + bh / 2);
    } else {
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString("connect fail", 58, 92);
      const int bw = 72, bh = 13, bx = SW - 8 - bw, by = 90;
      cv.fillRoundRect(bx, by, bw, bh, 2, 0x3000);
      cv.drawRoundRect(bx, by, bw, bh, 2, 0x6000);
      cv.setTextDatum(middle_center);
      cv.setTextColor(TFT_RED, 0x3000);
      cv.drawString("HTTP FAILED", bx + bw / 2, by + bh / 2);
    }

    // 4. 网络诊断终审裁定 Banner (y = 107..120, h = 14)
    const int vY = 107, vH = 14;
    uint16_t vBg = 0, vBdr = 0, vCol = 0;
    const char* vText = nullptr;
    char vBuf[48];

    if (probing) {
      vBg = 0x0114; vBdr = 0x033F; vCol = 0x07FF;
      snprintf(vBuf, sizeof(vBuf), "RUNNING DIAGNOSTIC [%d/%d] ...", probeIdx, PROBE_COUNT);
      vText = vBuf;
    } else {
      OverallVerdict v = getVerdict();
      switch (v) {
        case VERDICT_IDLE:
          vBg = 0x0821; vBdr = 0x2104; vCol = 0x8410;
          vText = "[*] READY: PRESS ENTER TO RUN DIAGNOSTIC";
          break;
        case VERDICT_UNRESTRICTED:
          vBg = 0x0260; vBdr = 0x05E0; vCol = 0x07E0;
          vText = "[v] ALL PASS: UNRESTRICTED INTERNET";
          break;
        case VERDICT_CAPTIVE_PORTAL:
          vBg = 0x31E0; vBdr = 0x63E0; vCol = 0xFFE0;
          vText = "[!] CAPTIVE PORTAL DETECTED - AUTH REQ";
          break;
        case VERDICT_DNS_POISONED:
          vBg = 0x3800; vBdr = 0x7800; vCol = 0xF980;
          vText = "[!] DNS POISONED (AS32934 / META IP)";
          break;
        case VERDICT_FIREWALL_BLOCK:
          vBg = 0x2804; vBdr = 0x5808; vCol = 0xFA08;
          vText = "[!] FIREWALL FILTERED (TCP 443 DROP)";
          break;
        case VERDICT_WAN_DISCONNECTED:
        default:
          vBg = 0x2000; vBdr = 0x4000; vCol = 0xCE79;
          vText = "[!] WAN OFFLINE / NO ROUTE";
          break;
      }
    }

    cv.fillRoundRect(4, vY, SW - 8, vH, 2, vBg);
    cv.drawRoundRect(4, vY, SW - 8, vH, 2, vBdr);
    cv.setTextDatum(middle_center);
    cv.setTextColor(vCol, vBg);
    cv.drawString(vText, SW / 2, vY + vH / 2);

    // 5. 底部高对比度彩色快捷键 (y = 125..134)
    const int footY = 125;
    cv.setTextDatum(top_left); cv.setTextSize(1);
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 6, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" run", 36, footY);   // " rerun" 画到 x=72 正好顶住后面的 "M"，粘成 "rerunM"

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("M", 72, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" dns", 78, footY);

    cv.setTextColor(autoMode ? TFT_YELLOW : 0x07FF, TFT_BLACK);
    cv.drawString("A", 108, footY);
    cv.setTextColor(autoMode ? TFT_YELLOW : 0x8410, TFT_BLACK);
    cv.drawString(autoMode ? " on" : " auto", 114, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("E", 150, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" vpn", 156, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 192, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" back", 198, footY);
  }
  // ---------------------------------------------------------------------------
  // [B] DNS 模式画面内容
  // ---------------------------------------------------------------------------
  else {
    // 1. Target HUD 卡片 (y = 15..28, h = 14)
    const int tgtY = 15, tgtH = 14;
    cv.fillRoundRect(4, tgtY, SW - 8, tgtH, 2, 0x0821);
    cv.drawRoundRect(4, tgtY, SW - 8, tgtH, 2, 0x18E3);
    cv.fillRect(4, tgtY, 2, tgtH, 0xFBE0);

    cv.setTextDatum(top_left);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("TARGET:", 10, tgtY + 3);
    cv.setTextColor(0x07FF, 0x0821);
    char tgtBuf[32];
    snprintf(tgtBuf, sizeof(tgtBuf), "%s:%u", DNSMASQ_IP.toString().c_str(), DNSMASQ_PORT);
    cv.drawString(tgtBuf, 56, tgtY + 3);

    cv.setTextDatum(top_right);
    cv.setTextColor(0x8410, 0x0821);
    cv.drawString("E=edit  G=gw", SW - 10, tgtY + 3);

    // 2. Fuzz 10大畸形用例测试矩阵 (y = 31..89, h = 59, 2列 x 5行)
    const int matY = 31, matH = 59;
    cv.fillRoundRect(4, matY, SW - 8, matH, 2, 0x0821);
    cv.drawRoundRect(4, matY, SW - 8, matH, 2, 0x18E3);
    cv.fillRect(4, matY, 2, matH, 0x88AED9);

    // 垂直中线
    cv.drawFastVLine(118, matY + 2, matH - 4, 0x18E3);

    auto drawCaseItem = [](int idx, int colX, int colW, int rowY) {
      const FuzzCase& c = CASES[idx];
      const CaseResult& r = dnsResults[idx];

      cv.setTextDatum(top_left);
      cv.setTextColor(0xCE79, 0x0821);
      cv.drawString(c.shortLabel, colX, rowY);

      const int badgeW = 38, badgeH = 9;
      const int badgeX = colX + colW - badgeW;
      const int badgeY = rowY - 1;

      if (r.state == FZ_WAIT) {
        cv.setTextDatum(top_right);
        cv.setTextColor(0x4208, 0x0821);
        cv.drawString("--", colX + colW, rowY);
      } else if (r.state == FZ_RUNNING) {
        cv.setTextDatum(top_right);
        cv.setTextColor(0x07FF, 0x0821);
        cv.drawString("...", colX + colW, rowY);
      } else if (r.state == FZ_ALIVE) {
        cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x0280);
        cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x04A0);
        cv.setTextDatum(middle_center);
        cv.setTextColor(0x07E0, 0x0280);
        char msBuf[12];
        snprintf(msBuf, sizeof(msBuf), "%ums", r.rtt);
        cv.drawString(msBuf, badgeX + badgeW / 2, badgeY + badgeH / 2);
      } else { // FZ_DEAD
        cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x4000);
        cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x8000);
        cv.setTextDatum(middle_center);
        cv.setTextColor(0xF800, 0x4000);
        cv.drawString("DEAD", badgeX + badgeW / 2, badgeY + badgeH / 2);
      }
    };

    // 左列 0..4 (UDP Core)
    for (int i = 0; i < 5; i++) {
      drawCaseItem(i, 10, 104, matY + 4 + i * 11);
    }
    // 右列 5..9 (UDP Ext & TCP)
    for (int i = 5; i < 10; i++) {
      drawCaseItem(i, 124, 104, matY + 4 + (i - 5) * 11);
    }

    // 3. 网络模糊测试终审裁定 Banner (y = 92..107, h = 16)
    const int vY = 92, vH = 16;
    uint16_t vBg = 0, vBdr = 0, vCol = 0;
    const char* vText = nullptr;
    char vBuf[64];

    if (dnsRunning) {
      vBg = 0x0114; vBdr = 0x033F; vCol = 0x07FF;
      snprintf(vBuf, sizeof(vBuf), "FUZZING IN PROGRESS [%d/%d] ...", dnsCaseIdx, CASE_COUNT);
      vText = vBuf;
    } else if (dnsHalted) {
      vBg = 0x3800; vBdr = 0x7800; vCol = 0xF800;
      snprintf(vBuf, sizeof(vBuf), "[!] CRASH AT #%d (%s) - HALTED",
               dnsHaltedCaseIdx + 1, CASES[dnsHaltedCaseIdx].shortLabel);
      vText = vBuf;
    } else if (dnsCaseIdx >= CASE_COUNT) {
      int aliveCount = 0;
      for (int i = 0; i < CASE_COUNT; i++) if (dnsResults[i].state == FZ_ALIVE) aliveCount++;
      if (aliveCount == CASE_COUNT) {
        vBg = 0x0280; vBdr = 0x04A0; vCol = 0x07E0;
        vText = "[v] RESILIENT: ALL 10 CASES PASSED";
      } else {
        vBg = 0x3800; vBdr = 0x7800; vCol = 0xF800;
        snprintf(vBuf, sizeof(vBuf), "[!] VULNERABLE: ONLY %d/%d SURVIVED", aliveCount, CASE_COUNT);
        vText = vBuf;
      }
    } else {
      vBg = 0x0821; vBdr = 0x2104; vCol = 0x9CD3;
      vText = "[*] READY: PRESS ENTER TO START FUZZ AUDIT";
    }

    cv.fillRoundRect(4, vY, SW - 8, vH, 2, vBg);
    cv.drawRoundRect(4, vY, SW - 8, vH, 2, vBdr);
    cv.setTextDatum(middle_center);
    cv.setTextColor(vCol, vBg);
    cv.drawString(vText, SW / 2, vY + vH / 2);

    // 4. 安全防护与 SD 日志指示条 (y = 110..122, h = 13)
    const int safeY = 110, safeH = 13;
    cv.fillRoundRect(4, safeY, SW - 8, safeH, 2, 0x0821);
    cv.drawRoundRect(4, safeY, SW - 8, safeH, 2, 0x18E3);

    cv.setTextDatum(top_left); cv.setTextSize(1);
    cv.setTextColor(0x632C, 0x0821);
    cv.drawString("SAFE:", 8, safeY + 3);
    cv.setTextColor(0x9CD3, 0x0821);
    cv.drawString("auto-halt", 42, safeY + 3);

    cv.setTextDatum(top_right);
    cv.setTextColor(sdReady() ? 0x07E0 : 0x632C, 0x0821);
    cv.drawString(sdReady() ? "LOG: /dnsfuzz.log" : "NO SD", SW - 8, safeY + 3);

    // 5. 底部高对比度彩色快捷键 (y = 125..134)
    const int footY = 125;
    cv.setTextDatum(top_left); cv.setTextSize(1);
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 6, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(dnsHalted ? " cont" : " run", 36, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("M", 72, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" probe", 78, footY);

    // DNS 模式没有 auto（见 switchMode），这里不画 A。
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("E", 120, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" edit", 126, footY);

    // G=一键对准网关/默认 DNS（仅 DNS 模式有效）。原 DnsFuzz 底栏有这条提示，合并后别丢。
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("G", 162, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" gw", 168, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 192, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" back", 198, footY);
  }
}
