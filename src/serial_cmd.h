// 串口调试控制台。跟主循环零耦合——除了把按键喂回 handleKey()、截图前让 render()
// 画一帧之外，其余全是自成一体的工具指令，所以从 main.cpp 里搬了出来。
// 支持的指令见 HELP。配套的 Mac 端脚本在 tools/ 下（shot.py 用 SHOT）。
#pragma once
#include <Arduino.h>

// main.cpp 提供的两个钩子——串口指令要能模拟按键、要能在截图前强制画一帧
void handleKey(char k);
void render();

// loop() 里调：串口有一整行就取出来执行，没有就立刻返回
void serialCmdPoll();
