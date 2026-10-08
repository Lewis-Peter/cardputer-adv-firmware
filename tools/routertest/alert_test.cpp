#include "../../src/rid_alert.h"
#include <cassert>
#include <cstdio>

int main() {
  AlertState s;
  // 关闭时什么都不报，包括紧急
  assert(alertEval(s, 50, 0, true, false) == ALERT_NONE);
  // 圈外不报；进入 500m 圈报一次；圈内再评估不重复
  assert(alertEval(s, 800, 2, false, false) == ALERT_NONE);
  assert(alertEval(s, 450, 2, false, false) == ALERT_ENTER);
  assert(alertEval(s, 300, 2, false, false) == ALERT_NONE);
  // 迟滞：略出圈（<600m）仍算圈内，回来不重报；出到 1.2 倍外才重新布防
  assert(alertEval(s, 550, 2, false, false) == ALERT_NONE);
  assert(alertEval(s, 480, 2, false, false) == ALERT_NONE);
  assert(alertEval(s, 650, 2, false, false) == ALERT_NONE);
  assert(alertEval(s, 480, 2, false, false) == ALERT_ENTER);
  // 紧急只在新出现时报一次，恢复后再出现再报
  AlertState e;
  assert(alertEval(e, -1, 1, true, false) == ALERT_EMERGENCY);
  assert(alertEval(e, -1, 1, true, false) == ALERT_NONE);
  assert(alertEval(e, -1, 1, false, false) == ALERT_NONE);
  assert(alertEval(e, -1, 1, true, false) == ALERT_EMERGENCY);
  // 进入和紧急同时发生：两个标志都有
  AlertState b;
  assert(alertEval(b, 100, 1, true, false) == (ALERT_ENTER | ALERT_EMERGENCY));
  // lost：不触发，且重新布防
  AlertState l;
  assert(alertEval(l, 100, 1, false, false) == ALERT_ENTER);
  assert(alertEval(l, 100, 1, true, true) == ALERT_NONE);
  assert(alertEval(l, 100, 1, false, false) == ALERT_ENTER);
  // 没距离（无定位）不报进入，也会清掉 inside
  AlertState n;
  assert(alertEval(n, 100, 1, false, false) == ALERT_ENTER);
  assert(alertEval(n, -1, 1, false, false) == ALERT_NONE);
  assert(alertEval(n, 100, 1, false, false) == ALERT_ENTER);
  // 档位表
  assert(ALERT_RANGE_M[0] == 0 && ALERT_RANGE_M[ALERT_LEVEL_N - 1] == 2000);
  std::puts("PASS: enter/emergency events, hysteresis, lost re-arm, no-fix, disabled");
}
