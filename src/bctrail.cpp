#include "bctrail.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

RTC_NOINIT_ATTR volatile uint8_t  bcTrail[64];
RTC_NOINIT_ATTR volatile uint8_t  bcTrailIndex;
RTC_NOINIT_ATTR volatile uint32_t bcTrailMagic;

static constexpr uint32_t BC_TRAIL_MAGIC = 0x42435452u;   // "BCTR"
static volatile uint32_t bcHeartbeat = 0;
static TaskHandle_t bcWatchdogTaskHandle = nullptr;

void bcTrailInit() {
  // 冷启动（断电上电）时 RTC 内容是垃圾，magic 对不上就清零。软复位时 magic 还在，
  // 于是上一轮的轨迹原样保留——这正是要的。
  if (bcTrailMagic != BC_TRAIL_MAGIC) {
    for (uint8_t i = 0; i < 64; ++i) bcTrail[i] = 0;
    bcTrailIndex = 0;
    bcTrailMagic = BC_TRAIL_MAGIC;
  }
}

void bcTrailDump(Stream& out, const char* label) {
  // 从 next 开始按环形顺序打，所以输出天然是**从旧到新**，最后一个就是卡死前的最后一步。
  uint8_t next = bcTrailIndex;
  out.printf("[bctrail] %s next=%u codes:", label, (unsigned)next);
  for (uint8_t n = 0; n < 64; ++n) {
    uint8_t pos = (uint8_t)((next + n) & 63u);
    out.printf(" %02X", (unsigned)bcTrail[pos]);
  }
  out.println();
}

void bcTrailDumpPrevious(Stream& out) {
  // 正常开机也会走这里；没有有效 RTC 内容时 bcTrailInit() 已清成全零。
  bcTrailDump(out, "previous/reset");
}

void bcTrailHeartbeat() {
  ++bcHeartbeat;
}

static void bcTrailWatchdog(void*) {
  uint32_t last = bcHeartbeat;
  uint32_t unchangedSince = millis();
  bool reported = false;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    uint32_t now = bcHeartbeat;
    if (now != last) {
      last = now;
      unchangedSince = millis();
      reported = false;
      continue;
    }
    if (!reported && (uint32_t)(millis() - unchangedSince) >= 5000u) {
      // 主循环若正卡在某处，USB-CDC 还能不能发要看 USB 任务和主机；这里先试着即时输出，
      // 发不出去也没关系——复位后的 setup() 还会从 RTC 回放同一份轨迹。
      Serial.println("[bctrail] loop heartbeat stalled for 5s");
      bcTrailDump(Serial, "stalled");
      reported = true;
    }
  }
}

// ⚠️ 优先级刻意压在 WiFi(23) 上面：这个任务的全部意义就是"别人都卡住时它还得能跑"，
// 放低了会跟着一起被饿死。代价是每 250ms 醒一次，以及 3072 字节栈**常驻**——这块板子
// 没 PSRAM，所以只在 Debug 开着时才启（见 main.cpp）。起了就不再停：任务自身随时可能
// 正在 printf，删它不安全，关掉 Debug 只是不再新起，已起的留到重启。
void bcTrailStartWatchdog() {
  if (bcWatchdogTaskHandle != nullptr) return;
  xTaskCreatePinnedToCore(bcTrailWatchdog, "bc-trail", 3072, nullptr,
                          configMAX_PRIORITIES - 1, &bcWatchdogTaskHandle, 0);
}
