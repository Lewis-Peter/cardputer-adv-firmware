#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

// 按目标域名聚合当前活跃连接：连接数 + 累计字节 + 出口节点。
// 固定槽位；满了以后只有字节数更大的新域名才能顶掉最小的一个。
struct RouterHostStats {
  static const int N = 24;
  struct Slot { char host[32]; char exit[20]; int count; uint64_t bytes; };
  Slot slots[N];
  int used = 0;
  int total = 0;
  uint64_t totalBytes = 0;
};

inline void addRouterHost(RouterHostStats& s, JsonDocument& item) {
  const char* host = item["metadata"]["host"] | "";
  if (!host[0]) host = item["metadata"]["destinationIP"] | "";
  if (!host[0]) host = "(unknown)";
  const char* exit = item["chains"][0] | "";
  uint64_t bytes = (item["download"] | uint64_t(0)) + (item["upload"] | uint64_t(0));
  ++s.total;
  s.totalBytes += bytes;

  int idx = -1;
  for (int i = 0; i < s.used; ++i)
    if (strncmp(s.slots[i].host, host, sizeof(s.slots[i].host) - 1) == 0) { idx = i; break; }
  if (idx < 0) {
    if (s.used < RouterHostStats::N) {
      idx = s.used++;
    } else {
      int min = 0;
      for (int i = 1; i < s.used; ++i) if (s.slots[i].bytes < s.slots[min].bytes) min = i;
      if (s.slots[min].bytes >= bytes) return; // 总数已计入，只是不进榜
      idx = min;
    }
    RouterHostStats::Slot& n = s.slots[idx];
    strncpy(n.host, host, sizeof(n.host) - 1); n.host[sizeof(n.host) - 1] = 0;
    n.exit[0] = 0; n.count = 0; n.bytes = 0;
  }
  RouterHostStats::Slot& sl = s.slots[idx];
  ++sl.count;
  sl.bytes += bytes;
  if (!sl.exit[0] && exit[0]) { strncpy(sl.exit, exit, sizeof(sl.exit) - 1); sl.exit[sizeof(sl.exit) - 1] = 0; }
}

// 取前 k 名的槽位下标（byBytes: 按字节，否则按连接数；并列时用另一项打破）。返回实际个数。
inline int topRouterHosts(const RouterHostStats& s, bool byBytes, int* out, int k) {
  int n = 0;
  for (int i = 0; i < s.used; ++i) {
    auto better = [&](int a, int b) {
      const auto& x = s.slots[a]; const auto& y = s.slots[b];
      if (byBytes) return x.bytes != y.bytes ? x.bytes > y.bytes : x.count > y.count;
      return x.count != y.count ? x.count > y.count : x.bytes > y.bytes;
    };
    int pos = n < k ? n++ : k;
    if (pos == k) { if (!better(i, out[k - 1])) continue; pos = k - 1; }
    while (pos > 0 && better(i, out[pos - 1])) { out[pos] = out[pos - 1]; --pos; }
    out[pos] = i;
  }
  return n;
}
