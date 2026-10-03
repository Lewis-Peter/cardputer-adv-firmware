// ram_profile 的桌面试跑台。语法检查只能保证"编得过"，这里回答的是另一件事：
// **那张表打出来到底长什么样**——列对不对齐、Δ 的正负号是不是反的、溢出那条提示会不会漏。
// 这些在板子上要烧一次才看得见，而烧一次的代价是这个项目一直在躲的。
//
// 手法跟 tools/irtest、tools/odidtest 一样：编的是**真的** src/ram_profile.cpp，
// 只把它底下的 heap_caps_* 换成一串编好的数（含一段"内存反而变多"的回收，专门试负号）。
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <unistd.h>
#include <cstdlib>

// ---- 假的 Arduino 面 ----
static uint32_t g_ms = 0;
static inline uint32_t millis() { return g_ms; }

struct Stream {
  size_t print(const char* s)   { return fputs(s, stdout), strlen(s); }
  size_t println()              { return fputc('\n', stdout), 1; }
  size_t println(const char* s) { return print(s) + println(); }
  size_t printf(const char* f, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap; va_start(ap, f); int n = vprintf(f, ap); va_end(ap); return n > 0 ? n : 0;
  }
};

// ---- 假的堆 ----
// 一串预先编好的 (free8, largest8, dma)，按调用顺序发。最后那一组比前一组**大**，
// 用来验证 Δ 的正号分支（真机上 bootAnim 之类的临时分配释放掉就会这样）。
struct Trip { uint32_t f, l, d; };
static std::vector<Trip> g_script;
static size_t g_i = 0;
static Trip cur() { return g_i < g_script.size() ? g_script[g_i] : g_script.back(); }
size_t heap_caps_get_free_size(uint32_t)          { return cur().f; }
size_t heap_caps_get_largest_free_block(uint32_t c) { return c == 8 ? cur().d : cur().l; }
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_DMA  8

#include "../../src/ram_profile.cpp"

static int fails = 0;
static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) fails++;
}

// 抓一次 ramDump 的输出，好在上面做断言（不然就只是"看起来对"）
static std::string dumpToString() {
  fflush(stdout);
  char path[] = "/tmp/ramdumpXXXXXX";
  int fd = mkstemp(path);
  FILE* f = fdopen(fd, "w+");
  int saved = dup(1); dup2(fileno(f), 1);
  Stream s; ramDump(s);
  fflush(stdout); dup2(saved, 1); close(saved);
  rewind(f);
  std::string out; char buf[4096]; size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f); remove(path);
  return out;
}

int main() {
  // 数字照着真机的量级编：72K 左右起步，画布吃掉 64K，WiFi 吃掉 36K。
  g_script = {
    {188000, 110000, 90000},   // boot
    {170000,  98000, 84000},   // M5.begin
    {105200,  60000, 52000},   // canvas   -64,800
    {108400,  62000, 54000},   // bootAnim +3,200（动画的临时分配还回来了）
    { 99000,  55000, 48000},   // speaker
    { 62000,  41000, 36000},   // wifi up
  };
  const char* names[] = {"boot", "M5.begin", "canvas", "bootAnim", "speaker", "wifi up"};

  std::string empty = dumpToString();
  check(empty.find("还没有任何阶段标记") != std::string::npos,
        "一个标记都没有时不打空表，直接说清楚");

  for (size_t i = 0; i < g_script.size(); i++) {
    g_i = i; g_ms = (uint32_t)(i * 137 + 12);
    ramMark(names[i]);
  }
  g_i = g_script.size() - 1;

  std::string t = dumpToString();
  fputs(t.c_str(), stdout);

  check(t.find("-64800") != std::string::npos, "画布那 64,800 打成负数（是这一步吃掉的）");
  check(t.find("+3200")  != std::string::npos, "回收的那一步打成正数，符号没写死");
  check(t.find("+126000") == std::string::npos && t.find("-126000") != std::string::npos,
        "合计 = 末行 - 首行 = -126,000");
  // 首行没有上一阶段可比，只能是占位符，不能拿 0 冒充"没变化"
  check(t.find("boot") != std::string::npos && t.find("boot        ") != std::string::npos,
        "首行 stage 列按 %-14s 左对齐补齐");

  // 溢出：再灌 20 个，早期的必须原样留着
  for (int i = 0; i < 20; i++) ramMark("late");
  std::string o = dumpToString();
  check(o.find("丢新的，保开头那几步") != std::string::npos, "溢出后有明确提示");
  check(o.find("canvas") != std::string::npos, "溢出后开头几步没被挤掉");
  // 只数表格行（"[ramlog] late"），别把末尾那句"合计：从 boot 到 late"也算进来
  size_t nLate = 0;
  const std::string row = "[ramlog] late";
  for (size_t p = o.find(row); p != std::string::npos; p = o.find(row, p + 1)) nLate++;
  check(nLate == 16 - g_script.size(), "只补到装满为止（16 格），多的直接丢");

  printf("\n%s (%d fail)\n", fails ? "FAILED" : "all ok", fails);
  return fails ? 1 : 0;
}
