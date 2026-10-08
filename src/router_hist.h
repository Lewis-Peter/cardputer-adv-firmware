#pragma once
#include <stdint.h>

// 定长环形历史：同时记内存(KB)和连接数两条曲线，每次 push 一对采样。
struct RouterHistory {
  static const int N = 120;
  int32_t mem[N];
  int32_t conns[N];
  int head = 0, count = 0;

  void clear() { head = 0; count = 0; }
  void push(int32_t memKb, int32_t c) {
    mem[head] = memKb; conns[head] = c;
    head = (head + 1) % N;
    if (count < N) ++count;
  }
  // i = 0 为最旧，count-1 为最新
  int idx(int i) const { return (head - count + i + N) % N; }
  int32_t memAt(int i) const { return mem[idx(i)]; }
  int32_t connsAt(int i) const { return conns[idx(i)]; }
};

inline void histRange(const RouterHistory& h, const int32_t* arr, int32_t& lo, int32_t& hi) {
  lo = hi = 0;
  for (int i = 0; i < h.count; ++i) {
    int32_t v = arr[h.idx(i)];
    if (i == 0 || v < lo) lo = v;
    if (i == 0 || v > hi) hi = v;
  }
}
