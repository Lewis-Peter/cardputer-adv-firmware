#!/usr/bin/env python3
"""memsweep —— 逐个 app 进出，量"进去再出来"净亏多少堆与连续块。

为什么要有它：内存泄漏在源码里几乎是看不出来的。这个项目里每个抢资源的 app 都有
自己的 xxxExit()，也都挂在 cleanupApp() 上——**读代码只能证明"有人写了释放"，
证明不了"释放干净了"**。真正的证据只有一个：进去、退出、看堆有没有回到原位。

为什么要有自适应等待稳定（--settle / --settle-max）：
退出 app 后，释放并非全都是瞬间完成的，有许多**异步、延迟释放**：
  · lwIP 网络协议栈的 ARP 队列（超时期通常需数秒才丢包清理）；
  · TLS 握手/挥手连接的收尾与 socket 销毁；
  · ping 等后台 FreeRTOS 任务在退出后延时 1 秒自删除；
  · WiFi 掉线重连的看门狗与后台 buffers 调度。
如果退出后只等很短的固定时间（例如 0.8s），会把这些"尚未还回"的暂时下降误报成永久泄漏
或碎片；而若一律固定等 15 秒，40 个 app 跑多轮要半个多小时，慢得不可接受。
因此采用自适应探测：
  1. 退出后先等最短时间 --settle（默认 0.8 秒，排空日志并完成基本同步清理）；
  2. 记录刚退出时的第一组读数（heap、largest）；
  3. 随后每隔约 1 秒读一次 STAT，直到连续两次读数的 heap 和 largest 都不变（已稳定），
     或者达到上限 --settle-max（默认 15 秒）；
  4. 判定一律以稳定后的数值为准；刚退出时下降但随后恢复的，标为"暂时下降（N 秒内恢复）"，
     不误报为问题；超时仍不稳定的标为"未稳定"，提示调大 --settle-max。

为什么要有两阶段与长平台期确认（--confirm-wait）：
lwIP 的 TCP 连接断开后会进入 TIME_WAIT 状态（2×MSL，约 120 秒），到期后才真正释放 PCB 缓冲区
（如 DnsFuzz 巡检退出后每轮暂扣 448 字节）。在释放之前堆是完全平的，自适应探测的"连续两次
读数相同即判定稳定"会被这个长达 2 分钟的平台期误导，误判为稳定泄漏。
若让所有 app 每一轮都等 2 分钟，全量 40 个 app 跑 3 轮需要数小时；
因此采用两阶段：
  第一阶段：全量 app 快速自适应扫描（--settle-max 默认 15 秒）；
  第二阶段：仅对第一阶段判为疑似泄漏或碎片化的极少数 app 进行确认（--confirm-wait 默认 150 秒），
           重新进出一次并长时观察，若堆和最大连续块回到进入前水平（允许 ±64 字节误差），
           则改判为"延迟释放（约 N 秒后归还，常见原因 TCP TIME_WAIT）"，不再算问题；
           若超时仍未归还则维持原判定并在报告写明。也可用 --no-confirm 跳过第二阶段。

做法就是 docs/serial-sweep.md 里那套原语：

    MENU   回主菜单，**会走 cleanupApp**（这一条是全部意义所在，别换成 BACK）
    STAT   报 menu=i/N 和 heap/largest/minEver
    /      右键，单字符，静默但生效
    ENTER  进当前选中的 app

每个 app 重复 N 轮，报 **每轮净变化的中位数**。一次性的开销（第一次进去建个缓存、
连一次网）会体现在第 1 轮，之后归零；真泄漏则是**每一轮都掉**，中位数就抓得住。

⚠️ 两个坑（docs/serial-sweep.md 记过，这里直接绕开）：
  · menuIndex 跨进出保留，且菜单是**环形**的，没有"最左"那堵墙。所以定位一律
    先读 STAT 的 menu=，再走 (目标-当前) % N 步，绝不数"从 0 开始第几格"。
  · ENTER 不能用裸的 '\\n'——串口是按行解析的，空行被直接忽略。

自检（不用插板子）：
    python3 tools/memsweep.py --selftest
拿一个假设备跑一遍全流程，验导航算术、解析、两阶段确认和判定：
  · Leaky：每轮漏 2KB 堆，第二阶段确认后仍判泄漏；
  · Frag：总堆完全还回，但最大连续块被永久切碎，第二阶段确认后仍判碎片化；
  · DnsFuzz：120 秒 TIME_WAIT 平台期，第一阶段初判疑似泄漏，第二阶段确认后改判为延迟释放；
  · SlowFree：退出后延迟数步才恢复，判定为暂时下降，不误报；
  · Weather：第 1 轮大额一次性开销，判定为一次性开销，不误报。
"""
import argparse
import re
import statistics
import sys
import time
from typing import NamedTuple

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from serialport import find_port                                    # noqa: E402

STAT_MENU = re.compile(r"\[stat\] menu=(\d+)/(\d+) \((.*?)\) group=(\S+)")
STAT_HEAP = re.compile(r"\[stat\] heap=(\d+) largest=(\d+) minEver=(\d+) screen=(-?\d+)")

# 默认跳过的 app，以及为什么。跳过不是"它们没问题"，是"它们的变化不是泄漏"：
SKIP_DEFAULT = {
    # BLE 那三个每次 deinit 都要付一次约 13KB 的协议栈库泄漏，这是 Arduino BLE 的已知
    # 行为、项目里有意接受的（见 src/bt.cpp btReleaseForOtherApps 的注释）。
    # 混在一起量会把整轮的基线带跑偏。
    "BT Keys", "BT Media", "BLE Scan",
    # Ducky 走 USB HID，会把自己注册成键盘；扫的时候插着电脑，别乱敲。
    "Ducky",
    # Hotspot 的 AP 是**故意**退出后继续开着的，堆当然回不来。
    "Hotspot",
    # Settings 里有格式化 SD 这类破坏性入口，别让自动脚本乱按。
    "Settings",
}


class Sample(NamedTuple):
    """每个 app 单轮进出的采样数据。"""
    before_heap: int
    before_largest: int
    first_heap: int
    first_largest: int
    after_heap: int
    after_largest: int
    settle_sec: float
    timed_out: bool

    @property
    def heap_delta(self) -> int:
        """稳定后的净堆变化（负数 = 泄漏）"""
        return self.after_heap - self.before_heap

    @property
    def largest_delta(self) -> int:
        """稳定后的最大连续块净变化（负数 = 碎片化）"""
        return self.after_largest - self.before_largest

    @property
    def first_heap_delta(self) -> int:
        """刚退出时的净堆变化"""
        return self.first_heap - self.before_heap

    @property
    def first_largest_delta(self) -> int:
        """刚退出时的最大连续块净变化"""
        return self.first_largest - self.before_largest


class ConfirmRecord(NamedTuple):
    """第二阶段长时观察确认结果。"""
    recovered: bool
    recover_sec: float
    before_heap: int
    before_largest: int
    final_heap: int
    final_largest: int
    timed_out: bool
    suspect_type: str  # "leak" 或 "frag"


class Link:
    """串口那一层。selftest 时换成 FakeDevice，其余代码一个字不改。"""

    def __init__(self, port, baud=115200, verbose=False):
        import serial
        self.ser = serial.Serial(port, baud, timeout=0.4)
        self.verbose = verbose
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def send(self, line):
        if self.verbose:
            print(f"    -> {line}")
        self.ser.write((line + "\n").encode())
        self.ser.flush()

    def read_for(self, seconds):
        out, t0 = [], time.time()
        while time.time() - t0 < seconds:
            raw = self.ser.readline()
            if not raw:
                continue
            s = raw.decode("utf-8", "replace").rstrip()
            if self.verbose and s:
                print(f"    <- {s}")
            out.append(s)
        return out

    def sleep(self, seconds):
        time.sleep(seconds)

    def now(self):
        return time.time()


def stat(link, settle=0.6):
    """发一次 STAT，回 (menuIndex, appCount, appName, heap, largest, screen)。"""
    link.send("STAT")
    lines = list(link.read_for(settle))
    menu = heap = None
    # 板子忙的时候（比如 DnsFuzz 还在刷用例日志）STAT 的几行会被夹在别的输出中间、晚一点才到。
    # 别急着报错或重发（重发只会让两份回复交错），接着多读一会儿把剩下的收齐，逐次放宽。
    for extra in (0, 0.8, 1.5, 2.5):
        if extra:
            lines += link.read_for(extra)
        for s in lines:
            m = STAT_MENU.search(s)
            if m:
                menu = (int(m.group(1)), int(m.group(2)), m.group(3))
            m = STAT_HEAP.search(s)
            if m:
                heap = (int(m.group(1)), int(m.group(2)), int(m.group(4)))
        if menu and heap:
            return menu + heap
    raise RuntimeError("STAT 没回全（收到：%r）——板子还在忙？把 --settle 调大" % lines[-8:])


def adaptive_settle(link, settle_min=0.8, settle_max=15.0, step=1.0):
    """退出 app 后的自适应等待：

    1. 先等最短时间 settle_min（期间排空串口日志）；
    2. 读出刚退出时的第 1 次 STAT；
    3. 每隔 step（约 1 秒）读一次 STAT，直到连续两次读数的 (heap, largest) 完全相同（稳定），
       或总等待时间达到 settle_max 上限；
    4. 返回：(first_heap, first_largest, after_heap, after_largest, screen, settle_sec, timed_out)
    """
    t0 = link.now() if hasattr(link, "now") else time.time()
    if settle_min > 0:
        link.read_for(settle_min)

    stat_timeout = min(settle_min, 0.6) if settle_min > 0 else 0.4
    _, _, _, first_h, first_l, screen = stat(link, stat_timeout)
    last_h, last_l = first_h, first_l
    prev_h, prev_l = first_h, first_l
    timed_out = False

    while True:
        now = link.now() if hasattr(link, "now") else time.time()
        elapsed = now - t0
        if elapsed >= settle_max:
            timed_out = True
            break
        wait_sec = min(step, settle_max - elapsed)
        if wait_sec > 0:
            link.read_for(wait_sec)

        _, _, _, h, l, screen = stat(link, stat_timeout)
        last_h, last_l = h, l
        if h == prev_h and l == prev_l:
            # 连续两次读数完全一致，判定为进入稳定平衡
            break
        prev_h, prev_l = h, l

    now = link.now() if hasattr(link, "now") else time.time()
    settle_sec = max(0.0, now - t0)
    return first_h, first_l, last_h, last_l, screen, settle_sec, timed_out


def goto_menu_index(link, target, gap):
    """把菜单光标挪到 target。⚠️ 一律先读当前位置再算步数，见文件顶上那条坑。"""
    cur, total, _, _, _, _ = stat(link)
    steps = (target - cur) % total
    for _ in range(steps):
        link.send("/")
        link.sleep(gap)
    cur2, _, name, _, _, _ = stat(link)
    if cur2 != target:
        raise RuntimeError(f"导航失败：想去 {target}，实际停在 {cur2} ({name})")
    return name


def sweep(link, rounds, dwell, settle, gap, skip, only, settle_max=15.0):
    _, total, _, _, _, _ = stat(link)
    link.send("MENU")
    if settle > 0:
        link.read_for(settle)

    # 先摸清全部 app 的名字：只有走一圈才知道每一格叫什么（APP_COUNT 会随版本变）
    names = []
    for i in range(total):
        names.append(goto_menu_index(link, i, gap))
    print(f"发现 {total} 个 app\n")

    results = {n: [] for n in names}
    for r in range(rounds):
        print(f"--- 第 {r + 1}/{rounds} 轮 " + "-" * 40)
        for i, name in enumerate(names):
            if name in skip or (only and name not in only):
                continue
            goto_menu_index(link, i, gap)
            _, _, _, before, before_l, _ = stat(link)

            link.send("ENTER")
            if dwell > 0:
                link.read_for(dwell)
            link.send("MENU")                   # ⚠️ 必须 MENU 而不是 BACK：只有它走 cleanupApp

            # 自适应等待稳定
            first_h, first_l, after, after_l, screen, settle_sec, timed_out = adaptive_settle(
                link, settle_min=settle, settle_max=settle_max, step=1.0
            )

            if screen != 0:
                print(f"  {name:<12} ⚠️ 没回到主菜单(screen={screen})，跳过这次采样")
                continue

            sample = Sample(
                before_heap=before,
                before_largest=before_l,
                first_heap=first_h,
                first_largest=first_l,
                after_heap=after,
                after_largest=after_l,
                settle_sec=settle_sec,
                timed_out=timed_out,
            )
            results[name].append(sample)

            delta = sample.heap_delta
            delta_l = sample.largest_delta
            first_delta = sample.first_heap_delta
            first_delta_l = sample.first_largest_delta

            # 标记单轮状态
            sec_str = f"{settle_sec:.1f}s" if settle_sec % 1 else f"{int(settle_sec)}s"
            if timed_out:
                flag = f"  <-- ⚠️ 未稳定(>{sec_str})"
            elif delta <= -512:
                flag = f"  <-- 掉了 ({sec_str} 稳定)"
            elif delta_l <= -1024:
                flag = f"  <-- 碎片化 ({sec_str} 稳定)"
            elif (first_delta <= -256 or first_delta_l <= -512) and delta > -256 and delta_l > -512:
                flag = f"  <-- 暂时下降（{sec_str}内恢复）"
            else:
                flag = f"  ({sec_str} 稳定)"

            print(f"  {name:<12} heap {before:>6} -> {after:>6}  ({delta:+6d})"
                  f"  largest {before_l:>6} -> {after_l:>6}  ({delta_l:+6d}){flag}")
        print()
    return results


def confirm_phase(link, results, dwell, gap, confirm_wait=150.0, confirm_step=5.0, verbose=False):
    """第二阶段：只针对第一阶段判为疑似泄漏或碎片化的 app 做长观察期确认。

    为什么只对疑似异常的 app 做确认：
    全量 40 个 app 跑 3 轮若每个都等 150 秒要数小时。绝大多数 app 在 15 秒内就能见分晓，
    唯有 TCP TIME_WAIT 这类协议栈行为具有长达 2 分钟的静止平台期。因此把耗时的长观察集中在
    极少数嫌疑对象上，兼顾全量扫描效率与判定的准确性。
    """
    suspects = {}
    for name, samples in results.items():
        if len(samples) < 2:
            continue
        rest = samples[1:]
        # 超时未稳定的优先建议加大 --settle-max，不在此处确认
        if any(s.timed_out for s in rest):
            continue
        med_h = statistics.median([s.heap_delta for s in rest])
        med_l = statistics.median([s.largest_delta for s in rest])
        if med_h <= -256:
            suspects[name] = ("leak", med_h, med_l)
        elif med_l <= -512:
            suspects[name] = ("frag", med_h, med_l)

    if not suspects:
        print("未发现疑似泄漏或碎片化的 app，跳过第二阶段长观察确认。\n")
        return {}

    print("=" * 96)
    print(f"第二阶段：长观察期确认（针对 {len(suspects)} 个疑似异常 app，观察上限 {confirm_wait:.0f}s）")
    print("=" * 96)

    name_to_index = {name: i for i, name in enumerate(results.keys())}
    confirm_records = {}

    for name, (stype, med_h, med_l) in suspects.items():
        idx = name_to_index[name]
        goto_menu_index(link, idx, gap)
        _, _, _, before_h, before_l, _ = stat(link)

        link.send("ENTER")
        if dwell > 0:
            link.read_for(dwell)
        link.send("MENU")

        # 确保回到主菜单
        _, _, _, _, _, screen = stat(link)
        if screen != 0:
            link.send("MENU")

        t0 = link.now() if hasattr(link, "now") else time.time()
        recovered = False
        recover_sec = 0.0
        last_h, last_l = before_h, before_l
        prev_h, prev_l = None, None

        desc = "疑似泄漏" if stype == "leak" else "疑似碎片化"
        print(f"  {name:<12} [{desc}] 进入长时观察 (上限 {confirm_wait:.0f}s，采样间隔 {confirm_step:.0f}s)...")

        last_print_t = 0.0
        while True:
            now = link.now() if hasattr(link, "now") else time.time()
            elapsed = now - t0
            if elapsed >= confirm_wait:
                break

            wait_sec = min(confirm_step, confirm_wait - elapsed)
            if wait_sec > 0:
                link.read_for(wait_sec)

            now = link.now() if hasattr(link, "now") else time.time()
            elapsed = now - t0

            _, _, _, cur_h, cur_l, screen = stat(link, settle=min(confirm_step, 0.6))
            last_h, last_l = cur_h, cur_l

            delta_h = cur_h - before_h
            delta_l = cur_l - before_l

            # 堆与最大连续块均恢复到进入前水平（允许 ±64 字节抖动与微小系统开销误差）
            if delta_h >= -64 and delta_l >= -64:
                recovered = True
                recover_sec = elapsed
                break

            # 周期性或在数值发生变化时打印心跳，便于观测释放进展
            if (cur_h != prev_h or cur_l != prev_l) or (elapsed - last_print_t >= 30.0):
                sec_str = f"{elapsed:.1f}s" if elapsed % 1 else f"{int(elapsed)}s"
                print(f"               t+{sec_str}: heap {cur_h} ({delta_h:+d})  largest {cur_l} ({delta_l:+d})")
                last_print_t = elapsed
                prev_h, prev_l = cur_h, cur_l

        delta_h = last_h - before_h
        delta_l = last_l - before_l

        if recovered:
            flag = f"<-- 延迟释放（约 {int(round(recover_sec))} 秒后归还，常见原因 TCP TIME_WAIT）"
        elif stype == "leak":
            flag = f"<-- 维持疑似泄漏（已观察 {int(confirm_wait)} 秒仍未归还）"
        else:
            flag = f"<-- 维持碎片化（已观察 {int(confirm_wait)} 秒仍未恢复）"

        print(f"  {name:<12} heap {before_h:>6} -> {last_h:>6}  ({delta_h:+6d})"
              f"  largest {before_l:>6} -> {last_l:>6}  ({delta_l:+6d})  {flag}\n")

        confirm_records[name] = ConfirmRecord(
            recovered=recovered,
            recover_sec=recover_sec,
            before_heap=before_h,
            before_largest=before_l,
            final_heap=last_h,
            final_largest=last_l,
            timed_out=not recovered,
            suspect_type=stype,
        )

    print()
    return confirm_records


def report(results, rounds, confirm_results=None, confirm_wait=150.0):
    if confirm_results is None:
        confirm_results = {}

    print("=" * 96)
    print("每个 app 进出一次的净变化（负数 = 没还回来；判定以稳定后为准）")
    print("=" * 96)
    print(f"{'app':<12}{'heap首轮':>10}{'heap中位':>10}{'larg首轮':>11}{'larg中位':>11}   判定")

    suspects_leak = []
    suspects_frag = []
    unstable_apps = []
    delayed_releases = []

    for name, samples in sorted(results.items()):
        if not samples:
            continue
        first = samples[0]
        rest = samples[1:]

        first_h = first.heap_delta
        first_l = first.largest_delta

        if not rest:
            verdict = "样本不足(--rounds 调大)"
            shown_h = ""
            shown_l = ""
        else:
            med_h = statistics.median([s.heap_delta for s in rest])
            med_l = statistics.median([s.largest_delta for s in rest])
            shown_h = f"{med_h:+.0f}"
            shown_l = f"{med_l:+.0f}"

            # 第二阶段确认覆盖判定：
            if name in confirm_results:
                c = confirm_results[name]
                if c.recovered:
                    sec_str = f"{int(round(c.recover_sec))} 秒"
                    verdict = f"延迟释放（约 {sec_str}后归还，常见原因 TCP TIME_WAIT）"
                    delayed_releases.append((name, c.recover_sec))
                elif c.suspect_type == "leak":
                    verdict = "⚠️ 疑似泄漏（确认后仍未归还）"
                    suspects_leak.append((name, med_h, True))
                else:
                    verdict = "⚠️ 碎片化（确认后仍未恢复）"
                    suspects_frag.append((name, med_l, True))
            else:
                # 第一阶段基础判定（或未开启第二阶段确认时）：
                if any(s.timed_out for s in rest):
                    verdict = "⚠️ 未稳定"
                    max_t = max(s.settle_sec for s in rest if s.timed_out)
                    unstable_apps.append((name, max_t))
                elif med_h <= -256:
                    verdict = "⚠️ 疑似泄漏"
                    suspects_leak.append((name, med_h, False))
                elif med_l <= -512:
                    verdict = "⚠️ 碎片化"
                    suspects_frag.append((name, med_l, False))
                elif any((s.first_heap_delta <= -256 or s.first_largest_delta <= -512) for s in rest):
                    rec_times = [s.settle_sec for s in rest if (s.first_heap_delta <= -256 or s.first_largest_delta <= -512)]
                    max_rec = max(rec_times) if rec_times else max(s.settle_sec for s in rest)
                    sec_str = f"{max_rec:.1f}s" if max_rec % 1 else f"{int(max_rec)}s"
                    verdict = f"暂时下降（{sec_str}内恢复）"
                elif first_h <= -256 or first_l <= -512:
                    verdict = "一次性开销"
                elif med_h <= -64:
                    verdict = "轻微，再多跑几轮确认"
                else:
                    verdict = "ok"

        print(f"{name:<12}{first_h:>+10d}{shown_h:>10}{first_l:>+11d}{shown_l:>11}   {verdict}")
    print()

    has_issues = False
    if suspects_leak:
        has_issues = True
        print("疑似泄漏（总堆持续下降）：")
        for name, med, confirmed in sorted(suspects_leak, key=lambda x: x[1]):
            confirmed_str = f"（已观察 {int(confirm_wait)} 秒仍未归还）" if confirmed else ""
            print(f"  {name}：稳定每轮 {med:+.0f} 字节{confirmed_str}。去看它的 xxxExit() 有没有挂进 "
                  f"main.cpp 的 cleanupApp()，以及有没有漏掉某个子屏。")
        print()

    if suspects_frag:
        has_issues = True
        print("疑似碎片化（总堆已还回，但最大连续块持续缩减）：")
        for name, med, confirmed in sorted(suspects_frag, key=lambda x: x[1]):
            confirmed_str = f"（已观察 {int(confirm_wait)} 秒仍未恢复）" if confirmed else ""
            print(f"  {name}：稳定每轮最大连续块 {med:+.0f} 字节{confirmed_str}。检查是否有交替分配/释放、"
                  f"或小对象未释放切碎了连续大块。")
        print()

    if delayed_releases:
        print("延迟释放（长平台期后归还，不算泄漏）：")
        for name, sec in sorted(delayed_releases):
            sec_str = f"{int(round(sec))} 秒"
            print(f"  {name}：经第二阶段确认，约 {sec_str}后完全归还（常见原因 TCP TIME_WAIT）。")
        print()

    if unstable_apps:
        has_issues = True
        print("超时未稳定：")
        for name, max_t in sorted(unstable_apps):
            print(f"  {name}：在等待上限 {max_t:.1f} 秒内 heap/largest 仍持续变动。"
                  f"建议加大 --settle-max 重新测量。")
        print()

    if not has_issues:
        print("没有发现每轮稳定下降或碎片化的 app。")
        return 0
    return 1


## ---------------------------------------------------------------- 自检
class FakeDevice:
    """假板子：够跑通全流程，用来验导航算术、解析和判定，不用插硬件。

    刻意还原几个真实行为：
      · 菜单是环形的、menuIndex 跨进出保留；
      · Leaky 每进出一次固定漏 2048 字节堆与 1024 字节最大块（真泄漏）；
      · Weather 第一次进出有一笔 48KB 的一次性初始化开销，之后不再掉；
      · SlowFree 退出后前几次读数堆和最大块尚未还回，随后几秒内逐步完全恢复（暂时下降）；
      · Frag 退出后总堆完全还回，但最大连续块被永久切碎 4096 字节（连续块碎片化）；
      · DnsFuzz 退出后进入长达 120 秒的 TIME_WAIT 平整平台期（暂扣 448 字节），到期后全部释放（延迟释放）。
    """

    APPS = ["Time", "IMU", "Files", "Leaky", "Calc", "Weather", "Quake", "IR", "SlowFree", "Frag", "DnsFuzz"]
    ONE_SHOT = {"Weather"}        # 第一次进去建缓存掉一大块，之后不再掉（不是泄漏）

    def __init__(self):
        self.menu = 5             # 故意不从 0 开始：现实里就是上次停在哪就是哪
        self.screen = 0
        self.heap = 120000
        self.largest = 60000
        self.pending = []
        self.seen_first = set()
        self.sim_time = 0.0
        self.slow_free_step = 0
        self.delayed_releases = []  # [(release_time, heap_bytes, largest_bytes)]

    def _flush_delayed_releases(self):
        still_pending = []
        for item in self.delayed_releases:
            rel_time, h_bytes, l_bytes = item
            if self.sim_time >= rel_time:
                self.heap += h_bytes
                self.largest += l_bytes
            else:
                still_pending.append(item)
        self.delayed_releases = still_pending

    def send(self, line):
        cmd = line.strip().upper()
        if cmd == "/":
            self.menu = (self.menu + 1) % len(self.APPS)
        elif cmd == "ENTER":
            self.screen = self.menu + 1
            name = self.APPS[self.menu]
            # 一次性开销：第一次进去建了 TLS 缓冲之类，再也不还，但之后也不再掉。
            # 这是最容易被误判成泄漏的形态，判定逻辑必须能把它跟 Leaky 区分开。
            if name in self.ONE_SHOT and name not in self.seen_first:
                self.seen_first.add(name)
                self.heap -= 48000
                self.largest -= 24000
        elif cmd == "MENU":
            name = self.APPS[self.menu]
            if self.screen != 0:
                if name == "Leaky":
                    self.heap -= 2048          # <- 每轮都掉：这才是泄漏
                    self.largest -= 1024
                elif name == "Frag":
                    # 最大块被永久切碎（碎片化），但总堆完整归还
                    self.largest -= 4096
                elif name == "SlowFree":
                    # 模拟 lwIP ARP 或 TLS 挥手延迟：退出后需经 2 次过渡读数逐步恢复
                    self.slow_free_step = 2
                elif name == "DnsFuzz":
                    # 模拟 lwIP TCP TIME_WAIT (2*MSL, 约 120 秒)：
                    # 退出后进入平整平台期暂扣 448 字节，120 秒到期后才还回 PCB 缓冲区
                    self.heap -= 448
                    self.delayed_releases.append((self.sim_time + 120.0, 448, 0))

                self.heap += (hash((name, self.heap)) % 41) - 20   # 一点噪声
            self.screen = 0
        elif cmd == "STAT":
            self._flush_delayed_releases()
            cur_h = self.heap
            cur_l = self.largest
            if self.slow_free_step > 0:
                if self.slow_free_step == 2:
                    cur_h -= 2000
                    cur_l -= 10000
                elif self.slow_free_step == 1:
                    cur_h -= 1000
                    cur_l -= 5000
                self.slow_free_step -= 1

            self.pending += [
                f"[stat] menu={self.menu}/{len(self.APPS)} ({self.APPS[self.menu]}) group=X",
                f"[stat] heap={cur_h} largest={cur_l} minEver=1000 screen={self.screen}",
            ]

    def read_for(self, seconds):
        if not self.pending:
            self.sim_time += seconds
            self._flush_delayed_releases()
        out, self.pending = self.pending, []
        return out

    def sleep(self, seconds):
        self.sim_time += seconds
        self._flush_delayed_releases()

    def now(self):
        return self.sim_time


def selftest():
    dev = FakeDevice()
    print("=== 第一阶段自检（自适应等待扫描） ===")
    res = sweep(dev, rounds=4, dwell=0, settle=0, gap=0, skip=set(), only=None, settle_max=15.0)

    fails = 0

    def check(cond, what):
        nonlocal fails
        print(("  ok     " if cond else "  FAIL   ") + what)
        if not cond:
            fails += 1

    print("\n第一阶段断言")
    check(set(res) == set(FakeDevice.APPS), "走一圈认全了所有 app（含新增的 DnsFuzz）")
    check(all(len(v) == 4 for v in res.values()), "每个 app 都采到 4 轮")

    dnsfuzz_rest = [s.heap_delta for s in res["DnsFuzz"][1:]]
    check(all(d < -400 for d in dnsfuzz_rest), "DnsFuzz 第一阶段每轮都掉了约 448 字节（处于 TIME_WAIT 平台期）")

    leaky_rest = [s.heap_delta for s in res["Leaky"][1:]]
    check(all(d < -1500 for d in leaky_rest), "Leaky 每一轮都掉了约 2KB")
    check(res["Weather"][0].heap_delta < -40000, "Weather 第 1 轮有一大笔一次性开销")
    check(all(abs(s.heap_delta) < 256 for s in res["Weather"][1:]),
          "Weather 之后几轮回到原位 —— 一次性开销不能被误判成泄漏")

    slowfree_first_drops = [s.first_heap_delta for s in res["SlowFree"][1:]]
    check(all(d <= -1500 for d in slowfree_first_drops), "SlowFree 刚退出时确实有延迟释放导致的暂时堆下降")
    check(all(abs(s.heap_delta) < 256 for s in res["SlowFree"][1:]),
          "SlowFree 稳定后堆完全恢复 —— 暂时下降不被误报为泄漏")
    check(all(abs(s.heap_delta) < 256 for s in res["Frag"][1:]), "Frag 的总堆没有泄漏")
    check(all(s.largest_delta <= -3000 for s in res["Frag"][1:]), "Frag 最大连续块被永久切碎（抓出碎片化）")

    for name in ("Time", "IMU", "Files", "Calc", "Quake", "IR"):
        check(all(abs(s.heap_delta) < 256 for s in res[name][1:]), f"{name} 没有误报")

    # 模拟真实全量扫描耗时（现实中 40 个 app 跑 4 轮耗时数十分钟，先前轮次的 TIME_WAIT 已在后台自然超时释放）
    dev.sleep(200)

    print("\n=== 第二阶段自检（长观察期确认） ===")
    confirm_res = confirm_phase(dev, res, dwell=0, gap=0, confirm_wait=150.0, confirm_step=5.0)

    print("\n第二阶段断言")
    check("DnsFuzz" in confirm_res, "DnsFuzz 第一阶段初判为疑似泄漏，进入第二阶段确认")
    check("Leaky" in confirm_res, "Leaky 第一阶段初判为疑似泄漏，进入第二阶段确认")
    check("Frag" in confirm_res, "Frag 第一阶段初判为碎片化，进入第二阶段确认")
    check("Weather" not in confirm_res, "Weather 判定为一次性开销，不进入第二阶段确认")
    check("SlowFree" not in confirm_res, "SlowFree 判定为暂时下降，不进入第二阶段确认")

    dns_rec = confirm_res.get("DnsFuzz")
    check(dns_rec is not None and dns_rec.recovered, "DnsFuzz 第二阶段确认成功恢复")
    check(dns_rec is not None and 115 <= dns_rec.recover_sec <= 125,
          f"DnsFuzz 恢复用时符合 TIME_WAIT 约 120 秒预期 (实际: {dns_rec.recover_sec if dns_rec else 0:.1f}s)")

    leaky_rec = confirm_res.get("Leaky")
    check(leaky_rec is not None and not leaky_rec.recovered, "Leaky 第二阶段观察 150 秒后仍未归还")

    frag_rec = confirm_res.get("Frag")
    check(frag_rec is not None and not frag_rec.recovered, "Frag 第二阶段观察 150 秒后最大连续块仍未恢复")

    print("\n=== 最终汇总报告与判定断言 ===")
    rc = report(res, 4, confirm_results=confirm_res, confirm_wait=150.0)

    check(rc == 1, "整体判定：抓出了真泄漏(Leaky)与真碎片化(Frag)，DnsFuzz 改判延迟释放不算问题")

    print(f"\n{'全部通过' if not fails else '有失败'}  ({fails} 处失败)")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description="逐个 app 进出，量净堆与最大连续块变化，找内存泄漏和堆碎片")
    ap.add_argument("-p", "--port", help="串口设备；不给就按 VID 0x303A 自动找")
    ap.add_argument("--rounds", type=int, default=3,
                    help="每个 app 进出几轮（默认 3；第 1 轮只算一次性开销，判定用其余轮）")
    ap.add_argument("--dwell", type=float, default=2.0, help="进去之后停留几秒（默认 2）")
    ap.add_argument("--settle", type=float, default=0.8,
                    help="退出 app 后最短等待时间（秒，默认 0.8）")
    ap.add_argument("--settle-max", type=float, default=15.0,
                    help="退出 app 后自适应等待上限（秒，默认 15；达到上限仍未稳定则标为未稳定）")
    ap.add_argument("--gap", type=float, default=0.1,
                    help="连发按键的间隔（默认 0.1；实测 >=0.08 一个都不丢）")
    ap.add_argument("--confirm-wait", type=float, default=150.0,
                    help="第二阶段确认等待上限（秒，默认 150；仅对疑似泄漏或碎片化做长时间观察）")
    ap.add_argument("--confirm-step", type=float, default=5.0,
                    help="第二阶段确认时的采样间隔（秒，默认 5.0）")
    ap.add_argument("--no-confirm", action="store_true",
                    help="跳过第二阶段长时观察确认，直接以第一阶段结果出报告")
    ap.add_argument("--only", nargs="*", help="只测这几个 app（按菜单里的名字）")
    ap.add_argument("--all", action="store_true",
                    help="连默认跳过的那几个也测（BLE/Ducky/Hotspot/Settings，理由见源码）")
    ap.add_argument("--verbose", action="store_true", help="打印收发的每一行")
    ap.add_argument("--selftest", action="store_true", help="拿假设备跑一遍，不用插板子")
    ap.add_argument("--self-test", dest="selftest", action="store_true", help="--selftest 的别名")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    link = Link(find_port(args.port), verbose=args.verbose)
    skip = set() if args.all else SKIP_DEFAULT
    only = set(args.only) if args.only else None
    settle_max = max(args.settle, args.settle_max)
    print("提醒：这一轮会真的进出每个 app。请先拔掉不该被乱按的外设，"
          "并确认 Settings→Debug 的状态条不会干扰你读数。\n")
    res = sweep(link, args.rounds, args.dwell, args.settle, args.gap, skip, only, settle_max=settle_max)

    confirm_res = None
    if not args.no_confirm:
        confirm_res = confirm_phase(
            link, res, dwell=args.dwell, gap=args.gap,
            confirm_wait=args.confirm_wait, confirm_step=args.confirm_step,
            verbose=args.verbose
        )

    return report(res, args.rounds, confirm_results=confirm_res, confirm_wait=args.confirm_wait)


if __name__ == "__main__":
    sys.exit(main())
