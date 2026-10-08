#include "../../src/router_types.h"
#include <cassert>
#include <cstdio>

static void add(RouterTypeStats& stats, const char* json) {
  JsonDocument item;
  assert(!deserializeJson(item, json));
  addRouterType(stats, item);
}

int main() {
  RouterTypeStats stats;
  add(stats, R"({"metadata":{"network":"tcp","destinationPort":"443"},"download":4294967296,"upload":100})");
  add(stats, R"({"metadata":{"network":"udp","destinationPort":443},"download":10,"upload":20})");
  add(stats, R"({"metadata":{"network":"tcp","destinationPort":"80"},"download":300})");
  add(stats, R"({"metadata":{"network":"udp","destinationPort":"53"},"upload":50})");
  add(stats, R"({"metadata":{"network":"tcp","destinationPort":22},"upload":60})");
  add(stats, R"({"metadata":{"destinationPort":"443"},"download":70})");
  assert(stats.network[0] == 3 && stats.network[1] == 2 && stats.network[2] == 1);
  assert(stats.ports[0] == 1 && stats.ports[1] == 1 && stats.ports[2] == 1 && stats.ports[3] == 1 && stats.ports[4] == 2);
  assert(stats.bytes[0] == 4294967396ULL && stats.bytes[4] == 130);
  assert(stats.totalBytes == 4294967906ULL);
  RouterTypeStats empty;
  assert(empty.totalBytes == 0 && empty.ports[0] == 0 && empty.network[0] == 0);
  std::puts("PASS: transport/port classification, string/numeric ports, missing metadata, 64-bit byte totals");
}
