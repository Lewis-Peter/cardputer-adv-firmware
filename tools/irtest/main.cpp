// 红外编码器测试台：编译 src/ir_proto.cpp 那份**真代码**，把它吐出来的 mark/space
// 序列逐段对着协议规范比。
//
// 存在理由跟 odidtest 一样：手上没有示波器、也没有红外接收头（这块板子**只有发射**，
// 见 HARDWARE.md），所以"发出去的波形对不对"在实机上根本观测不到——按一下电视没反应，
// 你不知道是编码错了、载波错了、还是没对准。把纯编码那一半拉到桌面上验，
// 至少能把问题范围压到"硬件那一半"。
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include "../../src/ir_proto.h"

static int fails = 0;
static void ok(bool cond, const char* what) {
  printf("  %s   %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) fails++;
}

static void okEq(long got, long want, const char* what) {
  bool c = (got == want);
  if (c) printf("  ok     %s\n", what);
  else { printf("  FAIL   %s (got %ld, want %ld)\n", what, got, want); fails++; }
}

// 把整条波形按段打出来，比对失败时肉眼查用
static void dump(const IrSignal& s) {
  printf("        carrier=%uHz n=%u repeats=%u gap=%ums  ",
         s.carrierHz, s.n, s.repeats, s.gapMs);
  for (int i = 0; i < s.n && i < 16; i++) printf("%s%u", i ? "," : "", s.dur[i]);
  if (s.n > 16) printf(",...");
  printf("\n");
}

// 从 mark/space 序列**反解**回比特，这是最能证明编码正确的做法：
// 不是把常数抄两遍对比，而是走一遍接收端会走的判决逻辑。
static bool decodeNec(const IrSignal& s, uint32_t& out) {
  if (s.n != 2 + 64 + 1) return false;
  if (s.dur[0] < 8000 || s.dur[0] > 10000) return false;      // 引导 mark
  if (s.dur[1] < 4000 || s.dur[1] > 5000) return false;       // 引导 space
  out = 0;
  for (int i = 0; i < 32; i++) {
    uint16_t mark = s.dur[2 + i * 2], space = s.dur[3 + i * 2];
    if (mark < 400 || mark > 800) return false;               // 每位的 mark 等长
    out = (out << 1) | (space > 1000 ? 1u : 0u);              // space 长 = 1
  }
  return true;
}

static bool decodeSony(const IrSignal& s, int bits, uint32_t& out) {
  if (s.n != 2 + bits * 2 - 1) return false;
  if (s.dur[0] < 2000 || s.dur[0] > 2800) return false;
  if (s.dur[1] < 400 || s.dur[1] > 800) return false;
  out = 0;
  for (int i = 0; i < bits; i++) {
    uint16_t mark = s.dur[2 + i * 2];
    out = (out << 1) | (mark > 900 ? 1u : 0u);                // mark 长 = 1
  }
  return true;
}

// RC5 反解：把波形铺成 889µs 一格的电平序列，再两格一位按跳变方向判。
static bool decodeRc5(const IrSignal& s, int bits, uint32_t& out) {
  std::vector<int> lvl;                       // 1=mark, 0=space，每格 889µs
  // 起始位是 1，波形以 mark 开头，前面那半格 space 被编码器丢掉了，这里补回来
  lvl.push_back(0);
  for (int i = 0; i < s.n; i++) {
    int cells = (s.dur[i] + 400) / 889;       // 四舍五入到半位格数
    if (cells < 1) return false;
    for (int c = 0; c < cells; c++) lvl.push_back((i % 2) == 0 ? 1 : 0);
  }
  while ((int)lvl.size() < bits * 2) lvl.push_back(0);   // 末位是 0 时结尾的 space 被裁掉了
  if ((int)lvl.size() < bits * 2) return false;
  out = 0;
  for (int i = 0; i < bits; i++) {
    int a = lvl[i * 2], b = lvl[i * 2 + 1];
    if (a == b) return false;                 // 曼彻斯特每位必须有跳变
    out = (out << 1) | (b == 1 ? 1u : 0u);    // 低->高 = 1
  }
  return true;
}

int main() {
  IrSignal s;

  printf("1. NEC32 —— 公开码表最常见的形态\n");
  {
    // LG 关机，网上到处都是这个值
    ok(irEncode(IR_NEC32, 0x20DF10EF, 32, s), "编码成功");
    okEq(s.carrierHz, 38000, "载波 38kHz");
    okEq(s.n, 2 + 64 + 1, "段数 = 引导2 + 32位×2 + 收尾1");
    okEq(s.dur[0], 9000, "引导 mark 9000us");
    okEq(s.dur[1], 4500, "引导 space 4500us");
    okEq(s.dur[s.n - 1], 560, "结尾是 560us 的收尾 mark");
    uint32_t back = 0;
    ok(decodeNec(s, back), "能按接收端的判决逻辑反解");
    okEq((long)back, (long)0x20DF10EF, "反解回原值（MSB 优先）");
    dump(s);
  }

  printf("2. NEC (addr, cmd) —— 自动补反码\n");
  {
    ok(irEncode(IR_NEC, (0x04 << 8) | 0x08, 32, s), "编码成功");
    uint32_t back = 0;
    ok(decodeNec(s, back), "反解成功");
    okEq((long)back, (long)0x04FB08F7, "addr/~addr/cmd/~cmd 拼对了");
    // 反码那两个字节是 NEC 的校验，接收端会核对；拼错了设备直接丢包
    okEq((long)((back >> 24) & 0xFF), 0x04, "字节0 = addr");
    okEq((long)((back >> 16) & 0xFF), 0xFB, "字节1 = ~addr");
    okEq((long)((back >> 8) & 0xFF), 0x08, "字节2 = cmd");
    okEq((long)(back & 0xFF), 0xF7, "字节3 = ~cmd");
  }

  printf("3. 三星：只有引导脉冲跟 NEC 不同\n");
  {
    ok(irEncode(IR_SAMSUNG, 0xE0E040BF, 32, s), "编码成功");
    okEq(s.dur[0], 4500, "引导 mark 4500us（NEC 是 9000）");
    okEq(s.dur[1], 4500, "引导 space 4500us");
    okEq(s.n, 2 + 64 + 1, "段数跟 NEC 一样");
    // 位时序完全相同，所以除了引导脉冲，两条波形应该逐段相等
    IrSignal nec;
    ok(irEncode(IR_NEC32, 0xE0E040BF, 32, nec), "同值的 NEC32 也编得出");
    bool same = true;
    for (int i = 2; i < s.n; i++) if (s.dur[i] != nec.dur[i]) same = false;
    ok(same, "引导之后逐段跟 NEC 完全一致");
  }

  printf("4. Sony SIRC 12/15/20 位\n");
  {
    ok(irEncode(IR_SONY, 0xA90, 12, s), "12 位编码成功");
    okEq(s.carrierHz, 40000, "载波 40kHz（不是 38k）");
    okEq(s.repeats, 3, "规范要求连发 3 次");
    okEq(s.dur[0], 2400, "引导 mark 2400us");
    okEq(s.n, 2 + 12 * 2 - 1, "段数 = 引导2 + 12位×2 - 末尾那个 space");
    uint32_t back = 0;
    ok(decodeSony(s, 12, back), "反解成功");
    okEq((long)back, 0xA90, "反解回原值");
    // 帧头到帧头 45ms：gap 应该是 45ms 减去帧长
    uint32_t us = irSignalDurationUs(s);
    okEq((long)(s.gapMs + us / 1000), 45, "gap + 帧长 = 45ms（帧头到帧头）");
    dump(s);

    ok(irEncode(IR_SONY, 0x1234, 15, s) && decodeSony(s, 15, back) && back == 0x1234, "15 位往返一致");
    ok(irEncode(IR_SONY, 0x1E3A5, 20, s) && decodeSony(s, 20, back) && back == 0x1E3A5, "20 位往返一致");
    ok(!irEncode(IR_SONY, 0x1, 13, s), "13 位不是合法位数，必须拒绝");
  }

  printf("5. RC5 曼彻斯特 —— 极性写反了会静默变成另一条合法指令\n");
  {
    // 起始位 11 + 翻转位 0 + 地址 00000 + 命令 001100(=12)
    const uint32_t V = 0x300C;   // 0b11'0'00000'001100
    ok(irEncode(IR_RC5, V, 14, s), "编码成功");
    okEq(s.carrierHz, 36000, "载波 36kHz");
    okEq(s.dur[0], 889, "第一段是起始位后半格的 mark（前半格 space 被丢掉）");
    uint32_t back = 0;
    ok(decodeRc5(s, 14, back), "按曼彻斯特反解成功");
    okEq((long)back, (long)V, "反解回原值 —— 极性没写反");
    // 整帧 14 位 × 2 × 889us ≈ 24.9ms，这是 RC5 的标志性长度
    uint32_t us = irSignalDurationUs(s);
    ok(us > 23000 && us < 25000, "整帧长度落在 24.9ms 附近");
    // 连续同电平必须被合并成一段：否则会出现两段相邻的 mark，发出去是双倍长脉冲
    bool merged = true;
    for (int i = 0; i + 1 < s.n; i++) if (s.dur[i] == 0) merged = false;
    ok(merged, "没有空段（相邻同极性已合并）");
    dump(s);

    // 全 0 命令：结尾正好是 space，必须被裁掉，否则帧尾没有边界
    ok(irEncode(IR_RC5, 0x3000, 14, s), "命令全 0 也编得出");
    okEq(s.n % 2, 1, "段数是奇数 = 以 mark 收尾");
    ok(decodeRc5(s, 14, back) && back == 0x3000, "全 0 命令往返一致");
  }

  printf("6. RAW 和边界\n");
  {
    uint16_t raw[] = {9000, 4500, 560, 560, 560, 1690, 560};
    ok(irEncodeRaw(raw, 7, 38000, s), "RAW 灌入成功");
    okEq(s.n, 7, "段数原样");
    okEq(s.dur[5], 1690, "内容原样");
    okEq((long)irSignalDurationUs(s), 9000 + 4500 + 560 + 560 + 560 + 1690 + 560, "总时长 = 各段之和");

    ok(!irEncodeRaw(raw, 0, 38000, s), "0 段拒绝");
    ok(!irEncodeRaw(nullptr, 4, 38000, s), "空指针拒绝");
    std::vector<uint16_t> big(IR_MAX_DUR + 1, 500);
    ok(!irEncodeRaw(big.data(), IR_MAX_DUR + 1, 38000, s), "超过 IR_MAX_DUR 拒绝（别越界写）");
    ok(irEncodeRaw(big.data(), IR_MAX_DUR, 38000, s), "正好 IR_MAX_DUR 收下");
    ok(!irEncode(IR_RAW, 0, 0, s), "IR_RAW 不能走 irEncode");
  }

  printf("7. 协议名字表（SD 码表靠它认协议）\n");
  {
    IrProto p;
    ok(irProtoFromName("nec", p) && p == IR_NEC, "nec");
    ok(irProtoFromName("NEC32", p) && p == IR_NEC32, "大小写不敏感");
    ok(irProtoFromName("sony", p) && p == IR_SONY, "sony");
    ok(irProtoFromName("rc5", p) && p == IR_RC5, "rc5");
    ok(irProtoFromName("samsung", p) && p == IR_SAMSUNG, "samsung");
    ok(irProtoFromName("raw", p) && p == IR_RAW, "raw");
    ok(!irProtoFromName("nope", p), "不认识的名字要拒绝，不能猜");
    ok(!irProtoFromName(nullptr, p), "空指针不能崩");
    // 名字表和枚举必须一一对上，否则 SD 码表写得出、存回去读不出来
    for (int i = IR_NEC32; i <= IR_RAW; i++) {
      IrProto q;
      ok(irProtoFromName(irProtoName((IrProto)i), q) && q == (IrProto)i,
         irProtoName((IrProto)i));
    }
  }

  printf("8. 每一段都要落在 RMT 能表示的范围内\n");
  {
    // ir.cpp 用 1µs/tick 的 RMT，每段 15 位 = 最大 32767µs。NEC 的 9000 是最长的一段，
    // 但 RAW 码表可能塞进更长的静默段——这条是给将来加协议时的护栏。
    struct { IrProto p; uint32_t v; uint8_t b; } cases[] = {
      {IR_NEC32, 0xFFFFFFFF, 32}, {IR_NEC32, 0x00000000, 32},
      {IR_SAMSUNG, 0xFFFFFFFF, 32}, {IR_SONY, 0xFFFFF, 20}, {IR_RC5, 0x3FFF, 14},
    };
    bool allFit = true;
    for (auto& c : cases) {
      if (!irEncode(c.p, c.v, c.b, s)) { allFit = false; continue; }
      for (int i = 0; i < s.n; i++) if (s.dur[i] > 32767) allFit = false;
    }
    ok(allFit, "全 1 / 全 0 的极端值下每段都 <= 32767us");
  }

  printf("\n%s  (%d 处失败)\n", fails ? "有失败" : "全部通过", fails);
  return fails ? 1 : 0;
}
