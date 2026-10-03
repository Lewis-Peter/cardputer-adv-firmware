// 编译期配置（非密钥类）：NTP、热点、网络探测默认目标。
// 想改默认值：不要编辑本文件，复制 src/config_local.h.example 为 src/config_local.h
// （已在 .gitignore），只写想覆盖的宏即可。运行期可在设备上改的项（时区、SSH、
// VPN 探测目标等）存在 NVS，不在这里。API key 等密钥仍放 secrets.h。
#pragma once

#if defined(__has_include)
#  if __has_include("config_local.h")
#    include "config_local.h"
#  endif
#endif

// --- NTP 服务器（按顺序尝试）。默认偏向中国大陆可达；海外用户可改成 pool.ntp.org 系 ---
#ifndef CFG_NTP_1
#define CFG_NTP_1 "ntp.aliyun.com"
#endif
#ifndef CFG_NTP_2
#define CFG_NTP_2 "cn.pool.ntp.org"
#endif
#ifndef CFG_NTP_3
#define CFG_NTP_3 "pool.ntp.org"
#endif

// --- 设备热点（Hotspot app）---
#ifndef CFG_HOTSPOT_SSID
#define CFG_HOTSPOT_SSID "Cardputer-ADV"
#endif
// 首次使用、NVS 里还没存密码时的默认密码（WPA2 至少 8 位）。公开固件请务必改掉。
#ifndef CFG_HOTSPOT_DEFAULT_PW
#define CFG_HOTSPOT_DEFAULT_PW "cardputer123"
#endif

// --- 网络探测（NetProbe app）---
// "vpn-node" 的出厂默认目标；设备上按 e 改后会存 NVS 并覆盖这里。
// 默认是文档保留地址 203.0.113.10（不可路由），需要自行配置。
#ifndef CFG_PROBE_VPN_HOST
#define CFG_PROBE_VPN_HOST "203.0.113.10"
#endif
#ifndef CFG_PROBE_VPN_PORT
#define CFG_PROBE_VPN_PORT 443
#endif
// 国内基线（应当总是通）。海外用户可改成自己网络里稳定可达的 IP，如 9.9.9.9
#ifndef CFG_PROBE_BASELINE_IP
#define CFG_PROBE_BASELINE_IP "223.5.5.5"
#endif
#ifndef CFG_PROBE_BASELINE_NAME
#define CFG_PROBE_BASELINE_NAME "aliyun"
#endif
