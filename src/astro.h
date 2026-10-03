// Astro：给时间和位置就能算出来的天文页，**完全不联网**。
//
// 这三页原本散在两个 app 里：月相和晨昏线是 Clock 的第 2、3 页，日照是 Weather 的第 3 页。
// 共同点是纯算——月相和晨昏线连一个字节网络都不要（晨昏线甚至不需要知道设备在哪，
// 形状只取决于太阳直射点），日照只要一对经纬度。跟 Weather/Planes/Sats/Typhoon 那批
// "拉了才有数据"的页面是两码事，所以单独成 app。
//
// ⚠️ 拆出来顺带修掉一个 bug：日照页原来在 drawWeatherSun() 里，开头是
//    `if (drawEmptyState()) return;`——天气拉不到就不画那条曲线，可曲线值全是本地算的。
//    而且底部 RISE/SET 原来直接用接口返回的字符串，现在改成自己算（见 .cpp 的 SUN_H0）。
#pragma once
#include "globals.h"

void astroEnter();        // 进 app 时登记一次取位置（延后到界面画出来再做，见 net_job.h）
void astroUpdate();       // loop() 里调
void astroKey(char k);    // r / Enter 重新取位置

void drawAstroSun();      // 日照：太阳高度角曲线 + 日出/正午/日落/昼长
void drawAstroTerm();     // 晨昏线：世界地图上的昼夜分界
// 月相那一页在 moon.cpp（drawMoon），它本来就是独立文件，没必要搬
