// 单位换算器：7 大类（长度/质量/温度/体积/速度/时间/面积），实时显示换算结果。
// 导航：,/ 切类别；;. 切 FROM 单位；[/] 切 TO 单位；数字键/. /- 输入；\b 退格；` 返回
#pragma once
#include "globals.h"

extern int    convCat;    // 当前类别索引
extern int    convFrom;   // FROM 单位索引
extern int    convTo;     // TO   单位索引
extern String convInput;  // 当前输入的数值字符串
extern String convResult; // 换算结果字符串

void convEnter();
void convKey(char k);
void drawConv();
