// 麦克风频谱分析器（玩具向）。ES8311 codec 跟 M5.Speaker 共用同一路 I2S，
// 进这个屏幕时会临时关掉 Speaker、开 Mic；退出时反过来换回去。
// FFT 核心搬自 M5Unified 官方自带的 examples/Advanced/Mic_FFT 示例
// （去掉了瀑布图/波形图/主频读数那几个面板，只留频谱条，配这块小屏更合适）。
#pragma once
#include "globals.h"

void spectrumEnter();    // 进屏幕时调用：懒加载 FFT 表 + 切到 Mic
void spectrumExit();     // 离开时调用：关 Mic、换回 Speaker
void spectrumUpdate();   // 每帧调用：录一段音频、做 FFT、更新柱状图
void spectrumKey(char k);   // W 切换瀑布图/柱状图，L 切换 LED 律动，S 切换串口流
void drawSpectrum();
