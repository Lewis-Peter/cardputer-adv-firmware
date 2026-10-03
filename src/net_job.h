// 「延后到界面画出来之后再干活」的小状态机。
//
// 为什么需要它：拉网络是同步阻塞的（一发请求几百毫秒到几秒），而 enter() 是从菜单的
// Enter 键回调里调的、draw() 是从 render() 里调的，两者都在 loop() 上——在里面直接拉网络
// 会把主循环整个卡住，键盘轮询不到、屏幕也不刷新，表现就是"进这一页卡一下"。
//
// 正确姿势是：enter 只登记一个待办，等 loading 那一帧真的推上屏之后，再由 loop 里的
// update 去拉。ADS-B / Sats / Router 三页是一模一样的需求，所以收在这里。
//
// 现在 update 里 due() 之后不再自己同步拉，而是 bgFetchRun(fetchXxx) 交给后台工人
// （bg_fetch.h）。"先让 loading 那一帧上屏"这一步仍然要：工人一开跑画布就被它借走了，
// 屏上停着的就是这一帧，右上角再压一个会动的小指示。
//
// 用法：
//   static DeferredFetch job;
//   void xxxEnter()  { ...重置状态...; job.request(); }
//   void xxxUpdate() { if (job.due()) { bgFetchRun(fetchXxx); return; } ...定时刷新... }
//   void drawXxx()   { job.markShown(); ... }   // ⚠️ 必须放在函数开头
//
// 拉取期间按下的键由 main.cpp 攒着，拉完再按原顺序重放（第 1 页的返回键会当场取消在途请求），
// 所以 update 里不用、也不该再 kbd::flushEvents()。只有起不来工人任务、退回同步拉取的那条
// 路径才会清 FIFO，那是 bgFetchRun() 自己做的。
//
// 按"选中项稳定下来"才触发的附带请求（ADS-B 的航线、Typhoon 的详情）要看一眼 pending：
// 按 r 之后列表马上要重拉，这时再按旧列表去查一次纯属白跑一趟 TLS。
//
// ⚠️ markShown() 一定要放在 draw 的第一行：这几页都有"没数据就提前 return"的空状态分支，
// 放末尾的话 loading 那一帧根本走不到那句，due() 永远不成立，就死在 loading 界面了。
#pragma once

#include "globals.h"

struct DeferredFetch {
  bool pending = false;   // 有活要干
  bool shown   = false;   // 界面已经画过一帧了

  void request()   { pending = true; shown = false; }
  void markShown() { shown = true; }

  // 该开工了吗。返回 true 的同时把待办清掉，所以每次 request() 只会触发一次
  bool due() {
    if (!pending) return false;
    // 正常情况下等首帧推屏上（shown=true）再启动后台拉取，避免切页瞬间无过渡帧。
    // 但若此时画布暂时无法分配（如上一个请求刚结束，堆尚在整理，render 走 noCanvas 分支）：
    // 后台工人首行即是 CanvasLease（本来就不需要画布，反而会释放画布让出显存），
    // 且忙时 render 直接画直推小药丸（bgFetchDrawIndicator）。
    // 因此在画布缺失时不能把 job 死锁在 shown=false 上，应直接放行起工。
    if (!shown && canvasAvailable()) return false;
    pending = false;
    return true;
  }
};
