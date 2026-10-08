#pragma once
#include <stdint.h>

// 接近告警的判定（纯逻辑，不碰硬件）。
// 每架无人机一个状态：是否已在告警圈内、是否已报过紧急。只在"进入"和"新出现紧急"时报一次，
// 离开时要退到 1.2 倍距离外才重新布防，免得在边界上来回抖动连响。
static const int ALERT_LEVEL_N = 5;
static const uint16_t ALERT_RANGE_M[ALERT_LEVEL_N] = {0, 200, 500, 1000, 2000};   // 0 = 关

enum AlertEvent : uint8_t { ALERT_NONE = 0, ALERT_ENTER = 1, ALERT_EMERGENCY = 2 };

struct AlertState {
  bool inside = false;
  bool emgSeen = false;
};

// distM < 0 表示没有可用距离（自己没定位或目标没位置）。lost 的目标不触发，也会重新布防。
inline uint8_t alertEval(AlertState& s, float distM, int level, bool emergency, bool lost) {
  uint8_t ev = ALERT_NONE;
  if (lost) { s.inside = false; s.emgSeen = false; return ev; }
  if (level > 0 && emergency && !s.emgSeen) ev |= ALERT_EMERGENCY;
  s.emgSeen = emergency;
  if (level <= 0 || distM < 0) { s.inside = false; return ev; }
  const float range = ALERT_RANGE_M[level];
  if (!s.inside && distM <= range) { s.inside = true; ev |= ALERT_ENTER; }
  else if (s.inside && distM > range * 1.2f) s.inside = false;
  return ev;
}
