#include "../../src/router_hist.h"
#include <cassert>
#include <cstdio>

int main() {
  RouterHistory h;
  int32_t lo, hi;
  histRange(h, h.mem, lo, hi);
  assert(h.count == 0 && lo == 0 && hi == 0);
  h.push(100, 5); h.push(80, 9); h.push(120, 2);
  assert(h.count == 3 && h.memAt(0) == 100 && h.memAt(2) == 120 && h.connsAt(1) == 9);
  histRange(h, h.mem, lo, hi); assert(lo == 80 && hi == 120);
  histRange(h, h.conns, lo, hi); assert(lo == 2 && hi == 9);
  // 回绕：推 N+10 个，只剩最近 N 个，顺序正确
  RouterHistory w;
  for (int i = 0; i < RouterHistory::N + 10; ++i) w.push(i, i * 2);
  assert(w.count == RouterHistory::N && w.memAt(0) == 10 && w.memAt(RouterHistory::N - 1) == RouterHistory::N + 9);
  assert(w.connsAt(0) == 20);
  histRange(w, w.mem, lo, hi); assert(lo == 10 && hi == RouterHistory::N + 9);
  w.clear(); assert(w.count == 0);
  w.push(7, 7); assert(w.memAt(0) == 7);
  std::puts("PASS: ring order, wraparound, min/max, clear");
}
