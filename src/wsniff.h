// WiFi 嗅探器（混杂模式，纯被动）：逐信道跳，抓原始 802.11 帧、分类计数，
// 重点抓「探测请求(probe request)」——附近设备在主动找哪些 SSID（暴露它们连过的网络名）；
// 同时统计 beacon、检测 deauth（有人打 deauth 会红字告警）。只监听不注入。
#pragma once
#include "globals.h"

void wsniffEnter();
void wsniffExit();
void wsniffUpdate();   // 每帧：逐信道跳
void wsniffKey(int k);
void drawWsniff();

// Remote ID 取样（串口 RIDSCAN 指令）：混杂模式跳信道 1-13，把管理帧里的
// vendor IE(tag 221) 全部打到串口。先看原始 OUI，再谈写解析器。
// fixedChan>0 时不跳频、蹲死这一个信道（NAN 固定在 ch6，跳频会整场错过）
void ridScanRun(int seconds, int fixedChan = 0);

// 2.4G 802.11 活动普查（串口 RFSCAN [秒]）：逐信道统计帧数、峰值 RSSI、噪声底。
// ⚠️ **看不到非 Wi-Fi 的信号**（O4 图传/蓝牙/微波炉）。原本就是为探测 O4 写的，
//    实测否掉了——混杂模式只在认出 802.11 前导后才回调，详见 wsniff.cpp 里那段。
void rfScanRun(int seconds);
