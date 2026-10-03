#include "ram_profile.h"
#include <esp_heap_caps.h>

// 16 个阶段够记完整个 setup() 再留几格给运行期（比如"Wi-Fi 连上了"）。
// 满了就不再记——**丢新的而不是挤掉旧的**：开头那几步（静态段之后、主画布之前）
// 才是这套东西存在的理由，绝不能被后面的覆盖掉。
static const int RAM_MARKS_MAX = 16;

static RamMark marks[RAM_MARKS_MAX];
static int markN = 0;
static bool overflowed = false;

int ramMarkCount() {
  return markN;
}

const RamMark& ramMarkAt(int i) {
  static const RamMark dummy = {"?", 0, 0, 0, 0};
  if (i < 0 || i >= markN) return dummy;
  return marks[i];
}

bool ramMarkOverflowed() {
  return overflowed;
}

void ramMark(const char* stage) {
  if (markN >= RAM_MARKS_MAX) { overflowed = true; return; }
  RamMark& m = marks[markN++];
  m.stage      = stage ? stage : "?";
  m.ms         = millis();
  m.free8      = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
  m.largest8   = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  m.largestDma = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
}

void ramDump(Stream& out) {
  if (markN == 0) { out.println("[ramlog] 还没有任何阶段标记"); return; }

  out.println("[ramlog] 开机各阶段内存轨迹。Δ 是相对**上一阶段**的变化，负数=这一步吃掉的。");
  out.println("[ramlog] 第一行的 free8 约等于\"静态段分配完之后还剩多少\"——想比较两个固件版本");
  out.println("[ramlog] 的静态 RAM 涨了多少，就比这一个数（README 里那个 94,876 -> 119,420）。");
  out.printf("[ramlog] %-14s %7s %9s %9s %9s %9s %9s\n",
             "stage", "t(ms)", "free8", "d-free", "largest8", "d-larg", "dma");

  for (int i = 0; i < markN; i++) {
    const RamMark& m = marks[i];
    if (i == 0) {
      out.printf("[ramlog] %-14s %7lu %9lu %9s %9lu %9s %9lu\n",
                 m.stage, (unsigned long)m.ms, (unsigned long)m.free8, "-",
                 (unsigned long)m.largest8, "-", (unsigned long)m.largestDma);
    } else {
      // 用有符号差值打印：这一列才是整张表的重点，所以带上 + / - 号
      const long dFree = (long)m.free8 - (long)marks[i - 1].free8;
      const long dLarg = (long)m.largest8 - (long)marks[i - 1].largest8;
      out.printf("[ramlog] %-14s %7lu %9lu %+9ld %9lu %+9ld %9lu\n",
                 m.stage, (unsigned long)m.ms, (unsigned long)m.free8, dFree,
                 (unsigned long)m.largest8, dLarg, (unsigned long)m.largestDma);
    }
  }

  if (markN >= 2) {
    const long total = (long)marks[markN - 1].free8 - (long)marks[0].free8;
    out.printf("[ramlog] 合计：从 %s 到 %s 共 %+ld 字节\n",
               marks[0].stage, marks[markN - 1].stage, total);
  }
  if (overflowed)
    out.printf("[ramlog] ⚠️ 标记位置只有 %d 个，后面的被丢掉了（丢新的，保开头那几步）\n",
               RAM_MARKS_MAX);
}
