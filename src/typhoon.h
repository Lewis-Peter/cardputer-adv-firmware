// 台风预警 + 路径。数据来自日本气象厅(JMA)的防灾 JSON：
//   /bosai/typhoon/data/targetTc.json          当前活跃的热带气旋编号表（~100B）
//   /bosai/typhoon/data/{id}/specifications.json 实况：强度/中心气压/最大风速/移速
//   /bosai/typhoon/data/{id}/forecast.json       历史路径点 + 官方预报路径点
//
// ⚠️ 覆盖范围只有**西北太平洋**（也就是中日韩这一带说的"台风"）。大西洋飓风、
// 东太平洋和印度洋的气旋 JMA 不发布，这一页会显示"no active typhoon"。
//
// 为什么不用中央气象台(NMC)的接口：它那份 view_{id} 是 21KB，每个路径点还挂着一整组
// 预报集合，实测整包进 ArduinoJson 要 61KB 堆——这块板子没有 PSRAM、主画布已经占了 64KB。
// 而且它的 JSON 是异构的定位数组（[id,时间,经度,纬度,...]），ArduinoJson 的数组过滤器
// 只能把同一个子过滤器套到所有元素上，没法按下标把那些预报集合丢掉。
// JMA 这份顶层是同构对象数组，过滤后峰值只有 4.5KB。
#pragma once
#include "globals.h"

void typhoonEnter();
void typhoonUpdate();
void typhoonKey(char k);
void drawTyphoon();        // 第1页：实况通报 + 对本机位置的影响
void drawTyphoonTrack();   // 第2页：路径图（历史 + 官方预报）
