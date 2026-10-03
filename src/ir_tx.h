// 红外发射的硬件那一半。**只有这个文件碰外设**——编码在 ir_proto.cpp（可以在桌面上验），
// 界面在 ir.cpp（不依赖板级，能编进 uisim），拆成三份就是为了让能验的部分真的被验到。
//
// 用 RMT 而不是"LEDC 出载波 + delayMicroseconds 打节拍"：这台机器的 Wi-Fi 是**常开**的
// （见 README），协议栈的任务随时会抢走 CPU 几百微秒，而红外的位间隔只有 560µs——
// 软件打节拍必然被拉长，接收端解出来就是另一串比特，表现是"有时能用有时不行"。
// RMT 是硬件定时的，发出去之后 CPU 干什么都不影响波形。
//
// ⚠️ 通道选择：ESP32-S3 有 4 个 RMT 发送通道，FastLED 驱动 SK6812 时会从低编号开始占，
// 所以这里取 3 号，并且**只在 IR 页面开着的时候安装驱动**（进页面装、退页面卸），
// 跟这个项目里别的抢硬件的 app 一个规矩。
#pragma once
#include "ir_proto.h"

bool irTxBegin();                     // 装 RMT 驱动。失败返回 false（页面会把原因显示出来）
void irTxEnd();                       // 卸驱动，把通道还给别人
bool irTxReady();
bool irTxSend(const IrSignal& s);     // 阻塞发完（含 repeats）。整帧最长也就几十毫秒
const char* irTxError();              // 最近一次失败的原因，没有就是空串
