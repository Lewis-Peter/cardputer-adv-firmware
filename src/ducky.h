// BadUSB / Ducky Script——原本想走 ESP32-S3 原生 USB HID，但那需要把
// platformio.ini 的 ARDUINO_USB_MODE 从 1(USB-Serial-JTAG，现有调试串口靠它)
// 切到 0(TinyUSB OTG)，会换掉调试串口的底层实现且这边烧不了机验证，风险太大。
// 改用已经跑通的 BLE HID 键盘（bt.cpp）做按键注入：配对上电脑/手机后，
// 从 SD 卡挑一个 Ducky 脚本逐行执行。支持的关键字见 ducky.cpp 顶部注释。
#pragma once
#include "globals.h"

void duckyEnter();     // 进屏幕：文件选择器 + 开始 BLE 广播等待配对
void duckyExit();      // 退出屏幕：释放脚本缓存与文件列表
void duckyUpdate();    // 每帧调用：跑脚本时逐行推进（DELAY 不阻塞 UI）
void duckyKey(char k);
void drawDucky();
