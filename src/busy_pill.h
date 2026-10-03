// 后台拉取的右上角小指示（"··· loading"），实现在 ui_common.cpp。
// 单独一个头文件：ui_common.h 被好几个主机测试台带进去，它们的 globals.h 桩里没有 lgfx 类型。
#pragma once
#include "globals.h"

// 画到任意目标上：真机上是 M5.Display（此时画布在工人手里），模拟器里画到 cv 上截图看排版。
// phase 0..2 是三个点里亮的那一个；leaving=true 换成黄色（用户已经按了离开）。
void drawBusyPill(lgfx::LovyanGFX& d, const char* label, int phase, bool leaving);
