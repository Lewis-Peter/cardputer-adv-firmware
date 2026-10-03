// 有上限、能中途放弃的 DNS 预解析。
//
// 为什么不直接让 HTTPClient 自己解析：它走的是框架的 WiFiGenericClass::hostByName()
// （WiFiGeneric.cpp:1566），那里面是两段写死的等待——先等别的查询让位最多 16s，
// 再等应答最多 15s——没有参数能改，也没法中途放弃。一个连着 Wi-Fi 但上游断了的网络
// （最常见的"有信号没网"）每个请求都要白等满 15 秒，而用户按了返回也只能干等。
//
// 做法：自己先向 lwIP 发一次查询（在 tcpip 线程上调 dns_gethostbyname，本工程没开
// TCPIP 核心锁，raw API 只能在那个线程上碰），每 50ms 看一眼是否已应答 / 已超时 /
// 后台拉取被取消。查到之后 lwIP 把结果记在它自己的 DNS 表里，紧接着 HTTPClient 连接时
// 那次 hostByName() 直接命中缓存、当场返回。查不到就不连了，直接报错。
//
// 已知的一个漏洞：TTL=0 的应答 lwIP 回调完就把表项清掉了（dns.c 按 RFC 行为），于是紧接着
// HTTPClient 那次 hostByName() 又得真查一遍，这一趟还是框架那段不可控的等待。本工程用到的
// 接口都在 CDN 后面、TTL 至少几十秒，碰不到；要彻底解决得把 IP 直接交给连接（还要保住 TLS 的
// SNI 和 Host 头），HTTPClient 这一层做不到。
//
// 顺带绕开框架那边的一个隐患：hostByName() 把结果指针指向调用方栈上的局部变量交给回调，
// 超时返回之后应答才到的话，回调会往一块已经不属于它的栈上写。这里的回调只写静态变量，
// 并且按"代号"丢掉过期应答。
#pragma once
#include <stdint.h>
#include <string.h>

enum NetResolveResult : uint8_t {
  NR_OK = 0,        // 已解析（或本来就是 IP 字面量）；结果在 lwIP 的 DNS 缓存里
  NR_FAIL,          // 查不到 / 没联网 / lwIP 拒绝
  NR_TIMEOUT,       // 超过 timeoutMs 没应答
  NR_CANCELLED,     // 后台拉取被用户取消（见 bg_fetch.h）
};

// 默认上限 10s：正常应答都在 1s 以内；lwIP 在首选 DNS 服务器重试约 7s 无果后才换备用服务器，
// 上限再短就会把"首选挂了、备用好的"这种网络误判成不通。这些请求现在大多在后台跑、
// 按返回键随时能放弃，所以上限宽一点不影响操作手感。
static const uint32_t NET_DNS_TIMEOUT_MS = 10000;

NetResolveResult netResolve(const char* host, uint32_t timeoutMs = NET_DNS_TIMEOUT_MS);

// 从 "https://host:port/path" 里抠出 host。抠不出来（空、比 outCap 长、带 user@ 或者是
// [IPv6] 字面量）返回 false——调用方的处理是"不预解析，交给 HTTPClient"。
// 纯函数、放在头文件里：主机端 jsontest 直接测它，不用拉 lwIP 进来。
static inline bool netUrlHost(const char* url, char* out, int outCap) {
  if (!url || outCap < 2) return false;
  const char* p = strstr(url, "://");
  p = p ? p + 3 : url;
  if (*p == '[') return false;          // IPv6 字面量：不用解析，交给 HTTPClient
  int n = 0;
  while (p[n] && p[n] != '/' && p[n] != ':' && p[n] != '?' && p[n] != '#' && p[n] != '@') n++;
  if (p[n] == '@' || p[n] == ':') {
    // user:pass@host 形式：冒号/@ 前面那段不是主机名。本工程没这么用的 URL，
    // 真遇到了就不预解析，交给 HTTPClient 自己处理（它认这种写法）
    const char* at = strchr(p, '@');
    const char* slash = strchr(p, '/');
    if (at && (!slash || at < slash)) return false;
  }
  if (n == 0 || n >= outCap) return false;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

// 给调用方拼错误信息用的短文本
static inline const char* netResolveErrText(NetResolveResult r) {
  switch (r) {
    case NR_OK:        return "";
    case NR_TIMEOUT:   return "dns timeout";
    case NR_CANCELLED: return "cancelled";
    default:           return "dns failed";
  }
}
