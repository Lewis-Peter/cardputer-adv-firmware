// 秒表 / 倒计时（同一个 app 里两种模式，m 键切换）。
// 计时基准是 millis()，跟屏幕状态无关——熄屏期间照样在走，倒计时到点会把屏幕叫醒并响铃。
#pragma once
#include "globals.h"

void stopwatchEnter();
void stopwatchExit();     // 离开时停掉还在响的铃（计时值保留，回来还能接着看）
void stopwatchUpdate();   // 每帧推进：判断倒计时到点、驱动响铃节奏
void stopwatchKey(char k);
bool stopwatchBusy();     // 正在计时或正在响铃 —— 给 loop 决定要不要持续重画
bool stopwatchIsAlarming(); // 闹钟是否正在响铃
void stopwatchStopAlarm();  // 全局停止闹钟响铃
void drawStopwatch();
