#include "lanscan.h"
#include <new>
#include <WiFi.h>
#include <esp_wifi.h>
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "ui_common.h"
#include "list_sel.h"
#include "hotspot.h"

// arduino-esp32 内部函数，WiFiSTA.cpp/WiFiAP.cpp 都这么前置声明后直接拿来用，
// 用来从 esp_netif 句柄反查 STA 接口——ARP缓存查询要用到底层 lwIP 的 netif 指针，
// 而 esp_netif 的公开API不直接给这个指针，得绕经 impl_index 转一手。
esp_netif_t* get_esp_interface_netif(esp_interface_t interface);

// ---- 常见厂商 OUI 表（手工整理，非官方IEEE完整库，查不到就是Unknown）----
struct OuiEntry { uint8_t b0, b1, b2; const char* name; };
static const OuiEntry OUI_TABLE[] = {
  {0xAC,0xDE,0x48,"Apple"}, {0x00,0x1E,0xC2,"Apple"}, {0x00,0x1F,0xF3,"Apple"}, {0x28,0x37,0x37,"Apple"},
  {0xF0,0x18,0x98,"Apple"}, {0x3C,0x22,0xFB,"Apple"}, {0x00,0x1C,0xB3,"Apple"}, {0xA4,0x83,0xE7,"Apple"},
  {0xD8,0x96,0x95,"Amazon"}, {0x74,0xC2,0x46,"Amazon"}, {0x40,0xB4,0xCD,"Amazon"}, {0x88,0x4A,0xEA,"Amazon"},
  {0x00,0x1A,0x11,"Google"}, {0xF4,0xF5,0xE8,"Google"}, {0x3C,0x5A,0xB4,"Google"}, {0x18,0xB4,0x30,"Google/Nest"},
  {0xB8,0x27,0xEB,"Raspberry Pi"}, {0xDC,0xA6,0x32,"Raspberry Pi"}, {0xE4,0x5F,0x01,"Raspberry Pi"},
  {0x24,0x0A,0xC4,"Espressif"}, {0x30,0xAE,0xA4,"Espressif"}, {0xA0,0x20,0xA6,"Espressif"}, {0x7C,0x9E,0xBD,"Espressif"}, {0x28,0x84,0x85,"Espressif"},
  {0xB0,0xB2,0x1C,"Xiaomi"}, {0x28,0x6C,0x07,"Xiaomi"}, {0x64,0xB4,0x73,"Xiaomi"},
  {0xE8,0x9F,0x80,"TP-Link"}, {0x50,0xC7,0xBF,"TP-Link"}, {0x14,0xCC,0x20,"TP-Link"},
  {0x00,0x14,0x6C,"Netgear"}, {0x20,0xE5,0x2A,"Netgear"}, {0xA0,0x40,0xA0,"Netgear"},
  {0x00,0x1D,0x7E,"D-Link"}, {0x1C,0x7E,0xE5,"D-Link"},
  {0x00,0x24,0x36,"ASUS"}, {0x1C,0x87,0x2C,"ASUS"}, {0x2C,0x56,0xDC,"ASUS"}, {0xA0,0x36,0xBC,"ASUS"},
  {0x00,0x50,0x56,"VMware"}, {0x08,0x00,0x27,"VirtualBox"},
  {0x3C,0xD9,0x2B,"HP"}, {0x00,0x1F,0x29,"HP"},
  {0x00,0x21,0x9B,"Dell"}, {0xB8,0xCA,0x3A,"Dell"},
  {0x00,0x1E,0x65,"Lenovo"}, {0x54,0xE1,0xAD,"Lenovo"},
  {0x00,0x16,0x6F,"Intel"}, {0x3C,0xA9,0xF4,"Intel"},
  {0xB4,0x2E,0x99,"Sonos"}, {0x94,0x9F,0x3E,"Sonos"},
  {0xEC,0xFA,0xBC,"Philips Hue"}, {0x00,0x17,0x88,"Philips Hue"},
  {0xFC,0xA6,0x67,"Samsung"}, {0x8C,0x77,0x12,"Samsung"}, {0xE8,0x50,0x8B,"Samsung"}, {0x5C,0x0A,0x5B,"Samsung"},
  {0x00,0x26,0x5E,"Huawei"}, {0x48,0x46,0xFB,"Huawei"}, {0x00,0x18,0x82,"Huawei"},
  {0x00,0x50,0xF2,"Microsoft"}, {0x28,0x18,0x78,"Microsoft"},
};
static const int OUI_COUNT = sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]);

static String vendorFor(const uint8_t* mac) {
  // 首字节 bit1 = "本地管理地址"，也就是随机化的私有 MAC（iOS/Android 的"私有 Wi-Fi 地址"
  // 默认就开着）。这种地址按定义不属于任何厂商，查表必然落空——与其报一个没信息量的
  // Unknown，不如直说是随机 MAC，这才解释了"为什么这台没名字"。
  if (mac[0] & 0x02) return "private MAC";
  for (int i = 0; i < OUI_COUNT; i++) {
    if (mac[0] == OUI_TABLE[i].b0 && mac[1] == OUI_TABLE[i].b1 && mac[2] == OUI_TABLE[i].b2) return OUI_TABLE[i].name;
  }
  return "Unknown";
}

// IP <-> "大端概念"32位整数的转换：IPAddress::operator uint32_t() 是按内存字节序原样转的
// （小端CPU上第一个点分段反而落在低位），直接对它做子网加减会加错八位组。改成跟
// netprobe.cpp的CIDR代码一样手动按八位组拼，才能正确算网段/广播地址/逐个host。
static uint32_t ipToBE(const IPAddress& a) { return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3]; }
static IPAddress beToIp(uint32_t v) { return IPAddress((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

struct LanDevice {
  IPAddress ip;
  bool      haveMac;
  uint8_t   mac[6];
  char      name[26];   // 反查到的主机名，空串=没查到
  bool      self;       // 本机那一条
};
static const int LAN_MAX = 64;
static LanDevice* devices = nullptr;
static int deviceCount = 0;
static int selIdx = 0, topIdx = 0;

static uint32_t netBE = 0;
static uint32_t hostOffset = 1, hostLimit = 0;
static bool scanning = false;

// 每次开扫都重新解析一遍，不缓存：STA 关掉再打开(WIFIOFF/WIFI 指令、Sniffer 退出等)会让
// 底层 netif 重建，上一轮拿到的指针可能已经失效——拿着悬空指针去查 ARP 缓存是要炸的。
// 解析本身就是两次查表，一轮扫描做一次的开销可以忽略。
static struct netif* staNetif = nullptr;
static void resolveStaNetif() {
  esp_netif_t* en = get_esp_interface_netif(ESP_IF_WIFI_STA);
  int idx = en ? esp_netif_get_netif_impl_index(en) : -1;
  staNetif = (idx >= 0) ? netif_get_by_index((u8_t)idx) : nullptr;
}


// ---- 存活探测：一个常驻的非阻塞 raw ICMP 套接字，由 loop 任务自己收发 ----
//
// 以前每个 IP 建一次 esp_ping 会话 = 每个 IP 新建/销毁一个 3KB 栈的 FreeRTOS 任务，外加
// 这个新线程自己的 netconn 信号量（LWIP_NETCONN_SEM_PER_THREAD）和一个新 socket。
// 一轮 /24 扫描就是两百多次任务栈进出堆，前后会话还会重叠——这正是当初把最大连续块
// 切碎的元凶（per-socket 锁那一半已经在开机时预建，见 wifi_net.cpp lwipSockLockWarmup）。
// esp_ping 没有公开的"换目标"接口，想复用会话只能去改它私有结构体里的地址，布局一变就写坏内存，
// 还跟 ping 线程抢着读写，所以干脆不用它：进页面开一个 raw 套接字，整轮扫描都用它 sendto()，
// 每帧 MSG_DONTWAIT 把收到的回包捞干净，离开页面 close()。全程没有额外任务，
// socket API 自带 tcpip 线程同步，loop 任务直接调用是安全的。
static int      icmpSock    = -1;
static bool     pingActive  = false;
static bool     pingOk      = false;
static uint32_t pingStartMs = 0;
static uint16_t pingSeq     = 0;
static const uint16_t PING_ID         = 0x4C53;   // "LS"：只认自己发出去的 echo 回包
static const uint32_t PING_TIMEOUT_MS = 250;

// lwIP 的 raw API（etharp_*）不是线程安全的，这个固件没开 LWIP_TCPIP_CORE_LOCKING，
// 从 loop 任务直接碰 ARP 表是在跟 tcpip 线程抢数据。用 tcpip_api_call 把它们送进 tcpip 线程执行：
// 同步调用、消息在栈上、等待用的是本线程已有的 netconn 信号量，不产生堆分配。
struct ArpCall {
  struct tcpip_api_call_data call;   // 必须是第一个成员，tcpip_api_call 按它回传
  ip4_addr_t target;
  bool       request;                 // true=发 ARP 请求，false=查 ARP 缓存
  bool       found;
  uint8_t    mac[6];
};
static err_t arpCallFn(struct tcpip_api_call_data* c) {
  ArpCall* a = (ArpCall*)c;
  if (!staNetif) return ERR_IF;
  if (a->request) return etharp_request(staNetif, &a->target);
  struct eth_addr* ethRet = nullptr;
  const ip4_addr_t* ipRet = nullptr;
  if (etharp_find_addr(staNetif, &a->target, &ethRet, &ipRet) >= 0 && ethRet) {
    memcpy(a->mac, ethRet->addr, 6);
    a->found = true;
  }
  return ERR_OK;
}

static bool lookupMac(uint32_t hostBE, uint8_t* macOut) {
  if (!staNetif) return false;
  ArpCall a = {};
  a.target.addr = (uint32_t)beToIp(hostBE);   // beToIp()转出来的IPAddress字节序跟lwIP的ip4_addr一致
  a.request = false;
  tcpip_api_call(arpCallFn, &a.call);
  if (a.found) memcpy(macOut, a.mac, 6);
  return a.found;
}

// 提前给下一个 IP 发一条 ARP 请求。ESP32 要 ping 一个陌生 IP，得先 ARP 解析出 MAC，
// 而 lwIP 的 ARP 重试节奏是秒级的，250ms 的 ping 超时几乎必然先到期——结果就是明明在线
// 且回 ICMP 的设备被整片漏掉（实测这个网段 .1/.107/.180 都回 ping，却一台都没扫到）。
// 在等上一个 ping 的这 250ms 里把下一个的 ARP 先问掉，轮到它时缓存里已经有了，
// 既不拖慢扫描也不用放大超时。
static void prefetchArp(uint32_t hostBE) {
  if (!staNetif) return;
  ArpCall a = {};
  a.target.addr = (uint32_t)beToIp(hostBE);
  a.request = true;
  tcpip_api_call(arpCallFn, &a.call);
}

static uint16_t inetChecksum(const uint8_t* p, int len) {
  uint32_t sum = 0;
  for (int i = 0; i + 1 < len; i += 2) sum += (p[i] << 8) | p[i + 1];
  if (len & 1) sum += p[len - 1] << 8;
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

static void openIcmpSock() {
  if (icmpSock >= 0) return;
  icmpSock = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (icmpSock < 0) return;
  int fl = lwip_fcntl(icmpSock, F_GETFL, 0);
  lwip_fcntl(icmpSock, F_SETFL, fl | O_NONBLOCK);
}

static void closeIcmpSock() {
  if (icmpSock >= 0) { lwip_close(icmpSock); icmpSock = -1; }
}

// 把已经到了的 ICMP 包全捞出来；是当前目标的 echo reply 就置 pingOk。
// 迟到的（上一个 IP 的）回包按 seq 对不上直接丢，不会被记到下一个 IP 头上。
static void drainIcmp(uint32_t targetBE) {
  if (icmpSock < 0) return;
  uint8_t buf[64];                                  // IP头(≤60)+ICMP头 只看前面，截断无所谓
  for (int guard = 0; guard < 16; guard++) {
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = lwip_recvfrom(icmpSock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&from, &fl);
    if (n <= 0) return;
    if (n < 20) continue;
    int ihl = (buf[0] & 0x0F) * 4;
    if (ihl < 20 || n < ihl + 8) continue;
    const uint8_t* ic = buf + ihl;
    uint16_t id  = (ic[4] << 8) | ic[5];
    uint16_t seq = (ic[6] << 8) | ic[7];
    if (ic[0] == 0 /*echo reply*/ && id == PING_ID && seq == pingSeq &&
        from.sin_addr.s_addr == (uint32_t)beToIp(targetBE))
      pingOk = true;
  }
}

static void startPingFor(uint32_t hostBE) {
  if (hostOffset < hostLimit) prefetchArp(hostBE + 1);
  pingOk = false;
  pingStartMs = millis();
  pingActive = true;                                // 发送失败也照样等超时，ARP 结果仍然算数
  if (icmpSock < 0) return;
  drainIcmp(0);                                     // 清掉上一个目标迟到的回包
  pingSeq++;
  uint8_t pkt[16] = {8, 0, 0, 0,                    // echo request, code 0, checksum 占位
                     (uint8_t)(PING_ID >> 8), (uint8_t)PING_ID,
                     (uint8_t)(pingSeq >> 8), (uint8_t)pingSeq,
                     'c', 'a', 'r', 'd', 'p', 'u', 't', 'r'};
  uint16_t ck = inetChecksum(pkt, sizeof(pkt));
  pkt[2] = ck >> 8; pkt[3] = ck & 0xFF;
  struct sockaddr_in to = {};
  to.sin_len = sizeof(to);
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = (uint32_t)beToIp(hostBE);
  lwip_sendto(icmpSock, pkt, sizeof(pkt), 0, (struct sockaddr*)&to, sizeof(to));
}

// ---- 反向 DNS（PTR）：这一页真正的"设备名"从哪来 ----
//
// 以前这页只有 IP + OUI 厂商猜测，所以满屏 "Unknown"/"?"——那本来就不是名字。
// 家用路由器（dnsmasq/ASUS/OpenWrt 这些）都会给 DHCP 客户端登记反向记录，问它要
// x.y.z.w.in-addr.arpa 的 PTR 就能拿到真名。实测这个网络 8 台在线设备里 7 台有名字：
// RT-AX86U-F738 / Xiaomi-15-Ultra / iPhone / iPad / Mac / Watch / esp32s3-7685E8。
//
// 自己发裸 UDP 包而不是用 Arduino 的 API，是因为 WiFi.hostByName() 只能正查，
// 整个 Arduino 层没有 PTR 接口。查询包很短（约 45 字节），手拼就够了。
// 也不用 WiFiUDP：它的 parsePacket() 每调一次先 malloc(1460) 再 recvfrom，没包就 free——
// 这页每帧都要轮询，等于每帧一次 1.4KB 的堆进出；beginPacket() 还常驻一个 1460B 发送缓冲。
// 直接用非阻塞 socket 收进栈上缓冲，零堆分配。
static int      dnsSock    = -1;
static bool     ptrActive  = false;
static void openDnsSock() {
  if (dnsSock >= 0) return;
  dnsSock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);   // 不 bind：首次 sendto 自动分配本地端口
  if (dnsSock < 0) return;
  int fl = lwip_fcntl(dnsSock, F_GETFL, 0);
  lwip_fcntl(dnsSock, F_SETFL, fl | O_NONBLOCK);
}
static void closeDnsSock() {
  if (dnsSock >= 0) { lwip_close(dnsSock); dnsSock = -1; }
}
static uint32_t ptrStartMs = 0;
static uint16_t ptrId      = 0;
static int      ptrTarget  = -1;                  // 正在等哪个 devices[] 下标的应答
static const uint32_t PTR_TIMEOUT_MS = 500;
static const uint16_t DNS_PORT = 53;

// 把 "x.y.z.w" 拼成 QNAME：w.z.y.x.in-addr.arpa，每段前面一个长度字节，末尾 0
static int buildPtrQuery(uint8_t* buf, int cap, const IPAddress& ip, uint16_t id) {
  if (cap < 64) return 0;
  int n = 0;
  buf[n++] = id >> 8; buf[n++] = id & 0xFF;
  buf[n++] = 0x01; buf[n++] = 0x00;               // 标准查询，要求递归
  buf[n++] = 0; buf[n++] = 1;                     // QDCOUNT=1
  buf[n++] = 0; buf[n++] = 0;                     // ANCOUNT
  buf[n++] = 0; buf[n++] = 0;                     // NSCOUNT
  buf[n++] = 0; buf[n++] = 0;                     // ARCOUNT
  for (int i = 3; i >= 0; i--) {
    char seg[4]; int len = snprintf(seg, sizeof(seg), "%u", ip[i]);
    buf[n++] = (uint8_t)len;
    memcpy(buf + n, seg, len); n += len;
  }
  static const char* SUF[] = {"in-addr", "arpa"};
  for (int i = 0; i < 2; i++) {
    int len = strlen(SUF[i]);
    buf[n++] = (uint8_t)len;
    memcpy(buf + n, SUF[i], len); n += len;
  }
  buf[n++] = 0;                                   // QNAME 结束
  buf[n++] = 0; buf[n++] = 12;                    // QTYPE=PTR
  buf[n++] = 0; buf[n++] = 1;                     // QCLASS=IN
  return n;
}

// 从 pos 处读一个域名到 out。要处理 0xC0 压缩指针——PTR 的应答几乎一定是压缩的。
// 返回读完之后的位置（遇到指针时返回指针字段之后的位置），失败返回 -1。
static int readName(const uint8_t* buf, int len, int pos, char* out, int outCap) {
  int o = 0, jumps = 0, ret = -1;
  out[0] = 0;   // 中途失败返回 -1 时也保证是个空串，调用方不必逐个查返回值
  while (pos >= 0 && pos < len) {
    uint8_t l = buf[pos];
    if (l == 0) { pos++; break; }
    if ((l & 0xC0) == 0xC0) {                     // 压缩指针
      if (pos + 1 >= len) return -1;
      if (ret < 0) ret = pos + 2;
      pos = ((l & 0x3F) << 8) | buf[pos + 1];
      if (++jumps > 8) return -1;                 // 防成环
      continue;
    }
    pos++;
    if (pos + l > len) return -1;
    if (o && o < outCap - 1) out[o++] = '.';
    for (int i = 0; i < l && o < outCap - 1; i++) out[o++] = (char)buf[pos + i];
    pos += l;
  }
  out[o] = 0;
  return ret >= 0 ? ret : pos;
}

static void startPtrFor(int devIdx) {
  ptrActive = false;
  ptrTarget = -1;
  IPAddress dns = WiFi.dnsIP();
  if (dns == IPAddress((uint32_t)0)) return;      // 没有 DNS 服务器，跳过
  uint8_t q[80];
  ptrId = (uint16_t)(millis() & 0xFFFF) ^ 0x5A5A;
  int n = buildPtrQuery(q, sizeof(q), devices[devIdx].ip, ptrId);
  if (n <= 0) return;
  if (dnsSock < 0) return;
  uint8_t junk[16];
  while (lwip_recv(dnsSock, junk, sizeof(junk), MSG_DONTWAIT) > 0) {}   // 丢掉上一轮迟到的应答
  struct sockaddr_in to = {};
  to.sin_len = sizeof(to);
  to.sin_family = AF_INET;
  to.sin_port = htons(DNS_PORT);
  to.sin_addr.s_addr = (uint32_t)dns;
  if (lwip_sendto(dnsSock, q, n, 0, (struct sockaddr*)&to, sizeof(to)) != n) return;
  ptrActive  = true;
  ptrTarget  = devIdx;
  ptrStartMs = millis();
}

// 返回 true = 这一轮结束了（拿到应答或超时），调用方可以继续扫下一个 IP
static bool pollPtr() {
  if (!ptrActive) return true;

  // ⚠️ 收到不是本轮的包（上一轮迟到的应答）时**不能**结束等待——第一版就是这么写的，
  // 结果那台设备的名字被白白丢掉，表现是"有的设备有名字有的没有"，看着像路由器不给记录。
  uint8_t buf[256];
  int n;
  // 一帧里可能攒了好几个包（迟到的旧应答在前），循环捞，别让旧包挡住本轮的应答
  while (dnsSock >= 0 && (n = lwip_recv(dnsSock, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
    if (n >= 12 && ((buf[0] << 8) | buf[1]) == ptrId) {
      int anCount = (buf[6] << 8) | buf[7];
      int pos = 12;
      char tmp[64];
      pos = readName(buf, n, pos, tmp, sizeof(tmp));   // 跳过 question
      if (pos >= 0) {
        pos += 4;                                      // QTYPE + QCLASS
        for (int a = 0; a < anCount && pos > 0 && pos < n; a++) {
          pos = readName(buf, n, pos, tmp, sizeof(tmp));
          if (pos < 0 || pos + 10 > n) break;
          int type = (buf[pos] << 8) | buf[pos + 1];
          int rdlen = (buf[pos + 8] << 8) | buf[pos + 9];
          pos += 10;
          if (type == 12) {                            // PTR
            if (readName(buf, n, pos, tmp, sizeof(tmp)) < 0) tmp[0] = 0;
            // 路由器给的多半是 "iPhone" 或 "iPhone.lan"，域名后缀没意义，砍掉只留主机名
            char* dot = strchr(tmp, '.');
            if (dot) *dot = 0;
            if (tmp[0] && ptrTarget >= 0 && ptrTarget < deviceCount) {
              strncpy(devices[ptrTarget].name, tmp, sizeof(devices[ptrTarget].name) - 1);
              devices[ptrTarget].name[sizeof(devices[ptrTarget].name) - 1] = 0;
            }
            break;
          }
          pos += rdlen;
        }
      }
      ptrActive = false;
      return true;                    // 只有本轮的应答才算结束
    }
  }

  if (millis() - ptrStartMs > PTR_TIMEOUT_MS) { ptrActive = false; return true; }
  return false;
}

static void startScan() {
  if (!devices) {
    devices = new (std::nothrow) LanDevice[LAN_MAX];
    if (!devices) return;
  }
  resolveStaNetif();
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  uint32_t ipBE = ipToBE(ip), maskBE = ipToBE(mask);
  netBE = ipBE & maskBE;
  uint32_t bcastBE = netBE | (~maskBE);
  uint32_t total = (bcastBE > netBE + 1) ? (bcastBE - netBE - 1) : 0;
  const uint32_t HOST_CAP = 256;   // 大网(比如/16)只扫前256个，避免扫几万台耗时太久
  hostLimit = total > HOST_CAP ? HOST_CAP : total;
  hostOffset = 1;
  deviceCount = 0;
  selIdx = 0; topIdx = 0;
  scanning = (hostLimit > 0);
  pingActive = false;
  ptrActive = false; ptrTarget = -1;

  openIcmpSock();
  openDnsSock();

  // 本机单独放进去：ping 自己不一定通，而且 ARP 缓存里永远不会有自己的条目，
  // 所以走正常扫描流程的话自己反而是唯一一台"扫不到"的设备。
  if (deviceCount < LAN_MAX) {
    LanDevice& me = devices[deviceCount];
    me.ip = ip;
    me.self = true;
    me.name[0] = 0;
    uint8_t m[6];
    WiFi.macAddress(m);
    memcpy(me.mac, m, 6);
    me.haveMac = true;
    const char* hn = WiFi.getHostname();
    if (hn && hn[0]) { strncpy(me.name, hn, sizeof(me.name) - 1); me.name[sizeof(me.name) - 1] = 0; }
    deviceCount++;
  }
}

void lanscanEnter() {
  if (!devices) {
    devices = new (std::nothrow) LanDevice[LAN_MAX];
  }
  if (WiFi.status() == WL_CONNECTED) startScan();
  else { scanning = false; deviceCount = 0; }
}

void lanscanExit() {
  scanning = false;
  pingActive = false;
  ptrActive = false;
  ptrTarget = -1;

  closeIcmpSock();
  closeDnsSock();

  // LAN 扫描只 ping 已连上的网段，不需要独占射频，热点开着就让它继续开着
  if (!hotspotRestoreAfterScan() && WiFi.status() != WL_CONNECTED) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }

  if (devices) {
    delete[] devices;
    devices = nullptr;
    deviceCount = 0;
  }
}

void lanscanUpdate() {
  // 进这一页时 Wi-Fi 还没连上（比如刚开机就进来）的话，enter 那会儿什么都没做。
  // 等连上了自动补跑一次，否则页面会一直停在 "found 0 devices" 干等着，
  // 而且屏上还看不出是在等网——得手动按 Enter 才动，很像坏了。
  if (!scanning && deviceCount == 0 && WiFi.status() == WL_CONNECTED) { startScan(); return; }

  if (!scanning) return;

  // 正在等某台设备的 PTR 应答：查完（或超时）之前不往下扫
  if (ptrActive) {
    if (!pollPtr()) return;
    // 白嫖这段等待：刚才记录时 ARP 缓存里没有 MAC 的，那会儿已经补发过一条 ARP 请求，
    // 等 PTR 的这几百毫秒足够它回来了，这里再查一次就能把 "?" 变成厂商名。
    // （lwIP 的 ARP 表默认只有 10 条，一路预热下来早期的条目会被挤掉，所以得补这一手。）
    if (ptrTarget >= 0 && ptrTarget < deviceCount && !devices[ptrTarget].haveMac)
      devices[ptrTarget].haveMac = lookupMac(ipToBE(devices[ptrTarget].ip), devices[ptrTarget].mac);
    hostOffset++;
    return;
  }

  if (!pingActive) {
    // 列表满了就收工：再往下 ping 几百个 IP 也没地方存，白白多花几分钟
    if (hostOffset > hostLimit || deviceCount >= LAN_MAX) { scanning = false; return; }
    startPingFor(netBE + hostOffset);
    return;
  }

  uint32_t hostBE = netBE + hostOffset;
  drainIcmp(hostBE);
  if (!pingOk && millis() - pingStartMs <= PING_TIMEOUT_MS) return;   // 还在等这一个

  pingActive = false;
  uint8_t  mac[6];
  bool     haveMac = lookupMac(hostBE, mac);
  // 回了 ARP 就算在线，不必非要回 ICMP：手机/手表这类设备普遍带防火墙不回 ping，
  // 但 ARP 是链路层的，同网段内没法不回（实测 .32/.54/.206 就是这种）。
  if ((pingOk || haveMac) && deviceCount < LAN_MAX) {
    IPAddress found = beToIp(hostBE);
    if (found == devices[0].ip && devices[0].self) { hostOffset++; return; }   // 自己已经在列表里了
    LanDevice& d = devices[deviceCount];
    d.ip = found;
    d.haveMac = haveMac;
    if (haveMac) memcpy(d.mac, mac, 6);
    d.name[0] = 0;
    d.self = false;
    deviceCount++;
    if (!haveMac) prefetchArp(hostBE);  // 补问一次，等 PTR 应答那会儿正好能回来
    startPtrFor(deviceCount - 1);       // 只对找到的设备反查，不是每个 ping 都查
    if (ptrActive) return;             // 下一帧起由上面那个分支收尾
  }
  hostOffset++;
}

void lanscanKey(char k) {
  if (k == '\n') {
    if (!scanning && WiFi.status() == WL_CONNECTED) startScan();
  } else if ((k == ';' || k == ',') && deviceCount > 0) {
    listMoveIndex(selIdx, deviceCount, -1);
  } else if ((k == '.' || k == '/') && deviceCount > 0) {
    listMoveIndex(selIdx, deviceCount, +1);
  }
}

void drawLanscan() {
  cv.fillScreen(TFT_BLACK);

  if (WiFi.status() != WL_CONNECTED) {
    drawPageHeader("LAN Scanner", "OFFLINE", 0xFBE0);
    drawNeedWifi();
    return;
  }

  // 1. 顶栏状态与扫描进度线
  char stBuf[32];
  if (scanning) {
    int pct = hostLimit > 0 ? (int)((float)hostOffset / hostLimit * 100) : 0;
    snprintf(stBuf, sizeof(stBuf), "SCAN %d%% (%lu/%lu)", pct, (unsigned long)hostOffset, (unsigned long)hostLimit);
    drawPageHeader("LAN Scanner", stBuf, TFT_YELLOW);

    // 扫描动态进度线
    int pW = hostLimit > 0 ? (int)((float)hostOffset / hostLimit * SW) : 0;
    if (pW > 0) cv.drawFastHLine(0, 13, pW, 0x07FF);
  } else {
    snprintf(stBuf, sizeof(stBuf), "%d HOST%s", deviceCount, deviceCount == 1 ? "" : "S");
    drawPageHeader("LAN Scanner", stBuf, deviceCount > 0 ? ACCENT : 0x9CD3);
  }

  // 2. 空状态处理
  if (deviceCount == 0 || !devices) {
    const int cx = SW / 2;
    cv.fillRoundRect(16, 36, 208, 56, 4, 0x0821);
    cv.drawRoundRect(16, 36, 208, 56, 4, scanning ? 0x2492 : 0x18C3);

    cv.setTextDatum(middle_center);
    cv.setTextSize(1);
    if (scanning) {
      cv.setTextColor(0x07FF, 0x0821);
      cv.drawString("SCANNING LOCAL NETWORK...", cx, 54);
      char scanSub[48];
      snprintf(scanSub, sizeof(scanSub), "Probing ARP/ICMP: %lu/%lu", (unsigned long)hostOffset, (unsigned long)hostLimit);
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString(scanSub, cx, 72);
    } else {
      cv.setTextColor(0xFBE0, 0x0821);
      cv.drawString("NO DEVICES DETECTED", cx, 54);
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString("Press Enter to initiate scan", cx, 72);
    }

    // 底部热键
    cv.setTextDatum(top_left);
    cv.setTextSize(1);
    const int footY = SH - 12;
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 8, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" scan", 38, footY);
    if (hotspotIsSuspended()) {
      cv.setFont(&fonts::efontCN_12); cv.setTextColor(TFT_YELLOW, TFT_BLACK);
      cv.drawString("热点已暂停", 90, footY); cv.setFont(&fonts::Font0);
    }
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 198, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" back", 204, footY);
    return;
  }

  // 3. 设备列表 (VIS = 4)
  const int VIS = 4;
  listClampScroll(selIdx, topIdx, deviceCount, VIS);

  IPAddress gw = WiFi.gatewayIP();
  const int listStartY = 17;
  const int rowH = 18;

  for (int p = 0; p < VIS; p++) {
    int i = topIdx + p;
    if (i >= deviceCount) break;
    LanDevice& d = devices[i];
    bool sel = (i == selIdx);
    int y = listStartY + p * (rowH + 1);

    uint16_t bg  = sel ? 0x1124 : 0x0821;
    uint16_t bdr = sel ? 0x07FF : 0x18C3;

    cv.fillRoundRect(6, y, SW - 16, rowH, 2, bg);
    cv.drawRoundRect(6, y, SW - 16, rowH, 2, bdr);

    cv.setTextSize(1);
    cv.setTextDatum(middle_left);

    // IP 地址
    cv.setTextColor(sel ? TFT_WHITE : 0xCE79, bg);
    cv.drawString(d.ip.toString(), 12, y + rowH / 2);

    // 主机名或厂商
    String label = d.name[0] ? String(d.name)
                 : d.haveMac ? vendorFor(d.mac)
                             : String("?");
    cv.setTextColor(sel ? 0x07FF : 0x9CD3, bg);
    cv.drawString(trunc(label, 11), 108, y + rowH / 2);

    // 角色标签胶囊
    cv.setTextDatum(middle_right);
    if (d.self) {
      cv.setTextColor(ACCENT, bg);
      cv.drawString("[ME]", SW - 20, y + rowH / 2);
    } else if (d.ip == gw) {
      cv.setTextColor(0xFBE0, bg);
      cv.drawString("[GW]", SW - 20, y + rowH / 2);
    } else {
      cv.setTextColor(0x632C, bg);
      cv.drawString("[HOST]", SW - 20, y + rowH / 2);
    }
  }

  // 滚动条
  drawScrollBar(SW - 7, listStartY, VIS * (rowH + 1) - 1, topIdx, deviceCount, VIS, ACCENT, 0x1082);

  // 4. 底部选定设备检视卡 (Inspector Card)
  if (selIdx >= 0 && selIdx < deviceCount) {
    LanDevice& d = devices[selIdx];
    const int insY = 94, insH = 27;
    cv.fillRoundRect(6, insY, SW - 12, insH, 3, 0x0821);
    cv.drawRoundRect(6, insY, SW - 12, insH, 3, 0x2492);

    cv.setTextSize(1);
    // 第一行：MAC 地址 与 主机名
    cv.setTextDatum(top_left);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("MAC", 12, insY + 4);

    char macStr[24];
    if (d.haveMac) {
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
      cv.setTextColor(TFT_WHITE, 0x0821);
    } else {
      snprintf(macStr, sizeof(macStr), "Unknown (ARP missing)");
      cv.setTextColor(0x632C, 0x0821);
    }
    cv.drawString(macStr, 36, insY + 4);

    cv.setTextDatum(top_right);
    cv.setTextColor(0x07FF, 0x0821);
    cv.drawString(trunc(d.name[0] ? d.name : "No Hostname", 14), SW - 12, insY + 4);

    // 第二行：厂商 与 角色
    cv.setTextDatum(top_left);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("VND", 12, insY + 15);

    String vnd = d.haveMac ? vendorFor(d.mac) : "Unknown";
    cv.setTextColor(ACCENT, 0x0821);
    cv.drawString(trunc(vnd, 16), 36, insY + 15);

    cv.setTextDatum(top_right);
    if (d.self) {
      cv.setTextColor(ACCENT, 0x0821);
      cv.drawString("THIS CARDPUTER", SW - 12, insY + 15);
    } else if (d.ip == gw) {
      cv.setTextColor(0xFBE0, 0x0821);
      cv.drawString("DEFAULT GATEWAY", SW - 12, insY + 15);
    } else {
      cv.setTextColor(0x632C, 0x0821);
      cv.drawString("ACTIVE NODE", SW - 12, insY + 15);
    }
  }

  // 5. 底部快捷键
  cv.setTextDatum(top_left);
  cv.setTextSize(1);
  const int footY = SH - 11;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 8, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" rescan", 38, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString(";.", 86, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" sel", 98, footY);

  if (hotspotIsSuspended()) {
    cv.setFont(&fonts::efontCN_12); cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("热点已暂停", 130, footY); cv.setFont(&fonts::Font0);
  }

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 198, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" back", 204, footY);
}
