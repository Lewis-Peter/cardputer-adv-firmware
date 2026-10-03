#pragma once
#include <Arduino.h>

// SSH 客户端核心生命周期与 UI 接口
void sshEnter();
void sshExit();
void sshUpdate();
bool sshKey(char k);
void drawSsh();
void drawSshConfig();
bool sshIsTerminalActive();
