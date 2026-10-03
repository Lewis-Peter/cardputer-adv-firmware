// 红外遥控。硬件红外发射在 GPIO44，**只发不收**（见 HARDWARE.md）——所以这一页
// 学不了别人的遥控器，码只能来自码表。
//
// 三层分开，为的是让能验的部分真的被验到：
//   ir_proto.cpp  纯编码，桌面上逐段验（tools/irtest）
//   ir_tx.cpp     RMT + 载波，只有对着电视按一下才知道对不对
//   ir.cpp（本页） 界面 + 码表，不碰任何外设，能编进 uisim 看布局
//
// 码表两个来源：
//   1. 内置几台常见电视，够开箱试一下（⚠️ 抄自公开码表，**没有实机验证过**）
//   2. SD 卡 /ir/*.ir，一行一个按键，格式见 README。这才是正经用法。
#pragma once
#include "globals.h"

void irEnter();
void irExit();
void irKey(char k);
void irUpdate();
void drawIr();
