// 板载 RGB LED（数据 GPIO21，SK6812，藏在主控芯片贴纸底下）。
// ADV：LED 电源(PWR_EN)跟屏幕背光同一条 GPIO38，由 M5GFX 背光 LEDC 管——勿自行驱动 GPIO38，
// 否则背光关不掉/亮度失效。屏亮时灯有电、熄屏时灯随背光灭——详见 led.cpp 顶部注释。
#pragma once
#include <cstdint>

enum LedMode {
  LED_MODE_OFF = 0,      // 关闭
  LED_MODE_BATTERY = 1,  // 电量指示（默认值）
  LED_MODE_BREATHE = 2,  // 呼吸灯
  LED_MODE_RAINBOW = 3,  // 彩虹渐变
  LED_MODE_CHASE   = 4,  // 跑马/闪烁提醒
  LED_MODE_MUSIC   = 5,  // 音乐律动
  LED_MODE_COUNT
};

void ledInit();        // 开机：读 NVS 状态并点亮/熄灭
void ledSet(bool on);  // 开/关（存 NVS）
void ledUpdate();      // 每帧调用：按模式刷新灯效
void ledApply();       // 立刻按当前状态刷新 LED（唤醒/状态切换时直接调）
bool ledIsOn();

LedMode ledGetMode();
void ledSetMode(LedMode mode);
void ledCycleMode();   // 循环切换模式 (OFF -> 1..5 -> OFF)
const char* ledModeName(LedMode m);
const char* ledModeName();

// 外部临时接管 LED（比如 Spectrum 屏的麦克风律动）：开启后 ledUpdate() 让路，不再跑
// 本地灯效逻辑；ledShowRGB() 才能生效。关闭时自动恢复当前模式。
void ledSetOverride(bool on);
void ledShowRGB(uint8_t r, uint8_t g, uint8_t b);
bool ledOverrideActive();   // 已经有人接管着（比如频谱律动）就别再抢，抢了会把对方的接管一起释放掉
