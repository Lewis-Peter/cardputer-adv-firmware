#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct RouterTypeStats {
  int network[3] = {}; // TCP / UDP / unknown
  int ports[5] = {};   // TCP:443 / UDP:443 / :80 / :53 / other
  uint64_t bytes[5] = {}; // cumulative bytes of currently active connections
  uint64_t totalBytes = 0;
};
inline void addRouterType(RouterTypeStats& stats, JsonDocument& item) {
  const char* network = item["metadata"]["network"] | "";
  int net = strcmp(network, "tcp") == 0 ? 0 : strcmp(network, "udp") == 0 ? 1 : 2;
  // mihomo serializes ports as strings; tolerate numeric API variants too.
  JsonVariant portValue = item["metadata"]["destinationPort"];
  int port = portValue.is<const char*>() ? atoi(portValue.as<const char*>()) : portValue.as<int>();
  int category = port == 443 && net == 0 ? 0 : port == 443 && net == 1 ? 1 :
                 port == 80 ? 2 : port == 53 ? 3 : 4;
  uint64_t bytes = (item["download"] | uint64_t(0)) + (item["upload"] | uint64_t(0));
  ++stats.network[net];
  ++stats.ports[category];
  stats.bytes[category] += bytes;
  stats.totalBytes += bytes;
}

