#pragma once

#include <Arduino.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 面包屑：给"卡死了但看不见现场"的问题用的取证设施
//
// 出身是查 BLE 拆除路径那个偶发卡死（见 docs/ble-teardown.md）。当时先用 Serial 插桩，
// 结果 15 轮一次没复现——`Serial.flush()` 太慢，把竞态窗口整个撑开，**插桩本身把故障
// 掩盖掉了**。所以记录动作必须便宜到不扰动时序，且要能在整机卡死、串口发不出去的
// 情况下活到下一次复位。
//
// 两件事，可以分开用：
//   ① 面包屑环形缓冲——`bcMark(code)` 埋在可疑路径上，卡死复位后读出来看走到哪一步。
//      存在 RTC slow memory 里，**不占 DRAM 堆**（这块板子没 PSRAM，这点很要紧）。
//   ② 主循环停滞看门狗——`bcTrailStartWatchdog()`，5 秒没心跳就把面包屑打出来。
//
// 用法：在怀疑的路径上撒 `bcMark(0x01)`、`bcMark(0x02)`…（编号自己约定，写清注释），
// 卡死后按复位，开机时 setup() 会自动回放上一轮的轨迹；也可以随时用串口 `TRAIL` 读。
// 判读要点是**看哪个编号没出现**——最后出现的那个之后就是卡住的地方。
// ---------------------------------------------------------------------------

// RTC_NOINIT_ATTR 让这几个字节在软件复位后继续存在；断电/上电时不保证内容有效，
// 所以由 bcTrailInit() 用 magic 判断。
extern RTC_NOINIT_ATTR volatile uint8_t  bcTrail[64];
extern RTC_NOINIT_ATTR volatile uint8_t  bcTrailIndex;
extern RTC_NOINIT_ATTR volatile uint32_t bcTrailMagic;

// 这是故障路径上的唯一记录动作：一个字节写入 + 一个字节下标递增。
// ⚠️ 禁止在这里加锁、printf、Serial、delay 或任何可能阻塞的操作——那正是上一轮
//    失败的原因。这个函数便宜到可以留在生产路径上。
inline void bcMark(uint8_t code) {
  bcTrail[bcTrailIndex] = code;
  bcTrailIndex = (uint8_t)((bcTrailIndex + 1u) & 63u);
}

void bcTrailInit();                              // setup() 里调一次，Serial.begin() 之后
void bcTrailDump(Stream& out, const char* label);
void bcTrailDumpPrevious(Stream& out);           // 开机回放上一轮（正常开机就是全零）
void bcTrailHeartbeat();                         // loop() 每帧调，喂看门狗
void bcTrailStartWatchdog();                     // 幂等；只在 Debug 开着时启，见 .cpp
