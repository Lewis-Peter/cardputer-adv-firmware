// 网络探针：在当前连接的 WiFi 上跑网络连通性巡检与 DNS Fuzz 模糊测试。
// 包含两种模式：
// 1. Probe 模式：多目标连通性探测——DNS解析(顺带识别AS32934投毒)、到已知IP的裸TCP握手
//    (判断是否路由级封锁)、明文HTTP(抓强制门户/captive portal 204跳转)。结果写 /netprobe.log。
// 2. DNS 模式：对路由器/网关 DNS 服务(如 dnsmasq)注入 10 种 RFC 畸形包与长包边界测试，
//    并在每个畸形包后通过存活探针量化 RTT 延迟或抓出宕机(自动暂停)。结果写 /dnsfuzz.log。
// 导航：Enter 重新跑/继续；M/Tab 切换 Probe/DNS 模式；A 自动巡检开关(仅Probe模式)；E 编辑目标；G 对准网关(DNS模式)；` 返回
#pragma once
#include "globals.h"

enum NetProbeMode {
  NETPROBE_MODE_PROBE = 0,
  NETPROBE_MODE_DNS   = 1,
};

void netprobeEnter(NetProbeMode mode = NETPROBE_MODE_PROBE);
void netprobeExit();
void netprobeUpdate();
// 返回 true 表示要退回主菜单（编辑目标地址时 ` 只取消编辑，不退出屏幕）
bool netprobeKey(char k);
void drawNetprobe();

// 串口一次性任意目标探测，格式 "host[:port]"，不进 NetProbe 屏幕，结果只走 Serial。
void netprobeAdhoc(const String& targetRaw);
