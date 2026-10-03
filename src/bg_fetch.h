// 页面联网的后台工人：ADS-B / Sats / Typhoon / Quake / FX / OKX / GitHub / Weather / Astro
// 这几页的"拉数据"不再在 loop() 上同步跑，挪到一个短命的 FreeRTOS 任务里。
//
// 为什么要挪：原来一次拉取最坏能把主循环钉住几十秒——DNS 最长 15s（框架写死，调不了）、
// TCP 连接 5~15s、TLS 握手 5~10s、读超时 10~20s，Weather 还要连着打三个请求。这期间
// 键盘没人读、GNSS 串口没人收、串口指令没人理、自动熄屏不走、BtnA 没反应，屏幕上只有一帧
// 不动的 "loading"。现在这些都照常转，屏上还有个会动的指示；按返回键会立刻请求取消，
// 手头这一步一结束就走。
//
// ---- 线程边界（要紧） ----
// 工人跑的就是原来那个 fetchXxx()，基本一行没改，所以规矩全压在"忙的时候主线程别碰什么"上：
//   * cv：从 bgFetchRun() 置忙那一刻起主线程一律不碰——fetch 里的 CanvasLease 是在**工人线程**
//     上把画布释放掉的。render() 忙时只直接往 M5.Display 画指示（bgFetchDrawIndicator），
//     canvasRestore() 忙时一律不干活（工人那边 lease 析构时的恢复也是空操作），等收尾时由
//     主线程在任务删掉之后统一恢复——这样工人线程残留的东西（lwIP 的每线程信号量）一定先释放了，
//     画布腾出的那个洞是干净的。
//   * 页面数据：fetch 往各页的静态数组/字符串里写，主线程忙时不画（见上）、不跑各页的 update、
//     按键先排队（见 main.cpp 的 bgKey*），所以没有并发读写。页面也不会在忙时被退出——退出类
//     动作（返回键 / BtnA）记下来，收尾后再执行，所以不存在"exit 把数组 free 了、工人还在写"。
//   * 屏幕：工人线程**绝不画屏**（LovyanGFX 不是线程安全的，主线程正在画指示）。
//     原来 fetch 里的 centerMsg("locating...") 改成 bgFetchStatus()，交给指示去画。
//   * 串口指令：忙时 serialCmdPoll() 不跑，指令留在 USB 接收缓冲里，收尾后照常处理。
//     指令会切屏、会释放/恢复画布（PCMODE、MENU、GOTO……），忙时处理它们等于重新引入上面那些竞争。
//
// ---- 内存 ----
// 栈 12KB，跟 loop 任务一样大（main.cpp 的 SET_LOOP_TASK_STACK_SIZE）——这些拉取原来就在 loop
// 上跑得好好的，而工人栈上没有 loop()/handleKey() 那几层帧，只会更宽裕。栈在 fetch 借走画布
// **之前**分配（建任务的时候），所以不会落进画布的洞里。
// 任务固定在 core 1（跟 loop 同核、同优先级 1，靠时间片轮转）。为什么不像 chat 那样放 core 0：
// IDF 4.4 的 vTaskDelete 删别的核上的任务只会挂进待清理队列，等那个核的 idle 任务有空再释放
// （见 IDF tasks.c vTaskDelete："can't delete a non-running task pinned to the other core"）。
// 同核删一个已挂起的任务则是当场释放栈、TCB 和线程本地存储——收尾时恢复画布才有确定的前提。
// 另一个理由：任务看门狗只盯 CPU0 的 idle（sdkconfig 里 CHECK_IDLE_TASK_CPU0=1，CPU1 不查），
// TLS 握手那几百毫秒的大数运算放在 core 1 上，怎么都饿不着被盯的那个 idle。
//
// ---- 退回路径 ----
// 堆里连 12KB 栈都凑不出来时 bgFetchRun() 退回老办法：在主线程上同步跑完。那条路径跟原来的
// 行为一字不差（包括跑完之后清掉键盘 FIFO），所以调用方不用区分。
#pragma once
#include <stdint.h>

using BgFetchFn = void (*)();

// 主线程调。起得来任务：fn 在后台跑，立即返回 true。起不来：在主线程上同步跑完 fn，返回 false。
// 已经有一个在跑时直接返回 false，什么都不做（调用方在 update 里调，而忙时 update 根本不会跑）。
bool bgFetchRun(BgFetchFn fn);

bool bgFetchBusy();        // 有活在跑（任何线程都能读）
bool bgFetchOnWorker();    // 当前线程是不是工人

// 取消：主线程在用户按了"离开"时调；工人在阶段之间查（net_resolve 每 50ms 查一次，
// 所以卡在 DNS 上的请求会立刻放弃，后续请求也都会在 DNS 这一步直接失败返回）。
void bgFetchCancel();
bool bgFetchCancelled();

// 进度文字（只传字符串字面量/静态串，不拷贝）。工人里调：交给指示去画；
// 同步退回路径（主线程）里调：直接 centerMsg 上屏，跟原来一样。
void bgFetchStatus(const char* msg);

// 主线程每轮 loop 调一次。返回 true = 这一轮刚收完尾（任务已删、画布已恢复），
// 调用方该把忙时攒下的按键/BtnA 重放了。
bool bgFetchService();

// render() 在忙时调：直接往 M5.Display 画右上角的指示（不碰 cv——画布此刻在工人手里）。
// leaving=true 时文字换成 "leaving"（用户已经按了返回，等这一步结束就走）。
void bgFetchDrawIndicator(bool leaving);

// 当画布暂时拿不回来时（render 走 noCanvas 分支），通知后台工人收尾服务开启重试轮询
// （每 250ms 翻一次 dirty，最长 20s），确保画布恢复后能自动唤醒重绘。
void bgFetchPokeRestore();
