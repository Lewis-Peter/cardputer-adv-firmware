#include "pages.h"

// ---- 唯一的一张表 ----
// 一行一个 app，按屏上的翻页顺序排。翻页按键和底部页码点都只认这里。
// Time：时钟 + 秒表/倒计时。这两个原本是两个 app，但都是"看时间"的一件事。
static const Screen TIME_PAGES[]    = { SCREEN_CLOCK, SCREEN_STOPWATCH };
// Astro：日照放第一页——"今天几点天黑"是这三页里最常用的；其次月相，最后晨昏线。
static const Screen ASTRO_PAGES[]   = { SCREEN_ASTRO_SUN, SCREEN_MOON, SCREEN_ASTRO_TERM };
static const Screen COMPASS_PAGES[] = { SCREEN_COMPASS, SCREEN_COMPASS_DETAIL };
// 地图页已拆成独立 app（APP_MAP），不在这条链里了
static const Screen GNSS_PAGES[]    = { SCREEN_GNSS, SCREEN_GNSS_DETAIL,
                                        SCREEN_GNSS_SAT, SCREEN_GNSS_SPEED,
                                        SCREEN_GNSS_TRIP };
static const Screen WEATHER_PAGES[] = { SCREEN_WEATHER, SCREEN_WEATHER_HOUR,
                                        SCREEN_WEATHER_AIR, SCREEN_WEATHER_AQI, SCREEN_WEATHER_FC };
static const Screen TYPHOON_PAGES[] = { SCREEN_TYPHOON, SCREEN_TYPHOON_TRACK };
// 地震：列表在前——"刚才哪儿地震了"比"它在地图哪个位置"更常问
static const Screen QUAKE_PAGES[]   = { SCREEN_QUAKE, SCREEN_QUAKE_MAP };
// 汇率：现价和走势图放第一页——"现在多少、今天涨还是跌"是每次进来都要看的
static const Screen FX_PAGES[]      = { SCREEN_FX, SCREEN_FX_DAYS };

struct PageChain { const Screen* p; int n; };
// 长度从数组本身推，别手写——手写的数字正是这次要消灭的东西
#define CHAIN(a) { a, (int)(sizeof(a) / sizeof((a)[0])) }
static const PageChain CHAINS[] = {
  CHAIN(TIME_PAGES), CHAIN(ASTRO_PAGES), CHAIN(COMPASS_PAGES), CHAIN(GNSS_PAGES), CHAIN(WEATHER_PAGES),
  CHAIN(TYPHOON_PAGES), CHAIN(QUAKE_PAGES), CHAIN(FX_PAGES),
};
#undef CHAIN
static const int CHAIN_COUNT = (int)(sizeof(CHAINS) / sizeof(CHAINS[0]));

// 找 s 落在哪条链的第几格
static const PageChain* locate(Screen s, int& idx) {
  for (int c = 0; c < CHAIN_COUNT; c++)
    for (int i = 0; i < CHAINS[c].n; i++)
      if (CHAINS[c].p[i] == s) { idx = i; return &CHAINS[c]; }
  return nullptr;
}

Screen pageStep(Screen s, int d) {
  int i;
  const PageChain* c = locate(s, i);
  if (!c) return s;
  return c->p[((i + d) % c->n + c->n) % c->n];
}

Screen pageFirst(Screen s) {
  int i;
  const PageChain* c = locate(s, i);
  return c ? c->p[0] : s;
}

bool pageIndex(Screen s, int& cur, int& count) {
  int i;
  const PageChain* c = locate(s, i);
  if (!c) return false;
  cur = i; count = c->n;
  return true;
}
