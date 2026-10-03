// 开机各阶段的内存轨迹。
//
// 它回答的问题跟 STAT / MEMCAP **不一样**：那两个报的是"此刻还剩多少"，而这里报的是
// "**是哪一步**吃掉的"。README 里记着静态 RAM 四周内从 94,876 涨到 119,420，
// 只靠快照永远查不出那 24.5KB 花在哪——得有阶段之间的差值。
//
// ⚠️ 两条设计上的取舍，都跟这块板子的具体情况有关：
//
// 1) **先攒着，不当场打。** 参考实现（Bruce 的 ram_profile）是每个阶段直接
//    Serial.printf。但这块板子走的是原生 USB CDC，`Serial.begin()` 在 setup() 里
//    排到第 916 行，在那之前打的东西**没有任何地方收得到**——而 M5.begin() 和
//    主画布那 64KB 恰恰都在那之前，正是最想看的两步。所以先记进一个定长数组，
//    等 serial 起来了（或者随时敲 RAMLOG）再打。
//
// 2) **记录本身不能分配内存。** 一个测内存的东西自己去 malloc，测出来的就是被它
//    扰动过的数。所以数组是静态的、名字只存指针（**只能传字符串字面量**，不复制）。
//
// 静态开销：16 × 20 = 320 字节。这个项目对静态 RAM 很敏感，但换来的是"静态 RAM
// 涨了是谁涨的"这个问题从猜变成可测——而那正是当初记下 94,876→119,420 时缺的东西。
// 所以不做成编译开关：要重新烧一次才能用的诊断，等于没有。
#pragma once
#include <Arduino.h>
#include <stdint.h>

struct RamMark {
  const char* stage;      // 只存指针，见头文件那条"必须是字面量"
  uint32_t ms;
  uint32_t free8;         // MALLOC_CAP_8BIT 的空闲总量
  uint32_t largest8;      // 同一个池的最大连续块
  uint32_t largestDma;    // DMA 池最大连续块
};

// 打一个阶段标记。stage 必须是**字符串字面量**（只存指针，不拷贝）。
void ramMark(const char* stage);

// 把轨迹打出来。串口 RAMLOG 指令调它；也可以在任何想看的时候自己调。
void ramDump(Stream& out);

// 只读访问接口：供 UI 等外部模块结构化读取开机内存轨迹
int ramMarkCount();
const RamMark& ramMarkAt(int i);
bool ramMarkOverflowed();

