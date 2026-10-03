// Wi-Fi 信道分析器：扫描周边 AP，按各自信道位置画出重叠的信号曲线（仿 WiFi Analyzer 类
// app 的经典山丘图），直观看出哪些信道拥挤/重叠。只覆盖 2.4GHz 1-14 信道（ESP32-S3 没有 5GHz）。
// 跟 Settings > Wi-Fi 里连网用的扫描列表（wifi_net.*）各扫各的，互不影响。
#pragma once
#include "globals.h"

extern int chanCount;   // 扫描到的 AP 数
extern int chanSel;     // 当前高亮的 AP（; . 切换）

enum WifiChanViewMode {
  WIFI_CHAN_VIEW_SPECTRUM = 0,  // RF 频谱山丘曲线 (RF Spectrum Analyzer)
  WIFI_CHAN_VIEW_RATING   = 1,  // 信道拥挤度与星级评级 (Channel Rating & Congestion)
  WIFI_CHAN_VIEW_LIST     = 2,  // AP 热点情报清单 (AP Feed Table)
  WIFI_CHAN_VIEW_COUNT    = 3
};
extern int wifiChanView;

void wifiChanScan();    // 异步扫描（WiFi.scanNetworks(true)）
void wifiChanUpdate();  // 轮询扫描状态与 AUTO 模式定时触发
void wifiChanToggleAuto();
bool wifiChanAutoMode();
bool wifiChanScanning();
void wifiChanExit();    // 离开时释放扫描结果占的内存
void wifiChanKey(char k); // 按键分发（视图切换、AP 切换、触发扫描等）
void drawWifiChan();
void drawWifiChanDetail();   // 选中 AP 的完整详情页
