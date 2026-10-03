// 月相：纯本地计算（不联网），挂在 Clock app 的第 3 页。
// 基于 Meeus《Astronomical Algorithms》第 49 章真朔望算法（平朔 JDE + 15 项主要周期项），
// 实测 2000–2040 年真朔望时刻最大误差约 1.4 分钟，月相相位偏差 <= 0.02 小时；
// 全年 NEXT FULL / NEXT NEW 在北京时间等各时区下日期 100% 准确（0 错天）。
// 按朔望月周期缓存关键时刻，避免每秒重绘时的 double 三角运算开销。
#pragma once
#include "globals.h"

void drawMoon();
