// LoRa 收发：Cap LoRa-1262 板载的 SX1262 射频芯片。三种模式一键切换：
//   SCAN   —— 扫一串频点读 RSSI，不管对方用什么调制参数，只要有人在发就看到能量峰
//             （"附近有没有人在广播"最可靠的答案，单台就有用）
//   LISTEN —— 把电台设到常见预设（Meshtastic LongFast 等）真去收包，收到就显示
//             长度/RSSI/SNR/前几字节。解密做不到，但能确认"抓到一个包"。
//   CHAT   —— LISTEN 页按 C 进入：自家设备间的简单双向文本聊天，私有帧格式，
//             不是 Meshtastic 协议兼容（不加密/不走protobuf），只跟同款固件互通。
// 射频通路：SX1262 走 SPI（SCK40/MISO39/MOSI14/NSS5/BUSY6/DIO1_4/RST3），
// RF 天线开关由 Cap 的 IO 扩展芯片(0x43) P0 控制（跟 GNSS 共用，这里也会拉高一次）。
#pragma once
#include "globals.h"

void loraEnter();     // 进屏幕：初始化 SX1262 + 开 RF 开关，默认进 SCAN
void loraExit();      // 离开：电台待机、释放 SPI、恢复 GPIO5 给 SD
void loraUpdate();    // 每帧：SCAN 扫几个频点 / LISTEN 收包
void loraKey(int k);  // 处理按键（模式切换、换预设、清屏）；` 返回由 main 处理
void drawLora();

// ---- LoRa 嗅探串口流（PC MODE 及独立串口能力）----
#include "lora_sniff_proto.h"

bool loraSniffStart(const LoraSniffConfig& cfg);
void loraSniffStop();
void loraSniffTick();
bool loraSniffIsActive();
float loraSniffGetMhz();
uint32_t loraSniffGetPktCount();
