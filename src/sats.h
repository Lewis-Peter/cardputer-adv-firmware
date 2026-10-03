// 卫星过顶：N2YO 的 /above 接口给出此刻在你头顶一定范围内的卫星，
// 本地把它们的星下点坐标+高度换算成「从你这儿看过去」的方位角/仰角，画成抬头天际图。
// 两个类别切换：Starlink(52) / GPS(20)。需要在 secrets.h 里填 N2YO_API_KEY。
#pragma once
#include "globals.h"

void satsEnter();
void satsExit();
void satsUpdate();     // 停在这页时按间隔自动重拉
void satsKey(char k);
void drawSats();
