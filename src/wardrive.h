// 战争驾驶：WiFi 扫描 + GPS 打点，发现的 AP 记到 SD 卡 /wardrive.csv。
// 扫描用异步 API（WiFi.scanNetworks(true) + 轮询 scanComplete()），不阻塞 UI，
// 跟 wifi_chan.cpp 那种一次性阻塞扫描不是一回事。同一个 BSSID 本次会话内只记一次，
// 屏幕上刷新它的最新信号强度即可，避免停在一个AP旁边把卡写爆。
#pragma once
#include "globals.h"

void wardriveEnter();
void wardriveExit();     // 离开时释放扫描结果、按需关WiFi（跟 wifiChanExit() 一样收尾）
void wardriveUpdate();   // 每帧调用：推进异步扫描/巡检定时器
void wardriveKey(char k);   // `（返回主菜单）在 main.cpp 里直接接 cleanupApp()，不经过这里
void drawWardrive();
