#include "settings_ui.h"
#include <WiFi.h>
#include "icons.h"
#include "sd_files.h"
#include "led.h"
#include "power_util.h"
#include "ui_common.h"
#include "weather.h"
#include "ram_profile.h"
#include "list_sel.h"
#include <esp_partition.h>
#include <esp_ota_ops.h>

// 设置项右侧的数值/状态
String setValue(SetId id) {
  char b[24];
  switch (id) {
    case SET_WIFI:    return String(WiFi.status() == WL_CONNECTED ? "ON" : "OFF");
    case SET_GNSS:    return String();
    case SET_PCMODE:  return String("USB link for PC");
    case SET_BRIGHT:  snprintf(b, sizeof(b), "%d%%", brightPct); return String(b);
    case SET_VOL:
      if (debugOn) return String("MUTE (DBG)");
      snprintf(b, sizeof(b), volPct == 0 ? "MUTE" : "%d%%", volPct); return String(b);
    case SET_BOOT_SOUND: return String(bootSoundOn ? "ON" : "OFF");
    case SET_LED:     return String(ledModeName());
    case SET_THEME:   return String(themeName(themeMode));
    case SET_SLEEP: {
      int s = SLEEP_OPTS[sleepOptIdx];
      if (s == 0) return String("Never");
      snprintf(b, sizeof(b), "%ds", s); return String(b);
    }
    case SET_TZ:      return String(TZ_OPTS[tzOptIdx].label);
    case SET_WX_UNIT: return String(weatherImperial ? "Imperial" : "Metric");
    case SET_BATTERY: {
      int lvl = powerBatteryLevel();
      if (lvl < 0) return String("--");
      snprintf(b, sizeof(b), "%d%%", lvl); return String(b);
    }
    case SET_DEBUG:   return String(debugOn ? "ON" : "OFF");
    case SET_FORMAT:  return String(sdMounted ? "Erase >" : "No SD");
    case SET_ABOUT:   return String("Info >");
  }
  return String();
}

static const int SET_PER_PAGE = 4;   // 每页 4 项，15 项共 4 页

void drawSettings() {
  cv.fillScreen(TFT_BLACK);

  int page = settingsIndex / SET_PER_PAGE;
  int pageCount = (SET_COUNT + SET_PER_PAGE - 1) / SET_PER_PAGE;
  int startIdx = page * SET_PER_PAGE;

  char pageStr[32];
  snprintf(pageStr, sizeof(pageStr), "%u/%u", (unsigned)(page + 1), (unsigned)pageCount);
  drawPageHeader("Settings", pageStr, 0x07FF);

  const int startY = 16, cardH = 24, gap = 2;
  const int cardX = 4, cardW = SW - 8;

  for (int p = 0; p < SET_PER_PAGE; p++) {
    int i = startIdx + p;
    if (i >= SET_COUNT) break;
    int y = startY + p * (cardH + gap);
    bool sel = (i == settingsIndex);

    // 1. 卡片背景与边框
    if (sel) {
      cv.fillRoundRect(cardX, y, cardW, cardH, 3, 0x0C82);
      cv.drawRoundRect(cardX, y, cardW, cardH, 3, ACCENT);
      cv.drawRoundRect(cardX + 1, y + 1, cardW - 2, cardH - 2, 2, 0x05E8);
    } else {
      cv.fillRoundRect(cardX, y, cardW, cardH, 3, 0x0821);
      cv.drawRoundRect(cardX, y, cardW, cardH, 3, 0x1062);
    }

    // 2. 左侧图标与设置项名称
    int midY = y + cardH / 2;
    drawSetIcon(cardX + 14, midY, 7, sel ? ACCENT : 0x52AA, SETTINGS[i].id);

    cv.setTextColor(sel ? TFT_WHITE : 0xCE79, sel ? 0x0C82 : 0x0821);
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.drawString(SETTINGS[i].name, cardX + 28, midY);

    // 3. 右侧状态微胶囊 (Pill Badge)
    int rx = cardX + cardW - 6;
    SetId sid = SETTINGS[i].id;

    if (sid == SET_WIFI || sid == SET_DEBUG || sid == SET_BOOT_SOUND) {
      // 开关型项
      bool on = false;
      if (sid == SET_WIFI) on = (WiFi.status() == WL_CONNECTED);
      else if (sid == SET_DEBUG) on = debugOn;
      else if (sid == SET_BOOT_SOUND) on = bootSoundOn;

      const int bw = 32, bh = 14, bx = rx - bw, by = y + 5;
      if (on) {
        uint16_t onCol = (sid == SET_DEBUG) ? 0x073F : (sid == SET_BOOT_SOUND ? 0xFDA0 : 0x07E0);
        cv.fillRoundRect(bx, by, bw, bh, 2, 0x0821);
        cv.drawRoundRect(bx, by, bw, bh, 2, onCol);
        cv.setTextColor(onCol, 0x0821);
        cv.setTextDatum(middle_center);
        cv.drawString("ON", bx + bw / 2, by + 7);
      } else {
        cv.fillRoundRect(bx, by, bw, bh, 2, 0x0821);
        cv.drawRoundRect(bx, by, bw, bh, 2, 0x2124);
        cv.setTextColor(0x7BEF, 0x0821);
        cv.setTextDatum(middle_center);
        cv.drawString("OFF", bx + bw / 2, by + 7);
      }
    } else if (sid == SET_LED) {
      // LED 灯效模式微胶囊
      const char* mnm = ledModeName();
      bool on = ledIsOn();
      int bw = strlen(mnm) * 6 + 10;
      if (bw < 32) bw = 32;
      int bx = rx - bw, by = y + 5;
      uint16_t col = on ? 0xFD02 : 0x7BEF;
      uint16_t borderCol = on ? 0xFD02 : 0x2124;
      cv.fillRoundRect(bx, by, bw, 14, 2, 0x0821);
      cv.drawRoundRect(bx, by, bw, 14, 2, borderCol);
      cv.setTextColor(col, 0x0821);
      cv.setTextDatum(middle_center);
      cv.drawString(mnm, bx + bw / 2, by + 7);
    } else if (sid == SET_THEME) {
      // 主题项：胶囊使用对应主题颜色
      const char* tnm = themeName(themeMode);
      uint16_t tcol = (themeMode == 0) ? 0x07FF : getGroupTheme(themeMode - 1).primary;
      int bw = strlen(tnm) * 6 + 10;
      if (bw < 36) bw = 36;
      int bx = rx - bw, by = y + 5;
      cv.fillRoundRect(bx, by, bw, 14, 2, 0x0821);
      cv.drawRoundRect(bx, by, bw, 14, 2, tcol);
      cv.setTextColor(tcol, 0x0821);
      cv.setTextDatum(middle_center);
      cv.drawString(tnm, bx + bw / 2, by + 7);
    } else if (sid == SET_BATTERY) {
      int lvl = powerBatteryLevel();
      uint16_t bcol = (lvl > 50) ? 0x07E0 : (lvl > 20) ? TFT_YELLOW : TFT_RED;
      char bb[16]; snprintf(bb, sizeof(bb), "%d%%%s", lvl, powerCharging() ? "+" : "");
      int bw = strlen(bb) * 6 + 10;
      if (bw < 34) bw = 34;
      int bx = rx - bw, by = y + 5;
      cv.fillRoundRect(bx, by, bw, 14, 2, 0x0821);
      cv.drawRoundRect(bx, by, bw, 14, 2, bcol);
      cv.setTextColor(bcol, 0x0821);
      cv.setTextDatum(middle_center);
      cv.drawString(bb, bx + bw / 2, by + 7);
    } else if (sid == SET_FORMAT || sid == SET_ABOUT || sid == SET_GNSS || sid == SET_PCMODE) {
      String val = setValue(sid);
    } else {
      // 其它数值/档位项 (Bright, Vol, Sleep, Tz, Units)
      String val = setValue(sid);
      int bw = val.length() * 6 + 10;
      if (bw < 34) bw = 34;
      int bx = rx - bw, by = y + 5;
      uint16_t vcol = sel ? 0x07FF : 0xCE79;
      cv.fillRoundRect(bx, by, bw, 14, 2, 0x0821);
      cv.drawRoundRect(bx, by, bw, 14, 2, sel ? vcol : 0x2124);
      cv.setTextColor(vcol, 0x0821);
      cv.setTextDatum(middle_center);
      cv.drawString(val, bx + bw / 2, by + 7);
    }
  }

  // 底部辅助导航与页码点 (y = 125..134)
  drawPageDots(page, pageCount);
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.setTextColor(0x52AA, TFT_BLACK);
  cv.drawString("; . nav", 6, SH - 2);

  cv.setTextDatum(bottom_right);
  cv.drawString("Enter set", SW - 6, SH - 2);
}

// 通用：段式霓虹发光刻度计（亮度/音量共用）
void drawBar(const char* title, int pct, const char* hint) {
  cv.fillScreen(TFT_BLACK);

  char pctStr[16]; snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
  drawPageHeader(title, pctStr, ACCENT);

  // 居中大卡片
  const int cx = 10, cy = 18, cw = SW - 20, ch = 96;
  cv.fillRoundRect(cx, cy, cw, ch, 4, 0x0821);
  cv.drawRoundRect(cx, cy, cw, ch, 4, 0x18C3);

  // 图标 + 百分比读数
  if (strstr(title, "Bright") != nullptr) {
    icoSun(32, 40, 10, ACCENT);
  } else {
    icoSpeaker(32, 40, 10, ACCENT);
  }

  cv.setTextDatum(middle_left); cv.setTextSize(2);
  cv.setTextColor(TFT_WHITE, 0x0821);
  cv.drawString(pctStr, 52, 40);

  // 12 段式霓虹水平刻度计 (y = 62)
  const int nSeg = 12;
  const int segW = 12, segH = 18, segGap = 4;
  int startX = cx + (cw - (nSeg * (segW + segGap) - segGap)) / 2;
  int filled = (pct * nSeg + 50) / 100;

  for (int s = 0; s < nSeg; s++) {
    int sx = startX + s * (segW + segGap);
    if (s < filled) {
      cv.fillRoundRect(sx, 62, segW, segH, 2, ACCENT);
      cv.drawFastVLine(sx + 1, 64, segH - 4, TFT_WHITE);
    } else {
      cv.fillRoundRect(sx, 62, segW, segH, 2, 0x1082);
      cv.drawRoundRect(sx, 62, segW, segH, 2, 0x18C3);
    }
  }

  // 刻度标注
  cv.setTextColor(0x52AA, 0x0821); cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.drawString("MIN", startX, 83);
  cv.setTextDatum(top_right);
  cv.drawString("MAX", startX + nSeg * (segW + segGap) - segGap, 83);

  // 底部操作提示
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_center); cv.setTextSize(1);
  cv.drawString(hint, SW / 2, SH - 2);
}

// 熄屏等待时长：标准 HUD 外框与时间档位胶囊
void drawSleep() {
  cv.fillScreen(TFT_BLACK);

  int s = SLEEP_OPTS[sleepOptIdx];
  char b[16];
  if (s == 0) snprintf(b, sizeof(b), "Never"); else snprintf(b, sizeof(b), "%ds", s);
  drawPageHeader("Screen Timeout", b, ACCENT);

  const int cx = 10, cy = 18, cw = SW - 20, ch = 96;
  cv.fillRoundRect(cx, cy, cw, ch, 4, 0x0821);
  cv.drawRoundRect(cx, cy, cw, ch, 4, 0x18C3);

  cv.setTextColor(ACCENT, 0x0821);
  cv.setTextDatum(middle_center); cv.setTextSize(3);
  cv.drawString(b, SW / 2, 46);

  // 6 个档位胶囊排开 (y = 74)
  const int pCount = SLEEP_OPT_COUNT;
  const int pw = 30, ph = 14, pgap = 4;
  int startX = cx + (cw - (pCount * (pw + pgap) - pgap)) / 2;

  for (int i = 0; i < pCount; i++) {
    int px = startX + i * (pw + pgap);
    bool on = (i == sleepOptIdx);
    char tb[8];
    if (SLEEP_OPTS[i] == 0) snprintf(tb, sizeof(tb), "Off");
    else                    snprintf(tb, sizeof(tb), "%ds", SLEEP_OPTS[i]);

    if (on) {
      cv.fillRoundRect(px, 74, pw, ph, 2, ACCENT);
      cv.setTextColor(TFT_BLACK, ACCENT);
    } else {
      cv.fillRoundRect(px, 74, pw, ph, 2, 0x1082);
      cv.drawRoundRect(px, 74, pw, ph, 2, 0x18C3);
      cv.setTextColor(0x7BEF, 0x1082);
    }
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString(tb, px + pw / 2, 74 + ph / 2);
  }

  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_center); cv.setTextSize(1);
  cv.drawString("; . prev/next    ` return", SW / 2, SH - 2);
}

void drawTz() {
  cv.fillScreen(TFT_BLACK);

  char pageBuf[16];
  snprintf(pageBuf, sizeof(pageBuf), "%d/%d", tzOptIdx + 1, TZ_OPT_COUNT);
  drawPageHeader("Timezone", pageBuf, ACCENT);

  const int cx = 10, cy = 18, cw = SW - 20, ch = 96;
  cv.fillRoundRect(cx, cy, cw, ch, 4, 0x0821);
  cv.drawRoundRect(cx, cy, cw, ch, 4, 0x18C3);

  cv.setTextColor(TFT_WHITE, 0x0821);
  cv.setTextDatum(middle_center); cv.setTextSize(2);
  cv.drawString(TZ_OPTS[tzOptIdx].label, SW / 2, 46);

  cv.setTextColor(0x07FF, 0x0821);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  char posixBuf[40];
  snprintf(posixBuf, sizeof(posixBuf), "Rule: %s", TZ_OPTS[tzOptIdx].posix);
  cv.drawString(posixBuf, SW / 2, 74);

  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_center); cv.setTextSize(1);
  cv.drawString("; . prev/next    ` return", SW / 2, SH - 2);
}

// 电池与电源仪表盘：双联硬件卡片
void drawBatteryDetail() {
  cv.fillScreen(TFT_BLACK);

  int lvl = powerBatteryLevel();
  int16_t mv = M5.Power.getBatteryVoltage();
  bool charging = powerCharging();

  drawPageHeader("Battery & Power", charging ? "CHARGING" : "DISCHARGING", charging ? 0x07E0 : TFT_YELLOW);

  // 卡片 1 (左)：实时电量与状态
  const int c1w = 113, c2w = 114;
  const int bpy = 16, bph = 102;
  cv.fillRoundRect(4, bpy, c1w, bph, 4, 0x0821);
  cv.drawRoundRect(4, bpy, c1w, bph, 4, 0x18C3);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(ACCENT, 0x0821);
  cv.drawString("LIVE STATUS", 8, bpy + 4);
  cv.drawFastHLine(6, bpy + 14, c1w - 4, 0x1082);

  // 电池电芯图标 (x = 10, y = 36)
  int bx = 10, by = bpy + 22, bw = 38, bh = 18;
  cv.drawRoundRect(bx, by, bw, bh, 3, TFT_LIGHTGREY);
  cv.fillRect(bx + bw, by + 5, 3, bh - 10, TFT_LIGHTGREY);
  if (lvl >= 0) {
    int fillW = (bw - 4) * constrain(lvl, 0, 100) / 100;
    uint16_t bcol = lvl <= 20 ? TFT_RED : (lvl <= 50 ? TFT_ORANGE : ACCENT);
    cv.fillRoundRect(bx + 2, by + 2, fillW, bh - 4, 1, bcol);
  }

  // 大字电量
  char b[32];
  if (lvl >= 0) snprintf(b, sizeof(b), "%d%%", lvl);
  else          snprintf(b, sizeof(b), "--%%");
  cv.setTextColor(TFT_WHITE, 0x0821); cv.setTextSize(2);
  cv.drawString(b, 54, bpy + 23);

  // 端电压
  cv.setTextSize(1);
  if (mv >= 0) snprintf(b, sizeof(b), "Volt: %.2f V", mv / 1000.0f);
  else         snprintf(b, sizeof(b), "Volt: n/a");
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString(b, 10, bpy + 48);

  // 充放电状态胶囊
  const int sbw = c1w - 18, sbh = 16, sbx = 9, sby = bpy + 68;
  if (charging) {
    cv.fillRoundRect(sbx, sby, sbw, sbh, 2, 0x0821);
    cv.drawRoundRect(sbx, sby, sbw, sbh, 2, 0x07E0);
    cv.setTextColor(0x07E0, 0x0821);
    cv.setTextDatum(middle_center);
    cv.drawString("CHARGING +", sbx + sbw / 2, sby + sbh / 2);
  } else {
    cv.fillRoundRect(sbx, sby, sbw, sbh, 2, 0x0821);
    cv.drawRoundRect(sbx, sby, sbw, sbh, 2, 0x2124);
    cv.setTextColor(0xCE79, 0x0821);
    cv.setTextDatum(middle_center);
    cv.drawString("DISCHARGING", sbx + sbw / 2, sby + sbh / 2);
  }

  // 卡片 2 (右)：硬件规格与设计参数
  const int c2x = 121;
  cv.fillRoundRect(c2x, bpy, c2w, bph, 4, 0x0821);
  cv.drawRoundRect(c2x, bpy, c2w, bph, 4, 0x18C3);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("POWER SPECS", c2x + 6, bpy + 4);
  cv.drawFastHLine(c2x + 2, bpy + 14, c2w - 4, 0x1082);

  int y2 = bpy + 18; const int lh = 13;
  cv.setTextColor(0xCE79, 0x0821);
  cv.drawString("PMIC : TP4057", c2x + 6, y2); y2 += lh;
  cv.drawString("Cap  : ~1400 mAh", c2x + 6, y2); y2 += lh;
  cv.drawString("Cell : 1S Li-Po", c2x + 6, y2); y2 += lh;
  cv.drawString("V_min: 3.30 V", c2x + 6, y2); y2 += lh;
  cv.drawString("V_max: 4.20 V", c2x + 6, y2); y2 += lh;
  cv.drawString("Sens : V-Curve Est", c2x + 6, y2);

  // 底部提示
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_center); cv.setTextSize(1);
  cv.drawString("`  return to settings", SW / 2, SH - 2);
}

void drawFormat() {
  cv.fillScreen(TFT_BLACK);
  if (!sdMounted) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(middle_center); cv.setTextSize(2);
    cv.drawString("No SD card", SW / 2, SH / 2 - 6);
    return;
  }
  // 顶部醒目警告条
  cv.fillRect(0, 0, SW, 20, TFT_RED);
  cv.setTextColor(TFT_WHITE, TFT_RED);
  cv.setTextDatum(middle_center); cv.setTextSize(2);
  cv.drawString(formatStep == 0 ? "Format SD?" : "ARE YOU SURE?", SW / 2, 10);

  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  cv.setTextDatum(middle_center); cv.setTextSize(1);
  if (formatStep == 0) {
    cv.drawString("This ERASES everything", SW / 2, 48);
    char c[32]; snprintf(c, sizeof(c), "on the %.1f GB card.", sdSizeMB / 1024.0);
    cv.drawString(c, SW / 2, 64);
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("Press Y to continue", SW / 2, 92);
  } else {
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.drawString("ALL DATA WILL BE LOST!", SW / 2, 52);
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("Press Y again to FORMAT", SW / 2, 84);
  }
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_center);
  cv.drawString("`  cancel", SW / 2, SH - 2);
}

// 一行：标签 + 占比进度条 + 百分比，下面再跟一行"已用/总量"细节
static void drawUsageBar(int y, const char* label, int pct, const String& detail) {
  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.drawString(label, 8, y);
  const int barX = 44, barW = SW - 44 - 34, barH = 10;
  cv.drawRect(barX, y, barW, barH, 0x2124);
  uint16_t col = pct >= 85 ? TFT_RED : (pct >= 65 ? TFT_ORANGE : ACCENT);
  cv.fillRect(barX + 1, y + 1, (barW - 2) * constrain(pct, 0, 100) / 100, barH - 2, col);
  char pb[8]; snprintf(pb, sizeof(pb), "%d%%", pct);
  cv.setTextColor(col, TFT_BLACK);
  cv.setTextDatum(top_right);
  cv.drawString(pb, SW - 6, y);
  cv.setTextColor(0x52AA, TFT_BLACK);
  cv.setTextDatum(top_left);
  cv.drawString(detail, barX, y + 12);
}

int aboutPage = 0;   // 0=硬件规格  1=RAM/ROM/SD 用量  2=开机内存轨迹
const int ABOUT_PAGE_COUNT = 3;
int aboutRamScroll = 0;

void aboutRamScrollMove(int dir) {
  const int VISIBLE_ROWS = 6;
  int count = ramMarkCount();
  int maxScroll = count > VISIBLE_ROWS ? count - VISIBLE_ROWS : 0;
  aboutRamScroll += dir;
  if (aboutRamScroll > maxScroll) aboutRamScroll = maxScroll;
  if (aboutRamScroll < 0) aboutRamScroll = 0;
}

void aboutRamReset() {
  aboutRamScroll = 0;
}

static void drawAboutInfo() {
  // 卡片 1 (左)：计算核心
  const int c1w = 113, c2w = 114;
  const int bpy = 16, bph = 102;
  cv.fillRoundRect(4, bpy, c1w, bph, 4, 0x0821);
  cv.drawRoundRect(4, bpy, c1w, bph, 4, 0x18C3);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(ACCENT, 0x0821);
  cv.drawString("COMPUTE", 8, bpy + 4);
  cv.drawFastHLine(6, bpy + 14, c1w - 4, 0x1082);

  int y1 = bpy + 18; const int lh = 13;
  cv.setTextColor(0xCE79, 0x0821);
  cv.drawString("MCU : ESP32-S3", 8, y1); y1 += lh;
  cv.drawString("Freq: 240 MHz", 8, y1); y1 += lh;
  cv.drawString("Arch: Xtensa LX7", 8, y1); y1 += lh;
  cv.drawString("Core: Dual-Core", 8, y1); y1 += lh;
  cv.drawString("SRAM: 320 KB", 8, y1); y1 += lh;
  cv.drawString("PSRAM: None", 8, y1);

  // 卡片 2 (右)：外设与系统
  const int c2x = 121;
  cv.fillRoundRect(c2x, bpy, c2w, bph, 4, 0x0821);
  cv.drawRoundRect(c2x, bpy, c2w, bph, 4, 0x18C3);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("SYSTEM & IO", c2x + 6, bpy + 4);
  cv.drawFastHLine(c2x + 2, bpy + 14, c2w - 4, 0x1082);

  int y2 = bpy + 18;
  cv.setTextColor(0xCE79, 0x0821);
  cv.drawString("Flash : 8 MB QSPI", c2x + 6, y2); y2 += lh;
  cv.drawString("LCD   : 1.14\" IPS", c2x + 6, y2); y2 += lh;
  cv.drawString("Keys  : TCA8418", c2x + 6, y2); y2 += lh;
  cv.drawString("Audio : NS4168", c2x + 6, y2); y2 += lh;
  cv.drawString("Sync  : NTP & GPS", c2x + 6, y2); y2 += lh;
  cv.drawString("Build : Slacker v4", c2x + 6, y2);
}

static void drawAboutUsage() {
  int y = 16;

  uint32_t ramTotal = ESP.getHeapSize(), ramFree = ESP.getFreeHeap();
  uint32_t ramUsed = ramTotal - ramFree;
  int ramPct = ramTotal ? (int)(((uint64_t)ramUsed * 100) / ramTotal) : 0;
  drawUsageBar(y, "RAM", ramPct, fmtBytes(ramUsed) + " / " + fmtBytes(ramTotal)); y += 26;

  uint32_t romUsed = ESP.getSketchSize();
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running) running = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  uint32_t romTotal = running ? running->size : (romUsed + ESP.getFreeSketchSpace());
  int romPct = romTotal ? (int)(((uint64_t)romUsed * 100) / romTotal) : 0;
  drawUsageBar(y, "ROM", romPct, fmtBytes(romUsed) + " / " + fmtBytes(romTotal)); y += 26;

  if (sdMounted) {
    int sdPct = sdSizeMB ? (int)(sdUsedMB * 100 / sdSizeMB) : 0;
    char det[24]; snprintf(det, sizeof(det), "%.1fG / %.1fG", sdUsedMB / 1024.0, sdSizeMB / 1024.0);
    drawUsageBar(y, "SD", sdPct, det);
  } else {
    drawUsageBar(y, "SD", 0, "No card");
  }
  y += 26;

  // 底部芯片指标卡片
  cv.fillRoundRect(4, y, SW - 8, 20, 3, 0x0821);
  cv.drawRoundRect(4, y, SW - 8, 20, 3, 0x18C3);

  cv.setTextDatum(middle_left); cv.setTextSize(1);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("Low-Water: " + fmtBytes(ESP.getMinFreeHeap()), 10, y + 10);

  char temp[24]; snprintf(temp, sizeof(temp), "Temp: %.0f C", temperatureRead());
  cv.setTextDatum(middle_right);
  cv.setTextColor(TFT_YELLOW, 0x0821);
  cv.drawString(temp, SW - 10, y + 10);
}

void drawAboutRam() {
  const int cardX = 4, cardY = 16, cardW = SW - 8, cardH = 102;
  cv.fillRoundRect(cardX, cardY, cardW, cardH, 4, 0x0821);
  cv.drawRoundRect(cardX, cardY, cardW, cardH, 4, 0x18C3);

  int count = ramMarkCount();
  if (count == 0) {
    cv.setTextColor(0x52AA, 0x0821);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString("No RAM marks recorded", SW / 2, cardY + cardH / 2);

    cv.setTextDatum(bottom_left);
    cv.drawString("; . page", 6, SH - 2);
    return;
  }

  // 表头
  const int hdrY = cardY + 7;
  cv.setTextSize(1);
  cv.setTextColor(ACCENT, 0x0821);

  cv.setTextDatum(middle_left);
  cv.drawString("Stage", cardX + 4, hdrY);

  cv.setTextDatum(middle_right);
  cv.drawString("free8", 107, hdrY);
  cv.drawString("dFree", 149, hdrY);
  cv.drawString("larg8", 188, hdrY);
  cv.drawString("dLarg", 228, hdrY);

  cv.drawFastHLine(cardX + 2, cardY + 15, cardW - 4, 0x1082);

  // 列表范围与滚动
  const int VISIBLE_ROWS = 6;
  listClampScroll(aboutRamScroll, aboutRamScroll, count, VISIBLE_ROWS);

  const int rowH = 14;
  const int startY = cardY + 16;

  for (int r = 0; r < VISIBLE_ROWS; r++) {
    int idx = aboutRamScroll + r;
    if (idx >= count) break;
    const RamMark& m = ramMarkAt(idx);

    int rowY = startY + r * rowH;
    int midY = rowY + 7;

    // 阶段名（最长 10 字符，例如 "wifi start"）
    char sBuf[12];
    snprintf(sBuf, sizeof(sBuf), "%.10s", m.stage);
    cv.setTextDatum(middle_left);
    cv.setTextColor(0xCE79, 0x0821);
    cv.drawString(sBuf, cardX + 4, midY);

    // free8
    char fb[12];
    snprintf(fb, sizeof(fb), "%lu", (unsigned long)m.free8);
    cv.setTextDatum(middle_right);
    cv.setTextColor(0x07FF, 0x0821);
    cv.drawString(fb, 107, midY);

    // dFree
    if (idx == 0) {
      cv.setTextColor(0x52AA, 0x0821);
      cv.drawString("-", 149, midY);
    } else {
      long dFree = (long)m.free8 - (long)ramMarkAt(idx - 1).free8;
      char dfb[14];
      snprintf(dfb, sizeof(dfb), "%+ld", dFree);
      uint16_t dfCol = (dFree > 0) ? 0x07E0 : (dFree < 0 ? 0xFDA0 : 0x52AA);
      cv.setTextColor(dfCol, 0x0821);
      cv.drawString(dfb, 149, midY);
    }

    // largest8
    char lb[12];
    snprintf(lb, sizeof(lb), "%lu", (unsigned long)m.largest8);
    cv.setTextColor(0x07FF, 0x0821);
    cv.drawString(lb, 188, midY);

    // dLarg
    if (idx == 0) {
      cv.setTextColor(0x52AA, 0x0821);
      cv.drawString("-", 228, midY);
    } else {
      long dLarg = (long)m.largest8 - (long)ramMarkAt(idx - 1).largest8;
      char dlb[14];
      snprintf(dlb, sizeof(dlb), "%+ld", dLarg);
      uint16_t dlCol = (dLarg > 0) ? 0x07E0 : (dLarg < 0 ? 0xFDA0 : 0x52AA);
      cv.setTextColor(dlCol, 0x0821);
      cv.drawString(dlb, 228, midY);
    }
  }

  // 滚动条（超过 VISIBLE_ROWS 时绘制）
  if (count > VISIBLE_ROWS) {
    drawScrollBar(SW - 7, startY, VISIBLE_ROWS * rowH, aboutRamScroll, count, VISIBLE_ROWS, ACCENT, 0x18C3);
  }

  // 底部提示
  cv.setTextDatum(bottom_left); cv.setTextSize(1);
  cv.setTextColor(0x52AA, TFT_BLACK);
  cv.drawString("; . page", 6, SH - 2);

  cv.setTextDatum(bottom_right);
  if (count > VISIBLE_ROWS) {
    cv.drawString("[ ] scroll", SW - 6, SH - 2);
  } else if (count >= 2) {
    char totBuf[16];
    long total = (long)ramMarkAt(count - 1).free8 - (long)ramMarkAt(0).free8;
    snprintf(totBuf, sizeof(totBuf), "%+ldB", total);
    cv.drawString(totBuf, SW - 6, SH - 2);
  }
}

void drawAbout() {
  cv.fillScreen(TFT_BLACK);
  char pageStr[16];
  snprintf(pageStr, sizeof(pageStr), "%d/%d", aboutPage + 1, ABOUT_PAGE_COUNT);
  drawPageHeader("About System", pageStr, ACCENT);

  if (aboutPage == 0)      drawAboutInfo();
  else if (aboutPage == 1) drawAboutUsage();
  else                     drawAboutRam();

  drawPageDots(aboutPage, ABOUT_PAGE_COUNT);
}
