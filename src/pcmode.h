#pragma once
#include "globals.h"

// 进入 / 退出 PC MODE
void pcmodeEnter();
void pcmodeExit();

// 状态屏绘制（直连 M5.Display，不分配任何 sprite）
void drawPcMode();

// 主循环推进（低频刷新检测）
void pcmodeUpdate();

// 记录最近一次 WiFi 扫描 AP 数量（用于状态屏展示）
void pcmodeRecordWifiScan(int count);
int  pcmodeGetLastWifiScan();

// 查询 PC MODE 能力列表 JSON 并输出到 Serial
void pcmodePrintCaps();

// 回 enter + hello 握手（进入时，或已在 PC MODE 里再收到 PCMODE 时重发）
void pcmodePrintHello();
