// 计算器：利用 Cardputer 的全键盘直接输入算式（如 12+3*4/2），Enter 求值。
// 支持 + - * / % ( ) 和小数、一元正负号，标准优先级（递归下降求值，不用浮点库额外依赖）。
// 导航：字符键输入；Enter 求值；Del 退格（算式空时清结果）；` 返回主菜单。
#pragma once
#include "globals.h"

extern String calcExpr;     // 当前输入的算式
extern String calcResult;   // 上次求值结果（或 "Error"）
extern bool   calcError;    // 上次求值是否出错
extern bool   calcJustEvaled; // 刚按过 Enter：下一个数字重新开始，运算符则接着结果算

void calcKey(char k);       // 处理一次按键（在 main 的 handleKey 里调用）
void drawCalc();
