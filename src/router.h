// 路由器/代理监控：读 Clash / mihomo 的 external-controller API（局域网内明文 HTTP，很快）。
// 六页：流量监控 / 连接桑基图 / 流量类型 / 节点切换与延迟 / Top 域名 / 内存·连接数历史。
// 地址和 secret 在 secrets.h 里（CLASH_BASE / CLASH_SECRET）。
#pragma once
#include "globals.h"

void routerEnter();
void routerUpdate();    // 停在这页时调：每帧非阻塞地读 /traffic 常开流 + 按间隔问其余接口
void routerExit();      // ⚠️ 必须调：关掉常开的 /traffic 连接（挂在 cleanupApp 上）
bool routerKey(char k);
void drawRouter();
