// 路由器/代理监控：读 Clash / mihomo 的 external-controller API（局域网内明文 HTTP，很快）。
// 上下行实时速率 + 下行速率曲线 + 连接数 + 内存 + 当前节点和它的延迟。
// 地址和 secret 在 secrets.h 里（CLASH_BASE / CLASH_SECRET）。
#pragma once
#include "globals.h"

void routerEnter();
void routerUpdate();    // 停在这页时调：每帧非阻塞地读 /traffic 常开流 + 按间隔问其余接口
void routerExit();      // ⚠️ 必须调：关掉常开的 /traffic 连接（挂在 cleanupApp 上）
bool routerKey(char k);
void drawRouter();
