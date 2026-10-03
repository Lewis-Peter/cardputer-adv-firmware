// 网络收音机：通过 WiFi 流式播放 HTTP MP3 电台。
// 使用 ESP8266Audio（Helix MP3 软解码）+ FreeRTOS 后台任务 + 双缓冲对接 M5.Speaker。
// 预置 6 个 SomaFM 公开电台，支持输入任意 HTTP MP3 直链（HTTPS 不支持）。
// 导航：;. 选台；Enter 播放/停止；U 输入自定义 URL；` 返回（自动停播）
#pragma once
#include "globals.h"

extern String radioCustomUrl;  // 用户输入的自定义电台 URL

void radioEnter();   // 初始化，尝试连 WiFi
void radioExit();    // 停止后台任务，释放 pipeline 资源
void radioKey(char k);
void radioUrlKey(char k);
void drawRadio();
void drawRadioPlay();
void drawRadioUrl();
bool radioIsActive();  // 后台任务是否还在运行
uint8_t radioGetVuLevel(); // 外部查询：当前播放电平 (0..100)
void radioUpdate();        // 主循环钩子：检测后台任务退出、执行挂起动作、串口诊断及直推刷新
inline void radioPlayUpdate() { radioUpdate(); } // 兼容旧调用点
