#include "led.h"
#include "globals.h"
#include "power_util.h"
#include "player.h"
#include "radio.h"
#include <cmath>
#define FASTLED_RMT_BUILTIN_DRIVER 1     // 用 ESP32 RMT 内置驱动（跟 Bruce 一致，减少 RMT 冲突）
#define FASTLED_RMT_MAX_CHANNELS 1
#include <FastLED.h>

// 板载灯珠 SK6812（数据 GPIO21，藏在芯片贴纸底下）。当普通 3 字节 SK6812/GRB 驱动。
//
// ⚠️ ADV 的坑：灯珠电源(PWR_EN) 跟屏幕背光是同一条 GPIO38（官方引脚表 G38 = DISP_BL + RGB LED
// PWR_EN 共用），M5GFX 用 LEDC PWM 占着 G38 控背光。灯珠要"常亮 HIGH"才有稳定供电——普通亮度下
// 背光 PWM 在斩波，灯珠电源被斩得点不亮；而如果自己 pinMode/digitalWrite 抢 G38 钉常亮，又会让
// 亮度调节/自动熄屏失效（熄屏时屏关不掉，扫描时像"卡死")。
//
// 解法：不抢引脚，直接把背光亮度设成 255——LEDC 100% 占空 = G38 定常 HIGH = 灯珠有电（顺带背光
// 满亮）。灯关时把亮度还原成用户设定；熄屏时 main 把亮度设 0，屏和灯一起灭（同网，正常）。
// 代价：亮灯期间屏幕固定满亮、亮度滑条不生效——可接受（点灯本就是用户主动开的模式）。
//
// 支持模式：
// 1. 电量指示：>50% 绿、20~50% 橙、<20% 红；充电中呼吸绿。
// 2. 呼吸灯：平滑正弦二次伽马单色呼吸（周期 3.5s）。
// 3. 彩虹渐变：HSV 色相平滑流动。
// 4. 跑马/闪烁：多色阶跃切换与节拍顿挫。
// 5. 音乐律动：播放时随播放器/收音机电平跳动，静止时回退至呼吸灯。
static const int RGB_LED_PIN = 21;   // 数据（电源 G38 由 M5GFX 背光管，这里只借"满亮=常HIGH"供电）
static CRGB leds[1];
static LedMode ledMode = LED_MODE_OFF;
static bool begun = false;
static bool ledForcedBL = false;     // 是否为了供电把背光钉成了 255

static int      lastBucket = -99;
static uint32_t lastShowMs = 0;
static bool     lastScreenOff = false;
static LedMode  lastMode = LED_MODE_OFF;
static bool     overrideActive = false;   // 外部(如 Spectrum 律动)临时接管 LED 时为 true
static float    vuEnvelope = 0.0f;

static void ledBegin() {
  if (begun) return;
  FastLED.addLeds<SK6812, RGB_LED_PIN, GRB>(leds, 1);
  FastLED.setBrightness(255);
  begun = true;
}

static int levelBucket(int lvl) {
  if (lvl < 0)   return -1;
  if (lvl <= 20) return 0;
  if (lvl <= 50) return 1;
  return 2;
}

static CRGB bucketColor(int bucket) {
  switch (bucket) {
    case 0:  return CRGB(255,   0, 0);   // 低：红
    case 1:  return CRGB(255,  90, 0);   // 中：橙
    case 2:  return CRGB(0,   255, 0);   // 高：绿
    default: return CRGB(255, 255, 255); // 未知：白
  }
}

// 呼吸灯：平滑正弦平方曲线消除线性锯齿感，周期 ~3.5 秒
static void renderBreathe(uint32_t now) {
  float ph = (float)(now % 3500) / 3500.0f;
  float s  = (sinf(ph * 2.0f * (float)M_PI - (float)M_PI_2) + 1.0f) * 0.5f;
  float g  = s * s;  // 二次方模拟人眼对亮度的对数响应
  uint8_t v = (uint8_t)(2.0f + g * 253.0f);
  leds[0] = CRGB(0, (uint8_t)(v * 200 / 255), v); // 冰蓝单色
}

// 彩虹渐变：HSV 色相缓慢循环
static void renderRainbow(uint32_t now) {
  uint8_t hue = (uint8_t)((now / 24) & 0xFF);
  leds[0] = CHSV(hue, 255, 255);
}

// 跑马/闪烁提醒：板载单灯以鲜明色彩依次切换，边缘带顿挫
static const CRGB CHASE_COLORS[] = {
  CRGB(255,   0,   0),   // 红
  CRGB(255, 120,   0),   // 橙
  CRGB(255, 220,   0),   // 黄
  CRGB(  0, 255,   0),   // 绿
  CRGB(  0, 220, 255),   // 青
  CRGB(  0,   0, 255),   // 蓝
  CRGB(220,   0, 255)    // 紫
};
static const int CHASE_COUNT = sizeof(CHASE_COLORS) / sizeof(CHASE_COLORS[0]);

static void renderChase(uint32_t now) {
  int step = (now / 220) % CHASE_COUNT;
  uint32_t sub = now % 220;
  if (sub > 180) {
    // 简短回落，突出单灯颜色切换的节拍顿挫
    leds[0] = CHASE_COLORS[step];
    leds[0].nscale8_video(80);
  } else {
    leds[0] = CHASE_COLORS[step];
  }
}

// 音乐律动：播放器/收音机电平驱动，不播放时退回呼吸灯
static void renderMusic(uint32_t now) {
  // 电台连接/预缓冲阶段 radioIsActive() 已经为真但电平一直是 0：超过 1 秒没声音就当没在放，退回呼吸灯
  static uint32_t lastSoundMs = 0;
  bool playing = playerIsPlaying() || radioIsActive();
  uint8_t rawVu = !playing ? 0 : (playerIsPlaying() ? playerGetVuLevel() : radioGetVuLevel());
  if (rawVu > 0) lastSoundMs = now;
  if (!playing || now - lastSoundMs > 1000) {
    renderBreathe(now);
    return;
  }
  float target = rawVu / 100.0f;
  if (target > vuEnvelope) {
    vuEnvelope = target;
  } else {
    vuEnvelope = vuEnvelope * 0.70f + target * 0.30f;
  }
  uint8_t v = (uint8_t)(25 + vuEnvelope * 230.0f);
  if (vuEnvelope > 0.70f) {
    leds[0] = CRGB(v, 0, 0);                       // 高响度：红
  } else if (vuEnvelope > 0.35f) {
    leds[0] = CRGB(v, (uint8_t)(v * 0.7f), 0);     // 中响度：橙黄
  } else {
    leds[0] = CRGB(0, v, 0);                       // 低响度：绿
  }
}

static void renderEffect(uint32_t now) {
  switch (ledMode) {
    case LED_MODE_BATTERY:
      if (powerCharging()) {
        float ph = (now % 2000) / 2000.0f;
        float s  = (sinf(ph * 2.0f * (float)M_PI) + 1.0f) * 0.5f;   // 0~1
        uint8_t v = 25 + (uint8_t)(s * 230.0f);
        leds[0] = CRGB(0, v, 0);       // 呼吸绿
      } else {
        leds[0] = bucketColor(levelBucket(powerBatteryLevel()));
      }
      break;
    case LED_MODE_BREATHE:
      renderBreathe(now);
      break;
    case LED_MODE_RAINBOW:
      renderRainbow(now);
      break;
    case LED_MODE_CHASE:
      renderChase(now);
      break;
    case LED_MODE_MUSIC:
      renderMusic(now);
      break;
    default:
      leds[0] = CRGB::Black;
      break;
  }
}

// 按当前 熄屏/开关/电量/充电 状态，把背光(供电)和灯色刷新到位
void ledApply() {
  ledBegin();

  if (screenOff) {                 // 熄屏中：屏和灯都由 main 把背光设 0 决定，别碰
    ledForcedBL = false;           // 醒来后需要重新钉一次 255
    return;
  }

  if (overrideActive) {
    // 外部接管中（如频谱律动）：唤醒后重新钉 255 保证 GPIO38 供电
    M5.Display.setBrightness(255);
    ledForcedBL = true;
    return;
  }

  if (ledMode == LED_MODE_OFF) {   // 灯关：把背光还给用户亮度设定，灯灭
    if (ledForcedBL) { M5.Display.setBrightness(brightVal()); ledForcedBL = false; }
    leds[0] = CRGB::Black;
    FastLED.show();
    return;
  }

  // 灯开且醒着：把背光钉成 255（G38 定常 HIGH）给灯珠供电，再按模式画灯
  M5.Display.setBrightness(255);
  ledForcedBL = true;

  renderEffect(millis());
  FastLED.show();
  lastShowMs = millis();
}

void ledInit() {
  int m = loadInt("led", "mode", -1);
  if (m >= 0 && m < LED_MODE_COUNT) {
    ledMode = (LedMode)m;
  } else {
    // 兼容旧版 NVS：若之前开着则默认切到电量指示，否则保持关闭
    bool on = loadBool("led", "on", false);
    ledMode = on ? LED_MODE_BATTERY : LED_MODE_OFF;
  }
  lastMode = ledMode;
  lastScreenOff = screenOff;
  ledApply();
}

void ledSetMode(LedMode mode) {
  if (mode < 0 || mode >= LED_MODE_COUNT) mode = LED_MODE_OFF;
  ledMode = mode;
  lastMode = ledMode;
  lastBucket = -99;
  vuEnvelope = 0.0f;
  saveInt("led", "mode", (int)ledMode);
  saveBool("led", "on", ledMode != LED_MODE_OFF);
  ledApply();
}

void ledCycleMode() {
  LedMode next = (LedMode)(((int)ledMode + 1) % LED_MODE_COUNT);
  ledSetMode(next);
}

void ledSet(bool on) {
  if (on) {
    if (ledMode == LED_MODE_OFF) ledSetMode(LED_MODE_BATTERY);
  } else {
    ledSetMode(LED_MODE_OFF);
  }
}

// 每帧调用：状态变化立即响应；灯开且醒着时按模式刷新
void ledUpdate() {
  if (overrideActive) {
    if (!screenOff && !ledForcedBL) {
      M5.Display.setBrightness(255);
      ledForcedBL = true;
    }
    return;   // 被外部接管期间（如 Spectrum 麦克风律动），电量/灯效让路
  }
  uint32_t now = millis();
  bool changed = (screenOff != lastScreenOff) || (ledMode != lastMode);
  lastScreenOff = screenOff;
  lastMode = ledMode;

  if (changed) { ledApply(); return; }   // 睡/醒、开/关/换模式：立刻处理一次
  if (screenOff || ledMode == LED_MODE_OFF) return; // 熄屏或灯关：无需持续刷新

  if (ledMode == LED_MODE_BATTERY) {
    if (powerCharging()) {
      if (now - lastShowMs >= 40) ledApply();   // ~25fps 呼吸
      lastBucket = -99;                          // 拔充电线后强制重画一次静态色
    } else {
      int bucket = levelBucket(powerBatteryLevel());
      if (bucket != lastBucket || now - lastShowMs > 5000) {
        ledApply();
        lastBucket = bucket;
      }
    }
  } else {
    // 呼吸灯/彩虹/跑马/音乐律动：控制在约 30ms (~33fps)，避免占用主循环
    if (now - lastShowMs >= 30) {
      // 调亮度（设置页 / 串口 BRIGHT / 秒表闹钟闪屏）会把背光改掉，灯珠跟着断电；每次刷新前补钉 255
      if (M5.Display.getBrightness() != 255) M5.Display.setBrightness(255);
      renderEffect(now);
      FastLED.show();
      lastShowMs = now;
    }
  }
}

bool ledIsOn() { return ledMode != LED_MODE_OFF; }

LedMode ledGetMode() { return ledMode; }

const char* ledModeName(LedMode m) {
  switch (m) {
    case LED_MODE_OFF:     return "OFF";
    case LED_MODE_BATTERY: return "Battery";
    case LED_MODE_BREATHE: return "Breathe";
    case LED_MODE_RAINBOW: return "Rainbow";
    case LED_MODE_CHASE:   return "Chase";
    case LED_MODE_MUSIC:   return "Music";
    default:               return "OFF";
  }
}

const char* ledModeName() {
  return ledModeName(ledMode);
}

bool ledOverrideActive() { return overrideActive; }

void ledSetOverride(bool on) {
  ledBegin();
  overrideActive = on;
  if (on) {
    // 跟 ledApply() 开灯分支一样，把背光钉 255 让 G38 常 HIGH 给灯珠供电（见文件头注释）
    M5.Display.setBrightness(255);
    ledForcedBL = true;
  } else if (ledMode == LED_MODE_OFF) {
    // 正常灯本来是关的：把背光还给用户设定、灯灭；开着的话交给下面 ledApply() 处理
    if (ledForcedBL) { M5.Display.setBrightness(brightVal()); ledForcedBL = false; }
    leds[0] = CRGB::Black;
    FastLED.show();
  }
  if (!on) { lastBucket = -99; ledApply(); }   // 退出接管：恢复当前选中的灯效
}

void ledShowRGB(uint8_t r, uint8_t g, uint8_t b) {
  if (!overrideActive || screenOff) return;
  if (!ledForcedBL) {
    M5.Display.setBrightness(255);
    ledForcedBL = true;
  }
  leds[0] = CRGB(r, g, b);
  FastLED.show();
}
