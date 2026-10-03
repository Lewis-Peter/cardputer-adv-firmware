// Wi-Fi 扫描/连接 + NTP 对时，以及扫描结果/密码输入屏。
#pragma once
#include "globals.h"

// ---- Wi-Fi 扫描/连接状态 ----
extern int scanCount;         // WiFi.scanNetworks() 结果（<0=失败）
extern uint32_t lastWifiScanMs;
extern int wifiIdx;           // 网络列表选中项
extern int wifiTop;           // 列表滚动窗口顶部
extern String selSSID;        // 选中的网络名
extern String pwInput;        // 密码输入缓冲
extern const int WIFI_VIS;    // 列表一屏可见网络数

// 用指定凭据连接 Wi-Fi（阻塞，最多 15s）。needNtp=true 时顺便对时。
bool connectWith(const char* ssid, const char* pass, bool needNtp);
void saveCreds(const String& ssid, const String& pass);
bool loadCreds(String& ssid, String& pass);
void doScan();   // 扫描周边网络（阻塞 2~4s）
void wifiScanExit(); // 离开扫描/密码输入页时释放扫描结果与输入缓存

// 需要联网的 app（Weather/ADS-B/Sats/Router…）统一用这个。
// **非阻塞**：只回答"现在通不通"，掉线时不会自己发起连接（那会在 loop() 里卡 15 秒，
// 还会抢屏刷握手日志）。重连是 wifiKeeperUpdate() 看门狗的活。
bool wifiEnsureConnected();

// Wi-Fi 看门狗（常开+自动重连策略的兜底）：挂在 loop() 上每帧调。
// allowed 传 false 表示"当前页面正拿着射频"（嗅探/热点/wardrive 之类），这时什么都不做。
void wifiKeeperUpdate(bool allowed);
// 串口 WIFIOFF 用：挂起看门狗，否则手动关掉的 WiFi 下一帧就被拉回来，没法做重连对照测试
void wifiKeeperSuspend();
void wifiKeeperResume();

void drawWifiScan();
void drawWifiPw();

// 开机后台连网+对时（非阻塞）：bootWifiStart() 在 setup() 里调一次，之后每帧 loop() 调
// bootWifiUpdate() 推进状态机，不卡开机动画，菜单立刻能用。
// 连上/超时之后都**保持 radio 常开**（早期版本会关掉，现在不会了）：连不上就让协议栈在
// 后台一直重试，回到覆盖范围自动就上。详见 wifi_net.cpp 里那段策略说明。
void bootWifiStart();
void bootWifiUpdate();
