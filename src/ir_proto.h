// 红外协议编码器：把"哪个协议 + 什么值"变成一串 mark/space 微秒数。
//
// **这个文件里没有一行硬件相关的代码**，故意的：偏移和时序才是这类代码真正会错的地方，
// 而它们在桌面上完全可以验（`tools/irtest`，跟 odidtest 一个路子）。发射那一半
// （RMT + 载波）在 ir.cpp 里，只有插着板子对着电视才能验。
//
// ⚠️ 协议选型是照着**已经公开流传的码表**来的，不是照着教科书：
// 网上能查到的绝大多数电视码写成 "NEC 32 位十六进制"（比如 LG 关机 0x20DF10EF、
// 三星关机 0xE0E040BF），那个值就是按 **MSB 优先**整包发出去的 32 个比特。
// 所以这里的一等公民是 `IR_NEC32(value)` 而不是 `(addr, cmd)`——抄码表的时候
// 少一层换算就少一处出错。手里只有 addr/cmd 的话用 IR_NEC，它自己拼成 32 位再走同一条路。
#pragma once
#include <stdint.h>

enum IrProto {
  IR_NEC32,     // NEC 时序，32 位 MSB 优先。公开码表最常见的形态
  IR_NEC,       // 同上，但由 (addr, cmd) 拼出 addr / ~addr / cmd / ~cmd
  IR_SAMSUNG,   // 三星：引导脉冲是 4500/4500（NEC 是 9000/4500），其余同 NEC32
  IR_SONY,      // SIRC，12/15/20 位 MSB 优先，40kHz，一帧要连发 3 次
  IR_RC5,       // 飞利浦 RC5，曼彻斯特编码，36kHz，14 位
  IR_RAW,       // 直接给 mark/space 序列（SD 码表里抓下来的原始波形）
};

// 一次发射要送出去的全部信息。
// dur[] 是 mark/space **交替**的微秒数，dur[0] 一定是 mark（有载波），dur[1] 是 space，以此类推。
// 长度上限按最长的用例定：RAW 码表里一条整机空调帧能到 200 多段。
static const int IR_MAX_DUR = 260;

struct IrSignal {
  uint16_t carrierHz;      // 38000 / 40000 / 36000
  uint16_t n;              // dur[] 里有效的段数
  uint16_t dur[IR_MAX_DUR];
  uint8_t  repeats;        // 整帧连发几次（Sony 规定 3 次）
  uint16_t gapMs;          // 两次之间的间隔
};

// bits 只对 IR_SONY 有意义（12/15/20）；IR_NEC 的 value 低 8 位是 cmd、次 8 位是 addr。
// 编不出来（协议不认识、位数不合法、段数超上限）返回 false，out 内容未定义。
bool irEncode(IrProto proto, uint32_t value, uint8_t bits, IrSignal& out);

// RAW：直接灌一串微秒数。n 超过 IR_MAX_DUR 或为 0 时返回 false。
bool irEncodeRaw(const uint16_t* durations, int n, uint16_t carrierHz, IrSignal& out);

// 整帧要占多久（微秒），不含重复之间的间隔。给 UI 估"按一下要多久"用。
uint32_t irSignalDurationUs(const IrSignal& s);

const char* irProtoName(IrProto p);
// 名字 -> 协议，给 SD 码表的解析用。认不出来返回 false。
bool irProtoFromName(const char* name, IrProto& out);
