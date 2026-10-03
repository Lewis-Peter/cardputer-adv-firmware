// 设置二级列表页，及其挂出去的各个简单子屏（亮度/音量/熄屏/电池/格式化/关于）。
// 蓝牙相关屏幕在 bt.h/bt.cpp 里（BLE 扫描器 + BLE HID 键盘）。
#pragma once
#include "globals.h"

String setValue(SetId id);   // 设置项右侧的数值/状态

void drawSettings();
void drawBar(const char* title, int pct, const char* hint);   // 亮度/音量共用：标题 + 进度条
void drawSleep();
void drawTz();
void drawBatteryDetail();
void drawFormat();

extern int aboutPage;                // About 分三页：0=设备信息  1=RAM/ROM/SD 用量  2=开机内存轨迹
extern const int ABOUT_PAGE_COUNT;
extern int aboutRamScroll;
void aboutRamScrollMove(int dir);
void aboutRamReset();
void drawAbout();
void drawAboutRam();

