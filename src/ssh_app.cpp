#include "ssh_app.h"
#include "config.h"
#include "globals.h"
#include "keyboard_adv.h"
#include "ui_common.h"
#include "sd_files.h"
#include "bt.h"
#include <WiFi.h>
#include <SD.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <atomic>
#include "libssh_esp32.h"
#include <libssh/libssh.h>

// =============================================================================
// SSH Client for Cardputer ADV (ESP32-S3FN8, No PSRAM)
// =============================================================================
// 内存策略核心：
// 1. 在进入活动 SSH 连接/终端时，通过 cv.deleteSprite() 释放 64.8KB 全屏画布，
//    转由 M5.Display 硬件直接写字符（零闪烁、极速、无显存浪费）。
// 2. 释放后 SRAM 空闲暴涨至 140KB+，足以为 LibSSH 分配 24KB 独立任务栈及加密握手堆。
// 3. 断开连接或退出 app 时，安全注销 LibSSH 与任务，恢复 cv.createSprite(SW, SH)，
//    保证主系统其他 40+ 个页面零风险无感恢复。
// =============================================================================

enum SshState {
  SSH_ST_CONFIG,
  SSH_ST_CONNECTING,
  SSH_ST_TERMINAL,
  SSH_ST_ERROR
};

static volatile SshState sshState = SSH_ST_CONFIG;

// 配置字段
static char sshName[32] = CFG_SSH_DEFAULT_NAME;
static char sshHost[64] = CFG_SSH_DEFAULT_HOST;
static uint16_t sshPort = 22;
static char sshUser[32] = CFG_SSH_DEFAULT_USER;
static char sshPass[64] = "";
static char sshKeyFile[64] = "";

enum SshAuthMode {
  AUTH_KEY = 0,
  AUTH_PASSWORD = 1
};
static volatile SshAuthMode authMode = AUTH_KEY;

// 选单项
enum CfgItem {
  CFG_HOST = 0,
  CFG_PORT,
  CFG_USER,
  CFG_AUTH,
  CFG_PASS,
  CFG_FONT,
  CFG_CONNECT,
  CFG_COUNT
};

static int cfgSel = CFG_CONNECT;

enum EditField {
  EF_NONE,
  EF_HOST,
  EF_PORT,
  EF_USER,
  EF_PASS
};

static EditField editField = EF_NONE;
static String editBuf = "";
static char configStatusMsg[48] = "";
static char connStatusMsg[48] = "";
static char errMsg[64] = "";
static bool connBgDrawn = false;
static int lastDrawnStage = -1;
static char lastDrawnMsg[48] = "";
static char lastSpinChar = 0;

// FreeRTOS Worker 与队列
static volatile bool stopRequested = false;
static volatile bool taskRunning = false;
static TaskHandle_t volatile sshTaskHandle = nullptr;
// libssh 资源的清理权：任务收尾（task_end）和主线程强删（sshExit 超时）谁先 CAS 抢到谁释放，
// 另一方绝不碰——否则两边都 ssh_free 同一个 session 就是 double free。
enum { SSH_CLEANUP_NONE = 0, SSH_CLEANUP_TASK = 1, SSH_CLEANUP_MAIN = 2 };
static std::atomic<int> sshCleanupOwner{SSH_CLEANUP_NONE};
static QueueHandle_t txQueue = nullptr;
static QueueHandle_t rxQueue = nullptr;
static volatile ssh_session currentSession = nullptr;
static volatile ssh_channel currentChannel = nullptr;
static volatile uint32_t lastTxMs = 0;
static volatile uint32_t lastRxMs = 0;

// =============================================================================
// VT100 / ANSI 终端引擎 (支持 8x16 大字 / 8x8 宽体 / 6x8 紧凑)
// =============================================================================
static const int MAX_TERM_COLS = 40;
static const int MAX_TERM_ROWS = 16;

enum SshFontMode {
  SSH_FONT_LARGE = 0,   // 8x16: 30 列 x 8 行 (经典 VGA 大字，字号大 2 倍，极易辨认)
  SSH_FONT_MEDIUM = 1,  // 8x8:  30 列 x 15 行 (C64 宽体中字，兼顾清晰度与行数)
  SSH_FONT_SMALL = 2,   // 6x8:  40 列 x 15 行 (紧凑小字，最大显示 40 列)
  SSH_FONT_COUNT = 3
};

static SshFontMode fontMode = SSH_FONT_LARGE;
static int curCols = 30;
static int curRows = 8;
static int curCharW = 8;
static int curCharH = 16;
static int curTopBarH = 7;
static const lgfx::IFont* curFont = &fonts::AsciiFont8x16;

static volatile bool reqPtyResize = false;
static volatile int reqPtyCols = 30;
static volatile int reqPtyRows = 8;

static void applyFontMode(SshFontMode mode) {
  fontMode = mode;
  switch (fontMode) {
    case SSH_FONT_LARGE:
      curCols = 30;
      curRows = 8;
      curCharW = 8;
      curCharH = 16;
      curTopBarH = 7;
      curFont = &fonts::AsciiFont8x16;
      break;
    case SSH_FONT_MEDIUM:
      curCols = 30;
      curRows = 15;
      curCharW = 8;
      curCharH = 8;
      curTopBarH = 10;
      curFont = &fonts::Font8x8C64;
      break;
    case SSH_FONT_SMALL:
    default:
      curCols = 40;
      curRows = 15;
      curCharW = 6;
      curCharH = 8;
      curTopBarH = 10;
      curFont = &fonts::Font0;
      break;
  }
}


struct TermCell {
  char c;
  uint16_t fg;
  uint16_t bg;
};

static TermCell termBuf[MAX_TERM_ROWS][MAX_TERM_COLS];
static bool lineDirty[MAX_TERM_ROWS];
static int cursorX = 0;
static int cursorY = 0;
static int prevCursorX = 0;
static int prevCursorY = 0;
static int savedCursorX = 0;
static int savedCursorY = 0;
static uint16_t curFg = TFT_WHITE;
static uint16_t curBg = TFT_BLACK;
static bool termCursorBlink = true;
static uint32_t lastCursorBlinkMs = 0;
// 顶栏脏标记：只有连接状态变化时才重画（避免光标闪烁每帧都刷顶栏）
static bool topBarDirty = true;
static bool prevTaskRunning = false;
static bool wrapPending = false;

enum AnsiState {
  ANSI_NORMAL,
  ANSI_ESC,
  ANSI_CSI,
  ANSI_OSC,
  ANSI_CHARSET
};

static AnsiState ansiState = ANSI_NORMAL;
static char csiBuf[32];
static uint8_t csiLen = 0;

// UTF-8 多字节序列解码缓冲
static uint8_t utf8Buf[4];
static uint8_t utf8Len = 0;
static uint8_t utf8Expected = 0;

static uint16_t ansiColor565(int code, bool isBright) {
  switch (code % 8) {
    case 0: return isBright ? 0x4208 : 0x0000;          // Black / Bright Dark Gray
    case 1: return isBright ? TFT_RED : 0xD800;         // Red
    case 2: return isBright ? TFT_GREEN : 0x05E0;       // Green
    case 3: return isBright ? TFT_YELLOW : 0xDE60;      // Yellow
    case 4: return isBright ? 0x5CDF : 0x3A5F;          // Blue
    case 5: return isBright ? TFT_MAGENTA : 0xD81B;     // Magenta
    case 6: return isBright ? TFT_CYAN : 0x05BF;        // Cyan
    case 7: return isBright ? TFT_WHITE : 0xCE79;       // White
    default: return TFT_WHITE;
  }
}

static void termScrollUp() {
  memmove(&termBuf[0], &termBuf[1], sizeof(termBuf[0]) * (curRows - 1));
  for (int col = 0; col < MAX_TERM_COLS; col++) {
    termBuf[curRows - 1][col] = { ' ', curFg, curBg };
  }
  for (int r = 0; r < curRows; r++) {
    lineDirty[r] = true;
  }
}

static void termReset() {
  for (int r = 0; r < MAX_TERM_ROWS; r++) {
    for (int c = 0; c < MAX_TERM_COLS; c++) {
      termBuf[r][c] = { ' ', TFT_WHITE, TFT_BLACK };
    }
    lineDirty[r] = true;
  }
  cursorX = cursorY = 0;
  prevCursorX = prevCursorY = 0;
  savedCursorX = savedCursorY = 0;
  curFg = TFT_WHITE;
  curBg = TFT_BLACK;
  ansiState = ANSI_NORMAL;
  utf8Len = utf8Expected = 0;
  topBarDirty = true;        // 连接建立后顶栏需要重画
  prevTaskRunning = false;
  wrapPending = false;
}

static void switchFontMode(SshFontMode newMode) {
  applyFontMode(newMode);
  saveInt("ssh", "font", (int)fontMode);

  if (sshState == SSH_ST_TERMINAL) {
    reqPtyCols = curCols;
    reqPtyRows = curRows;
    reqPtyResize = true;

    M5.Display.fillScreen(TFT_BLACK);
    termReset();
    topBarDirty = true;
    dirty = true;
  }
}

static void handleCsi(char cmd) {
  wrapPending = false;
  int args[8] = {0};
  int argCount = 0;
  char* p = csiBuf;
  if (*p == '?') p++;  // 忽略前导 '?' (DECTCEM 等私有控制序列)

  while (*p && argCount < 8) {
    args[argCount++] = atoi(p);
    char* comma = strchr(p, ';');
    if (!comma) break;
    p = comma + 1;
  }

  switch (cmd) {
    case 'm': {  // SGR 配色与样式（支持 16色、256色、RGB 24位色）
      if (argCount == 0) {
        curFg = TFT_WHITE;
        curBg = TFT_BLACK;
      } else {
        for (int i = 0; i < argCount; i++) {
          int a = args[i];
          if (a == 0) {
            curFg = TFT_WHITE;
            curBg = TFT_BLACK;
          } else if (a == 7) {
            uint16_t tmp = curFg; curFg = curBg; curBg = tmp;
          } else if (a == 38) {
            // 前景色：38;5;<n> (256色) 或 38;2;<r>;<g>;<b> (RGB)
            if (i + 2 < argCount && args[i + 1] == 5) {
              int n = args[i + 2];
              if (n < 8) curFg = ansiColor565(n, false);
              else if (n < 16) curFg = ansiColor565(n - 8, true);
              else if (n >= 232) {
                uint8_t g = (n - 232) * 10 + 8;
                curFg = M5.Display.color565(g, g, g);
              } else {
                int c = n - 16;
                uint8_t b = (c % 6) ? (c % 6) * 40 + 55 : 0;
                uint8_t g = ((c / 6) % 6) ? ((c / 6) % 6) * 40 + 55 : 0;
                uint8_t r = (c / 36) ? (c / 36) * 40 + 55 : 0;
                curFg = M5.Display.color565(r, g, b);
              }
              i += 2;
            } else if (i + 4 < argCount && args[i + 1] == 2) {
              curFg = M5.Display.color565(args[i + 2], args[i + 3], args[i + 4]);
              i += 4;
            }
          } else if (a == 48) {
            // 背景色：48;5;<n> 或 48;2;<r>;<g>;<b>
            if (i + 2 < argCount && args[i + 1] == 5) {
              int n = args[i + 2];
              if (n < 8) curBg = ansiColor565(n, false);
              else if (n < 16) curBg = ansiColor565(n - 8, true);
              else if (n >= 232) {
                uint8_t g = (n - 232) * 10 + 8;
                curBg = M5.Display.color565(g, g, g);
              } else {
                int c = n - 16;
                uint8_t b = (c % 6) ? (c % 6) * 40 + 55 : 0;
                uint8_t g = ((c / 6) % 6) ? ((c / 6) % 6) * 40 + 55 : 0;
                uint8_t r = (c / 36) ? (c / 36) * 40 + 55 : 0;
                curBg = M5.Display.color565(r, g, b);
              }
              i += 2;
            } else if (i + 4 < argCount && args[i + 1] == 2) {
              curBg = M5.Display.color565(args[i + 2], args[i + 3], args[i + 4]);
              i += 4;
            }
          } else if (a >= 30 && a <= 37) {
            curFg = ansiColor565(a - 30, false);
          } else if (a == 39) {
            curFg = TFT_WHITE;
          } else if (a >= 40 && a <= 47) {
            curBg = ansiColor565(a - 40, false);
          } else if (a == 49) {
            curBg = TFT_BLACK;
          } else if (a >= 90 && a <= 97) {
            curFg = ansiColor565(a - 90, true);
          } else if (a >= 100 && a <= 107) {
            curBg = ansiColor565(a - 100, true);
          }
        }
      }
      break;
    }
    case 'H':
    case 'f': {  // 光标绝对定位 (行;列，1-indexed)
      int row = (argCount > 0 && args[0] > 0) ? args[0] - 1 : 0;
      int col = (argCount > 1 && args[1] > 0) ? args[1] - 1 : 0;
      cursorY = constrain(row, 0, curRows - 1);
      cursorX = constrain(col, 0, curCols - 1);
      break;
    }
    case 'A': {  // 光标上移
      int n = (argCount > 0 && args[0] > 0) ? args[0] : 1;
      cursorY = max(0, cursorY - n);
      break;
    }
    case 'B': {  // 光标下移
      int n = (argCount > 0 && args[0] > 0) ? args[0] : 1;
      cursorY = min(curRows - 1, cursorY + n);
      break;
    }
    case 'C': {  // 光标右移
      int n = (argCount > 0 && args[0] > 0) ? args[0] : 1;
      cursorX = min(curCols - 1, cursorX + n);
      break;
    }
    case 'D': {  // 光标左移
      int n = (argCount > 0 && args[0] > 0) ? args[0] : 1;
      cursorX = max(0, cursorX - n);
      break;
    }
    case 'G': {  // 光标绝对列
      int col = (argCount > 0 && args[0] > 0) ? args[0] - 1 : 0;
      cursorX = constrain(col, 0, curCols - 1);
      break;
    }
    case 'd': {  // 光标绝对行
      int row = (argCount > 0 && args[0] > 0) ? args[0] - 1 : 0;
      cursorY = constrain(row, 0, curRows - 1);
      break;
    }
    case 'K': {  // 清除行内字符
      int mode = (argCount > 0) ? args[0] : 0;
      cursorY = constrain(cursorY, 0, curRows - 1);
      cursorX = constrain(cursorX, 0, curCols - 1);
      if (mode == 0) {
        for (int c = cursorX; c < curCols; c++) termBuf[cursorY][c] = { ' ', curFg, curBg };
      } else if (mode == 1) {
        for (int c = 0; c <= cursorX; c++) termBuf[cursorY][c] = { ' ', curFg, curBg };
      } else if (mode == 2) {
        for (int c = 0; c < curCols; c++) termBuf[cursorY][c] = { ' ', curFg, curBg };
      }
      lineDirty[cursorY] = true;
      break;
    }
    case 'J': {  // 清屏 (0=光标至屏尾, 1=屏首至光标, 2/3=整屏)
      int mode = (argCount > 0) ? args[0] : 0;
      cursorY = constrain(cursorY, 0, curRows - 1);
      cursorX = constrain(cursorX, 0, curCols - 1);
      if (mode == 0) {
        for (int c = cursorX; c < curCols; c++) termBuf[cursorY][c] = { ' ', curFg, curBg };
        lineDirty[cursorY] = true;
        for (int r = cursorY + 1; r < curRows; r++) {
          for (int c = 0; c < curCols; c++) termBuf[r][c] = { ' ', curFg, curBg };
          lineDirty[r] = true;
        }
      } else if (mode == 1) {
        for (int r = 0; r < cursorY; r++) {
          for (int c = 0; c < curCols; c++) termBuf[r][c] = { ' ', curFg, curBg };
          lineDirty[r] = true;
        }
        for (int c = 0; c <= cursorX; c++) termBuf[cursorY][c] = { ' ', curFg, curBg };
        lineDirty[cursorY] = true;
      } else if (mode == 2 || mode == 3) {
        for (int r = 0; r < curRows; r++) {
          for (int c = 0; c < curCols; c++) termBuf[r][c] = { ' ', curFg, curBg };
          lineDirty[r] = true;
        }
        cursorX = 0;
        cursorY = 0;
      }
      break;
    }
    case 's':
      savedCursorX = cursorX;
      savedCursorY = cursorY;
      break;
    case 'u':
      cursorX = savedCursorX;
      cursorY = savedCursorY;
      break;
    default:
      break;
  }
}

static void termPutChar(char c) {
  if (wrapPending) {
    cursorX = 0;
    cursorY++;
    if (cursorY >= curRows) {
      termScrollUp();
      cursorY = curRows - 1;
    }
    wrapPending = false;
  }
  cursorX = constrain(cursorX, 0, curCols - 1);
  cursorY = constrain(cursorY, 0, curRows - 1);
  termBuf[cursorY][cursorX] = { c, curFg, curBg };
  lineDirty[cursorY] = true;
  if (cursorX + 1 >= curCols) {
    wrapPending = true;
  } else {
    cursorX++;
  }
}

static void termFeed(char c) {
  int oldX = cursorX, oldY = cursorY;

  switch (ansiState) {
    case ANSI_NORMAL:
      if (c == '\x1B') {
        ansiState = ANSI_ESC;
      } else if (c == '\r') {
        cursorX = 0;
        wrapPending = false;
      } else if (c == '\n') {
        cursorY++;
        if (cursorY >= curRows) {
          termScrollUp();
          cursorY = curRows - 1;
        }
        wrapPending = false;
      } else if (c == '\b') {
        if (cursorX > 0) cursorX--;
        wrapPending = false;
      } else if (c == '\t') {
        cursorX = min(curCols - 1, (cursorX + 8) & ~7);
        wrapPending = false;
      } else if (c == '\a') {
        // Bell: ignore
      } else if ((uint8_t)c >= 32 && (uint8_t)c <= 126) {
        termPutChar(c);
      } else if ((uint8_t)c >= 0x80) {
        // UTF-8 多字节字符处理（支持终端边框盒形图回退）
        if (utf8Expected == 0) {
          if (((uint8_t)c & 0xE0) == 0xC0)      { utf8Expected = 2; utf8Buf[0] = (uint8_t)c; utf8Len = 1; }
          else if (((uint8_t)c & 0xF0) == 0xE0) { utf8Expected = 3; utf8Buf[0] = (uint8_t)c; utf8Len = 1; }
          else if (((uint8_t)c & 0xF8) == 0xF0) { utf8Expected = 4; utf8Buf[0] = (uint8_t)c; utf8Len = 1; }
        } else {
          if (((uint8_t)c & 0xC0) == 0x80 && utf8Len < 4) {
            utf8Buf[utf8Len++] = (uint8_t)c;
            if (utf8Len == utf8Expected) {
              char mapped = ' ';
              if (utf8Expected == 3) {
                uint32_t cp = ((utf8Buf[0] & 0x0F) << 12) | ((utf8Buf[1] & 0x3F) << 6) | (utf8Buf[2] & 0x3F);
                if (cp >= 0x2500 && cp <= 0x2501) mapped = '-';
                else if (cp >= 0x2502 && cp <= 0x2503) mapped = '|';
                else if ((cp >= 0x250C && cp <= 0x254B) || (cp >= 0x2550 && cp <= 0x256C)) mapped = '+';
                else if (cp == 0x2022) mapped = '*';
                else if (cp == 0x2190) mapped = '<';
                else if (cp == 0x2191) mapped = '^';
                else if (cp == 0x2192) mapped = '>';
                else if (cp == 0x2193) mapped = 'v';
              }
              termPutChar(mapped);
              utf8Expected = 0;
              utf8Len = 0;
            }
          } else {
            utf8Expected = 0;
            utf8Len = 0;
          }
        }
      }
      break;

    case ANSI_ESC:
      if (c == '[') {
        ansiState = ANSI_CSI;
        csiLen = 0;
        csiBuf[0] = '\0';
      } else if (c == ']') {
        ansiState = ANSI_OSC;
      } else if (c == '(' || c == ')') {
        ansiState = ANSI_CHARSET;
      } else if (c == '7') {
        savedCursorX = cursorX;
        savedCursorY = cursorY;
        wrapPending = false;
        ansiState = ANSI_NORMAL;
      } else if (c == '8') {
        cursorX = savedCursorX;
        cursorY = savedCursorY;
        wrapPending = false;
        ansiState = ANSI_NORMAL;
      } else if (c == 'c') {
        termReset();
        ansiState = ANSI_NORMAL;
      } else {
        ansiState = ANSI_NORMAL;
      }
      break;

    case ANSI_CHARSET:
      ansiState = ANSI_NORMAL;
      break;

    case ANSI_OSC:
      if (c == '\x07') ansiState = ANSI_NORMAL;
      else if (c == '\x1B') ansiState = ANSI_ESC;
      break;

    case ANSI_CSI:
      if ((c >= '0' && c <= '9') || c == ';' || c == '?') {
        if (csiLen < sizeof(csiBuf) - 1) {
          csiBuf[csiLen++] = c;
          csiBuf[csiLen] = '\0';
        }
      } else if (c >= 0x40 && c <= 0x7E) {
        handleCsi(c);
        ansiState = ANSI_NORMAL;
      } else {
        ansiState = ANSI_NORMAL;
      }
      break;
  }

  // 若光标位置变动，标记旧行与新行，消除光标残留
  if (cursorX != oldX || cursorY != oldY) {
    if (oldY >= 0 && oldY < curRows) lineDirty[oldY] = true;
    if (cursorY >= 0 && cursorY < curRows) lineDirty[cursorY] = true;
  }
}



// =============================================================================
// 配置文件读取与智能密钥检索 (SD 卡)
// =============================================================================

// 寻找存储卡上的私钥文件（按优先级自动检索）
static String findSshKeyOnSD() {
  if (!sdReady()) return "";

  // 1. 若用户显式指定了私钥文件名且存在
  if (strlen(sshKeyFile) > 0 && SD.exists(sshKeyFile)) {
    return String(sshKeyFile);
  }

  // 2. 根据当前配置的主机别名自动匹配
  if (strlen(sshName) > 0) {
    String nKey = String("/") + sshName + ".key";
    if (SD.exists(nKey.c_str())) return nKey;
    String nPem = String("/") + sshName + ".pem";
    if (SD.exists(nPem.c_str())) return nPem;
    String nRaw = String("/") + sshName;
    if (SD.exists(nRaw.c_str())) return nRaw;
  }

  // 3. 自动智能检测常见候选私钥路径
  static const char* CANDIDATES[] = {
    "/id_ed25519",
    "/id_rsa",
    "/id_ecdsa",
    "/ssh.key",
    "/ssh_key",
    "/id_ed25519.pem",
    "/id_rsa.pem",
    "/id_ed25519.txt",
    "/id_rsa.txt",
    "/.ssh/id_ed25519",
    "/ssh/id_ed25519",
    "/.ssh/id_rsa",
    "/ssh/id_rsa"
    CFG_SSH_EXTRA_KEYS
  };

  for (const char* path : CANDIDATES) {
    if (SD.exists(path)) {
      return String(path);
    }
  }

  return "";
}

static bool loadSshConfigFromSD() {
  if (!sdReady()) return false;
  if (!SD.exists("/ssh.cfg") && !SD.exists("/ssh.txt")) {
    return false;
  }
  const char* path = SD.exists("/ssh.cfg") ? "/ssh.cfg" : "/ssh.txt";
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    int eq = line.indexOf('=');
    if (eq < 0) eq = line.indexOf(':');
    if (eq < 0) continue;

    String k = line.substring(0, eq);
    String v = line.substring(eq + 1);
    k.trim();
    v.trim();
    k.toLowerCase();

    if (k == "host" || k == "ip" || k == "server") {
      strncpy(sshHost, v.c_str(), sizeof(sshHost) - 1);
      sshHost[sizeof(sshHost) - 1] = '\0';
      saveString("ssh", "host", sshHost);
    } else if (k == "name" || k == "label" || k == "alias") {
      strncpy(sshName, v.c_str(), sizeof(sshName) - 1);
      sshName[sizeof(sshName) - 1] = '\0';
      saveString("ssh", "name", sshName);
    } else if (k == "port") {
      long p = v.toInt();
      sshPort = (p > 0 && p <= 65535) ? (uint16_t)p : 22;
      saveInt("ssh", "port", sshPort);
    } else if (k == "user" || k == "username") {
      strncpy(sshUser, v.c_str(), sizeof(sshUser) - 1);
      sshUser[sizeof(sshUser) - 1] = '\0';
      saveString("ssh", "user", sshUser);
    } else if (k == "pass" || k == "password" || k == "pw" || k == "passphrase") {
      strncpy(sshPass, v.c_str(), sizeof(sshPass) - 1);
      sshPass[sizeof(sshPass) - 1] = '\0';
      saveString("ssh", "pass", sshPass);
    } else if (k == "auth" || k == "authmode" || k == "type") {
      String vLow = v;
      vLow.toLowerCase();
      if (vLow == "key" || vLow == "pubkey" || vLow == "publickey" || vLow == "rsa" || vLow == "ed25519") {
        authMode = AUTH_KEY;
      } else if (vLow == "pass" || vLow == "password" || vLow == "pwd") {
        authMode = AUTH_PASSWORD;
      }
      saveInt("ssh", "auth", (int)authMode);
    } else if (k == "key" || k == "keyfile" || k == "privkey" || k == "identity") {
      strncpy(sshKeyFile, v.c_str(), sizeof(sshKeyFile) - 1);
      sshKeyFile[sizeof(sshKeyFile) - 1] = '\0';
      saveString("ssh", "keyfile", sshKeyFile);
    } else if (k == "font" || k == "fontsize" || k == "size") {
      String vLow = v;
      vLow.toLowerCase();
      if (vLow == "large" || vLow == "8x16" || vLow == "big") {
        fontMode = SSH_FONT_LARGE;
      } else if (vLow == "wide" || vLow == "medium" || vLow == "8x8") {
        fontMode = SSH_FONT_MEDIUM;
      } else if (vLow == "small" || vLow == "6x8" || vLow == "default") {
        fontMode = SSH_FONT_SMALL;
      }
      applyFontMode(fontMode);
      saveInt("ssh", "font", (int)fontMode);
    }
  }
  f.close();
  return true;
}

// =============================================================================
// FreeRTOS SSH 任务 (运行于 Core 0)
// =============================================================================
static void sshWorkerTask(void* param) {
  libssh_begin();
  ssh_init();

  ssh_session session = ssh_new();
  currentSession = session;
  ssh_channel channel = nullptr;

  if (!session) {
    snprintf(errMsg, sizeof(errMsg), "LibSSH alloc failed");
    sshState = SSH_ST_ERROR;
    goto task_end;
  }

  if (stopRequested) goto task_end;

  ssh_options_set(session, SSH_OPTIONS_HOST, sshHost);
  {
    int p = sshPort;
    ssh_options_set(session, SSH_OPTIONS_PORT, &p);
  }
  ssh_options_set(session, SSH_OPTIONS_USER, sshUser);
  {
    long timeoutSec = 5;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeoutSec);
    int strict = 0;
    ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strict);
  }

  snprintf(connStatusMsg, sizeof(connStatusMsg), "Connecting...");
  if (ssh_connect(session) != SSH_OK) {
    if (!stopRequested) {
      snprintf(errMsg, sizeof(errMsg), "Connect: %s", ssh_get_error(session));
      sshState = SSH_ST_ERROR;
    }
    goto task_end;
  }

  if (stopRequested) goto task_end;

  snprintf(connStatusMsg, sizeof(connStatusMsg), "Authenticating...");
  {
    int rc = SSH_AUTH_ERROR;
    bool authed = false;

    if (authMode == AUTH_KEY) {
      String keyPath = findSshKeyOnSD();
      if (keyPath.length() == 0) {
        if (!stopRequested) {
          if (!sdReady()) {
            snprintf(errMsg, sizeof(errMsg), "Key Auth: SD not mounted");
          } else {
            snprintf(errMsg, sizeof(errMsg), "Key not found on SD (/id_ed25519..)");
          }
          sshState = SSH_ST_ERROR;
        }
        goto task_end;
      }

      File kf = SD.open(keyPath.c_str(), FILE_READ);
      if (!kf) {
        if (!stopRequested) {
          snprintf(errMsg, sizeof(errMsg), "Open key failed: %s", keyPath.c_str());
          sshState = SSH_ST_ERROR;
        }
        goto task_end;
      }

      size_t ksz = kf.size();
      if (ksz == 0 || ksz > 8192) {
        kf.close();
        if (!stopRequested) {
          snprintf(errMsg, sizeof(errMsg), "Bad key file size (%u B)", (unsigned)ksz);
          sshState = SSH_ST_ERROR;
        }
        goto task_end;
      }

      char* keyBuf = (char*)malloc(ksz + 1);
      if (!keyBuf) {
        kf.close();
        if (!stopRequested) {
          snprintf(errMsg, sizeof(errMsg), "OOM allocating key buf");
          sshState = SSH_ST_ERROR;
        }
        goto task_end;
      }

      size_t rd = kf.readBytes(keyBuf, ksz);
      keyBuf[rd] = '\0';
      kf.close();

      snprintf(connStatusMsg, sizeof(connStatusMsg), "Importing %s...", keyPath.c_str());
      ssh_key privkey = NULL;

      // 1. 先尝试无口令导入
      int imp_rc = ssh_pki_import_privkey_base64(keyBuf, NULL, NULL, NULL, &privkey);

      // 2. 若失败且配置了密码/口令，尝试用 sshPass 作为 passphrase 导入
      if (imp_rc != SSH_OK && strlen(sshPass) > 0) {
        imp_rc = ssh_pki_import_privkey_base64(keyBuf, sshPass, NULL, NULL, &privkey);
      }

      // 密钥数据已解析完成，立即释放临时堆内存
      free(keyBuf);

      if (imp_rc != SSH_OK || !privkey) {
        if (!stopRequested) {
          snprintf(errMsg, sizeof(errMsg), "Key decode failed (%d)", imp_rc);
          sshState = SSH_ST_ERROR;
        }
        goto task_end;
      }

      snprintf(connStatusMsg, sizeof(connStatusMsg), "Key auth (%s)...", keyPath.c_str());
      rc = ssh_userauth_publickey(session, NULL, privkey);
      ssh_key_free(privkey);

      if (rc == SSH_AUTH_SUCCESS) {
        authed = true;
      } else {
        if (!stopRequested) {
          snprintf(errMsg, sizeof(errMsg), "Key auth failed: %s", ssh_get_error(session));
          sshState = SSH_ST_ERROR;
        }
        goto task_end;
      }
    }

    if (!authed) {
      snprintf(connStatusMsg, sizeof(connStatusMsg), "Password auth...");
      if (strlen(sshPass) == 0) {
        rc = ssh_userauth_none(session, NULL);
        if (rc != SSH_AUTH_SUCCESS) {
          rc = ssh_userauth_password(session, NULL, "");
        }
      } else {
        rc = ssh_userauth_password(session, NULL, sshPass);
        if (rc != SSH_AUTH_SUCCESS) {
          if (ssh_userauth_kbdint(session, NULL, NULL) == SSH_AUTH_INFO) {
            ssh_userauth_kbdint_setanswer(session, 0, sshPass);
            rc = ssh_userauth_kbdint(session, NULL, NULL);
          }
        }
      }

      if (rc != SSH_AUTH_SUCCESS) {
        if (!stopRequested) {
          snprintf(errMsg, sizeof(errMsg), "Auth failed: %s", ssh_get_error(session));
          sshState = SSH_ST_ERROR;
        }
        goto task_end;
      }
    }
  }

  if (stopRequested) goto task_end;

  snprintf(connStatusMsg, sizeof(connStatusMsg), "Opening shell...");
  channel = ssh_channel_new(session);
  currentChannel = channel;
  if (!channel) {
    if (!stopRequested) {
      snprintf(errMsg, sizeof(errMsg), "Channel alloc failed");
      sshState = SSH_ST_ERROR;
    }
    goto task_end;
  }

  if (ssh_channel_open_session(channel) != SSH_OK) {
    if (!stopRequested) {
      snprintf(errMsg, sizeof(errMsg), "Channel open failed: %s", ssh_get_error(session));
      sshState = SSH_ST_ERROR;
    }
    goto task_end;
  }

  if (stopRequested) goto task_end;

  ssh_channel_request_pty_size(channel, "vt100", curCols, curRows);
  if (ssh_channel_request_shell(channel) != SSH_OK) {
    if (!stopRequested) {
      snprintf(errMsg, sizeof(errMsg), "Shell request failed: %s", ssh_get_error(session));
      sshState = SSH_ST_ERROR;
    }
    goto task_end;
  }

  termReset();
  sshState = SSH_ST_TERMINAL;

  // 活动终端会话循环
  while (!stopRequested && WiFi.status() == WL_CONNECTED &&
         ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel)) {
    // 0. 动态调整 PTY 尺寸与 SIGWINCH (线程安全：在 Worker 任务内部执行)
    if (reqPtyResize) {
      reqPtyResize = false;
      ssh_channel_change_pty_size(channel, reqPtyCols, reqPtyRows);
      char ctrlL = '\x0C';
      ssh_channel_write(channel, &ctrlL, 1);
    }

    // 1. 发送输入队列
    char txBuf[64];
    int txLen = 0;
    while (txLen < (int)sizeof(txBuf) && txQueue && xQueueReceive(txQueue, &txBuf[txLen], 0) == pdTRUE) {
      txLen++;
    }
    if (txLen > 0) {
      int wrc = ssh_channel_write(channel, txBuf, txLen);
      if (wrc < 0) break;
      lastTxMs = millis();
    }

    // 2. 接收网络数据
    char rxBuf[128];
    int n = ssh_channel_read_timeout(channel, rxBuf, sizeof(rxBuf), 0, 25);
    if (n > 0) {
      lastRxMs = millis();
      Serial.write((const uint8_t*)rxBuf, n);
      for (int i = 0; i < n; i++) {
        if (stopRequested) break;
        if (rxQueue) xQueueSend(rxQueue, &rxBuf[i], pdMS_TO_TICKS(10));
      }
    } else if (n < 0 && n != SSH_AGAIN) {
      // 遇到真正错误 (SSH_ERROR) 退出，超时 (SSH_AGAIN) 继续循环
      break;
    }

    if (txLen == 0 && (n == 0 || n == SSH_AGAIN)) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

task_end:
  {
    int expected = SSH_CLEANUP_NONE;
    if (!sshCleanupOwner.compare_exchange_strong(expected, SSH_CLEANUP_TASK)) {
      // 主线程已经接管清理并马上会删掉本任务：什么都别碰，原地等着被删
      for (;;) vTaskSuspend(NULL);
    }
  }
  if (channel) {
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    channel = nullptr;
    currentChannel = nullptr;
  }
  if (session) {
    ssh_disconnect(session);
    ssh_free(session);
    session = nullptr;
    currentSession = nullptr;
  }
  ssh_finalize();

  if (sshState == SSH_ST_TERMINAL) {
    const char msg[] = "\r\n\x1b[33m[Connection closed]\x1b[0m\r\n";
    for (size_t i = 0; msg[i] && rxQueue; i++) {
      xQueueSend(rxQueue, &msg[i], 0);
    }
  }

  taskRunning = false;
  sshTaskHandle = nullptr;
  vTaskDelete(NULL);
}

// =============================================================================
// 生命周期控制
// =============================================================================
void sshEnter() {
  btReleaseForOtherApps();
  sshState = SSH_ST_CONFIG;
  editField = EF_NONE;
  cfgSel = CFG_CONNECT;
  configStatusMsg[0] = '\0';
  connStatusMsg[0] = '\0';
  errMsg[0] = '\0';
  connBgDrawn = false;

  // 从 NVS 恢复
  String name = loadString("ssh", "name", CFG_SSH_DEFAULT_NAME);
  strncpy(sshName, name.c_str(), sizeof(sshName) - 1);
  sshName[sizeof(sshName) - 1] = '\0';

  String h = loadString("ssh", "host", CFG_SSH_DEFAULT_HOST);
  strncpy(sshHost, h.c_str(), sizeof(sshHost) - 1);
  sshHost[sizeof(sshHost) - 1] = '\0';

  int p = loadInt("ssh", "port", 22);
  sshPort = (p > 0 && p <= 65535) ? (uint16_t)p : 22;

  String u = loadString("ssh", "user", CFG_SSH_DEFAULT_USER);
  strncpy(sshUser, u.c_str(), sizeof(sshUser) - 1);
  sshUser[sizeof(sshUser) - 1] = '\0';

  String psw = loadString("ssh", "pass", "");
  strncpy(sshPass, psw.c_str(), sizeof(sshPass) - 1);
  sshPass[sizeof(sshPass) - 1] = '\0';

  authMode = (SshAuthMode)loadInt("ssh", "auth", (int)AUTH_KEY);

  String kf = loadString("ssh", "keyfile", "");
  strncpy(sshKeyFile, kf.c_str(), sizeof(sshKeyFile) - 1);
  sshKeyFile[sizeof(sshKeyFile) - 1] = '\0';

  fontMode = (SshFontMode)loadInt("ssh", "font", (int)SSH_FONT_LARGE);
  if (fontMode < 0 || fontMode >= SSH_FONT_COUNT) fontMode = SSH_FONT_LARGE;
  applyFontMode(fontMode);

  // 若 SD 存在配置文件，自动静默导入
  if (sdReady() && (SD.exists("/ssh.cfg") || SD.exists("/ssh.txt"))) {
    loadSshConfigFromSD();
  }

  topBarDirty = true;   // 进入时强制重绘顶栏
  canvasRestore();
}

void sshExit() {
  stopRequested = true;

  // 立即解除可能挂在 socket 上的阻塞
  if (currentSession) {
    socket_t fd = ssh_get_fd(currentSession);
    if (fd >= 0) {
      shutdown(fd, SHUT_RDWR);
    }
  }

  // 优雅等待任务安全注销
  bool taskGone = true;
  if (taskRunning) {
    uint32_t startWait = millis();
    while (taskRunning && millis() - startWait < 1500) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (taskRunning) {
      int expected = SSH_CLEANUP_NONE;
      if (sshCleanupOwner.compare_exchange_strong(expected, SSH_CLEANUP_MAIN)) {
        // 抢到清理权：任务还没进 task_end，句柄和 current* 都还有效。
        // 最后手段——被删的任务若正卡在 libssh/lwIP 内部持锁，那把锁会跟着丢。
        TaskHandle_t th = sshTaskHandle;
        sshTaskHandle = nullptr;
        if (th) vTaskDelete(th);
        taskRunning = false;

        ssh_channel ch = (ssh_channel)currentChannel;
        currentChannel = nullptr;
        if (ch) ssh_channel_free(ch);
        ssh_session sess = (ssh_session)currentSession;
        currentSession = nullptr;
        if (sess) { ssh_disconnect(sess); ssh_free(sess); }
        ssh_finalize();
      } else {
        // 任务已经在 task_end 里自己清理：不能删它，再多等一会儿让它收完尾
        startWait = millis();
        while (taskRunning && millis() - startWait < 3000) {
          vTaskDelay(pdMS_TO_TICKS(10));
        }
        taskGone = !taskRunning;
      }
    }
  }

  // 任务确认停止才删队列；否则它收尾时还要往 rxQueue 写，留着给下次连接复用
  if (taskGone) {
    if (txQueue) { vQueueDelete(txQueue); txQueue = nullptr; }
    if (rxQueue) { vQueueDelete(rxQueue); rxQueue = nullptr; }
  }

  // 给 Core 0 空闲任务 (Idle) 调度时间回收刚才删掉的任务栈（24KB）
  vTaskDelay(pdMS_TO_TICKS(30));

  // 退出 SSH 时无条件恢复主画布与状态
  connBgDrawn = false;
  canvasRestore();
}

bool sshIsTerminalActive() {
  return sshState == SSH_ST_CONNECTING || sshState == SSH_ST_TERMINAL;
}

// =============================================================================
// 主循环与数据推进
// =============================================================================
void sshUpdate() {
  if (sshState == SSH_ST_TERMINAL) {
    char c;
    bool gotData = false;
    while (rxQueue && xQueueReceive(rxQueue, &c, 0) == pdTRUE) {
      termFeed(c);
      gotData = true;
      lastActivityMs = millis();
    }

    if (millis() - lastCursorBlinkMs >= 450) {
      lastCursorBlinkMs = millis();
      termCursorBlink = !termCursorBlink;
      // 光标由 drawSshTerminal 单独绘制，不再污染整行 lineDirty
      gotData = true;
    }

    if (gotData) dirty = true;
  } else if (sshState == SSH_ST_CONNECTING) {
    static uint32_t lastConnAnimMs = 0;
    static char prevMsg[48] = "";
    bool msgChanged = (strcmp(connStatusMsg, prevMsg) != 0);
    if (msgChanged) {
      strncpy(prevMsg, connStatusMsg, sizeof(prevMsg) - 1);
      dirty = true;
    } else if (millis() - lastConnAnimMs >= 250) {
      lastConnAnimMs = millis();
      dirty = true;
    }
  } else if (sshState == SSH_ST_ERROR) {
    // 发生错误时确保画布重建并触发重绘
    if (!cv.getBuffer()) {
      canvasRestore();
    }
    dirty = true;
  } else if (sshState == SSH_ST_CONFIG) {
    if (!cv.getBuffer()) {
      canvasRestore();
      if (cv.getBuffer()) dirty = true;
    }
    if (editField != EF_NONE) {
      if (millis() - lastCursorBlinkMs >= 400) {
        lastCursorBlinkMs = millis();
        termCursorBlink = !termCursorBlink;
        dirty = true;
      }
    }
  }
}

// =============================================================================
// 按键处理
// =============================================================================
static void sendStringToTx(const char* s) {
  if (!txQueue) return;
  while (*s) {
    xQueueSend(txQueue, s, 0);
    s++;
  }
}

bool sshKey(char k) {
  if (sshState == SSH_ST_CONFIG) {
    if (editField != EF_NONE) {
      if (k == '`' || (uint8_t)k == kbd::KX_BACK || (uint8_t)k == kbd::KX_ESC) {
        editField = EF_NONE;
        dirty = true;
      } else if (k == '\b') {
        if (editBuf.length() > 0) {
          editBuf.remove(editBuf.length() - 1);
          dirty = true;
        }
      } else if (k == '\n') {
        if (editField == EF_HOST) {
          editBuf.trim();
          // 支持粘贴 user@host[:port] 复合格式自动拆分
          int atIdx = editBuf.indexOf('@');
          if (atIdx > 0) {
            String u = editBuf.substring(0, atIdx);
            u.trim();
            if (u.length() > 0) {
              strncpy(sshUser, u.c_str(), sizeof(sshUser) - 1);
              sshUser[sizeof(sshUser) - 1] = '\0';
              saveString("ssh", "user", sshUser);
            }
            editBuf = editBuf.substring(atIdx + 1);
            editBuf.trim();
          }
          int colIdx = editBuf.indexOf(':');
          if (colIdx > 0 && colIdx == editBuf.lastIndexOf(':')) {
            long p = editBuf.substring(colIdx + 1).toInt();
            if (p > 0 && p <= 65535) {
              sshPort = (uint16_t)p;
              saveInt("ssh", "port", sshPort);
            }
            editBuf = editBuf.substring(0, colIdx);
            editBuf.trim();
          }
          strncpy(sshHost, editBuf.c_str(), sizeof(sshHost) - 1);
          sshHost[sizeof(sshHost) - 1] = '\0';
          saveString("ssh", "host", sshHost);
        } else if (editField == EF_PORT) {
          long p = editBuf.toInt();
          sshPort = (p > 0 && p <= 65535) ? (uint16_t)p : 22;
          saveInt("ssh", "port", sshPort);
        } else if (editField == EF_USER) {
          editBuf.trim();
          strncpy(sshUser, editBuf.c_str(), sizeof(sshUser) - 1);
          sshUser[sizeof(sshUser) - 1] = '\0';
          saveString("ssh", "user", sshUser);
        } else if (editField == EF_PASS) {
          strncpy(sshPass, editBuf.c_str(), sizeof(sshPass) - 1);
          sshPass[sizeof(sshPass) - 1] = '\0';
          saveString("ssh", "pass", sshPass);
        }
        editField = EF_NONE;
        dirty = true;
      } else if (k >= 0x20 && k <= 0x7E && editBuf.length() < 48) {
        editBuf += k;
        dirty = true;
      }
      return false;
    }

    if (k == '`' || (uint8_t)k == kbd::KX_BACK || (uint8_t)k == kbd::KX_ESC) {
      return true; // 返回主菜单
    } else if (k == ';' || k == ',') {
      cfgSel = (cfgSel - 1 + CFG_COUNT) % CFG_COUNT;
      dirty = true;
    } else if (k == '.' || k == '/') {
      cfgSel = (cfgSel + 1) % CFG_COUNT;
      dirty = true;
    } else if (k == '\n') {
      if (cfgSel == CFG_HOST) {
        editField = EF_HOST; editBuf = String(sshHost); dirty = true;
      } else if (cfgSel == CFG_PORT) {
        editField = EF_PORT; editBuf = String(sshPort); dirty = true;
      } else if (cfgSel == CFG_USER) {
        editField = EF_USER; editBuf = String(sshUser); dirty = true;
      } else if (cfgSel == CFG_AUTH) {
        authMode = (authMode == AUTH_KEY) ? AUTH_PASSWORD : AUTH_KEY;
        saveInt("ssh", "auth", (int)authMode);
        dirty = true;
      } else if (cfgSel == CFG_PASS) {
        editField = EF_PASS; editBuf = String(sshPass); dirty = true;
      } else if (cfgSel == CFG_FONT) {
        SshFontMode nextMode = (SshFontMode)((fontMode + 1) % SSH_FONT_COUNT);
        switchFontMode(nextMode);
        dirty = true;
      } else if (cfgSel == CFG_CONNECT) {
        if (WiFi.status() != WL_CONNECTED) {
          snprintf(configStatusMsg, sizeof(configStatusMsg), "WiFi disconnected!");
          dirty = true;
        } else if (strlen(sshHost) == 0) {
          snprintf(configStatusMsg, sizeof(configStatusMsg), "Host cannot be empty");
          dirty = true;
        } else {
          // 释放画布，启动连接
          btReleaseForOtherApps();
          canvasRelease();

          sshState = SSH_ST_CONNECTING;
          connBgDrawn = false;
          lastDrawnStage = -1;
          lastDrawnMsg[0] = '\0';
          lastSpinChar = 0;
          snprintf(connStatusMsg, sizeof(connStatusMsg), "Init LibSSH...");
          stopRequested = false;

          if (!txQueue) txQueue = xQueueCreate(256, 1);
          if (!rxQueue) rxQueue = xQueueCreate(1024, 1);
          if (!txQueue || !rxQueue) {
            if (txQueue) { vQueueDelete(txQueue); txQueue = nullptr; }
            if (rxQueue) { vQueueDelete(rxQueue); rxQueue = nullptr; }
            canvasRestore();
            sshState = SSH_ST_CONFIG;
            snprintf(configStatusMsg, sizeof(configStatusMsg), "OOM: queue alloc failed");
            dirty = true;
            return false;
          }
          xQueueReset(txQueue);
          xQueueReset(rxQueue);

          // 建任务之前置位：否则任务还没被调度时 sshExit() 会以为它不存在，直接删掉队列
          sshCleanupOwner.store(SSH_CLEANUP_NONE);
          taskRunning = true;

          // 24KB 独立线程栈，绑定至 Core 0
          BaseType_t created = xTaskCreatePinnedToCore(
              sshWorkerTask, "ssh_task", 24576 / sizeof(StackType_t),
              NULL, 1, const_cast<TaskHandle_t*>(&sshTaskHandle), 0);
          if (created != pdPASS) {
            taskRunning = false;
            // 任务创建失败：释放已分配的队列，恢复画布与状态
            if (txQueue) { vQueueDelete(txQueue); txQueue = nullptr; }
            if (rxQueue) { vQueueDelete(rxQueue); rxQueue = nullptr; }
            canvasRestore();
            sshState = SSH_ST_CONFIG;
            snprintf(configStatusMsg, sizeof(configStatusMsg), "OOM: task create failed");
          }
          dirty = true;
        }
      }
    }
    return false;
  }

  if (sshState == SSH_ST_CONNECTING) {
    if ((uint8_t)k == kbd::KX_BACK || (uint8_t)k == kbd::KX_ESC || k == '`') {
      sshExit();
      sshState = SSH_ST_CONFIG;
      dirty = true;
    }
    return false;
  }

  if (sshState == SSH_ST_ERROR) {
    sshExit();
    sshState = SSH_ST_CONFIG;
    dirty = true;
    return false;
  }

  if (sshState == SSH_ST_TERMINAL) {
    // 终端已断开/结束时，按任意键返回配置界面
    if (!taskRunning) {
      sshExit();
      sshState = SSH_ST_CONFIG;
      dirty = true;
      return false;
    }

    // 终端内退出为 Fn+`
    if ((uint8_t)k == kbd::KX_BACK) {
      sshExit();
      sshState = SSH_ST_CONFIG;
      dirty = true;
      return false;
    }

    // 动态字号切换快捷键：Fn+Enter
    if ((uint8_t)k == kbd::KX_FN_ENTER) {
      SshFontMode nextMode = (SshFontMode)((fontMode + 1) % SSH_FONT_COUNT);
      switchFontMode(nextMode);
      return false;
    }

    if ((uint8_t)k == kbd::KX_ESC) {
      char esc = '\x1B';
      if (txQueue) xQueueSend(txQueue, &esc, 0);
      return false;
    }

    // 方向键映射
    if (k == (char)151) { sendStringToTx("\x1B[A"); return false; } // Up
    if (k == (char)150) { sendStringToTx("\x1B[B"); return false; } // Down
    if (k == (char)149) { sendStringToTx("\x1B[D"); return false; } // Left
    if (k == (char)148) { sendStringToTx("\x1B[C"); return false; } // Right
    if ((uint8_t)k == kbd::KX_DEL) { sendStringToTx("\x1B[3~"); return false; } // Delete
    if (k == (char)145) { sendStringToTx("\x1B[H"); return false; } // Home
    if ((uint8_t)k == kbd::KX_END) { sendStringToTx("\x1B[F"); return false; } // End

    // 功能键映射 F1..F12 (Fn + 1..=)
    if ((uint8_t)k >= 128 && (uint8_t)k <= 139) {
      static const char* const FKEYS[] = {
        "\x1BOP", "\x1BOQ", "\x1BOR", "\x1BOS",
        "\x1B[15~", "\x1B[17~", "\x1B[18~", "\x1B[19~",
        "\x1B[20~", "\x1B[21~", "\x1B[23~", "\x1B[24~"
      };
      sendStringToTx(FKEYS[(uint8_t)k - 128]);
      return false;
    }

    // 回车与退格
    if (k == '\n') {
      char cr = '\r';
      if (txQueue) xQueueSend(txQueue, &cr, 0);
      return false;
    }
    if (k == '\b') {
      char del = 0x7F;
      if (txQueue) xQueueSend(txQueue, &del, 0);
      return false;
    }
    if (k == '\t') {
      char tab = '\t';
      if (txQueue) xQueueSend(txQueue, &tab, 0);
      return false;
    }

    // Alt (Meta) 组合键处理：发送 ESC 前缀
    if (kbd::modMask() & kbd::MOD_ALT) {
      char esc = '\x1B';
      if (txQueue) {
        xQueueSend(txQueue, &esc, 0);
        xQueueSend(txQueue, &k, 0);
      }
      kbd::consumeSticky();
      return false;
    }

    // Ctrl 组合键处理（支持 Ctrl+A..Z, Ctrl+@ / Ctrl+Space (NUL), Ctrl+[ (ESC), Ctrl+^, Ctrl+_ 等）
    if (kbd::modMask() & kbd::MOD_CTRL) {
      char ctrl = 0;
      bool sendCtrl = false;
      if (k >= 'a' && k <= 'z') { ctrl = (char)(k - 'a' + 1); sendCtrl = true; }
      else if (k >= 'A' && k <= 'Z') { ctrl = (char)(k - 'A' + 1); sendCtrl = true; }
      else if (k == ' ' || k == '@') { ctrl = '\0'; sendCtrl = true; }
      else if ((uint8_t)k >= '@' && (uint8_t)k <= '_') { ctrl = (char)(k & 0x1F); sendCtrl = true; }

      if (sendCtrl && txQueue) {
        xQueueSend(txQueue, &ctrl, 0);
      }
      kbd::consumeSticky();
      return false;
    }

    // 普通字符发送
    if (txQueue) {
      xQueueSend(txQueue, &k, 0);
    }
    return false;
  }

  return false;
}

// =============================================================================
// =============================================================================
// UI 绘制分发
// =============================================================================

// 赛博风格的字段编辑模态弹窗（浮层于配置页面之上，输入体验更完整）
static void drawSshEditModal() {
  cv.fillRect(0, 13, SW, SH - 13, 0x0000);

  const int mw = 216, mh = 84;
  const int mx = (SW - mw) / 2, my = 26;

  // 双层科技感外框
  cv.fillRoundRect(mx, my, mw, mh, 4, 0x0821);
  cv.drawRoundRect(mx, my, mw, mh, 4, ACCENT);
  cv.drawRoundRect(mx + 1, my + 1, mw - 2, mh - 2, 3, 0x0000);

  // 标头栏
  cv.fillRoundRect(mx + 2, my + 2, mw - 4, 15, 3, CARD_BG);
  cv.drawFastHLine(mx + 2, my + 17, mw - 4, DIM_BORDER);

  const char* title = (editField == EF_HOST) ? "EDIT: SSH HOST / IP" :
                      (editField == EF_PORT) ? "EDIT: PORT (1-65535)" :
                      (editField == EF_USER) ? "EDIT: USERNAME" :
                      ((authMode == AUTH_KEY) ? "EDIT: KEY PASSPHRASE" : "EDIT: PASSWORD");
  cv.setTextDatum(middle_left);
  cv.setFont(&fonts::Font0);
  cv.setTextSize(1);
  cv.setTextColor(ACCENT, CARD_BG);
  cv.drawString(title, mx + 8, my + 9);

  // 输入框内凹底板
  int bx = mx + 10, by = my + 24, bw = mw - 20, bh = 22;
  cv.fillRoundRect(bx, by, bw, bh, 3, 0x0000);
  cv.drawRoundRect(bx, by, bw, bh, 3, 0x07FF);

  // 待显示的内容（若超长做截断显示）
  String disp = editBuf;
  if (disp.length() > 28) {
    disp = disp.substring(disp.length() - 28);
  }
  if (termCursorBlink) disp += "_";

  cv.setTextDatum(middle_left);
  cv.setTextColor(TFT_WHITE, 0x0000);
  cv.drawString(disp, bx + 6, by + bh / 2);

  // 字段说明副标题
  cv.setTextDatum(top_left);
  cv.setTextColor(0x8410, 0x0821);
  const char* hint = (editField == EF_HOST) ? "IPv4 or host (e.g. 203.0.113.10)" :
                     (editField == EF_PORT) ? "Standard SSH port is 22" :
                     (editField == EF_USER) ? "User on server (e.g. ubuntu)" :
                     ((authMode == AUTH_KEY) ? "Passphrase for private key (or empty)" : "Leave empty for no-password auth");
  cv.drawString(hint, bx, by + bh + 4);

  // 底部按键提示
  cv.drawFastHLine(mx + 4, my + mh - 16, mw - 8, 0x18C3);
  cv.setTextDatum(middle_center);
  cv.setTextColor(0xCE79, 0x0821);
  cv.drawString("[Enter] Save     [`] Cancel", mx + mw / 2, my + mh - 8);
}

void drawSshConfig() {
  if (cv.getBuffer()) memset(cv.getBuffer(), 0, (size_t)SW * SH * 2);
  else cv.fillScreen(TFT_BLACK);

  // 顶栏
  String headerTitle = (strlen(sshName) > 0) ? (String("SSH: ") + sshName) : "SSH Terminal";
  drawPageHeader(headerTitle.c_str(), WiFi.status() == WL_CONNECTED ? "WiFi OK" : "NO WIFI",
                 WiFi.status() == WL_CONNECTED ? ACCENT : TFT_RED);

  cv.setTextSize(1);
  cv.setFont(&fonts::Font0);

  // 6 个参数行卡片
  auto drawParamRow = [](int idx, const char* name, const String& val, int y) {
    bool sel = (cfgSel == idx);
    int h = 13;
    // 选中时有更丰富的微光背景
    cv.fillRoundRect(6, y, SW - 12, h, 3, sel ? 0x0842 : 0x0821);
    cv.drawRoundRect(6, y, SW - 12, h, 3, sel ? ACCENT : 0x18C3);

    // 选中态高亮左侧发光微胶囊导轨
    if (sel) {
      cv.fillRoundRect(6, y, 3, h, 1, ACCENT);
    }

    cv.setTextDatum(middle_left);
    cv.setTextColor(sel ? ACCENT : ICON_DIM, sel ? 0x0842 : 0x0821);
    char labelBuf[16];
    snprintf(labelBuf, sizeof(labelBuf), "%s%s", sel ? "> " : "  ", name);
    cv.drawString(labelBuf, 10, y + h / 2);

    cv.setTextDatum(middle_right);
    cv.setTextColor(sel ? TFT_WHITE : 0xCE79, sel ? 0x0842 : 0x0821);
    cv.drawString(val, SW - 12, y + h / 2);
  };

  String hostVal = (strlen(sshName) > 0) ? (String(sshName) + " (" + sshHost + ")") : String(sshHost);
  drawParamRow(CFG_HOST, "Host", hostVal, 14);
  drawParamRow(CFG_PORT, "Port", (sshPort == 22) ? "22 (default)" : String(sshPort), 28);
  drawParamRow(CFG_USER, "User", String(sshUser), 42);

  // Auth 认证方式行
  String authVal = "";
  if (authMode == AUTH_KEY) {
    String kp = findSshKeyOnSD();
    if (kp.length() > 0) {
      authVal = "Key (" + kp.substring(kp.lastIndexOf('/') + 1) + ")";
    } else {
      authVal = sdReady() ? "Key (No key on SD)" : "Key (No SD)";
    }
  } else {
    authVal = "Password";
  }
  drawParamRow(CFG_AUTH, "Auth", authVal, 56);

  String maskedPass = "";
  size_t pLen = strlen(sshPass);
  if (pLen == 0) maskedPass = "<none>";
  else {
    for (size_t i = 0; i < pLen && i < 10; i++) maskedPass += '*';
    if (pLen > 10) maskedPass += "..";
  }
  drawParamRow(CFG_PASS, (authMode == AUTH_KEY) ? "KeyPass" : "Pass", maskedPass, 70);

  // Font 字号选择行 (大字 8x16 / 宽体 8x8 / 小字 6x8)
  String fontVal = (fontMode == SSH_FONT_LARGE) ? "Large (8x16, 30x8)" :
                   (fontMode == SSH_FONT_MEDIUM) ? "Wide (8x8, 30x15)" :
                   "Small (6x8, 40x15)";
  drawParamRow(CFG_FONT, "Font", fontVal, 84);

  // 操作按钮：[ ▶ Connect ] 独占一行
  int btnW = SW - 12;
  int btnH = 16;
  int btnY = 99;

  // [ Connect ]
  bool selConn = (cfgSel == CFG_CONNECT);
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  uint16_t connBg = !wifiOk ? (selConn ? 0x4000 : 0x1800) : (selConn ? ACCENT : CARD_BG);
  uint16_t connBorder = !wifiOk ? TFT_RED : (selConn ? TFT_WHITE : ACCENT);
  uint16_t connFg = !wifiOk ? TFT_RED : (selConn ? TFT_BLACK : ACCENT);

  cv.fillRoundRect(6, btnY, btnW, btnH, 4, connBg);
  cv.drawRoundRect(6, btnY, btnW, btnH, 4, connBorder);
  cv.setTextDatum(middle_center);
  cv.setTextColor(connFg, connBg);
  cv.drawString(wifiOk ? "> Connect to Host" : "! Connect (No WiFi)", 6 + btnW / 2, btnY + btnH / 2);

  // 底部状态或快捷帮助栏
  cv.drawFastHLine(6, 117, SW - 12, 0x18C3);

  if (configStatusMsg[0] != '\0') {
    cv.setTextDatum(middle_center);
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString(configStatusMsg, SW / 2, 125);
  } else if (!wifiOk) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.drawString("WiFi offline! Connect in Settings", SW / 2, 125);
  } else if (cfgSel == CFG_FONT) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(0x07FF, TFT_BLACK);
    cv.drawString("Enter: Toggle Large / Wide / Small", SW / 2, 125);
  } else if (cfgSel == CFG_AUTH) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(0x07FF, TFT_BLACK);
    cv.drawString("Enter: Toggle Key / Password", SW / 2, 125);
  } else if (cfgSel == CFG_CONNECT) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(0x07FF, TFT_BLACK);
    cv.drawString("Enter: Start SSH Connection", SW / 2, 125);
  } else {
    cv.setTextDatum(middle_center);
    cv.setTextColor(0xCE79, TFT_BLACK);
    cv.drawString("; . Sel    Enter Edit    ` Back", SW / 2, 125);
  }

  // 若处于编辑字段状态，覆盖展示 Cyber Modal
  if (editField != EF_NONE) {
    drawSshEditModal();
  }
}

static void drawSshConnecting() {
  const int cx = SW / 2;
  const int cw = SW - 20, ch = 88;
  const int cy = 16;
  const int pipeY = cy + 40;

  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);

  // 1. 静态背景：首次进入只画一次，后续绝不全屏 fillScreen，彻底杜绝闪屏
  if (!connBgDrawn) {
    M5.Display.fillScreen(TFT_BLACK);

    // 顶栏背景与底边分割线
    M5.Display.fillRect(0, 0, SW, 12, CARD_BG);
    M5.Display.drawFastHLine(0, 11, SW, DIM_BORDER);

    // 顶栏右侧端口标签
    M5.Display.setTextDatum(middle_right);
    M5.Display.setTextColor(ICON_DIM, CARD_BG);
    char pBuf[16];
    snprintf(pBuf, sizeof(pBuf), "PORT %u", sshPort);
    M5.Display.drawString(pBuf, SW - 8, 6);

    // 中央科技感外框
    M5.Display.drawRoundRect(10, cy, cw, ch, 4, 0x07FF);

    // HUD 四角准星切角装饰
    M5.Display.drawFastHLine(10, cy, 6, TFT_WHITE);
    M5.Display.drawFastVLine(10, cy, 6, TFT_WHITE);
    M5.Display.drawFastHLine(10 + cw - 6, cy, 6, TFT_WHITE);
    M5.Display.drawFastVLine(10 + cw - 1, cy, 6, TFT_WHITE);
    M5.Display.drawFastHLine(10, cy + ch - 1, 6, TFT_WHITE);
    M5.Display.drawFastVLine(10, cy + ch - 6, 6, TFT_WHITE);
    M5.Display.drawFastHLine(10 + cw - 6, cy + ch - 1, 6, TFT_WHITE);
    M5.Display.drawFastVLine(10 + cw - 1, cy + ch - 6, 6, TFT_WHITE);

    // 目标显示槽
    M5.Display.fillRoundRect(16, cy + 8, cw - 12, 18, 3, 0x0821);
    M5.Display.drawRoundRect(16, cy + 8, cw - 12, 18, 3, 0x18C3);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE, 0x0821);
    char targetStr[160];
    if (strlen(sshName) > 0) {
      snprintf(targetStr, sizeof(targetStr), "%s@%s (%s:%u)", sshUser, sshName, sshHost, sshPort);
    } else {
      snprintf(targetStr, sizeof(targetStr), "%s@%s:%u", sshUser, sshHost, sshPort);
    }
    M5.Display.drawString(targetStr, cx, cy + 17);

    // 三阶段流水线基准横线
    M5.Display.drawFastHLine(cx - 70, pipeY, 140, 0x18C3);

    // 底部取消操作提示胶囊
    M5.Display.fillRoundRect(cx - 48, SH - 22, 96, 16, 3, 0x2000);
    M5.Display.drawRoundRect(cx - 48, SH - 22, 96, 16, 3, TFT_RED);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_RED, 0x2000);
    M5.Display.drawString("Fn+` : Abort", cx, SH - 14);

    connBgDrawn = true;
    lastDrawnStage = -1;
    lastDrawnMsg[0] = '\0';
    lastSpinChar = 0;
  }

  // 2. 动态旋转光标（带背景色写入，单次覆盖，零闪烁）
  static const char spinChars[] = "|/-\\";
  char spin = spinChars[(millis() / 250) % 4];
  if (spin != lastSpinChar) {
    lastSpinChar = spin;
    M5.Display.setTextDatum(middle_left);
    M5.Display.setTextColor(0x07FF, CARD_BG);
    char titleBuf[48];
    if (strlen(sshName) > 0) {
      snprintf(titleBuf, sizeof(titleBuf), "SSH: %s  %c", sshName, spin);
    } else {
      snprintf(titleBuf, sizeof(titleBuf), "SSH HANDSHAKE  %c", spin);
    }
    M5.Display.drawString(titleBuf, 8, 6);
  }

  // 3. 三阶段流水线指示（阶段变化时局部重绘）
  int stage = 1;
  if (strstr(connStatusMsg, "Auth") != nullptr) stage = 2;
  else if (strstr(connStatusMsg, "shell") != nullptr || strstr(connStatusMsg, "Open") != nullptr) stage = 3;

  if (stage != lastDrawnStage) {
    lastDrawnStage = stage;
    auto drawStep = [](int x, int y, const char* name, int stepNum, int curStage) {
      bool done = (curStage > stepNum);
      bool active = (curStage == stepNum);
      uint16_t col = done ? 0x07E0 : (active ? 0x07FF : 0x3186);
      M5.Display.fillCircle(x, y, 7, TFT_BLACK); // 抹除原高亮外圈
      M5.Display.fillCircle(x, y, 5, col);
      if (active) M5.Display.drawCircle(x, y, 7, TFT_WHITE);
      M5.Display.setTextDatum(middle_center);
      M5.Display.setTextColor(active ? TFT_WHITE : (done ? 0x07E0 : 0x630C), TFT_BLACK);
      M5.Display.drawString(name, x, y + 13);
    };

    drawStep(cx - 60, pipeY, "TCP", 1, stage);
    drawStep(cx, pipeY, "Auth", 2, stage);
    drawStep(cx + 60, pipeY, "Shell", 3, stage);
  }

  // 4. 动态状态提示（文字变动时才重绘文字所在区域）
  if (strcmp(connStatusMsg, lastDrawnMsg) != 0) {
    strncpy(lastDrawnMsg, connStatusMsg, sizeof(lastDrawnMsg) - 1);
    M5.Display.fillRect(cx - 85, cy + 64, 170, 16, TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(0x07FF, TFT_BLACK);
    M5.Display.drawString(connStatusMsg, cx, cy + 72);
  }
}

static void drawSshError() {
  if (cv.getBuffer()) memset(cv.getBuffer(), 0, (size_t)SW * SH * 2);
  else cv.fillScreen(TFT_BLACK);

  drawPageHeader("SSH Terminal", "ERROR", TFT_RED);

  cv.setTextSize(1);
  cv.setFont(&fonts::Font0);

  // 居中告警卡片
  const int ew = 216, eh = 92;
  const int ex = (SW - ew) / 2, ey = 22;

  // 双层红色警告外框
  cv.fillRoundRect(ex, ey, ew, eh, 4, 0x0821);
  cv.drawRoundRect(ex, ey, ew, eh, 4, TFT_RED);
  cv.drawRoundRect(ex + 1, ey + 1, ew - 2, eh - 2, 3, 0x3800);

  // 标头栏
  cv.fillRoundRect(ex + 2, ey + 2, ew - 4, 15, 3, 0x3000);
  cv.drawFastHLine(ex + 2, ey + 17, ew - 4, 0x8000);
  cv.setTextDatum(middle_left);
  cv.setTextColor(TFT_RED, 0x3000);
  cv.drawString(" [!] CONNECTION FAILED", ex + 6, ey + 9);

  // 错误信息主体
  cv.setTextDatum(middle_center);
  cv.setTextColor(TFT_WHITE, 0x0821);
  String errStr = String(errMsg);
  if (errStr.length() > 32) {
    errStr = errStr.substring(0, 30) + "..";
  }
  cv.drawString(errStr, ex + ew / 2, ey + 33);

  // 智能排错提示
  cv.setTextColor(0xCE79, 0x0821);
  const char* tip = "Tip: Check host IP, port & WiFi status";
  if (strstr(errMsg, "Key") != nullptr || strstr(errMsg, "key") != nullptr) {
    tip = "Tip: Put SSH key on SD root (/id_ed25519..)";
  } else if (strstr(errMsg, "Auth") != nullptr) {
    tip = "Tip: Verify SSH username or key permissions";
  } else if (strstr(errMsg, "Channel") != nullptr || strstr(errMsg, "Shell") != nullptr) {
    tip = "Tip: Remote server rejected PTY request";
  } else if (strstr(errMsg, "Timeout") != nullptr) {
    tip = "Tip: Connection timed out, check firewall";
  }
  cv.drawString(tip, ex + ew / 2, ey + 49);

  // 底部返回指引
  cv.drawFastHLine(ex + 6, ey + eh - 20, ew - 12, 0x4000);
  cv.fillRoundRect(ex + ew / 2 - 60, ey + eh - 17, 120, 14, 2, 0x2000);
  cv.drawRoundRect(ex + ew / 2 - 60, ey + eh - 17, 120, 14, 2, TFT_RED);
  cv.setTextColor(TFT_RED, 0x2000);
  cv.drawString("Press Any Key : Return", ex + ew / 2, ey + eh - 10);
}

static void drawSshTerminal() {
  // 1. 顶栏：仅在状态变化时重画（避免每次光标闪烁都刷顶栏）
  bool curTaskRunning = taskRunning;
  if (topBarDirty || curTaskRunning != prevTaskRunning) {
    M5.Display.setTextSize(1);
    M5.Display.setFont(&fonts::Font0);

    char topStr[112];
    if (strlen(sshName) > 0) {
      snprintf(topStr, sizeof(topStr), "%s@%s", sshUser, sshName);
    } else {
      snprintf(topStr, sizeof(topStr), "%s@%s", sshUser, sshHost);
    }
    if (strlen(topStr) > 16) {
      topStr[13] = '.'; topStr[14] = '.'; topStr[15] = '.'; topStr[16] = '\0';
    }

    M5.Display.fillRect(0, 0, SW, curTopBarH, CARD_BG);
    M5.Display.drawFastHLine(0, curTopBarH - 1, SW, DIM_BORDER);

    // 左侧：状态指示微灯（发光 LED）
    int dotY = curTopBarH / 2;
    if (curTaskRunning) {
      M5.Display.fillCircle(5, dotY, 2, 0x07E0);
      M5.Display.drawCircle(5, dotY, 3, 0x0280);
    } else {
      M5.Display.fillCircle(5, dotY, 2, TFT_RED);
    }

    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(ACCENT, CARD_BG);
    M5.Display.drawString(topStr, 11, (curTopBarH > 8) ? 1 : 0);

    // 中间：当前字号徽章 (按 Fn+Enter 可切换)
    const char* fontTag = (fontMode == SSH_FONT_LARGE) ? "8x16" :
                          (fontMode == SSH_FONT_MEDIUM) ? "8x8" : "6x8";
    int badgeX = SW - 88;
    int badgeH = (curTopBarH > 8) ? 8 : (curTopBarH - 1);
    M5.Display.fillRoundRect(badgeX, (curTopBarH > 8) ? 1 : 0, 30, badgeH, 2, 0x18C3);
    M5.Display.setTextDatum(top_center);
    M5.Display.setTextColor(TFT_WHITE, 0x18C3);
    M5.Display.drawString(fontTag, badgeX + 15, (curTopBarH > 8) ? 1 : 0);

    // 右侧：状态微胶囊徽章 + 退出指引
    int liveBadgeX = SW - 54;
    int liveBadgeW = 26;
    int liveBadgeH = (curTopBarH > 8) ? 8 : (curTopBarH - 1);
    uint16_t badgeCol = curTaskRunning ? 0x07E0 : TFT_RED;
    M5.Display.fillRoundRect(liveBadgeX, (curTopBarH > 8) ? 1 : 0, liveBadgeW, liveBadgeH, 2, badgeCol);
    M5.Display.setTextDatum(top_center);
    M5.Display.setTextColor(curTaskRunning ? TFT_BLACK : TFT_WHITE, badgeCol);
    M5.Display.drawString(curTaskRunning ? "ON" : "OFF", liveBadgeX + liveBadgeW / 2, (curTopBarH > 8) ? 1 : 0);

    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(ICON_DIM, CARD_BG);
    M5.Display.drawString("Fn+`", SW - 2, (curTopBarH > 8) ? 1 : 0);

    topBarDirty = false;
    prevTaskRunning = curTaskRunning;
  }

  // 2. 脏行文本绘制（按颜色分段批量输出，减少 SPI 事务数）
  M5.Display.setFont(curFont);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(top_left);
  for (int r = 0; r < curRows; r++) {
    if (!lineDirty[r]) continue;
    int y = curTopBarH + r * curCharH;
    int col = 0;
    while (col < curCols) {
      uint16_t fg = termBuf[r][col].fg;
      uint16_t bg = termBuf[r][col].bg;
      char chunk[MAX_TERM_COLS + 1];
      int chunkLen = 0;
      int startCol = col;
      while (col < curCols && termBuf[r][col].fg == fg && termBuf[r][col].bg == bg) {
        chunk[chunkLen++] = termBuf[r][col].c;
        col++;
      }
      chunk[chunkLen] = '\0';
      M5.Display.setTextColor(fg, bg);
      M5.Display.drawString(chunk, startCol * curCharW, y);
    }
    lineDirty[r] = false;
  }

  // 3. 光标下划线：单独绘制，不污染整行 dirty
  int safeX = constrain(cursorX, 0, curCols - 1);
  int safeY = constrain(cursorY, 0, curRows - 1);
  if (prevCursorX != safeX || prevCursorY != safeY) {
    int oldX = constrain(prevCursorX, 0, curCols - 1);
    int oldY = constrain(prevCursorY, 0, curRows - 1);
    uint16_t oldBg = termBuf[oldY][oldX].bg;
    M5.Display.drawFastHLine(oldX * curCharW, curTopBarH + oldY * curCharH + curCharH - 1, curCharW, oldBg);
    prevCursorX = safeX;
    prevCursorY = safeY;
  }
  uint16_t cursorCol = termCursorBlink ? ACCENT : termBuf[safeY][safeX].bg;
  M5.Display.drawFastHLine(safeX * curCharW, curTopBarH + safeY * curCharH + curCharH - 1, curCharW, cursorCol);

  // 4. 底部微型总线与 RX/TX 硬件指示微灯
  bool txActive = (millis() - lastTxMs < 120);
  bool rxActive = (millis() - lastRxMs < 120);
  if (curRows == 15) {
    static bool botBarDrawn = false;
    if (!botBarDrawn || topBarDirty) {
      M5.Display.drawFastHLine(0, 130, SW, 0x18C3);
      M5.Display.setFont(&fonts::Font0);
      M5.Display.setTextDatum(top_left);
      M5.Display.setTextColor(0x4208, TFT_BLACK);
      M5.Display.drawString("Fn+Ent:Font  ESC:Fn+Tab", 4, 131);
      botBarDrawn = true;
    }
    M5.Display.fillRect(SW - 20, 132, 3, 2, txActive ? 0x07FF : 0x0842);
    M5.Display.fillRect(SW - 8, 132, 3, 2, rxActive ? 0x07E0 : 0x0842);
  } else {
    // 8 行模式：TX/RX 指示灯直接融入顶栏
    M5.Display.fillRect(SW - 100, 2, 3, 3, txActive ? 0x07FF : 0x0842);
    M5.Display.fillRect(SW - 95, 2, 3, 3, rxActive ? 0x07E0 : 0x0842);
  }
}

void drawSsh() {
  switch (sshState) {
    case SSH_ST_CONFIG:
      drawSshConfig();
      break;
    case SSH_ST_CONNECTING:
      drawSshConnecting();
      break;
    case SSH_ST_TERMINAL:
      drawSshTerminal();
      break;
    case SSH_ST_ERROR:
      drawSshError();
      break;
  }
}
