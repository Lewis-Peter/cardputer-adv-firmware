#include "ir_tx.h"
#include <Arduino.h>
#include "driver/rmt.h"

// 红外发射管在 GPIO44，**只发不收**（HARDWARE.md 确认过；3/4/5/6/13/15 是 Cap LoRa/GPS 占的脚，
// 不是红外）。所以这块板子学不了别人的遥控器，码只能从码表来。
static const gpio_num_t IR_TX_PIN = GPIO_NUM_44;
static const rmt_channel_t IR_CH  = RMT_CHANNEL_3;

// clk_div=80：APB 80MHz / 80 = 1MHz，于是 **1 个 tick 正好 1µs**，dur[] 里的微秒数
// 可以原样填进去，不用换算。RMT 每段的 duration 是 15 位 = 最大 32767 tick，
// 也就是 32.7ms —— 比任何一段红外脉冲都长得多（最长的 NEC 引导才 9ms）。
// tools/irtest 第 8 组就是守这条上界的。
static const uint8_t  IR_CLK_DIV = 80;
static const uint32_t RMT_SRC_HZ = 80000000UL;

static bool  installed = false;
static char  lastErr[40] = "";
static uint32_t curCarrier = 0;

// 全局 TX 完成回调过滤包装器：
// FastLED 在 ESP32 默认会注册全局 rmt_tx_end_callback (doneOnChannel)。
// 而 IDF 的默认 ISR 在任何通道（包括 IR_CH 通道 3）完成时都会调用该回调。
// FastLED 的 doneOnChannel 缺乏空指针判断，收到 IR_CH 会直接引发 LoadProhibited 崩溃重启！
// 通过此 wrapper 拦截来自 IR_CH 的事件，其它通道原样转交给外部注册者。
static rmt_tx_end_callback_t s_orig_tx_end = { nullptr, nullptr };

static void IRAM_ATTR ir_tx_end_filter(rmt_channel_t channel, void *arg) {
  if (channel == IR_CH) {
    // 拦截 IR 发送完成中断，避免转发给 FastLED 等无空指针检查的回调
    return;
  }
  if (s_orig_tx_end.function) {
    s_orig_tx_end.function(channel, s_orig_tx_end.arg);
  }
}

static void setErr(const char* m) {
  strncpy(lastErr, m ? m : "", sizeof(lastErr) - 1);
  lastErr[sizeof(lastErr) - 1] = 0;
}

bool irTxReady() { return installed; }
const char* irTxError() { return lastErr; }

bool irTxBegin() {
  if (installed) return true;
  setErr("");

  rmt_config_t c = RMT_DEFAULT_CONFIG_TX(IR_TX_PIN, IR_CH);
  c.clk_div        = IR_CLK_DIV;
  // ESP32-S3 每个 TX 通道分配 48 words (1 block)，4 个通道共 4 个 block (0~3)。
  // 通道 3 只能使用 1 个 block（Block 3）。写 2 会导致 block 越界。
  c.mem_block_num  = 1;
  c.tx_config.loop_en             = false;
  c.tx_config.carrier_en          = true;
  c.tx_config.carrier_freq_hz     = 38000;
  c.tx_config.carrier_duty_percent = 33; // 33% 比 50% 省电，接收头一样认
  c.tx_config.carrier_level       = RMT_CARRIER_LEVEL_HIGH;
  c.tx_config.idle_output_en      = true;
  c.tx_config.idle_level          = RMT_IDLE_LEVEL_LOW;   // 不发的时候把管子关掉

  if (rmt_config(&c) != ESP_OK)                    { setErr("rmt_config failed"); return false; }
  if (rmt_driver_install(IR_CH, 0, 0) != ESP_OK)   { setErr("rmt_driver_install failed"); return false; }
  installed  = true;
  curCarrier = 38000;
  return true;
}

void irTxEnd() {
  if (!installed) return;
  rmt_driver_uninstall(IR_CH);
  installed = false;
  curCarrier = 0;
}

// 换载波频率。⚠️ high/low 的单位是**分频前的源时钟**（80MHz），不是 clk_div 之后的 1MHz——
// IDF 的 rmt_config() 内部就是这么算的。按 1MHz 去算的话 38kHz 只有 26 个 tick 的分辨率，
// 载波会偏出接收头的通带。80MHz 下是 2105 个 tick，绰绰有余。
static void setCarrier(uint32_t hz) {
  if (hz == curCarrier || hz == 0) return;
  uint32_t div = RMT_SRC_HZ / hz;
  uint16_t hi  = (uint16_t)(div / 3);          // 33%
  uint16_t lo  = (uint16_t)(div - hi);
  if (rmt_set_tx_carrier(IR_CH, true, hi, lo, RMT_CARRIER_LEVEL_HIGH) == ESP_OK) curCarrier = hz;
}

bool irTxSend(const IrSignal& s) {
  if (!installed) { setErr("rmt not installed"); return false; }
  if (s.n == 0)   { setErr("empty signal"); return false; }
  setCarrier(s.carrierHz);

  // 在发射前挂上安全过滤 wrapper，截断来自 IR_CH 的中断触发，保护 FastLED
  rmt_tx_end_callback_t prev = rmt_register_tx_end_callback(ir_tx_end_filter, nullptr);
  if (prev.function != ir_tx_end_filter) {
    s_orig_tx_end = prev;
  }

  // 一个 rmt_item32_t 装两段（level0/duration0 + level1/duration1）。dur[] 是
  // mark/space 交替的，所以偶数下标一律 level=1（有载波）、奇数一律 level=0。
  // 段数是奇数时最后一项的后半段填 0 —— RMT 把 duration=0 当作"到此为止"。
  static rmt_item32_t items[IR_MAX_DUR / 2 + 1];
  int ni = 0;
  for (int i = 0; i < s.n; i += 2) {
    items[ni].level0    = 1;
    items[ni].duration0 = s.dur[i];
    items[ni].level1    = 0;
    items[ni].duration1 = (i + 1 < s.n) ? s.dur[i + 1] : 0;
    ni++;
  }

  uint8_t reps = s.repeats ? s.repeats : 1;
  bool ok = true;
  for (uint8_t r = 0; r < reps; r++) {
    // 第三个参数 true = 等发完再返回。整帧最长几十毫秒，比这个项目里随便一次
    // HTTP 请求都短得多，阻塞主循环这一下可以接受。
    if (rmt_write_items(IR_CH, items, ni, true) != ESP_OK) {
      setErr("rmt_write_items failed");
      ok = false;
      break;
    }
    if (r + 1 < reps && s.gapMs) delay(s.gapMs);
  }

  // 发送完成后恢复原有的 callback，互不影响
  if (s_orig_tx_end.function && s_orig_tx_end.function != ir_tx_end_filter) {
    rmt_register_tx_end_callback(s_orig_tx_end.function, s_orig_tx_end.arg);
  }

  if (ok) setErr("");
  return ok;
}
