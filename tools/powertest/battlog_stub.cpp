// 测试台里的空日志。真实现（src/battlog.cpp）要 SD / WiFi / 时间戳，那些在 Mac/Linux 上
// 没有；而这个测试台要验的是充电判定和电量推算的**纯逻辑**，跟日志无关。
// 见 src/battlog.h 顶上那段：把板级依赖挡在 power_util.cpp 之外，正是为了这个测试台。
#include "battlog.h"
void battLogSet(bool) {}
bool battLogEnabled() { return false; }
void battLogTick(uint32_t, int, float, int, bool, int) {}
