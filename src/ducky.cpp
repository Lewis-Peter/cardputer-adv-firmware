// 支持的关键字（不认识的行直接跳过，不会中断整个脚本）：
//   REM ...                注释
//   STRING <text>          原样打字（ASCII，走 bt.cpp 的键位表）
//   STRINGLN <text>        打字 + 回车
//   ENTER / TAB / BACKSPACE / ESC / DELETE
//   UP / DOWN / LEFT / RIGHT / HOME / END / PAGEUP / PAGEDOWN
//   GUI <key> (或 WINDOWS <key>) / CTRL <key> / ALT <key> / SHIFT <key>   —— 单键组合
//   CTRL ALT <key>（含 CTRL ALT DEL） / CTRL SHIFT <key>                 —— 双修饰键组合
//   DELAY <ms>             这一行专属延时，不叠加 DEFAULTDELAY
//   DEFAULTDELAY <ms> / DEFAULT_DELAY <ms>   之后每行之间的默认间隔
//   REPEAT <n>             把上一条（非REPEAT）指令再执行 n 次
#include "ducky.h"
#include "bt.h"
#include "sd_files.h"
#include "ui_common.h"
#include <SD.h>
#include <ctype.h>

static const uint8_t HID_ESC       = 0x29;
static const uint8_t HID_DELETE    = 0x4C;
static const uint8_t HID_UP        = 0x52;
static const uint8_t HID_DOWN      = 0x51;
static const uint8_t HID_LEFT      = 0x50;
static const uint8_t HID_RIGHT     = 0x4F;
static const uint8_t HID_HOME      = 0x4A;
static const uint8_t HID_END       = 0x4D;
static const uint8_t HID_PAGEUP    = 0x4B;
static const uint8_t HID_PAGEDOWN  = 0x4E;
static const uint8_t MOD_CTRL  = 0x01;
static const uint8_t MOD_SHIFT = 0x02;
static const uint8_t MOD_ALT   = 0x04;
static const uint8_t MOD_GUI   = 0x08;

enum DkMode { DK_PICK, DK_READY, DK_RUN, DK_DONE };
static DkMode mode = DK_PICK;

static String script;          // 整个脚本内容，一次性读进内存（上限见 loadScript）
static int scriptPos = 0;      // 下一行开始的偏移
static int lineNum = 0;
static int totalLines = 0;
static String lastCmd = "";
static String scriptName = "";
static uint32_t nextActionMs = 0;
static uint32_t defaultDelayMs = 0;
static bool customDelaySet = false;
static String repeatLine = "";
static int repeatRemaining = 0;

static void sendString(const String& s) {
  for (size_t i = 0; i < s.length(); i++) btKeyboardSendKey(s[i]);
}

static int countLines(const String& s) {
  int n = 1;
  for (size_t i = 0; i < s.length(); i++) if (s[i] == '\n') n++;
  return n;
}

static void execLine(const String& raw) {
  customDelaySet = false;
  String line = raw; line.trim();
  lastCmd = line.length() ? line : String("(blank)");
  if (line.length() == 0 || line.startsWith("#")) return;

  String up = line; up.toUpperCase();
  if (up.startsWith("REM")) return;
  if (up.startsWith("REPEAT ")) { repeatRemaining = line.substring(7).toInt(); return; }

  repeatLine = line;   // 记住"最近一条真正执行过的指令"，给下一个 REPEAT 用

  if      (up.startsWith("STRINGLN ")) { sendString(line.substring(9)); btKeyboardSendKey('\n'); }
  else if (up.startsWith("STRING "))   { sendString(line.substring(7)); }
  else if (up == "ENTER")     btKeyboardSendKey('\n');
  else if (up == "TAB")       btKeyboardSendKey('\t');
  else if (up == "BACKSPACE") btKeyboardSendKey('\b');
  else if (up == "ESC" || up == "ESCAPE") btKeyboardSendRaw(0, HID_ESC);
  else if (up == "DELETE")    btKeyboardSendRaw(0, HID_DELETE);
  else if (up == "UP" || up == "UPARROW")       btKeyboardSendRaw(0, HID_UP);
  else if (up == "DOWN" || up == "DOWNARROW")   btKeyboardSendRaw(0, HID_DOWN);
  else if (up == "LEFT" || up == "LEFTARROW")   btKeyboardSendRaw(0, HID_LEFT);
  else if (up == "RIGHT" || up == "RIGHTARROW") btKeyboardSendRaw(0, HID_RIGHT);
  else if (up == "HOME")      btKeyboardSendRaw(0, HID_HOME);
  else if (up == "END")       btKeyboardSendRaw(0, HID_END);
  else if (up == "PAGEUP")    btKeyboardSendRaw(0, HID_PAGEUP);
  else if (up == "PAGEDOWN")  btKeyboardSendRaw(0, HID_PAGEDOWN);
  else if (up.startsWith("CTRL ALT ")) {
    String arg = up.substring(9);
    if (arg == "DEL" || arg == "DELETE") btKeyboardSendRaw(MOD_CTRL | MOD_ALT, HID_DELETE);
    else if (line.length() > 9) btKeyboardSendRaw(MOD_CTRL | MOD_ALT, btKeyUsageFor(tolower(line[9])));
  }
  else if (up.startsWith("CTRL SHIFT ") && line.length() > 11) {
    btKeyboardSendRaw(MOD_CTRL | MOD_SHIFT, btKeyUsageFor(tolower(line[11])));
  }
  else if ((up.startsWith("GUI ") || up.startsWith("WINDOWS ")) ) {
    int sp = line.indexOf(' ');
    if (sp >= 0 && line.length() > (size_t)sp + 1) btKeyboardSendRaw(MOD_GUI, btKeyUsageFor(tolower(line[sp + 1])));
  }
  else if (up.startsWith("CTRL ") && line.length() > 5)  btKeyboardSendRaw(MOD_CTRL,  btKeyUsageFor(tolower(line[5])));
  else if (up.startsWith("ALT ")  && line.length() > 4)  btKeyboardSendRaw(MOD_ALT,   btKeyUsageFor(tolower(line[4])));
  else if (up.startsWith("SHIFT ")&& line.length() > 6)  btKeyboardSendRaw(MOD_SHIFT, btKeyUsageFor(tolower(line[6])));
  else if (up.startsWith("DELAY ")) {
    long ms = line.substring(6).toInt();
    nextActionMs = millis() + (uint32_t)(ms > 0 ? ms : 0);
    customDelaySet = true;
  }
  else if (up.startsWith("DEFAULTDELAY ") || up.startsWith("DEFAULT_DELAY ")) {
    int sp = line.indexOf(' ');
    long ms = line.substring(sp + 1).toInt();
    defaultDelayMs = (uint32_t)(ms > 0 ? ms : 0);
  }
  // 其余不认识的关键字：忽略，继续跑下一行，不因为一行没见过就整个中断
}

static const size_t SCRIPT_MAX = 8192;   // 够绝大多数Ducky脚本；再大的直接截断

static bool loadScript(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  // 按块读、先 reserve 好：Arduino 的 String 每次 += 都可能重新分配+拷贝，一个字节一个字节
  // 追加 8KB 就是几千次 realloc（O(n²)），在这块板子上肉眼可见地卡一下。
  size_t want = f.size() < SCRIPT_MAX ? (size_t)f.size() : SCRIPT_MAX;
  script = "";
  script.reserve(want + 1);
  char chunk[129];
  while (script.length() < want) {
    size_t room = want - script.length();
    if (room > sizeof(chunk) - 1) room = sizeof(chunk) - 1;
    int got = f.readBytes(chunk, room);
    if (got <= 0) break;
    chunk[got] = '\0';
    script += chunk;
  }
  f.close();
  scriptPos = 0; lineNum = 0; totalLines = countLines(script);
  repeatRemaining = 0; repeatLine = ""; defaultDelayMs = 0; lastCmd = "";
  return true;
}

void duckyEnter() {
  mode = DK_PICK;
  curPath = "/";
  loadDir(curPath);
  btKeyboardStart();   // 跟 Keyboard mode 一样：开始广播，等主机配对连接
}

void duckyExit() {
  script = "";
  lastCmd = "";
  repeatLine = "";
  scriptName = "";
  filesExit();
}

void duckyUpdate() {
  if (mode != DK_RUN) return;
  uint32_t now = millis();
  // 有符号差值比较，millis() 跨过 49.7 天翻转时也不会误判成"还要再等 49 天"
  if ((int32_t)(now - nextActionMs) < 0) return;

  if (repeatRemaining > 0) {
    repeatRemaining--;
    execLine(repeatLine);
    lineNum++;
    if (!customDelaySet) nextActionMs = now + defaultDelayMs;
    return;
  }

  if (scriptPos >= (int)script.length()) { mode = DK_DONE; return; }

  int nl = script.indexOf('\n', scriptPos);
  String line = (nl < 0) ? script.substring(scriptPos) : script.substring(scriptPos, nl);
  scriptPos = (nl < 0) ? script.length() : nl + 1;
  lineNum++;

  execLine(line);
  if (!customDelaySet) nextActionMs = now + defaultDelayMs;
}

void duckyKey(char k) {
  if (mode == DK_PICK) {
    if ((k == ';' || k == ',') && fileCount > 0) {
      fileIdx = (fileIdx - 1 + fileCount) % fileCount;
      if (fileIdx < fileTop) fileTop = fileIdx;
    } else if ((k == '.' || k == '/') && fileCount > 0) {
      fileIdx = (fileIdx + 1) % fileCount;
      if (fileIdx >= fileTop + FILES_VIS) fileTop = fileIdx - FILES_VIS + 1;
    } else if (k == '\n' && fileCount > 0 && fileList) {
      if (fileList[fileIdx].isDir) {
        curPath = joinPath(curPath, fileList[fileIdx].name);
        loadDir(curPath);
      } else if (fileList[fileIdx].name.endsWith(".txt")) {
        if (loadScript(joinPath(curPath, fileList[fileIdx].name))) {
          scriptName = fileList[fileIdx].name;
          mode = DK_READY;
        }
      }
    }
  } else if (mode == DK_READY) {
    if (k == '\n' && btIsConnected()) { mode = DK_RUN; nextActionMs = millis(); }
    else if (k == 'p' || k == 'P') mode = DK_PICK;
  } else if (mode == DK_DONE) {
    if (k == '\n') mode = DK_PICK;
  }
  // DK_RUN 期间不接受额外按键；` 由 main.cpp 直接接 cleanupApp() 中止整个 app
}

void drawDucky() {
  cv.fillScreen(TFT_BLACK);

  const char* modeBadge = (mode == DK_PICK) ? "FILE PICK" :
                          (mode == DK_READY) ? "ARMED" :
                          (mode == DK_RUN) ? "INJECTING" : "COMPLETE";
  uint16_t modeCol = (mode == DK_RUN) ? 0xFDA0 :
                     (mode == DK_DONE) ? 0x07E0 :
                     (btIsConnected() ? 0x07E0 : 0x07FF);
  drawPageHeader("BadUSB Ducky", modeBadge, modeCol);

  if (mode == DK_PICK) {
    const int cardX = 4, cardW = SW - 8;
    const int topY = 16, topH = 104;

    if (!sdReady()) {
      cv.fillRoundRect(cardX, topY, cardW, topH, 3, 0x0821);
      cv.drawRoundRect(cardX, topY, cardW, topH, 3, 0x3800);
      cv.fillRect(cardX, topY + 4, 3, topH - 8, 0xF800);
      cv.setTextDatum(middle_center); cv.setTextSize(1);
      cv.setTextColor(0xF800, 0x0821);
      cv.drawString("[!] SD CARD NOT MOUNTED", SW / 2, topY + 40);
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString("Insert FAT32 MicroSD card", SW / 2, topY + 56);
      cv.setTextColor(0x632C, 0x0821);
      cv.drawString("Place .txt ducky scripts on card", SW / 2, topY + 70);
    } else if (fileCount == 0 || !fileList) {
      cv.fillRoundRect(cardX, topY, cardW, topH, 3, 0x0821);
      cv.drawRoundRect(cardX, topY, cardW, topH, 3, 0x18C3);
      cv.fillRect(cardX, topY + 4, 3, topH - 8, 0x07FF);
      cv.setTextDatum(middle_center); cv.setTextSize(1);
      cv.setTextColor(0x07FF, 0x0821);
      cv.drawString("NO DUCKY SCRIPTS FOUND", SW / 2, topY + 36);
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString("Path: " + truncPx(curPath, 180), SW / 2, topY + 52);
      cv.setTextColor(0x632C, 0x0821);
      cv.drawString("Place .txt scripts in /ducky/ or root", SW / 2, topY + 68);
    } else {
      // 瓷砖文件列表
      const int visSlots = 5;
      const int rowH = 19, gapY = 2;
      for (int p = 0; p < visSlots; p++) {
        int i = fileTop + p;
        if (i >= fileCount) break;
        bool sel = (i == fileIdx);
        int y = topY + p * (rowH + gapY);

        uint16_t rowBg = sel ? 0x1084 : 0x0821;
        uint16_t rowBdr = sel ? 0x07FF : 0x18C3;
        cv.fillRoundRect(cardX, y, cardW, rowH, 3, rowBg);
        cv.drawRoundRect(cardX, y, cardW, rowH, 3, rowBdr);
        if (sel) cv.fillRect(cardX, y + 2, 3, rowH - 4, 0x07FF);

        // 左侧类型徽标
        const int badgeW = 28, badgeH = 11;
        const int badgeX = cardX + 6, badgeY = y + 4;
        bool isDir = fileList[i].isDir;
        uint16_t tBg = isDir ? 0x0280 : 0x2180;
        uint16_t tBdr = isDir ? 0x04A0 : 0x4300;
        uint16_t tCol = isDir ? 0x07E0 : 0xFDA0;
        cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, tBg);
        cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, tBdr);
        cv.setTextDatum(middle_center); cv.setTextSize(1);
        cv.setTextColor(tCol, tBg);
        cv.drawString(isDir ? "DIR" : "TXT", badgeX + badgeW / 2, badgeY + badgeH / 2);

        // 文件名
        cv.setTextDatum(middle_left);
        cv.setTextColor(sel ? TFT_WHITE : 0xCE79, rowBg);
        cv.drawString(truncPx(fileList[i].name, 128), cardX + 38, y + rowH / 2);

        // 右侧大小或子项数
        cv.setTextDatum(middle_right);
        cv.setTextColor(sel ? 0x07FF : 0x632C, rowBg);
        if (isDir) {
          cv.drawString("FOLDER", cardX + cardW - 8, y + rowH / 2);
        } else {
          cv.drawString(fmtBytes(fileList[i].size), cardX + cardW - 8, y + rowH / 2);
        }
      }
    }

    // 底部快捷键
    const int footY = 125;
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 4, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" open", 32, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString(";.", 74, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" select", 88, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 188, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" menu", 194, footY);

  } else if (mode == DK_READY) {
    const int cardX = 4, cardW = SW - 8;

    // 1. 上方装载脚本卡片 (y = 16..66, h = 51)
    const int topY = 16, topH = 51;
    cv.fillRoundRect(cardX, topY, cardW, topH, 3, 0x0821);
    cv.drawRoundRect(cardX, topY, cardW, topH, 3, 0x07FF);
    cv.fillRect(cardX, topY + 4, 3, topH - 8, 0x07FF);

    cv.setTextDatum(top_left); cv.setTextSize(1);
    cv.setTextColor(0x07FF, 0x0821);
    cv.drawString("ARMED DUCKY SCRIPT //", cardX + 8, topY + 5);

    cv.setTextDatum(top_right);
    char lBuf[24];
    snprintf(lBuf, sizeof(lBuf), "%d LINES", totalLines);
    cv.setTextColor(0xFDA0, 0x0821);
    cv.drawString(lBuf, cardX + cardW - 8, topY + 5);

    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setTextColor(TFT_WHITE, 0x0821);
    cv.drawString(truncPx(scriptName, cardW - 16), cardX + 8, topY + 23);

    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("TARGET: BLE HID KEYBOARD", cardX + 8, topY + 39);

    // 2. 下方目标连接就绪卡片 (y = 70..120, h = 51)
    const int botY = 70, botH = 51;
    bool connected = btIsConnected();
    uint16_t bdrCol = connected ? 0x07E0 : 0xFDA0;
    cv.fillRoundRect(cardX, botY, cardW, botH, 3, 0x0821);
    cv.drawRoundRect(cardX, botY, cardW, botH, 3, bdrCol);
    cv.fillRect(cardX, botY + 4, 3, botH - 8, connected ? 0x07E0 : 0xFDA0);

    cv.setTextDatum(top_left);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("HOST LINK STATUS:", cardX + 8, botY + 5);

    // 状态胶囊
    const int badgeW = 90, badgeH = 12;
    const int badgeX = cardX + cardW - badgeW - 6, badgeY = botY + 4;
    cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, connected ? 0x0280 : 0x3A00);
    cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, connected ? 0x04A0 : 0x7BE0);
    cv.setTextDatum(middle_center);
    cv.setTextColor(connected ? 0x07E0 : 0xFDA0, connected ? 0x0280 : 0x3A00);
    cv.drawString(connected ? "BLE PAIRED" : "ADVERTISING", badgeX + badgeW / 2, badgeY + badgeH / 2);

    cv.setTextDatum(middle_left);
    if (connected) {
      cv.setTextColor(0x07E0, 0x0821);
      cv.drawString("Host connected. Ready to inject.", cardX + 8, botY + 26);
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString("Press [Enter] to launch payload now.", cardX + 8, botY + 39);
    } else {
      cv.setTextColor(0xFDA0, 0x0821);
      cv.drawString("Pair \"Cardputer ADV\" in host Bluetooth", cardX + 8, botY + 26);
      cv.setTextColor(0x632C, 0x0821);
      cv.drawString("Inject unlocks automatically upon link", cardX + 8, botY + 39);
    }

    // 底部快捷键
    const int footY = 125;
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    if (connected) {
      cv.setTextColor(0x07E0, TFT_BLACK); cv.drawString("Enter", 4, footY);
      cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" inject", 32, footY);
    } else {
      cv.setTextColor(0x632C, TFT_BLACK); cv.drawString("Enter", 4, footY);
      cv.setTextColor(0x4208, TFT_BLACK); cv.drawString(" wait pair", 32, footY);
    }

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("P", 100, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" select other", 106, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 188, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" menu", 194, footY);

  } else if (mode == DK_RUN) {
    const int cardX = 4, cardW = SW - 8;

    // 1. 上方注入进度仪表舱 (y = 16..54, h = 39)
    const int topY = 16, topH = 39;
    cv.fillRoundRect(cardX, topY, cardW, topH, 3, 0x0821);
    cv.drawRoundRect(cardX, topY, cardW, topH, 3, 0xFDA0);
    cv.fillRect(cardX, topY + 4, 3, topH - 8, 0xFDA0);

    cv.setTextDatum(top_left); cv.setTextSize(1);
    cv.setTextColor(0xFDA0, 0x0821);
    cv.drawString("INJECTING KEYSTROKES...", cardX + 8, topY + 5);

    cv.setTextDatum(top_right);
    char pBuf[32];
    snprintf(pBuf, sizeof(pBuf), "LINE %d / %d", lineNum, totalLines);
    cv.setTextColor(TFT_WHITE, 0x0821);
    cv.drawString(pBuf, cardX + cardW - 8, topY + 5);

    // 进度条
    const int barX = cardX + 8, barY = topY + 20, barW = cardW - 16, barH = 10;
    cv.fillRoundRect(barX, barY, barW, barH, 2, 0x18C3);
    int fillW = (totalLines > 0) ? (lineNum * (barW - 2) / totalLines) : 0;
    if (fillW > barW - 2) fillW = barW - 2;
    if (fillW > 0) cv.fillRoundRect(barX + 1, barY + 1, fillW, barH - 2, 2, 0x07E0);

    // 2. 下方指令流监视卡片 (y = 57..120, h = 64)
    const int botY = 57, botH = 64;
    cv.fillRoundRect(cardX, botY, cardW, botH, 3, 0x0821);
    cv.drawRoundRect(cardX, botY, cardW, botH, 3, 0x18C3);
    cv.fillRect(cardX, botY + 4, 3, botH - 8, 0x07FF);

    cv.setTextDatum(top_left);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("ACTIVE INSTRUCTION STREAM:", cardX + 8, botY + 5);

    cv.setTextDatum(top_right);
    if (repeatRemaining > 0) {
      char rBuf[24];
      snprintf(rBuf, sizeof(rBuf), "REPEAT x%d", repeatRemaining);
      cv.setTextColor(0x07FF, 0x0821);
      cv.drawString(rBuf, cardX + cardW - 8, botY + 5);
    } else {
      cv.setTextColor(0x07E0, 0x0821);
      cv.drawString("BLE HID -> HOST", cardX + cardW - 8, botY + 5);
    }

    cv.drawFastHLine(cardX + 6, botY + 16, cardW - 12, 0x18C3);

    // 当前执行指令预览框
    const int cmdBoxY = botY + 22, cmdBoxH = 22;
    cv.fillRoundRect(cardX + 8, cmdBoxY, cardW - 16, cmdBoxH, 2, 0x0280);
    cv.drawRoundRect(cardX + 8, cmdBoxY, cardW - 16, cmdBoxH, 2, 0x04A0);

    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setTextColor(TFT_WHITE, 0x0280);
    String dispCmd = "> " + (lastCmd.length() ? lastCmd : String("(idle)"));
    cv.drawString(truncPx(dispCmd, cardW - 28), cardX + 14, cmdBoxY + cmdBoxH / 2);

    cv.setTextColor(0x632C, 0x0821);
    cv.drawString("DO NOT DISCONNECT // ESCAPE = `", cardX + 8, botY + 52);

    // 底部按键提示
    const int footY = 125;
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setTextColor(0xF800, TFT_BLACK); cv.drawString("`", 4, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" abort payload execution", 10, footY);

  } else {   // DK_DONE
    const int cardX = 4, cardW = SW - 8;
    const int topY = 16, topH = 104;

    cv.fillRoundRect(cardX, topY, cardW, topH, 3, 0x0821);
    cv.drawRoundRect(cardX, topY, cardW, topH, 3, 0x07E0);
    cv.fillRect(cardX, topY + 4, 3, topH - 8, 0x07E0);

    // 顶部完成横幅
    cv.setTextDatum(top_left); cv.setTextSize(1);
    cv.setTextColor(0x07E0, 0x0821);
    cv.drawString("MISSION ACCOMPLISHED //", cardX + 8, topY + 6);

    cv.setTextDatum(top_right);
    cv.drawString("SUCCESS", cardX + cardW - 8, topY + 6);

    cv.drawFastHLine(cardX + 6, topY + 18, cardW - 12, 0x04A0);

    cv.setTextDatum(top_left);
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("Script:", cardX + 8, topY + 26);
    cv.setTextColor(TFT_WHITE, 0x0821);
    cv.drawString(truncPx(scriptName, cardW - 60), cardX + 56, topY + 26);

    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("Result:", cardX + 8, topY + 40);
    char doneBuf[48];
    snprintf(doneBuf, sizeof(doneBuf), "%d lines dispatched cleanly", totalLines);
    cv.setTextColor(0x07E0, 0x0821);
    cv.drawString(doneBuf, cardX + 56, topY + 40);

    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString("Channel:", cardX + 8, topY + 54);
    cv.setTextColor(0x07FF, 0x0821);
    cv.drawString("Bluetooth LE Keyboard", cardX + 56, topY + 54);

    cv.setTextDatum(middle_center);
    cv.setTextColor(0xFDA0, 0x0821);
    cv.drawString("Press [Enter] to select another script", SW / 2, topY + 84);

    // 底部快捷键
    const int footY = 125;
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 4, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" pick new", 32, footY);

    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 188, footY);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" menu", 194, footY);
  }
}
