#include "net_resolve.h"
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <lwip/tcpip.h>
#include <lwip/priv/tcpip_priv.h>
#include "bg_fetch.h"

// 设计见 net_resolve.h。
//
// 同一时刻只会有一个调用方在等（后台工人或者主线程，两者不会同时拉网络：主线程忙时不跑
// 页面 update），所以状态做成单份静态的；不同调用之间靠"代号"区分，过期的回调直接丢掉。
// 信号量用静态存储：第一次调用可能正好在 CanvasLease 里面，那时候往堆上建一个永不释放的
// 小对象，会落进画布腾出的洞里把画布钉死（globals.h 的 CanvasLease 规矩）。

static StaticSemaphore_t s_semBuf;
static SemaphoreHandle_t s_sem = nullptr;
static volatile uint32_t s_gen = 0;       // 当前这次查询的代号（主调方写，tcpip 线程读）
static volatile uint32_t s_doneGen = 0;   // 回调把它设成完成的那次代号
static volatile bool     s_found = false; // 那次有没有查到

static void dnsFound(const char* /*name*/, const ip_addr_t* ipaddr, void* arg) {
  // tcpip 线程上跑。arg 是发起查询时的代号：跟当前的对不上就是一次早已放弃的查询，丢掉。
  uint32_t gen = (uint32_t)(uintptr_t)arg;
  if (gen != s_gen) return;
  s_found = (ipaddr != nullptr);
  s_doneGen = gen;
  xSemaphoreGive(s_sem);   // FreeRTOS 原语自带内存屏障：等的那边拿到信号量之后一定看得到上面两次写
}

struct DnsStartCall {
  struct tcpip_api_call_data base;   // 必须是第一个成员（tcpip_api_call 按它来传）
  const char* host;
  uint32_t    gen;
  err_t       result;
  bool        immediate;             // 缓存命中 / 本来就是 IP：不用等回调
};

static err_t dnsStartFn(struct tcpip_api_call_data* p) {
  DnsStartCall* c = reinterpret_cast<DnsStartCall*>(p);
  ip_addr_t addr;
  c->result = dns_gethostbyname(c->host, &addr, dnsFound, (void*)(uintptr_t)c->gen);
  c->immediate = (c->result == ERR_OK);
  return ERR_OK;
}

NetResolveResult netResolve(const char* host, uint32_t timeoutMs) {
  if (!host || !host[0]) return NR_FAIL;
  if (bgFetchCancelled()) return NR_CANCELLED;
  if (!s_sem) s_sem = xSemaphoreCreateBinaryStatic(&s_semBuf);
  xSemaphoreTake(s_sem, 0);             // 清掉上一次可能遗留的 give

  DnsStartCall call{};
  call.host = host;
  call.gen  = s_gen + 1;
  s_gen     = call.gen;                  // 先登记代号再发查询：回调可能比 tcpip_api_call 返回还早
  err_t e = tcpip_api_call(dnsStartFn, &call.base);
  if (e != ERR_OK) return NR_FAIL;
  if (call.immediate) return NR_OK;
  if (call.result != ERR_INPROGRESS) return NR_FAIL;   // 没联网、名字非法、查询表满……

  const uint32_t t0 = millis();
  while (true) {
    if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(50)) == pdTRUE && s_doneGen == call.gen) {
      return s_found ? NR_OK : NR_FAIL;
    }
    if (bgFetchCancelled()) { s_gen++; return NR_CANCELLED; }        // 作废这次代号，迟到的应答丢掉
    if (millis() - t0 >= timeoutMs) { s_gen++; return NR_TIMEOUT; }
  }
}
