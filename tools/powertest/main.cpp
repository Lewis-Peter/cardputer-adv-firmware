#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "power_util.h"

uint32_t g_fakeMs = 0;
int      g_fakeMv = 3600;   // 实测：离电满电约 3.6V（重负载），不是教科书的 3.75
uint8_t  g_nvsPct = 0;
class Preferences; Preferences* dummy;
#include <Preferences.h>
Preferences prefs;

// NVS 替身。824d639（抽 NVS helper）把 power_util.cpp 从直接 prefs.begin/getUChar/end
// 改成了调 globals.h 的 loadUChar/saveUChar，而那两个的实现在 globals.cpp 里——链进来
// 等于把整个固件拖进来。所以在这儿给它们打桩，背后还是上面那个 g_nvsPct。
// ⚠️ 以后再动 globals.h 的 NVS helper，这里要跟着补，否则本测试台又会链接失败
//   （上次就漏了两周没人发现：它不在 CI 里，也没人手动跑）。
uint8_t loadUChar(const char*, const char*, uint8_t def) { return g_nvsPct ? g_nvsPct : def; }
void    saveUChar(const char*, const char*, uint8_t v)   { g_nvsPct = v; }
#include <M5Unified.h>
SimM5 M5;
SimSerial Serial;   // powerDiag() 的输出落到 stdout

// 把时钟往前推 sec 秒，期间按固件的节奏调 powerUpdate()
static void advance(int sec, int mv) {
  g_fakeMv = mv;
  for (int i = 0; i < sec; i++) { g_fakeMs += 1000; powerUpdate(); }
}
static void show(const char* tag) {
  printf("  %-30s  %5.2fV  charging=%-5s  level=%d%%\n",
         tag, g_fakeMv / 1000.0f, powerCharging() ? "true" : "false", powerBatteryLevel());
}

int main(int argc, char** argv) {
  int sc = argc > 1 ? atoi(argv[1]) : 1;

  if (sc == 1) {
    puts("场景1：离电使用 → 插上充电 → 拔掉   (NVS 里没有历史)");
    // ⚠️ 这几个电压 2026-08-26 按实测重设过。原来用的 3.75/3.90/4.20 是照教科书 LiPo
    // 曲线编的，跟这块板子的实际读数对不上（实测离电区间 3.63~2.88V、插电 4.12~4.42V），
    // 拿它们测出来的"对"是假的。
    advance(300, 3600);  show("离电 5 分钟 @3.60V(实测满电)");
    advance(180, 4200);  show("插上充电线 3 分钟后");
    for (int m = 15; m <= 60; m += 15) { advance(15 * 60, 4200); char t[40];
      snprintf(t, sizeof(t), "充电 %d 分钟", m); show(t); }
    advance(120, 3560);  show("拔掉，回到 3.56V(实测离电区间)");
    advance(600, 3500);  show("再放电 10 分钟 @3.50V");
  } else {
    puts("场景2：开机时就已经插着电（用户报的那个情况）");
    g_nvsPct = 35;                       // 上次关机前存的电量
    printf("  (NVS 里上次关机存的是 %d%%)\n", g_nvsPct);
    advance(60, 4210);   show("开机即插电，1 分钟后");
    advance(600, 4210);  show("再过 10 分钟");
    advance(1800, 4210); show("再过 30 分钟");
  }
  return 0;
}
