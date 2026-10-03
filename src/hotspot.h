// WiFi 热点（SoftAP）：纯本地局域网，不联网、不做 NAT。
// 典型用途：户外没有可用 WiFi 时，手机和电脑都连上这个热点，
// 就能在同一局域网里跑 adb over WiFi（adb connect <手机IP>:5555）之类的东西。
#pragma once
#include "globals.h"
#include <WiFi.h>

extern String hotspotPwInput;   // 密码编辑屏用的输入缓冲

void hotspotToggle();           // 开/关 AP
void hotspotEnsureOn();         // 确保 AP 已经开着（进二维码页用：扫出来就得能连上）
bool hotspotIsActive();         // 检查当前 AP 是否处于激活状态
void hotspotStop();             // 安全关闭 AP 并释放无线资源
wifi_mode_t hotspotScanMode();  // 扫描该用的模式：热点开着就 AP+STA 共存，别把连着热点的设备踢掉
bool hotspotRestoreAfterScan(); // 扫描页退出：热点开着就退回纯 AP 并返回 true（调用方别再关 radio）
bool hotspotSuspend();          // 独占射频前暂挂热点：若之前开着则返回 true 并关闭 AP
void hotspotResume();           // 独占射频退出后恢复热点
bool hotspotIsSuspended();      // 热点当前是否因 app 暂挂
String hotspotPassword();       // 读取当前保存的密码（给密码编辑屏做初始值）
void hotspotSetPassword(const String& pw);   // 存并（如果正开着）立即用新密码重开

void drawHotspot();
void drawHotspotPw();
void drawHotspotQr();           // 扫码入网：手机相机扫一下直接连，不用手打密码
