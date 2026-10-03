// 天气：Open-Meteo 免费接口（不用 API key），六页——
//   P1 实况 / P2 未来24小时(温度曲线+降水概率) / P3 日照(一整天的太阳高度角曲线)
//   P4 风与空气(罗盘+蒲福等级+UV/气压/降水) / P5 空气质量(AQI/PM2.5/PM10/O3) / P6 五天预报
// 定位走 geoloc.*（GNSS 优先，退回 IP 定位）。
// 空气质量走的是 air-quality-api.open-meteo.com 这个独立端点，同样免 key、同样支持明文 HTTP。
#pragma once
#include "globals.h"

extern bool weatherImperial;
void weatherUnitsInit();
void weatherUnitsSet(bool imperial);

void weatherEnter();      // 进来时没数据（或数据太旧）就登记一次待拉（不在这儿真拉，见 net_job.h）
void weatherUpdate();     // loop() 里调：loading 那一帧推上屏之后才真正开工
void weatherKey(char k);
void drawWeather();       // P1 实况
void drawWeatherHour();   // P2 未来 24 小时
void drawWeatherAir();    // P4 风与空气
void drawWeatherAqi();    // P5 空气质量
void drawWeatherFc();     // P6 五天预报
