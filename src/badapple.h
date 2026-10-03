// Bad Apple!! 播放器：SD 卡里的 1bpp 位图逐帧流式播放。
// 文件格式（打包脚本见 tools/badapple_pack.py）：16 字节头 + 逐帧 1bpp 位图（MSB-first，
// 行按字节对齐，无行间 padding——这正是 LovyanGFX 1bpp Sprite 内部读取时用的位序，
// 所以每帧读进 Sprite 的 getBuffer() 直接 memcpy，不用自己转位）。
// 头：magic[4]="BAP1" + width(u16) + height(u16) + fps(u16) + reserved(u16) + frameCount(u32)
// 文件位置固定：/video/badapple.dat（跟 player.cpp 扫 /music、ir.cpp 扫 /ir 是同一套"SD 卡
// 上按 app 分文件夹"的约定，只是这里内容固定成一个文件，不用扫目录）。
//
// 播放节奏：按 millis() 算目标帧号，每次 loop() 最多读一帧——追不上就任由变慢，不丢帧
// （没有音轨要同步，慢一点比跳帧更耐看）。屏幕小(240x135)+1bpp(4050B/帧)，SD 20MHz 下
// 带宽不是瓶颈，瓶颈在 SPI 推屏——这也是留一个角落 fps 计数器的意义：真实測得的帧率。
#pragma once
#include "globals.h"

void badappleEnter();
void badappleExit();
void badappleUpdate();   // 每帧调：按时间线推进播放，读到新帧才 dirty=true
void badappleKey(char k);
void drawBadApple();
bool badappleIsPlaying();
