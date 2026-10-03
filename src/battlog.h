// 电池曲线日志：每隔一段时间把电压写到 SD，用来解决"拔掉之后测不了"这个死结。
//
// ⚠️ 单独成一个翻译单元是有意的。它要用 SD / WiFi / 时间戳，而 power_util.cpp 是
// **被独立测试的纯逻辑**（tools/powertest 用合成的电压序列跑充电判定和电量推算）。
// 把这堆板级依赖塞进 power_util.cpp 会让那个测试台编不过——2026-08-26 就这么发生了一次，
// 而这已经是当天第三次"改了 src 但测试台的桩没跟上"。所以从源头上分开：
// powertest 只需要提供一份空实现（tools/powertest/battlog_stub.cpp）。
#pragma once
#include <cstdint>

void battLogSet(bool on);        // 串口 BATTLOG ON/OFF；开关存 NVS，重启保持
bool battLogEnabled();
// 每次采样后调。参数都由 power_util 传进来，这边不反过来问它要状态。
void battLogTick(uint32_t now, int mv, float ema, int level, bool charging, int spread);
