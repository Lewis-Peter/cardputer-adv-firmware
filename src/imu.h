// IMU（BMI270 六轴：加速度计+陀螺仪，无磁力计）。
// 照搬 M5Stack 官方 Cardputer-Adv demo 的 app_imu 思路：数字读数 + 水平仪气泡
// + 陀螺仪 Z 轴积分驱动的旋转表盘。
#pragma once
#include "globals.h"

void imuSample();
void imuTare();
void drawCompass();
void drawImuDetail();

// PC 主导模式：IMU 串口流式输出 (IMU ON [hz] / IMU OFF)
void imuStreamSet(bool on, int hz = 50);
void imuStreamTick();
bool imuStreamIsActive();
int  imuStreamGetHz();
