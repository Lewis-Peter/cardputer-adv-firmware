// ADS-B 飞机雷达：api.adsb.lol 的开放接口（免 key 免注册），列出你头顶/周边的航班。
// 画法是「抬头天际」：X=相对你的方位角，Y=从你这儿看过去的仰角，大小按距离、颜色按高度。
// 位置来自 geoloc.*（GNSS 优先——有实测坐标时方位角才真的对得上天上那架）。
#pragma once
#include "globals.h"

void adsbEnter();
void adsbUpdate();       // 停在这页时按间隔自动重拉
void adsbKey(char k);
void drawAdsb();
