#include "stopwatch.h"
#include "ui_common.h"
#include "led.h"

#include "player.h"
#include "radio.h"

// ---- 计时状态 ----
// 累计时长拆成「已停下的累计 accMs」+「本段从 startMs 起跑的部分」，
// 这样开始/暂停只是搬一次数，不用每帧累加（也就不会积累浮点/取整误差）。
static bool     timerMode = false;      // false=秒表(往上数) true=倒计时(往下数)
static bool     running   = false;
static uint32_t startMs   = 0;
static uint32_t accMs     = 0;
static uint32_t presetMs  = 5 * 60 * 1000;   // 倒计时预设，默认 5 分钟

static const int LAP_MAX = 4;
static uint32_t laps[LAP_MAX];
static int      lapCount = 0;
static int      lapTotal = 0;

// ---- 响铃 ----
static const uint32_t ALARM_MS   = 15000;   // 没人理就响这么久自动停
static const uint32_t BEEP_EVERY = 600;
static bool     alarming    = false;
static uint32_t alarmStart  = 0;
static uint32_t lastBeepMs  = 0;
static bool     alarmToneActive = false; // 是否由秒表触发了蜂鸣
static bool     alarmLedPhase   = false;
static bool     alarmOwnsLed    = false; // LED 接管是闹钟自己拿的才由闹钟释放

static bool isAudioBusy() {
  return M5.Mic.isEnabled() || playerIsPlaying() || radioIsActive();
}

static uint32_t elapsedMs() { return accMs + (running ? millis() - startMs : 0); }
static uint32_t remainMs()  { uint32_t e = elapsedMs(); return e >= presetMs ? 0 : presetMs - e; }

// mm:ss.t（超过 100 分钟就退化成 hh:mm:ss，屏幕上那行大字宽度是固定的）
static void fmtTime(uint32_t ms, char* out, size_t n) {
  uint32_t totalSec = ms / 1000;
  if (totalSec >= 6000) {
    snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(totalSec / 3600),
             (unsigned long)(totalSec / 60 % 60), (unsigned long)(totalSec % 60));
  } else {
    snprintf(out, n, "%02lu:%02lu.%lu", (unsigned long)(totalSec / 60),
             (unsigned long)(totalSec % 60), (unsigned long)(ms % 1000 / 100));
  }
}

static void stopAlarm() {
  if (!alarming) return;
  alarming = false;
  if (alarmToneActive) {
    alarmToneActive = false;
    M5.Speaker.stop(); // 只在自己发声时停，避免误停播放器/电台
  }
  if (alarmOwnsLed) { alarmOwnsLed = false; ledSetOverride(false); }
}

static void resetAll() {
  running = false; accMs = 0; startMs = 0; lapCount = 0; lapTotal = 0;
  stopAlarm();
}

void stopwatchEnter() { dirty = true; }
void stopwatchExit()  { stopAlarm(); }

bool stopwatchBusy() { return running || alarming; }
bool stopwatchIsAlarming() { return alarming; }
void stopwatchStopAlarm()  { stopAlarm(); }

void stopwatchUpdate() {
  if (timerMode && running && elapsedMs() >= presetMs) {
    // 到点：把计时钉死在预设值上（别让它继续往下走成负数），然后开始响
    running = false;
    accMs = presetMs;
    alarming = true;
    alarmStart = lastBeepMs = millis() - BEEP_EVERY;   // 立刻响第一声，不等一个周期
    // 熄屏时到点也得看得见：主动把屏幕叫醒，并重置无操作计时（不然下一帧又睡回去）
    if (screenOff) {
      M5.Display.setBrightness(brightVal());
      screenOff = false;
      ledApply();                 // SK6812 跟背光同一路电，断电后状态丢了要重推一帧
    }
    lastActivityMs = millis();
    dirty = true;
  }

  if (alarming) {
    if (millis() - alarmStart >= ALARM_MS) { stopAlarm(); dirty = true; return; }
    if (millis() - lastBeepMs >= BEEP_EVERY) {
      lastBeepMs = millis();
      if (!isAudioBusy()) {
        M5.Speaker.tone(2200, 140);
        alarmToneActive = true;
      }
      // 麦克风或播放器/电台正在使用时改用 LED 闪烁警示，不抢喇叭
      // 频谱页的律动已经接管着 LED 就不抢：那边自己在闪，抢完再释放会把它的接管也关掉
      if (!alarmOwnsLed && !ledOverrideActive()) { ledSetOverride(true); alarmOwnsLed = true; }
      if (alarmOwnsLed) {
        alarmLedPhase = !alarmLedPhase;
        ledShowRGB(alarmLedPhase ? 255 : 0, 0, 0);
      }
      lastActivityMs = millis();   // 响铃期间不熄屏
      dirty = true;
    }
  }
}

void stopwatchKey(char k) {
  dirty = true;

  if (alarming) { stopAlarm(); return; }   // 响铃时任意键先只负责消音

  if (k == '\n' || k == ' ') {
    if (running) { accMs += millis() - startMs; running = false; }
    else {
      if (timerMode && remainMs() == 0) accMs = 0;   // 上一轮已经跑完，再按就是重新开始
      startMs = millis(); running = true;
    }
    return;
  }
  if (k == 'r' || k == 'R' || k == 'c' || k == 'C') { resetAll(); return; }
  if (k == 'm' || k == 'M') { timerMode = !timerMode; resetAll(); return; }

  if (!timerMode) {
    if ((k == 'l' || k == 'L') && running) {
      if (lapCount < LAP_MAX) {
        laps[lapCount++] = elapsedMs();
      } else {
        for (int i = 0; i < LAP_MAX - 1; i++) laps[i] = laps[i + 1];
        laps[LAP_MAX - 1] = elapsedMs();
      }
      lapTotal++;
    }
    return;
  }

  // 倒计时预设只在没跑的时候能调；范围 10s ~ 99min。
  // ⚠️ 用 [ ] - = 而不是方向键：这一页现在是 Time app 的第 2 页，而 handleKey() 里的
  //    翻页块会把 ; . , / 四个键在进 switch 之前就吃掉（见 main.cpp 那段注释）。
  //    GNSS 配置页同样在翻页链里、同样用这套键，保持一致。
  if (running) return;
  int32_t d = 0;
  if      (k == ']') d =  60000;   // 粗调：±1 分钟
  else if (k == '[') d = -60000;
  else if (k == '=') d =  10000;   // 细调：±10 秒
  else if (k == '-') d = -10000;
  if (d == 0) return;
  int32_t v = (int32_t)presetMs + d;
  if (v < 10000) v = 10000;
  if (v > 99L * 60 * 1000) v = 99L * 60 * 1000;
  presetMs = (uint32_t)v;
  accMs = 0;   // 改了预设，之前那段残留的计时作废
}

void drawStopwatch() {
  cv.fillScreen(TFT_BLACK);

  // 1. 标题 + 右上角状态
  const char* st = alarming ? "TIME UP" : (running ? "RUNNING" : "PAUSED");
  uint16_t stCol = alarming ? TFT_RED : (running ? ACCENT : 0x9CD3);
  drawPageHeader(timerMode ? "Timer" : "Stopwatch", st, stCol);

  // 2. 主数字显示卡片
  const int cardX = 8, cardW = 224;
  const int topCardY = 16, topCardH = 53;
  cv.fillRoundRect(cardX, topCardY, cardW, topCardH, 4, 0x0821);
  bool flash = alarming && ((millis() - alarmStart) / (BEEP_EVERY / 2)) % 2 == 0;
  uint16_t borderCol = alarming ? (flash ? TFT_RED : 0x4000) : (running ? 0x2492 : 0x18C3);
  cv.drawRoundRect(cardX, topCardY, cardW, topCardH, 4, borderCol);

  // 主卡片顶部信息行
  cv.setTextSize(1);
  cv.setTextDatum(top_left);
  cv.setTextColor(timerMode ? 0x07FF : ACCENT, 0x0821);
  cv.drawString(timerMode ? "COUNTDOWN" : "CHRONO", cardX + 6, topCardY + 4);

  cv.setTextDatum(top_right);
  char badgeBuf[24];
  if (timerMode) {
    snprintf(badgeBuf, sizeof(badgeBuf), "SET %lu:%02lu",
             (unsigned long)(presetMs / 60000), (unsigned long)(presetMs / 1000 % 60));
    cv.setTextColor(0x9CD3, 0x0821);
  } else {
    snprintf(badgeBuf, sizeof(badgeBuf), "LAPS %d", lapTotal);
    cv.setTextColor(lapTotal > 0 ? 0x07FF : 0x632C, 0x0821);
  }
  cv.drawString(badgeBuf, cardX + cardW - 6, topCardY + 4);

  // 大字读数
  uint32_t showMs = timerMode ? remainMs() : elapsedMs();
  char buf[16];
  fmtTime(showMs, buf, sizeof(buf));

  cv.setTextDatum(middle_center);
  cv.setTextSize(4);
  cv.setTextColor(flash ? TFT_RED : (running ? TFT_WHITE : 0xCE79), 0x0821);
  cv.drawString(buf, SW / 2, topCardY + 31);

  // 3. 底部卡片
  const int botCardY = 72, botCardH = 47;
  cv.fillRoundRect(cardX, botCardY, cardW, botCardH, 4, 0x0821);
  cv.drawRoundRect(cardX, botCardY, cardW, botCardH, 4, 0x18C3);

  if (timerMode) {
    // 进度条 (已用比例)
    const int bx = cardX + 10, bw = cardW - 20, by = botCardY + 6, bh = 7;
    cv.fillRoundRect(bx, by, bw, bh, 3, 0x1082);
    uint32_t el = elapsedMs();
    float pct = presetMs > 0 ? (float)el / (float)presetMs : 0;
    if (pct > 1.0f) pct = 1.0f;
    int fill = (int)(pct * (bw - 2));
    uint16_t barCol = alarming ? TFT_RED : (running ? ACCENT : 0x07FF);
    if (fill > 2) cv.fillRoundRect(bx + 1, by + 1, fill, bh - 2, 2, barCol);

    // 遥测三栏：已用时间、百分比、预设目标
    cv.setTextSize(1);
    // 左栏：已用
    cv.setTextDatum(top_left);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("ELAPSED", cardX + 12, botCardY + 18);
    char eb[16];
    fmtTime(el >= presetMs ? presetMs : el, eb, sizeof(eb));
    cv.setTextColor(TFT_WHITE, 0x0821);
    cv.drawString(eb, cardX + 12, botCardY + 30);

    // 中栏：百分比
    cv.setTextDatum(top_center);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("PROGRESS", SW / 2, botCardY + 18);
    char pb[16];
    snprintf(pb, sizeof(pb), "%d%%", (int)(pct * 100));
    cv.setTextColor(running ? ACCENT : 0x07FF, 0x0821);
    cv.drawString(pb, SW / 2, botCardY + 30);

    // 右栏：目标
    cv.setTextDatum(top_right);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("TARGET", cardX + cardW - 12, botCardY + 18);
    char tb[16];
    fmtTime(presetMs, tb, sizeof(tb));
    cv.setTextColor(0x07FF, 0x0821);
    cv.drawString(tb, cardX + cardW - 12, botCardY + 30);

  } else {
    // 秒表模式：4 槽计次列
    const int colW = cardW / LAP_MAX; // 56
    for (int i = 0; i < LAP_MAX; i++) {
      if (i > 0) {
        cv.drawFastVLine(cardX + i * colW, botCardY + 4, botCardH - 8, 0x1082);
      }
      int cx = cardX + i * colW + colW / 2;
      cv.setTextDatum(top_center);
      cv.setTextSize(1);

      if (i < lapCount) {
        bool isLatest = (i == lapCount - 1);
        int lapNum = (lapTotal <= LAP_MAX) ? (i + 1) : (lapTotal - LAP_MAX + 1 + i);
        snprintf(buf, sizeof(buf), "LAP %d", lapNum);
        cv.setTextColor(isLatest ? 0x07FF : 0x9CD3, 0x0821);
        cv.drawString(buf, cx, botCardY + 4);

        char lb[16];
        fmtTime(laps[i], lb, sizeof(lb));
        cv.setTextColor(isLatest ? TFT_WHITE : 0xCE79, 0x0821);
        cv.drawString(lb, cx, botCardY + 17);

        // 增量
        if (i == 0 && lapTotal <= LAP_MAX) {
          cv.setTextColor(0x632C, 0x0821);
          cv.drawString("START", cx, botCardY + 31);
        } else {
          uint32_t prev = (i > 0) ? laps[i - 1] : laps[0];
          uint32_t delta = (laps[i] >= prev) ? (laps[i] - prev) : 0;
          char db[12];
          snprintf(db, sizeof(db), "+%lu.%lus", (unsigned long)(delta / 1000), (unsigned long)(delta % 1000 / 100));
          cv.setTextColor(0x7BEF, 0x0821);
          cv.drawString(db, cx, botCardY + 31);
        }
      } else {
        snprintf(buf, sizeof(buf), "LAP %d", i + 1);
        cv.setTextColor(0x39E7, 0x0821);
        cv.drawString(buf, cx, botCardY + 4);
        cv.setTextColor(0x2945, 0x0821);
        cv.drawString("--:--.-", cx, botCardY + 17);
        cv.drawString("----", cx, botCardY + 31);
      }
    }
  }

  // 4. 底部按键提示（避开中央 x=92..142 的页码指示圆点）
  cv.setTextDatum(top_left);
  cv.setTextSize(1);
  const int footY = 120;
  if (timerMode) {
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 6, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" run", 36, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("[]", 62, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" 1m", 74, footY);

    // 中央留空给页码指示点
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("-=", 142, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" 10s", 154, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", 182, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" rst", 188, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("M", 214, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" sw", 220, footY);
  } else {
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 6, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" run", 36, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("L", 66, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" lap", 72, footY);

    // 中央留空给页码指示点
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("R", 146, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" reset", 152, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("M", 196, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" timer", 202, footY);
  }

  drawPageDots();
}
