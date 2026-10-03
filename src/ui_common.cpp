#include "ui_common.h"
#include "busy_pill.h"
#include "pages.h"
#include <WiFi.h>
#include <ctime>
#include <cmath>
#include "icons.h"
#include "sd_files.h"
#include "worldmap.h"
#include "gnss.h"
#include "keyboard_adv.h"

static const char* WEEKDAYS[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

void centerMsg(const char* msg, uint16_t color) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(color, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString(msg, M5.Display.width() / 2, M5.Display.height() / 2);
}

void drawBusyPill(lgfx::LovyanGFX& d, const char* label, int phase, bool leaving) {
  // 压在页面标题栏右端，拉完整页重画时自然盖掉。文字区定宽 8 个字符：标签会在
  // "locating" -> "fetching" -> "leaving" 之间换，宽度跟着变的话，变窄时旧边框会剩一条在屏上。
  const int textW = 8 * 6;
  const int w = 4 + 3 * 5 + 3 + textW + 5;
  const int h = 12;
  const int x = SW - w - 2, y = 1;
  const uint16_t bg = 0x0821;
  const uint16_t fg = leaving ? TFT_YELLOW : ACCENT;
  d.fillRoundRect(x, y, w, h, 3, bg);
  d.drawRoundRect(x, y, w, h, 3, fg);
  for (int i = 0; i < 3; i++) {
    d.fillCircle(x + 6 + i * 5, y + h / 2, (i == phase) ? 2 : 1, (i == phase) ? fg : ICON_DIM);
  }
  d.setFont(&fonts::Font0);
  d.setTextSize(1);
  d.setTextDatum(middle_left);
  d.setTextColor(fg, bg);
  d.drawString(label, x + 4 + 3 * 5 + 3, y + h / 2 + 1);
}

// 截断过长文本
String trunc(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  int cut = maxChars - 1;
  while (cut > 0 && cut < (int)s.length() &&
         (((unsigned char)s[cut] & 0xC0) == 0x80)) cut--;
  return s.substring(0, cut) + "~";
}

// 按像素宽度截断过长文本（支持中英混排与当前字体度量，不截断 UTF-8 字符）
String truncPx(const String& s, int maxPx) {
  if (cv.textWidth(s.c_str()) <= maxPx) return s;
  int tw = cv.textWidth("~");
  int avail = maxPx - tw;
  if (avail <= 0) return "~";
  String res;
  res.reserve(s.length());
  size_t i = 0;
  while (i < s.length()) {
    uint8_t c0 = (uint8_t)s[i];
    int n = (c0 & 0x80) == 0 ? 1 : (c0 & 0xE0) == 0xC0 ? 2 : (c0 & 0xF0) == 0xE0 ? 3 : (c0 & 0xF8) == 0xF0 ? 4 : 1;
    if (i + n > s.length()) n = 1;
    String ch = s.substring(i, i + n);
    if (cv.textWidth((res + ch).c_str()) > avail) break;
    res += ch;
    i += n;
  }
  return res + "~";
}

String fmtBytes(uint64_t bytes) {
  char b[24];
  if (bytes < 1024ULL) {
    snprintf(b, sizeof(b), "%uB", (unsigned)bytes);
  } else if (bytes < 1024ULL * 1024ULL) {
    float kb = (float)bytes / 1024.0f;
    if (kb < 10.0f) snprintf(b, sizeof(b), "%.1fKB", kb);
    else            snprintf(b, sizeof(b), "%.0fKB", kb);
  } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
    float mb = (float)bytes / (1024.0f * 1024.0f);
    if (mb < 100.0f) snprintf(b, sizeof(b), "%.1fMB", mb);
    else             snprintf(b, sizeof(b), "%.0fMB", mb);
  } else {
    float gb = (float)bytes / (1024.0f * 1024.0f * 1024.0f);
    snprintf(b, sizeof(b), "%.2fGB", gb);
  }
  return String(b);
}

void drawScrollBar(int x, int y, int h, int top, int total, int visible, uint16_t col, uint16_t trackCol) {
  if (total <= visible || total <= 0 || h <= 4) return;

  if (trackCol != 0) {
    cv.drawFastVLine(x + 1, y, h, trackCol);
  }

  int thumbH = h * visible / total;
  if (thumbH < 4) thumbH = 4;
  if (thumbH > h) thumbH = h;

  int range = total - visible;
  int thumbY = y + (range > 0 ? (h - thumbH) * top / range : 0);
  if (thumbY + thumbH > y + h) thumbY = y + h - thumbH;
  if (thumbY < y) thumbY = y;

  cv.fillRoundRect(x, thumbY, 3, thumbH, 1, col);
}

void drawMiniVuMeter(int x, int y, uint8_t level, bool active, uint16_t col, uint16_t dimCol) {
  static const uint8_t scale[6] = {85, 100, 75, 95, 65, 80};
  for (int b = 0; b < 6; b++) {
    int h = 0;
    if (active) {
      h = (level * scale[b] * 7) / (100 * 100);
      if (h < 1 && level > 5) h = 1;
      if (h > 7) h = 7;
    }
    cv.fillRect(x + b * 4, y + 7 - h, 3, max(1, h), active ? col : dimCol);
  }
}


bool nowHM(int& h, int& m, int& s) {
  // ⚠️ getLocalTime() 的返回值必须判，而且 ti 必须初始化。
  // Arduino 那个实现是 `start = millis(); while (millis() - start <= ms) {...}`，
  // 传 ms=0 时只要这两句之间正好跨过一次毫秒滴答，循环体就一次都不执行 ——
  // 直接 return false 而 ti 从头到尾没被写过，拿去用就是栈垃圾。
  // 概率约等于"两条语句之间跨毫秒"，极小；但顶栏时间是每帧都画的，菜单高亮框做缓动时
  // 帧率会飙到 ~125fps，于是"快速切图标时左上角偶尔闪一个乱数字"就冒出来了。
  // 失败时退回上一次的好值，而不是退回那个 millis 假时钟——后者会跳到 12:xx，看着也是闪。
  static int lastH = -1, lastM = 0, lastS = 0;
  if (timeSynced) {
    struct tm ti{};
    if (getLocalTime(&ti, 0)) {
      h = lastH = ti.tm_hour; m = lastM = ti.tm_min; s = lastS = ti.tm_sec;
      return true;
    }
    if (lastH >= 0) { h = lastH; m = lastM; s = lastS; return true; }
  }
  uint32_t e = millis() / 1000; int t = 12 * 3600 + e;
  h = (t / 3600) % 24; m = (t / 60) % 60; s = t % 60; return false;
}

void drawTopBar() {
  int h, m, s; nowHM(h, m, s);
  char t[8]; snprintf(t, sizeof(t), "%02d:%02d", h, m);
  cv.setTextColor(TFT_CYAN, TFT_BLACK);
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.drawString(t, 6, 5);

  int rightX = SW - 34;
  if (sdMounted) {
    cv.setTextColor(0x07E0, TFT_BLACK);
    cv.setTextDatum(top_right); cv.setTextSize(1);
    cv.drawString("[SD]", rightX, 5);
    rightX -= 26;
  }
  if (WiFi.status() == WL_CONNECTED) {
    drawWifiSignal(rightX - 8, 12, WiFi.RSSI(), TFT_CYAN);
    rightX -= 18;
  }
  if (gnssCapConnected()) {
    cv.setTextColor(0xFD02, TFT_BLACK);
    cv.setTextDatum(top_right); cv.setTextSize(1);
    cv.drawString("[CAP]", rightX, 5);
    rightX -= 32;
  }
  drawBattery(SW - 30, 2);
}

// =============================================================================
// 主菜单：分组网格启动器
//
// APPS[] 按组连续排列、每组正好一屏(4×2)，所以这里不再有滚动逻辑——左右键在组内走、
// 走出边界切到相邻组，顶部 tab 条随时告诉你共几组、在第几组、旁边是什么。
//
// 版面(240×135)：
//   0 ~16    顶栏：时间 + 日期 + [SD] + Wi-Fi + 电池
//   17       1px 分隔线，把顶栏和内容区切开
//   18~30    组 tab 条：当前组高亮微胶囊 + 自适应下划线，其余组清晰浅灰
//   31~121   2 行图标卡片网格（带暗底 Cyber Tray 托盘）
//   124~134  底部动态应用一句话导览条（Tooltip Bar）
// =============================================================================
static const int MENU_TOP      = 32;   // 网格区顶边
static const int MENU_ROW_H    = 46;   // 行距
static const int MENU_CARD_W   = 54;   // 高亮卡片宽
static const int MENU_CARD_H   = 43;   // 高亮卡片高
static const int MENU_ICON_DY  = 16;   // 图标中心相对格顶
static const int MENU_LABEL_DY = 33;   // 标签上沿相对格顶

// 某个 app 在网格里的位置：cx=格子中心X，top=格顶Y（高亮框就锚在这两个量上）
static void menuCellPos(int appIdx, float& cx, float& top) {
  const GroupDef& grp = GROUPS[menuGroupOf(appIdx)];
  int p = appIdx - grp.first;
  const int cw = SW / MENU_COLS;
  cx  = (p % MENU_COLS) * cw + cw / 2.0f;
  top = MENU_TOP + (p / MENU_COLS) * MENU_ROW_H;
}

void menuSnapSelection() {
  float cx, top; menuCellPos(menuIndex, cx, top);
  menuSelX = cx; menuSelY = top;
}

// 每帧把高亮框往目标格拉一截。用指数逼近而不是"记开始时间+按时间插值"：中途连按方向键
// 换目标时能平滑接上，不用重置任何计时。返回 true = 还在动（main 会提刷新率并继续标脏）。
bool menuUpdateAnim() {
  float cx, top; menuCellPos(menuIndex, cx, top);
  float dx = cx - menuSelX, dy = top - menuSelY;
  if (fabsf(dx) < 0.6f && fabsf(dy) < 0.6f) { menuSelX = cx; menuSelY = top; return false; }
  menuSelX += dx * 0.35f;
  menuSelY += dy * 0.35f;
  return true;
}

// 顶部那颗 BTN GO 在主菜单里当"翻页键"：一下跳到下一组的第一格，转一圈回到开头。
// 用方向键横着挪要按满一整组（最多 8 下）才能换组，这个是一步到位。
void menuNextGroup() {
  const GroupDef& ng = GROUPS[(menuGroupOf(menuIndex) + 1) % GROUP_COUNT];
  menuIndex = ng.first;
  menuSnapSelection();   // 整页内容都换了，滑过去反而怪，直接吸附（跟 menuMove 换组时一致）
  dirty = true;
}

void menuMove(int dx, int dy) {
  int g = menuGroupOf(menuIndex);
  const GroupDef& grp = GROUPS[g];
  int p = menuIndex - grp.first;
  int rows = (grp.count + MENU_COLS - 1) / MENU_COLS;

  if (dy != 0) {                                   // 上下：组内换行，只有一行就原地不动
    if (rows <= 1) return;
    int col = p % MENU_COLS;
    int np = ((p / MENU_COLS + dy + rows) % rows) * MENU_COLS + col;
    if (np >= grp.count) np = grp.count - 1;        // 末行不满时落到该组最后一个
    menuIndex = grp.first + np;
    dirty = true;
    return;
  }

  int np = p + dx;                                 // 左右：组内按序号走，越界换组
  if (np < 0) {
    const GroupDef& ng = GROUPS[(g - 1 + GROUP_COUNT) % GROUP_COUNT];
    menuIndex = ng.first + ng.count - 1;           // 从上一组的最后一个进
    menuSnapSelection();                            // 换组整页都变了，滑过去反而怪，直接吸附
  } else if (np >= grp.count) {
    const GroupDef& ng = GROUPS[(g + 1) % GROUP_COUNT];
    menuIndex = ng.first;                           // 从下一组的第一个进
    menuSnapSelection();
  } else {
    menuIndex = grp.first + np;
  }
  dirty = true;
}

// 7 大分组的专属科技感主题配色定义
// [primary, cardBg, innerRing, pillBg, pillBorder]
static const GroupTheme GROUP_THEMES[7] = {
  // 0. TOOLS: 霓虹矩阵绿 (Hardware & Utilities)
  { 0x07E0, 0x0922, 0x0408, 0x08E2, 0x1204 },
  // 1. SCAN: 警戒雷达橙 (Passive RF Sniffing / Radar / Drone)
  { 0xFD02, 0x28C0, 0x9240, 0x2080, 0x5140 },
  // 2. NET: 电光深海青 (Active Network / TCP / DNS / Hotspot)
  { 0x073F, 0x00E5, 0x0330, 0x00C4, 0x020A },
  // 3. HID: 赛博渗透紫 (Bluetooth / USB Keystroke Injection)
  { 0xE21F, 0x2045, 0x7911, 0x1844, 0x48AB },
  // 4. SIG: 深空珊瑚粉 (LoRa / GNSS / Sub-GHz / Infrared)
  { 0xFB28, 0x2861, 0x8984, 0x2061, 0x50E3 },
  // 5. SKY: 天穹极光冰蓝 (Atmosphere / Weather / Satellite Passes / Flight Radar)
  { 0x3DFF, 0x08C5, 0x1AD0, 0x00A4, 0x11EB },
  // 6. MORE: 钛金极客银白 (Personal Services / APIs / Settings)
  { 0xD71D, 0x18E4, 0x4A8B, 0x10A3, 0x31A7 },
};

const GroupTheme& getGroupTheme(int groupIdx) {
  if (groupIdx < 0 || groupIdx >= 7) return GROUP_THEMES[0];
  return GROUP_THEMES[groupIdx];
}

// 顶部组 tab 条：7 个组名平分屏宽，当前组高亮并带精准自适应下划线与大类专属主题色
static void drawGroupTabs() {
  const int tw = SW / GROUP_COUNT;
  int cur = menuGroupOf(menuIndex);
  cv.setTextDatum(top_center); cv.setTextSize(1);
  for (int g = 0; g < GROUP_COUNT; g++) {
    int cx = g * tw + tw / 2;
    bool on = (g == cur);
    const GroupTheme& thm = getGroupTheme(g);
    int nw = strlen(GROUPS[g].name) * 6;
    if (on) {
      cv.fillRoundRect(cx - nw / 2 - 3, 19, nw + 6, 11, 2, thm.pillBg);
      cv.drawRoundRect(cx - nw / 2 - 3, 19, nw + 6, 11, 2, thm.pillBorder);
      cv.setTextColor(thm.primary, thm.pillBg);
      cv.drawString(GROUPS[g].name, cx, 20);
      cv.fillRect(cx - nw / 2 - 1, 29, nw + 2, 2, thm.primary);
    } else {
      cv.setTextColor(0x7BEF, TFT_BLACK);
      cv.drawString(GROUPS[g].name, cx, 20);
    }
  }
}

// 37 个 App 的简练功能一句话导览 (常量字符串，0 额外堆开销)
static const char* appDesc(AppId id) {
  switch (id) {
    case APP_TIME:      return "Clock, Stopwatch & Countdown Timer";
    case APP_COMPASS:   return "IMU Compass & Attitude Sensor";
    case APP_FILES:     return "SD Card File Explorer & Reader";
    case APP_SPECTRUM:  return "I2S Mic Real-time Audio FFT";
    case APP_CALC:      return "Scientific Math Calculator";
    case APP_CONV:      return "Unit & Currency Converter";
    case APP_PLAYER:    return "WAV Audio Player from SD";
    case APP_HASHOVEN:  return "SHA-512 Stress Oven & Cracker";
    case APP_BADAPPLE:  return "Bad Apple!! 1bpp Video Player";
    case APP_WIFICHAN:  return "2.4GHz RF Spectrum Analyzer";
    case APP_WSNIFF:    return "Promiscuous 802.11 Packet Sniffer";
    case APP_WARDRIVE:  return "WiFi Wardriving & GPS Logger";
    case APP_RID:       return "Drone ID (ASTM F3411 & GB)";
    case APP_BTSCAN:    return "BLE Beacon & Device Scanner";
    case APP_HOTSPOT:   return "WiFi SoftAP & Captive Portal";
    case APP_LANSCAN:   return "Local Network ARP & Port Scanner";
    case APP_NETPROBE:  return "Network Diagnostics & DNS Fuzzer";
    case APP_BTKB:      return "BLE Wireless Keyboard (HID)";
    case APP_BTMEDIA:   return "BLE Media Remote & Volume";
    case APP_DUCKY:     return "USB Rubber Ducky Keystroke Inject";
    case APP_LORA:      return "SX1262 LoRa 433/868/915MHz";
    case APP_GNSS:      return "GPS/BDS Satellites & Speed";
    case APP_MAP:       return "Offline World & Regional Map";
    case APP_IR:        return "Infrared Remote Transmitter";
    case APP_ASTRO:     return "Sun, Moon Phase & Terminator";
    case APP_WEATHER:   return "Multi-day Forecast & Air Quality";
    case APP_ADSB:      return "Live Aircraft Flight Radar";
    case APP_SATS:      return "Satellite Passes & Tracker";
    case APP_TYPHOON:   return "Pacific Typhoon Warnings";
    case APP_QUAKE:     return "USGS & JMA Earthquake Map";
    case APP_CHAT:      return "ChatGPT AI Terminal";
    case APP_GITHUB:    return "GitHub Event & Star Tracker";
    case APP_RADIO:     return "Internet Web Streaming Radio";
    case APP_ROUTER:    return "Clash / Mihomo Dashboard";
    case APP_FX:        return "Foreign Exchange Currency Rates";
    case APP_OKX:       return "OKX USDT/CNY Market Ticker";
    case APP_READER:    return "Novel & TXT Reader (GBK/UTF-8)";
    case APP_SETTINGS:  return "System Configuration & Sleep";
    default:            return "";
  }
}

void drawMenu() {
  cv.fillScreen(TFT_BLACK);
  drawTopBar();
  cv.drawFastHLine(0, 17, SW, DIM_BORDER);

  // 顶栏左边时间后面补日期（冷灰色，不与主屏抢焦点）
  if (timeSynced) {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
      char d[16];
      snprintf(d, sizeof(d), "%s %02d-%02d", WEEKDAYS[ti.tm_wday % 7], ti.tm_mon + 1, ti.tm_mday);
      cv.setTextColor(0xCE79, TFT_BLACK);
      cv.setTextDatum(top_left); cv.setTextSize(1);
      cv.drawString(d, 42, 5);
    }
  }

  drawGroupTabs();

  int curGroup = menuGroupOf(menuIndex);
  const GroupTheme& curThm = (themeMode == 0) ? getGroupTheme(curGroup) : getGroupTheme(themeMode - 1);
  ACCENT = curThm.primary;   // 联动全局当前大类主色

  const GroupDef& grp = GROUPS[curGroup];
  const int cw = SW / MENU_COLS;

  // 1. 底层工整网格卡片底托 (Cyber Grid Tray):
  //    为 4x2 槽位绘制微弱暗槽，即使未填满的分组（如 HID/NET）也呈现严整工业感
  for (int p = 0; p < MENU_COLS * 2; p++) {
    int cx = (p % MENU_COLS) * cw + cw / 2;
    int top = MENU_TOP + (p / MENU_COLS) * MENU_ROW_H;
    int kx = cx - MENU_CARD_W / 2;
    if (p < grp.count) {
      cv.fillRoundRect(kx, top, MENU_CARD_W, MENU_CARD_H, 4, 0x0821);
      cv.drawRoundRect(kx, top, MENU_CARD_W, MENU_CARD_H, 4, 0x1062);
    } else {
      // 空槽虚化轮廓
      cv.drawRoundRect(kx, top, MENU_CARD_W, MENU_CARD_H, 4, 0x0821);
    }
  }

  // 2. 动画高亮选中框 (插值平滑滑动，采用当前大类的专属发光色彩):
  int hx = (int)(menuSelX + 0.5f) - MENU_CARD_W / 2, hy = (int)(menuSelY + 0.5f);
  cv.fillRoundRect(hx, hy, MENU_CARD_W, MENU_CARD_H, 4, curThm.cardBg);
  cv.drawRoundRect(hx, hy, MENU_CARD_W, MENU_CARD_H, 4, curThm.primary);
  cv.drawRoundRect(hx + 1, hy + 1, MENU_CARD_W - 2, MENU_CARD_H - 2, 3, curThm.innerRing);

  // 3. 绘制应用图标与文字标签
  for (int p = 0; p < grp.count; p++) {
    int i = grp.first + p;
    int cx = (p % MENU_COLS) * cw + cw / 2;
    int top = MENU_TOP + (p / MENU_COLS) * MENU_ROW_H;
    bool sel = (i == menuIndex);
    drawAppIcon(cx, top + MENU_ICON_DY, sel ? 16 : 14, sel ? curThm.primary : 0x52AA, APPS[i].id);
    cv.setTextColor(sel ? TFT_WHITE : 0x9CD3);
    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.drawString(trunc(APPS[i].name, 9), cx, top + MENU_LABEL_DY);
  }

  // 4. 底部动态功能导览条 (y = 124..134，采用大类专属色，醒目呼应)
  if (!debugOn) {
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(curThm.primary, TFT_BLACK);
    cv.drawString(appDesc(APPS[menuIndex].id), SW / 2, 128);
  }
}

String nowStamp() {
  // getLocalTime() 的返回值必须判：失败时 ti 是没被写过的栈垃圾，直接拿去 strftime
  // 就是读未初始化内存，打出来的时间戳还是随机的。失败就跟没对时一样降级成 t+秒数。
  struct tm ti;
  if (timeSynced && getLocalTime(&ti, 0)) {
    char b[24];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &ti);
    return String(b);
  }
  return String("t+") + String(millis() / 1000) + "s";
}
void sdAppend(const char* path, const String& line) {
  if (!sdMounted) return;
  if (!SD.exists(path)) { File nf = SD.open(path, FILE_WRITE); if (nf) nf.close(); }  // FILE_APPEND在这个SD库版本不会自动建文件
  File f = SD.open(path, FILE_APPEND);
  if (!f) return;
  f.println(line);
  f.close();
}

void drawFieldEditor(const char* label, const String& buf) {
  // datum/size 在这里显式设一遍，不依赖调用方进来之前是什么状态
  int y = 40;
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(ACCENT, TFT_BLACK);
  cv.drawString(label, 8, y); y += 14;
  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  cv.drawString(buf + "_", 8, y);
  cv.setTextDatum(bottom_center);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString("Enter=next/save  `=cancel", SW / 2, SH - 2);
}

// 底部居中的页码点，跟主菜单轮播同款：当前页 ACCENT 实心大点，其余暗灰小点
void drawPageHeader(const char* title, const char* right, uint16_t rightCol) {
  cv.setTextSize(1);
  cv.setTextDatum(top_left);
  // 给所有子页面一个统一的视觉锚点；窄屏上比加粗标题更省空间。
  cv.fillRoundRect(1, 3, 2, 7, 1, ACCENT);
  cv.setTextColor(ACCENT, TFT_BLACK);
  cv.drawString(title, 7, 3);
  if (right) {
    cv.setTextDatum(top_right);
    cv.setTextColor(rightCol ? rightCol : ICON_DIM, TFT_BLACK);
    cv.drawString(right, SW - 6, 3);
  }
  cv.drawFastHLine(0, PAGE_HDR_BOTTOM, SW, DIM_BORDER);
}

void drawNeedWifi(int y) {
  const int x = 14, w = SW - 28;
  cv.drawRoundRect(x, y - 5, w, 34, 5, DIM_BORDER);
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(TFT_RED, TFT_BLACK);
  cv.drawString("WiFi not connected", x + 10, y + 2);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString("Settings > Wi-Fi  to connect", x + 10, y + 16);
}

void drawPageDots(int current, int count) {
  if (count <= 1) return;
  if (current < 0) current = 0;
  if (current >= count) current = count - 1;

  // 页数一多，圆点排开就超出屏宽（startX 会变负、点全画到屏外），指示器反而没了意义。
  // Chat 回复的页数由回复长度决定、没有上限，所以超过阈值退回 "3/38" 这种文本。
  const int gap = 10;
  if ((count - 1) * gap + 8 > SW - 8) {
    char b[24];
    snprintf(b, sizeof(b), "%d/%d", current + 1, count);
    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(bottom_center);
    cv.drawString(b, SW / 2, SH - 1);
    return;
  }

  const int dotY = SH - 4, startX = SW / 2 - (count - 1) * gap / 2;
  for (int i = 0; i < count; i++) {
    int dx = startX + i * gap;
    // 当前页用短胶囊，远距离看仍能一眼分辨；其它页保持轻量小点。
    if (i == current) cv.fillRoundRect(dx - 4, dotY - 2, 8, 4, 2, ACCENT);
    else              cv.fillCircle(dx, dotY, 1, TFT_DARKGREY);
  }
}

void drawPageDots() {
  int cur, count;
  if (!pageIndex(screen, cur, count)) return;
  drawPageDots(cur, count);
}

// ---- 终端风格滚动日志（开机自检 / WiFi 握手共用）----
static const int TLOG_N = 11;
static char tlog[TLOG_N][40];
static uint16_t tlogCol[TLOG_N];
static int tlogCount = 0;
void termLogReset() { tlogCount = 0; }
void termLogLine(const char* s, uint16_t col) {
  int idx;
  if (tlogCount < TLOG_N) idx = tlogCount++;
  else { for (int i = 1; i < TLOG_N; i++) { memcpy(tlog[i - 1], tlog[i], 40); tlogCol[i - 1] = tlogCol[i]; } idx = TLOG_N - 1; }
  snprintf(tlog[idx], 40, "%s", s);
  tlogCol[idx] = col;
}
void termLogDraw(bool cursor) {
  cv.fillScreen(TFT_BLACK);

  // 赛博控制台外框与标题
  bool isBoot = (tlogCount > 0 && strncmp(tlog[0], "boot>", 5) == 0);
  bool isWifi = (tlogCount > 0 && strncmp(tlog[0], "wifi>", 5) == 0);

  // 顶部标题栏
  cv.setTextDatum(top_left);
  cv.setTextFont(0);
  cv.setTextSize(1);
  cv.fillRect(6, 4, 4, 4, ACCENT);
  cv.setTextColor(0x9CD3, TFT_BLACK);
  if (isBoot) {
    cv.drawString("HW POST // AUDIT", 14, 2);
    cv.setTextColor(0x07FF, TFT_BLACK);
    cv.setTextDatum(top_right);
    cv.drawString("[ ESP32-S3 ]", SW - 6, 2);
  } else if (isWifi) {
    cv.drawString("WIFI HANDSHAKE", 14, 2);
    cv.setTextColor(0x07FF, TFT_BLACK);
    cv.setTextDatum(top_right);
    cv.drawString("[ WLAN0 ]", SW - 6, 2);
  } else {
    cv.drawString("SYSTEM CONSOLE", 14, 2);
  }

  // 终端控制台卡片
  cv.fillRoundRect(4, 13, SW - 8, SH - 16, 3, 0x0821);
  cv.drawRoundRect(4, 13, SW - 8, SH - 16, 3, 0x18C3);

  cv.setTextDatum(top_left);
  int y = 17;
  for (int i = 0; i < tlogCount; i++) {
    uint16_t c = tlogCol[i] ? tlogCol[i] : (tlog[i][0] != ' ' ? ACCENT : 0xC618);
    cv.setTextColor(c, 0x0821);
    cv.drawString(tlog[i], 10, y);
    y += 10;
  }
  if (cursor && (millis() / 350) % 2) {
    cv.fillRect(10, y + 1, 5, 7, ACCENT); // 绿色闪烁光标
  }
  cv.pushSprite(0, 0);
}

// 绘制开机动画单帧：支持精确时间回溯与实时渲染
void drawBootFrame(uint32_t t) {
  cv.fillScreen(TFT_BLACK);
  const int cx = SW / 2; // 120

  // 1. 赛博矩阵微光网格背景 (Micro Grid Matrix)
  for (int x = 12; x < SW; x += 24) {
    for (int y = 10; y < SH; y += 20) {
      cv.drawPixel(x, y, 0x0922);
    }
  }

  // 2. 四角战术 HUD 取景标尺 (Tactical Viewfinder Corner Brackets)
  const int CORNER_LEN = 10;
  // 左上
  cv.drawFastHLine(5, 5, CORNER_LEN, ACCENT);
  cv.drawFastVLine(5, 5, CORNER_LEN, ACCENT);
  cv.drawPixel(5, 5, TFT_WHITE);
  // 右上
  cv.drawFastHLine(SW - 5 - CORNER_LEN, 5, CORNER_LEN, ACCENT);
  cv.drawFastVLine(SW - 6, 5, CORNER_LEN, ACCENT);
  cv.drawPixel(SW - 6, 5, TFT_WHITE);
  // 左下
  cv.drawFastHLine(5, SH - 6, CORNER_LEN, ACCENT);
  cv.drawFastVLine(5, SH - 5 - CORNER_LEN, CORNER_LEN, ACCENT);
  cv.drawPixel(5, SH - 6, TFT_WHITE);
  // 右下
  cv.drawFastHLine(SW - 5 - CORNER_LEN, SH - 6, CORNER_LEN, ACCENT);
  cv.drawFastVLine(SW - 6, SH - 5 - CORNER_LEN, CORNER_LEN, ACCENT);
  cv.drawPixel(SW - 6, SH - 6, TFT_WHITE);

  // 3. 顶部微型状态条 (Top HUD Ribbon)
  cv.setTextDatum(top_left);
  cv.setTextFont(0);
  cv.setTextSize(1);
  if ((t / 300) % 2 == 0 || t >= 1000) {
    cv.fillRect(18, 7, 5, 5, ACCENT);
  } else {
    cv.drawRect(18, 7, 5, 5, 0x07FF);
  }
  cv.setTextColor(0x9CD3, TFT_BLACK);
  cv.drawString("ADV-OS v2.5", 27, 6);

  // 右侧核心平台标签
  cv.drawRoundRect(SW - 68, 4, 52, 11, 2, 0x18C3);
  cv.fillRoundRect(SW - 67, 5, 50, 9, 2, 0x0821);
  cv.setTextColor(0x07FF, 0x0821);
  cv.setTextDatum(middle_center);
  cv.drawString("ESP32-S3", SW - 42, 10);

  // 4. 水平光能唤醒束 (0 ~ 350ms)
  if (t < 350) {
    int bw = (int)t * SW / 350;
    if (bw > SW) bw = SW;
    cv.drawFastHLine(cx - bw / 2, 60, bw, ACCENT);
    cv.drawFastHLine(cx - bw / 4, 59, bw / 2, 0x07FF);
    cv.drawFastHLine(cx - bw / 4, 61, bw / 2, 0x07FF);
    return; // 初始光束阶段
  }

  // 5. 核心勋章与总线导轨 (Hero Core Node & Circuit Bus Rails)
  const int NODE_Y = 28;
  cv.drawFastHLine(20, NODE_Y, 84, 0x18C3);
  cv.drawFastHLine(136, NODE_Y, 84, 0x18C3);
  cv.fillCircle(54, NODE_Y, 2, 0x07FF);
  cv.fillCircle(186, NODE_Y, 2, 0x07FF);
  cv.drawPixel(54, NODE_Y, TFT_WHITE);
  cv.drawPixel(186, NODE_Y, TFT_WHITE);

  // 脉冲信号沿导轨移动
  int pulseX1 = 20 + ((t * 80 / 500) % 84);
  int pulseX2 = 220 - ((t * 80 / 500) % 84);
  cv.drawPixel(pulseX1, NODE_Y, 0x07E0);
  cv.drawPixel(pulseX2, NODE_Y, 0x07E0);

  // 六边形科技核心 (Hexagonal Node Core)
  const int HR = 9;
  for (int a = 0; a < 6; a++) {
    float r1 = a * 60.0f * 0.0174533f;
    float r2 = (a + 1) * 60.0f * 0.0174533f;
    int x1 = cx + (int)(cosf(r1) * HR);
    int y1 = NODE_Y + (int)(sinf(r1) * HR);
    int x2 = cx + (int)(cosf(r2) * HR);
    int y2 = NODE_Y + (int)(sinf(r2) * HR);
    cv.drawLine(x1, y1, x2, y2, ACCENT);
  }
  cv.fillCircle(cx, NODE_Y, 3, 0x07FF);
  cv.drawPixel(cx, NODE_Y, TFT_WHITE);

  // 6. "CARDPUTER" 赛博矩阵解密字形 (Decryption Typography)
  const char* TARGET_WORD = "CARDPUTER";
  const int CHAR_W = 16;
  const int WORD_X0 = cx - (9 * CHAR_W) / 2 + 2; // ~50
  const int WORD_Y = 44;
  const char* CIPHER_CHARS = "018$#%&*<>[]/X";
  const int NUM_CIPHERS = 14;

  cv.setTextFont(0);
  cv.setTextSize(2); // 12x16 像素
  cv.setTextDatum(top_left);

  for (int i = 0; i < 9; i++) {
    int charX = WORD_X0 + i * CHAR_W;
    uint32_t lockTime = 320 + i * 45; // 320ms ~ 680ms 逐字锁定
    if (t < lockTime - 120) {
      cv.drawFastHLine(charX + 4, WORD_Y + 7, 5, 0x18C3);
      cv.drawFastVLine(charX + 6, WORD_Y + 5, 5, 0x18C3);
    } else if (t < lockTime) {
      char fakeChar[2] = { CIPHER_CHARS[(t / 25 + i * 3) % NUM_CIPHERS], '\0' };
      cv.setTextColor(((i + t / 40) % 2 == 0) ? 0x07FF : 0x07E0, TFT_BLACK);
      cv.drawString(fakeChar, charX, WORD_Y);
    } else {
      char realChar[2] = { TARGET_WORD[i], '\0' };
      if (t < lockTime + 80) {
        cv.setTextColor(0x07FF, TFT_BLACK);
      } else {
        cv.setTextColor(TFT_WHITE, TFT_BLACK);
      }
      cv.drawString(realChar, charX, WORD_Y);
    }
  }

  // 7. "ADV" 科技胶囊徽标与激光翼轨 (y = 66..80)
  if (t >= 600) {
    int pillW = (t < 750) ? (int)((t - 600) * 56 / 150) : 56;
    if (pillW < 4) pillW = 4;
    int pillH = 15;
    int pillY = 66;
    cv.fillRoundRect(cx - pillW / 2, pillY, pillW, pillH, 3, ACCENT);

    if (t >= 750) {
      int wingL = 18;
      int wingR = SW - 18;
      int leftEdge = cx - pillW / 2 - 4;
      int rightEdge = cx + pillW / 2 + 4;
      if (leftEdge > wingL) {
        cv.drawFastHLine(wingL, pillY + pillH / 2, leftEdge - wingL, 0x18C3);
        cv.drawFastHLine(leftEdge - 15, pillY + pillH / 2, 15, ACCENT);
        cv.fillCircle(wingL, pillY + pillH / 2, 2, ACCENT);
      }
      if (rightEdge < wingR) {
        cv.drawFastHLine(rightEdge, pillY + pillH / 2, wingR - rightEdge, 0x18C3);
        cv.drawFastHLine(rightEdge, pillY + pillH / 2, 15, ACCENT);
        cv.fillCircle(wingR, pillY + pillH / 2, 2, ACCENT);
      }
      cv.setTextDatum(middle_center);
      cv.setTextColor(TFT_BLACK, ACCENT);
      cv.setTextSize(1);
      cv.setTextFont(0);
      cv.drawString("<< ADV >>", cx, pillY + pillH / 2);
    }
  }

  // 8. 实时系统加载阶段文字与能量轨道 (y = 86..120)
  int pct = 0;
  if (t >= 750) {
    if (t >= 1450) pct = 100;
    else pct = (int)((t - 750) * 100 / 700);
  }

  // 状态诊断文字 (y = 87)
  cv.setTextDatum(top_left);
  cv.setTextFont(0);
  cv.setTextSize(1);
  char statStr[40];
  if (pct < 25) {
    snprintf(statStr, sizeof(statStr), ">> CORE INTEGRITY CHECK: OK");
  } else if (pct < 55) {
    snprintf(statStr, sizeof(statStr), ">> BUS INIT: SPI / I2C / I2S DMA");
  } else if (pct < 85) {
    snprintf(statStr, sizeof(statStr), ">> RF STACK: 2.4GHz AIRSPACE READY");
  } else if (pct < 100) {
    snprintf(statStr, sizeof(statStr), ">> SYNCHRONIZING TACTICAL HUD...");
  } else {
    snprintf(statStr, sizeof(statStr), ">> ALL SUBSYSTEMS NOMINAL // READY");
  }
  cv.setTextColor(pct >= 100 ? 0x07E0 : 0x9CD3, TFT_BLACK);
  cv.drawString(statStr, 22, 87);

  // 能量充电导轨外框 (y = 100..110)
  const int RAIL_X = 20;
  const int RAIL_W = 200;
  const int RAIL_Y = 100;
  const int RAIL_H = 10;
  cv.drawRoundRect(RAIL_X, RAIL_Y, RAIL_W, RAIL_H, 2, 0x18C3);

  // 16 节等宽能量 LED 柱 (每节宽 10, 高 6, 间隙 2)
  const int NUM_SEGS = 16;
  int activeSegs = (pct * NUM_SEGS) / 100;
  for (int s = 0; s < NUM_SEGS; s++) {
    int sx = RAIL_X + 5 + s * 12;
    int sy = RAIL_Y + 2;
    if (s < activeSegs) {
      uint16_t segCol = ACCENT;
      if (s >= 14) segCol = TFT_WHITE;
      else if (s >= 11) segCol = 0x07FF;
      cv.fillRect(sx, sy, 10, 6, segCol);
    } else {
      cv.fillRect(sx, sy, 10, 6, 0x0821);
      cv.drawRect(sx, sy, 10, 6, 0x1082);
    }
  }

  // 底部百分比与系统状态 (y = 114)
  cv.setTextDatum(top_left);
  cv.setTextColor(pct >= 100 ? 0x07E0 : 0x7BEF, TFT_BLACK);
  cv.drawString(pct >= 100 ? "TACTICAL OS READY" : "INITIALIZING...", 22, 115);

  char pctStr[12];
  snprintf(pctStr, sizeof(pctStr), "[%3d%%]", pct);
  cv.setTextDatum(top_right);
  cv.setTextColor(pct >= 100 ? 0x07E0 : 0x07FF, TFT_BLACK);
  cv.drawString(pctStr, SW - 22, 115);

  // 9. 激光辉光扫描扫掠效果 (Laser Shimmer Specular Sweep, 1100ms ~ 1550ms)
  if (t >= 1100 && t < 1550) {
    int sweepX = 24 + (int)((t - 1100) * (SW - 48) / 450);
    cv.drawFastVLine(sweepX, 40, 42, TFT_WHITE);
    if (sweepX > 0) cv.drawFastVLine(sweepX - 1, 42, 38, 0x07FF);
    if (sweepX < SW - 1) cv.drawFastVLine(sweepX + 1, 42, 38, 0x07FF);
    if (sweepX > 1) cv.drawFastVLine(sweepX - 2, 45, 32, 0x001F);
    if (sweepX < SW - 2) cv.drawFastVLine(sweepX + 2, 45, 32, 0x001F);
  }
}

// 开机动画：赛博战术终端唤醒序列 + 矩阵解密 + 能量轨道 + 合成音效。约 1.8s。
void bootAnim() {
  uint32_t t0 = millis();
  M5.Display.setBrightness(0);
  bool s1 = false, s2 = false, s3 = false, s4 = false, s5 = false;
  int vol = (bootSoundOn && volVal() > 0) ? volVal() : 0;

  for (;;) {
    uint32_t t = millis() - t0;
    if (t > 1800) break;

    // 音频合成启动音效 (Ascending Cyber Chime Arpeggio)
    if (vol > 0) {
      if (t >= 180 && !s1) { M5.Speaker.tone(523, 30); s1 = true; }       // C5
      else if (t >= 340 && !s2) { M5.Speaker.tone(659, 30); s2 = true; }  // E5
      else if (t >= 520 && !s3) { M5.Speaker.tone(784, 35); s3 = true; }  // G5
      else if (t >= 750 && !s4) { M5.Speaker.tone(1046, 45); s4 = true; } // C6
      else if (t >= 1450 && !s5) { M5.Speaker.tone(1568, 80); s5 = true; } // G6 Ready!
    }

    drawBootFrame(t);
    cv.pushSprite(0, 0);

    // 背光平滑爬升
    int targetB = brightVal();
    M5.Display.setBrightness(t < 350 ? (int)(targetB * t / 350) : targetB);

    // 用户按键跳过
    M5.update();
    if (M5.BtnA.wasPressed() || kbd::readKey() != 0) {
      break;
    }
    delay(16);
  }
  M5.Display.setBrightness(brightVal());
}



