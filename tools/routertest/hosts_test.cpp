#include "../../src/router_hosts.h"
#include <cassert>
#include <cstdio>
#include <string>

static void add(RouterHostStats& s, const char* json) {
  JsonDocument item;
  assert(!deserializeJson(item, json));
  addRouterHost(s, item);
}

int main() {
  RouterHostStats s;
  add(s, R"({"metadata":{"host":"a.com"},"download":100,"upload":50,"chains":["NodeA","G"]})");
  add(s, R"({"metadata":{"host":"a.com"},"download":10,"chains":["NodeB"]})");
  add(s, R"({"metadata":{"host":"","destinationIP":"1.2.3.4"},"download":5000})");
  add(s, R"({"metadata":{}})");
  assert(s.used == 3 && s.total == 4 && s.totalBytes == 5160);
  assert(s.slots[0].count == 2 && s.slots[0].bytes == 160 && !strcmp(s.slots[0].exit, "NodeA"));
  int top[5];
  int n = topRouterHosts(s, true, top, 5);
  assert(n == 3 && !strcmp(s.slots[top[0]].host, "1.2.3.4") && !strcmp(s.slots[top[1]].host, "a.com"));
  n = topRouterHosts(s, false, top, 5);
  assert(!strcmp(s.slots[top[0]].host, "a.com"));
  // 长域名截断且同一域名仍可匹配
  RouterHostStats l;
  std::string longHost(60, 'x');
  std::string j = "{\"metadata\":{\"host\":\"" + longHost + "\"},\"download\":1}";
  add(l, j.c_str()); add(l, j.c_str());
  assert(l.used == 1 && l.slots[0].count == 2 && strlen(l.slots[0].host) == 31);
  // 满槽：大的顶掉最小的，小的不进榜，总数仍计
  RouterHostStats f;
  for (int i = 0; i < RouterHostStats::N; ++i) {
    std::string h = "{\"metadata\":{\"host\":\"h" + std::to_string(i) + "\"},\"download\":" + std::to_string(100 + i) + "}";
    add(f, h.c_str());
  }
  add(f, R"({"metadata":{"host":"small"},"download":1})");
  assert(f.used == RouterHostStats::N && f.total == RouterHostStats::N + 1);
  add(f, R"({"metadata":{"host":"big"},"download":99999})");
  n = topRouterHosts(f, true, top, 5);
  assert(!strcmp(f.slots[top[0]].host, "big"));
  bool h0 = false; for (int i = 0; i < f.used; ++i) if (!strcmp(f.slots[i].host, "h0")) h0 = true;
  assert(!h0);
  // 空榜
  RouterHostStats e; assert(topRouterHosts(e, true, top, 5) == 0);
  std::puts("PASS: host aggregation, fallback to IP, truncation, eviction, top-k ordering");
}
