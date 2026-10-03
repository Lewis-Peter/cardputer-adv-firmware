#pragma once
#include "globals.h"
#include "odid.h"
// ---------------------------------------------------------------------------
// Drone ID —— 带屏扫无人机 Remote ID
//
// 跟串口的 RIDSCAN 是同一套解码器（odid.cpp），区别在于取数方式：
//   RIDSCAN  阻塞 N 秒、一次性打一堆 hex，是排查格式用的取样工具
//   这一页   常驻混杂模式、每帧推进，列表实时更新，是拿出门用的
//
// ⚠️ 只收 2.4G Wi-Fi。ESP32-S3 没有 5G，而 BLE 那条路要独占射频、跟 Wi-Fi 混杂模式
//    共存不了（内存也不够，见 bt.cpp 里那段 deinit 的血泪注释）。实测国产机走的是
//    Wi-Fi beacon，所以先做这条；真遇到只播 BLE 的机器，用串口 BTDUMP/BTEXT 兜底。
// ---------------------------------------------------------------------------

void ridAppEnter();
void ridAppExit();
void ridAppUpdate();      // 每帧调：跳频/锁频 + 老化
void ridAppKey(char k);
void drawRidApp();        // 列表页
void drawRidAppDetail();  // 单机详情页

// 快速预锁定指定信道（例如捕获到 RID- SSID 信号时）
void ridAppPreLockChannel(uint8_t chan, uint32_t holdMs = 4000);

// 串口 RIDFAKE：把真机抓到的两帧国标载荷灌进解码/合并路径，用来在没有无人机的时候
// 核对列表和详情页的排版。走的是跟射频完全相同的那条路（ridDecode → odidMerge），
// 所以它验证的是真实渲染，不是另画一份假界面。
void ridAppInjectSample();

// 机载 SD 卡抓包录制控制
bool ridAppIsRecording();
void ridAppToggleRecord();

// ---- PC 主导模式：无屏 Remote ID 流 ----
// RID ON [信道] / RID OFF 串口命令的实现。跟带屏页共用同一套混杂模式、解码（odid.cpp）、
// 目标合并和跳频，区别只在输出：协议格式的 "RID {...}" 行（t=start / d / end），
// 而不是给 tools/rid_view.py 用的 RIDPKT。带屏页开着时 RID ON 回 err busy。
// 只收 2.4G Wi-Fi（beacon / NAN），不收 BLE——原因见文件顶上那段。
void ridStreamStart(int fixedChan = 0);   // 0 = 跳频；1..13 = 蹲死在该信道
void ridStreamStop();                     // 幂等，总是回一行 end
bool ridStreamIsActive();
int  ridStreamDroneCount();
uint32_t ridStreamPktCount();
int  ridStreamChannel();                  // 蹲守的信道，跳频时 0

// ODID 解码结果 → JSON 字段（每个字段前带逗号）。BLE 流（bt.cpp）也用它，保证两条路字段一致。
void ridAppendOdidFields(char* buf, int cap, int& n, const OdidResult& r);

// PC 主导模式开 RID / BLE 流前的射频冲突检查：返回 "busy: ..." 文本，空闲返回 nullptr。
const char* pcmRadioConflict();
