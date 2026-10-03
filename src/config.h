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

// --- 电台（Radio app）---
// 内置预置电台列表：逗号分隔的 {名称, 直链URL, 风格} 初始化项，只支持 http:// MP3 直链。
// 列表末尾固定会再追加一个 "[Custom URL]" 项。想不重编译就改列表：SD 卡根目录放 /radio.txt
// （每行 名称|URL|风格，# 开头为注释，最多 12 条），存在且有有效行时整体替换内置列表。
#ifndef CFG_RADIO_PRESETS
#define CFG_RADIO_PRESETS \
  {"Groove Salad",   "http://ice1.somafm.com/groovesalad-128-mp3",  "Downtempo"},    \
  {"Lush",           "http://ice6.somafm.com/lush-128-mp3",         "Chillout"},     \
  {"Deep Space 1",   "http://ice1.somafm.com/deepspaceone-128-mp3", "Deep Ambient"}, \
  {"Secret Agent",   "http://ice1.somafm.com/secretagent-128-mp3",  "Spy Lounge"},   \
  {"Soul Fly",       "http://ice1.somafm.com/sofly-128-mp3",        "Soul Funk"},    \
  {"Drone Zone",     "http://ice1.somafm.com/dronezone-128-mp3",    "Drone Ambient"},
#endif

// --- 地图在线瓦片（GNSS Map）---
// URL 模板，占位符 {s}=子域、{x}、{y}、{z}。必须是 http://（TLS 握手需要 40KB+ 连续堆，
// 地图页拿不出来，见 gnss.cpp 注释）。默认是高德（GCJ-02 坐标，国内可达）。
// 换成 OSM 系的 WGS-84 源时，务必把 CFG_MAP_TILE_WGS84 设成 1，否则位置会偏几百米。
// 注意：多数官方 OSM 服务器只有 https 且有使用条款，请换成自己可用的 http 瓦片源/自建镜像。
#ifndef CFG_MAP_TILE_URL
#define CFG_MAP_TILE_URL "http://webst0{s}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}"
#endif
// 子域字符集合，按轮次轮流用；模板里没有 {s} 时忽略。
#ifndef CFG_MAP_TILE_SUBS
#define CFG_MAP_TILE_SUBS "1234"
#endif
// 在线瓦片的坐标系：0 = GCJ-02（高德等国内源），1 = WGS-84（OSM 等）。
// SD 卡上的离线瓦片包仍以 /map/wgs84 等标牌文件为准（见 README）。
#ifndef CFG_MAP_TILE_WGS84
#define CFG_MAP_TILE_WGS84 0
#endif
