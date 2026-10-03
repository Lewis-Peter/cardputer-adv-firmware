#include "ir_proto.h"
#include <cstring>
#include <strings.h>   // strcasecmp：ESP32 的 string.h 顺带给了，桌面上要显式引

// ---- 各协议的时序常数 ----
//
// 数值来源是各协议的公开规范，单位微秒。⚠️ 接收端普遍有 ±25% 的容差，所以这些数字
// 不必精确到个位；真正不能错的是**顺序和位数**——那才是抄错了会整条发不出去的地方，
// 也正是 tools/irtest 逐段比对的东西。
namespace {

// NEC：9ms 引导 mark + 4.5ms space，之后每位都是 560µs mark + (560 / 1690)µs space，
// 最后补一个 560µs 的收尾 mark（不然最后一位的 space 没有边界）。
const uint16_t NEC_HDR_MARK = 9000, NEC_HDR_SPACE = 4500;
const uint16_t NEC_BIT_MARK = 560,  NEC_ONE_SPACE = 1690, NEC_ZERO_SPACE = 560;

// 三星：只有引导脉冲跟 NEC 不一样（4.5/4.5 而不是 9/4.5），位时序完全相同。
const uint16_t SAM_HDR_MARK = 4500, SAM_HDR_SPACE = 4500;

// Sony SIRC：引导 2.4ms mark + 0.6ms space；位是 **mark 变长**（1.2ms=1 / 0.6ms=0），
// 每位后面固定 0.6ms space。载波 40kHz，而且规定一帧要连发 3 次、周期 45ms。
const uint16_t SONY_HDR_MARK = 2400, SONY_SPACE = 600;
const uint16_t SONY_ONE_MARK = 1200, SONY_ZERO_MARK = 600;

// RC5：曼彻斯特，半位 889µs。没有引导脉冲——起始位本身就是同步。
const uint16_t RC5_HALF = 889;

// 往 dur[] 里追加一段，越界就把 ok 打成 false（调用方最后统一检查，中间不用到处判）
struct Builder {
  IrSignal& s;
  bool ok = true;
  explicit Builder(IrSignal& sig) : s(sig) { s.n = 0; }
  void add(uint16_t us) {
    if (s.n >= IR_MAX_DUR) { ok = false; return; }
    s.dur[s.n++] = us;
  }
  // 曼彻斯特用：跟上一段同极性时要合并，否则会出现两段连续的 mark，
  // 发出去就是一段双倍长的 mark，接收端解成完全不同的位。
  void addMerge(uint16_t us, bool isMark) {
    // ⚠️ 帧还没开始时的前导 space 要直接丢掉：dur[0] 按约定必须是 mark，而 RC5 的
    // 起始位是 1、按曼彻斯特就是"先 space 后 mark"。线本来就空闲在低电平，
    // 那半个 space 不丢的话整条波形的极性就整体错开一格。
    if (s.n == 0 && !isMark) return;
    bool nextIsMark = (s.n % 2) == 0;      // 偶数下标是 mark
    if (s.n > 0 && nextIsMark != isMark) { s.dur[s.n - 1] += us; return; }
    add(us);
  }
};

// NEC 家族（NEC / 三星）共用的位循环：MSB 优先，每位 mark 等长、space 长短表示 0/1
void necBits(Builder& b, uint32_t value, uint8_t bits) {
  for (int i = bits - 1; i >= 0; i--) {
    b.add(NEC_BIT_MARK);
    b.add((value >> i) & 1 ? NEC_ONE_SPACE : NEC_ZERO_SPACE);
  }
  b.add(NEC_BIT_MARK);            // 收尾 mark
}

}  // namespace

bool irEncode(IrProto proto, uint32_t value, uint8_t bits, IrSignal& out) {
  Builder b(out);
  out.repeats = 1;
  out.gapMs = 40;

  switch (proto) {
    case IR_NEC:
    case IR_NEC32:
      // IR_NEC 先把 (addr, cmd) 拼成 32 位：addr / ~addr / cmd / ~cmd，
      // 之后跟 IR_NEC32 完全一样。这么拼是为了让手里只有 addr/cmd 的人不用自己算反码。
      if (proto == IR_NEC) {
        uint32_t addr = (value >> 8) & 0xFF, cmd = value & 0xFF;
        value = (addr << 24) | ((~addr & 0xFF) << 16) | (cmd << 8) | (~cmd & 0xFF);
      }
      out.carrierHz = 38000;
      b.add(NEC_HDR_MARK);
      b.add(NEC_HDR_SPACE);
      necBits(b, value, 32);
      break;

    case IR_SAMSUNG:
      out.carrierHz = 38000;
      b.add(SAM_HDR_MARK);
      b.add(SAM_HDR_SPACE);
      necBits(b, value, 32);
      break;

    case IR_SONY:
      if (bits != 12 && bits != 15 && bits != 20) return false;
      out.carrierHz = 40000;
      out.repeats = 3;              // 规范要求；只发一次的话很多设备根本不理
      // gapMs 在下面按实际帧长算：Sony 的 45ms 是"帧头到帧头"，不是帧尾到帧头
      b.add(SONY_HDR_MARK);
      b.add(SONY_SPACE);
      for (int i = bits - 1; i >= 0; i--) {
        b.add((value >> i) & 1 ? SONY_ONE_MARK : SONY_ZERO_MARK);
        if (i) b.add(SONY_SPACE);   // 最后一位后面不补 space：帧间隔本身就是分隔
      }
      break;

    case IR_RC5: {
      // 14 位：起始位 S1=1、S2=1（RC5 扩展里 S2 是命令第 7 位的反）、翻转位、5 位地址、6 位命令。
      // 调用方给的 value 就按这 14 位整体传（跟公开码表的写法一致）。
      //
      // ⚠️ 极性：RC5 的逻辑 1 是 **space 在前、mark 在后**（低到高跳变），0 反过来。
      // 写反了的话每一位都恰好取反，发出去是另一条完全合法的指令——这是 RC5 最容易
      // 静默出错的地方，所以 irtest 里专门有一条按位比对的用例。
      if (bits == 0 || bits > 14) bits = 14;
      out.carrierHz = 36000;
      for (int i = bits - 1; i >= 0; i--) {
        bool one = (value >> i) & 1;
        if (one) { b.addMerge(RC5_HALF, false); b.addMerge(RC5_HALF, true); }
        else     { b.addMerge(RC5_HALF, true);  b.addMerge(RC5_HALF, false); }
      }
      // 帧必须以 mark 结尾才有边界；最后一位是 0 的话结尾正好是 space，去掉它。
      if (out.n && (out.n % 2) == 0) out.n--;
      break;
    }

    case IR_RAW:
    default:
      return false;               // RAW 走 irEncodeRaw
  }

  if (!b.ok || out.n == 0) return false;
  if (proto == IR_SONY) {
    // 帧周期 45ms 是"帧头到帧头"，所以间隔 = 45ms 减去帧本身
    uint32_t us = irSignalDurationUs(out);
    out.gapMs = (uint16_t)(us / 1000 >= 45 ? 1 : 45 - us / 1000);
  }
  return true;
}

bool irEncodeRaw(const uint16_t* durations, int n, uint16_t carrierHz, IrSignal& out) {
  if (!durations || n <= 0 || n > IR_MAX_DUR) return false;
  out.carrierHz = carrierHz ? carrierHz : 38000;
  out.n = (uint16_t)n;
  memcpy(out.dur, durations, (size_t)n * sizeof(uint16_t));
  out.repeats = 1;
  out.gapMs = 40;
  return true;
}

uint32_t irSignalDurationUs(const IrSignal& s) {
  uint32_t t = 0;
  for (int i = 0; i < s.n; i++) t += s.dur[i];
  return t;
}

const char* irProtoName(IrProto p) {
  switch (p) {
    case IR_NEC32:   return "nec32";
    case IR_NEC:     return "nec";
    case IR_SAMSUNG: return "samsung";
    case IR_SONY:    return "sony";
    case IR_RC5:     return "rc5";
    case IR_RAW:     return "raw";
  }
  return "?";
}

bool irProtoFromName(const char* name, IrProto& out) {
  if (!name) return false;
  struct { const char* n; IrProto p; } T[] = {
    {"nec32", IR_NEC32}, {"nec", IR_NEC}, {"samsung", IR_SAMSUNG},
    {"sony", IR_SONY}, {"rc5", IR_RC5}, {"raw", IR_RAW},
  };
  for (auto& e : T) if (!strcasecmp(name, e.n)) { out = e.p; return true; }
  return false;
}
