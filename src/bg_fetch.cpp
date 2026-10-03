#include "bg_fetch.h"
#include <M5Unified.h>
#include <atomic>
#include "globals.h"
#include "ui_common.h"
#include "busy_pill.h"
#include "keyboard_adv.h"

// 设计与线程边界见 bg_fetch.h 顶上那段。

static const uint32_t WORKER_STACK = 12 * 1024;   // 跟 loop 任务一样（main.cpp 的 SET_LOOP_TASK_STACK_SIZE）
static const BaseType_t WORKER_CORE = 1;          // 跟 loop 同核：同核删已挂起的任务才会当场释放（见头文件）

static TaskHandle_t s_task = nullptr;
static std::atomic<bool> s_busy{false};
static std::atomic<bool> s_done{false};
static std::atomic<bool> s_cancel{false};
static std::atomic<const char*> s_status{nullptr};

// 收尾时画布没要回来：接下来一段时间里定期翻 dirty，让 render() 去重试（它开头就会 canvasRestore，
// 而且知道 PC MODE / SSH 这些直推屏幕的页面不该要画布——这里自己去要就绕过了那些判断）。
// 不重试的话 render() 只在 dirty 时才跑，要不回来就停在 "finishing request..." 不动了。
// 窗口给 20s：典型的钉子是 DNS 查询超时/被取消后 lwIP 还留着的那个 UDP pcb（MEMP_MEM_MALLOC，
// 在堆上）——它可能正好落在画布的洞里，要等 lwIP 自己放弃那次查询（单个 DNS 服务器约 7s，
// 两个约 14s）才释放。
static bool     s_restorePending = false;
static uint32_t s_restoreSinceMs = 0;
static uint32_t s_restorePokeMs = 0;
static const uint32_t RESTORE_RETRY_MS = 20000;
static const uint32_t RESTORE_POKE_MS  = 250;

static void worker(void* arg) {
  // 正文返回 = 它栈上的 WiFiClientSecure / HTTPClient / JsonDocument 都已经析构了。
  // 绝不在正文里 vTaskDelete：那样 C++ 析构一个都不跑（chat.cpp 那段注释讲过，每次漏几十 KB）。
  reinterpret_cast<BgFetchFn>(arg)();
  // release 放最后：主线程 acquire 到它之后，正文对页面数据的所有写都保证可见。
  s_done.store(true, std::memory_order_release);
  // 不自删，挂起等主线程来删：同核删一个已挂起的任务是当场释放（见头文件）。
  vTaskSuspend(nullptr);
}

bool bgFetchRun(BgFetchFn fn) {
  if (s_busy.load(std::memory_order_acquire)) {
    if (debugOn) Serial.printf("[bgfetch] run: rejected (s_busy=1)\n");
    return false;
  }
  if (debugOn) Serial.printf("[bgfetch] run: starting worker\n");
  s_done.store(false, std::memory_order_relaxed);
  s_cancel.store(false, std::memory_order_relaxed);
  s_status.store(nullptr, std::memory_order_relaxed);
  // 先置忙再建任务：从这一刻起主线程不再碰 cv（render 改画指示、canvasRestore 变空操作）。
  // 同核同优先级、时间片轮转：工人最早下一个 tick 就可能开跑，也就是 loop 这一轮还没走完。
  // 所以 bgFetchRun() 返回之后，调用方和 loop 这一轮剩下的部分都不能再碰页面数据和 cv
  // （各页 update 里调它之后都是直接 return；再往下的 loop 代码也不碰这些）。
  // s_task 由 xTaskCreatePinnedToCore 在任务进就绪队列之前写好，工人读它一定读得到。
  s_busy.store(true, std::memory_order_release);
  if (xTaskCreatePinnedToCore(worker, "bgfetch", WORKER_STACK, reinterpret_cast<void*>(fn),
                              1, &s_task, WORKER_CORE) != pdPASS) {
    s_task = nullptr;
    s_busy.store(false, std::memory_order_release);
    if (debugOn) Serial.printf("[bgfetch] no memory for worker, running inline\n");
    fn();                  // 退回老路子：主线程同步跑，行为跟改之前一样
    kbd::flushEvents();    // 同步阻塞期间按下的键全丢掉，免得醒来重放一串（见 keyboard_adv.cpp）
    dirty = true;
    return false;
  }
  dirty = true;
  return true;
}

bool bgFetchBusy() { return s_busy.load(std::memory_order_acquire); }

bool bgFetchOnWorker() {
  return s_task != nullptr && xTaskGetCurrentTaskHandle() == s_task;
}

void bgFetchCancel()    { s_cancel.store(true, std::memory_order_relaxed); }
// 只在有活在跑时才算数：上一次被取消之后标志要是一直挂着，主线程上别的同步请求（比如地图页的
// geoGet）走到 netResolve 时会被当成"已取消"直接放弃。收尾时也会清掉，这里再兜一层。
bool bgFetchCancelled() {
  return s_busy.load(std::memory_order_acquire) && s_cancel.load(std::memory_order_relaxed);
}

void bgFetchStatus(const char* msg) {
  if (bgFetchOnWorker()) {
    s_status.store(msg, std::memory_order_relaxed);
    dirty = true;
  } else {
    centerMsg(msg, TFT_YELLOW);   // 同步退回路径：主线程，照原样直接上屏
  }
}

void bgFetchPokeRestore() {
  if (!s_restorePending && !canvasAvailable()) {
    s_restorePending = true;
    s_restoreSinceMs = millis();
    s_restorePokeMs  = millis();
  }
}

bool bgFetchService() {
  if (s_restorePending) {
    if (canvasAvailable() || millis() - s_restoreSinceMs >= RESTORE_RETRY_MS) {
      s_restorePending = false;
      dirty = true;
    } else if (millis() - s_restorePokeMs >= RESTORE_POKE_MS) {
      s_restorePokeMs = millis();
      dirty = true;   // render() 开头会再 canvasRestore() 一次
    }
  }

  if (!s_busy.load(std::memory_order_acquire)) return false;
  if (!s_done.load(std::memory_order_acquire)) return false;
  // done 置位和 vTaskSuspend 之间可能被时间片切走：等它真的挂起了再删，
  // 否则删的是一个还在跑的任务，释放又会被推迟。
  if (eTaskGetState(s_task) != eSuspended) return false;

  if (debugOn) {
    Serial.printf("[bgfetch] done, worker stack free %u bytes\n",
                  (unsigned)uxTaskGetStackHighWaterMark(s_task));
  }
  vTaskDelete(s_task);     // 同核、已挂起：栈、TCB、lwIP 每线程信号量当场释放
  s_task = nullptr;
  s_status.store(nullptr, std::memory_order_relaxed);
  s_cancel.store(false, std::memory_order_relaxed);
  s_busy.store(false, std::memory_order_release);

  canvasRestore();
  if (!canvasAvailable()) {
    if (debugOn) Serial.printf("[bgfetch] done: canvasRestore failed, pending retry\n");
    s_restorePending = true;
    s_restoreSinceMs = millis();
  }
  dirty = true;
  return true;
}

void bgFetchDrawIndicator(bool leaving) {
  static uint32_t lastMs = 0;
  static int lastPhase = -1;
  static const char* lastLabel = nullptr;

  const char* label = leaving ? "leaving" : s_status.load(std::memory_order_relaxed);
  if (!label) label = "loading";
  const int phase = (int)((millis() / 180) % 3);
  if (phase == lastPhase && label == lastLabel && millis() - lastMs < 1000) return;
  lastMs = millis();
  lastPhase = phase;
  lastLabel = label;

  drawBusyPill(M5.Display, label, phase, leaving);
}
