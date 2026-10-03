#include "ir.h"
#include <cstring>
#include <strings.h>   // strcasecmp
#include <cstdlib>
#include "ir_proto.h"
#include "ir_tx.h"
#include "ui_common.h"
#include "sd_files.h"
#include "list_sel.h"

// ---- 容量。⚠️ 这几个数直接吃静态 RAM，而这块板子没有 PSRAM、静态段已经 119KB。
// 6×10 条命令 + 512 个原始段合计约 2.5KB，再往上加之前先去 STAT 看一眼余量。 ----
static const int IR_MAX_DEVS = 6;
static const int IR_MAX_CMDS = 10;
static const int IR_RAW_POOL = 512;      // 所有 raw 命令共用的段池（uint16，1KB）
static const int ROWS_VISIBLE = 5;

struct IrCmdEntry {
  char     name[13];
  IrProto  proto;
  uint32_t value;
  uint8_t  bits;        // 只有 Sony 用
  uint16_t rawAt;       // raw 专用：在段池里的起点
  uint16_t rawLen;      // raw 专用：段数；非 raw 时为 0
};

struct IrDevice {
  char name[15];
  IrCmdEntry cmds[IR_MAX_CMDS];
  uint8_t n;
  bool fromSd;
};

struct IrContext {
  IrDevice devs[IR_MAX_DEVS];
  uint16_t rawPool[IR_RAW_POOL];
};
static IrContext* irCtx = nullptr;
#define devs (irCtx->devs)
#define rawPool (irCtx->rawPool)

static int devN = 0;
static int devIdx = 0, cmdIdx = 0, scrollTop = 0;
static int rawUsed = 0;

static char statusMsg[40] = "";
static uint32_t statusMs = 0;
static bool sweeping = false;
static int  sweepAt = 0;
// ⚠️ 存"上一次发是什么时候"而不是"下一次该几点发"：millis() 会在 49.7 天后回绕，
// `millis() < sweepNext` 那种写法一回绕就卡死到下一个 49.7 天。
// 差值写法 `millis() - last >= 间隔` 在无符号回绕下照样是对的，全项目统一这么写。
static uint32_t sweepLastMs = 0;
static const uint32_t SWEEP_STEP_MS = 250;

// TX 脉冲视觉反馈状态
static uint32_t txActiveMs = 0;
static int lastTxIdx = -1;

static void setStatus(const char* m) {
  strncpy(statusMsg, m ? m : "", sizeof(statusMsg) - 1);
  statusMsg[sizeof(statusMsg) - 1] = 0;
  statusMs = millis();
  dirty = true;
}

// ---- 内置码表 ----
//
// ⚠️ **全部抄自公开流传的码表，一条都没有在实机上验证过**（这台设备没有红外接收，
// 验不了；见 HARDWARE.md）。它们的作用是"开箱能按一下试试"，不是"保证能用"。
// 真要用得舒服，把自己设备的码写成 /ir/xxx.ir 放 SD 卡，格式见 README。
//
// 值的写法刻意跟码表原文一致：NEC 家族写成 32 位十六进制、Sony 写成 12 位十六进制，
// 抄的时候少一层换算就少一处出错（ir_proto.h 顶上解释了为什么 IR_NEC32 是一等公民）。
struct BuiltinCmd { const char* name; IrProto proto; uint32_t value; uint8_t bits; };
struct BuiltinDev { const char* name; const BuiltinCmd* cmds; uint8_t n; };

static const BuiltinCmd LG_CMDS[] = {
  {"Power",  IR_NEC32, 0x20DF10EF, 32}, {"Vol+",   IR_NEC32, 0x20DF40BF, 32},
  {"Vol-",   IR_NEC32, 0x20DFC03F, 32}, {"Ch+",    IR_NEC32, 0x20DF00FF, 32},
  {"Ch-",    IR_NEC32, 0x20DF807F, 32}, {"Mute",   IR_NEC32, 0x20DF906F, 32},
  {"Input",  IR_NEC32, 0x20DFD02F, 32},
};
static const BuiltinCmd SAMSUNG_CMDS[] = {
  {"Power",  IR_SAMSUNG, 0xE0E040BF, 32}, {"Vol+",   IR_SAMSUNG, 0xE0E0E01F, 32},
  {"Vol-",   IR_SAMSUNG, 0xE0E0D02F, 32}, {"Ch+",    IR_SAMSUNG, 0xE0E048B7, 32},
  {"Ch-",    IR_SAMSUNG, 0xE0E008F7, 32}, {"Mute",   IR_SAMSUNG, 0xE0E0F00F, 32},
  {"Source", IR_SAMSUNG, 0xE0E0807F, 32},
};
static const BuiltinCmd SONY_CMDS[] = {
  {"Power",  IR_SONY, 0xA90, 12}, {"Vol+",   IR_SONY, 0x490, 12},
  {"Vol-",   IR_SONY, 0xC90, 12}, {"Ch+",    IR_SONY, 0x090, 12},
  {"Ch-",    IR_SONY, 0x890, 12}, {"Mute",   IR_SONY, 0x290, 12},
};
#define BDEV(n, a) { n, a, (uint8_t)(sizeof(a) / sizeof((a)[0])) }
static const BuiltinDev BUILTINS[] = {
  BDEV("LG TV", LG_CMDS), BDEV("Samsung TV", SAMSUNG_CMDS), BDEV("Sony TV", SONY_CMDS),
};
#undef BDEV
static const int BUILTIN_COUNT = (int)(sizeof(BUILTINS) / sizeof(BUILTINS[0]));

// ---- 码表装载 ----

static void addBuiltins() {
  for (int i = 0; i < BUILTIN_COUNT && devN < IR_MAX_DEVS; i++) {
    IrDevice& d = devs[devN++];
    strncpy(d.name, BUILTINS[i].name, sizeof(d.name) - 1); d.name[sizeof(d.name) - 1] = 0;
    d.fromSd = false;
    d.n = 0;
    for (int c = 0; c < BUILTINS[i].n && d.n < IR_MAX_CMDS; c++) {
      IrCmdEntry& e = d.cmds[d.n++];
      strncpy(e.name, BUILTINS[i].cmds[c].name, sizeof(e.name) - 1); e.name[sizeof(e.name) - 1] = 0;
      e.proto  = BUILTINS[i].cmds[c].proto;
      e.value  = BUILTINS[i].cmds[c].value;
      e.bits   = BUILTINS[i].cmds[c].bits;
      e.rawLen = 0;
      e.rawAt  = 0;
    }
  }
}

// 解析 SD 上的一行。格式（空白分隔，# 开头是注释）：
//   Power   nec32  0x20DF10EF
//   Ch+     nec    0x0408                  <- 高字节 addr、低字节 cmd，反码自动补
//   Mute    sony   0x290 12                <- Sony 要给位数
//   Beep    rc5    0x300C
//   AcOn    raw    38000 3400,1700,430,1300,...
// 返回 false = 这一行不算命令（空行/注释/格式不对）。
static bool parseCmdLine(char* line, IrCmdEntry& out) {
  char* p = line;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n') return false;

  char* name = strtok(p, " \t\r\n");
  char* pro  = strtok(nullptr, " \t\r\n");
  char* a1   = strtok(nullptr, " \t\r\n");
  if (!name || !pro || !a1) return false;

  IrProto proto;
  if (!irProtoFromName(pro, proto)) return false;

  strncpy(out.name, name, sizeof(out.name) - 1); out.name[sizeof(out.name) - 1] = 0;
  out.proto  = proto;
  out.bits   = 32;
  out.rawAt  = 0;
  out.rawLen = 0;

  if (proto == IR_RAW) {
    // a1 是载波，后面那一坨是逗号分隔的微秒数。段存进公共池，IrCmdEntry 里只记起止——
    // 每条命令都自带一个 260 段的数组的话，6×10 条就是 31KB 静态 RAM，这块板子给不起。
    out.value = (uint32_t)atol(a1);
    char* list = strtok(nullptr, " \t\r\n");
    if (!list) return false;
    int start = rawUsed, cnt = 0;
    for (char* t = strtok(list, ","); t; t = strtok(nullptr, ",")) {
      long v = atol(t);
      if (v <= 0 || v > 32767) { rawUsed = start; return false; } // RMT 一段最多 32767µs，见 ir_tx.cpp
      if (rawUsed >= IR_RAW_POOL || cnt >= IR_MAX_DUR) { rawUsed = start; return false; }
      rawPool[rawUsed++] = (uint16_t)v;
      cnt++;
    }
    if (cnt == 0) { rawUsed = start; return false; }
    out.rawAt  = (uint16_t)start;
    out.rawLen = (uint16_t)cnt;
    return true;
  }

  out.value = (uint32_t)strtoul(a1, nullptr, 0);      // 0 前缀 = 自动认 0x
  char* a2 = strtok(nullptr, " \t\r\n");
  if (proto == IR_SONY) out.bits = a2 ? (uint8_t)atoi(a2) : 12;
  else if (proto == IR_RC5) out.bits = 14;
  return true;
}

// 从 /ir/*.ir 读设备。文件名（去掉扩展名）就是设备名，除非文件里有一行 `name XXX`。
static void loadFromSd() {
  if (!sdReady()) return;
  sdScanFiles("/ir", ".ir", [](File& f, const String& fn, const String& /*fullPath*/) -> bool {
    if (devN >= IR_MAX_DEVS) return false;

    IrDevice& d = devs[devN];
    d.n = 0;
    d.fromSd = true;
    String base = fn.substring(0, fn.length() - 3);
    strncpy(d.name, base.c_str(), sizeof(d.name) - 1); d.name[sizeof(d.name) - 1] = 0;

    static char line[1024];   // 空调 RAW 码常超过 192 字符；1KB 别放 loop 栈上
    int li = 0;
    bool overflow = false;
    while (f.available() && d.n < IR_MAX_CMDS) {
      char ch = (char)f.read();
      if (ch != '\n') {
        if (overflow) continue;
        if (li < (int)sizeof(line) - 1) {
          line[li++] = ch;
        } else {
          overflow = true;
          li = 0;
        }
        continue;
      }
      if (overflow) {
        overflow = false;
        li = 0;
        continue;
      }
      line[li] = 0; li = 0;
      // `name XXX` 覆盖文件名。放在解析命令之前判，否则 "name" 会被当成按键名。
      if (!strncmp(line, "name ", 5)) {
        char* v = line + 5;
        while (*v == ' ') v++;
        char* e = v + strlen(v);
        while (e > v && (e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (*v) { strncpy(d.name, v, sizeof(d.name) - 1); d.name[sizeof(d.name) - 1] = 0; }
        continue;
      }
      IrCmdEntry e;
      if (parseCmdLine(line, e)) d.cmds[d.n++] = e;
    }
    if (!overflow && li > 0 && d.n < IR_MAX_CMDS) {         // 文件末尾没有换行的最后一行
      line[li] = 0;
      IrCmdEntry e;
      if (parseCmdLine(line, e)) d.cmds[d.n++] = e;
    }
    if (d.n > 0) devN++;
    return true;
  });
}

static bool irTxStarted = false;   // irEnter 分配失败会提前返回，发射器就没初始化过

static void reload() {
  if (!irCtx) {
    irCtx = (IrContext*)malloc(sizeof(IrContext));
    if (!irCtx) { setStatus("out of memory"); devN = 0; return; }
  }
  if (!irTxStarted) irTxStarted = irTxBegin();
  devN = 0; rawUsed = 0;
  loadFromSd();          // SD 的排在前面：自己配的码比内置的猜测更该先看到
  addBuiltins();
  if (devIdx >= devN) devIdx = 0;
  if (devN && cmdIdx >= devs[devIdx].n) cmdIdx = 0;
  scrollTop = 0;
}

// ---- 发送 ----

static bool buildSignal(const IrCmdEntry& e, IrSignal& sig) {
  if (e.proto == IR_RAW) return irEncodeRaw(&rawPool[e.rawAt], e.rawLen, (uint16_t)e.value, sig);
  return irEncode(e.proto, e.value, e.bits, sig);
}

static bool sendCmd(const IrCmdEntry& e) {
  IrSignal sig;
  if (!buildSignal(e, sig)) { setStatus("encode failed"); return false; }
  if (!irTxSend(sig)) { setStatus(irTxError()[0] ? irTxError() : "send failed"); return false; }
  char b[40];
  snprintf(b, sizeof(b), "Sent %s", e.name);
  setStatus(b);
  txActiveMs = millis();
  lastTxIdx = cmdIdx;
  dirty = true;
  return true;
}

// ---- 生命周期 ----

void irEnter() {
  if (!irCtx) {
    irCtx = (IrContext*)malloc(sizeof(IrContext));
    if (!irCtx) { setStatus("out of memory"); dirty = true; return; }
  }
  reload();
  cmdIdx = 0; scrollTop = 0;
  sweeping = false;
  txActiveMs = 0;
  lastTxIdx = -1;
  statusMsg[0] = 0;
  if (!irTxStarted) irTxStarted = irTxBegin();
  if (!irTxStarted) setStatus(irTxError()[0] ? irTxError() : "IR init failed");
  dirty = true;
}

void irExit() {
  sweeping = false;
  txActiveMs = 0;
  irTxEnd();          // 把 RMT 通道还回去（FastLED 驱动 SK6812 时也要用 RMT）
  irTxStarted = false;
  if (irCtx) {
    free(irCtx);
    irCtx = nullptr;
    devN = 0;
    rawUsed = 0;
  }
}

// 关机扫频：把每台设备名叫 Power 的那条挨个发一遍。
// 一帧几十毫秒，但要留出接收端的解码时间，所以每 250ms 发一条、由 update 推进——
// 在按键回调里 for 循环发完的话，整个扫频期间主循环是冻住的。
void irUpdate() {
  if (!irCtx) return;
  if (txActiveMs && millis() - txActiveMs > 350) {
    txActiveMs = 0;
    dirty = true;
  }
  if (!sweeping) return;
  if (millis() - sweepLastMs < SWEEP_STEP_MS) return;

  while (sweepAt < devN) {
    IrDevice& d = devs[sweepAt];
    for (int c = 0; c < d.n; c++) {
      if (strcasecmp(d.cmds[c].name, "power")) continue;
      char b[40];
      snprintf(b, sizeof(b), "sweep %d/%d %s", sweepAt + 1, devN, d.name);
      setStatus(b);
      sendCmd(d.cmds[c]);
      sweepAt++;
      sweepLastMs = millis();
      dirty = true;
      return;
    }
    sweepAt++;      // 这台没有 Power，跳过
  }
  sweeping = false;
  setStatus("sweep done");
  dirty = true;
}

void irKey(char k) {
  if (!irCtx) {
    if (k == 'r' || k == 'R') {
      reload();
      if (!irCtx) setStatus("out of memory");
      else        setStatus("reloaded");
    }
    dirty = true;
    return;
  }
  if (sweeping && k != '`') { sweeping = false; setStatus("sweep stopped"); dirty = true; return; }
  if (devN == 0) {
    if (k == 'r' || k == 'R') { reload(); setStatus(irCtx ? "reloaded" : "out of memory"); }
    dirty = true;
    return;
  }
  IrDevice& d = devs[devIdx];

  if (k == ',' || k == '<' || k == 'h') { devIdx = (devIdx - 1 + devN) % devN; cmdIdx = 0; scrollTop = 0; }
  else if (k == '/' || k == '>' || k == 'l') { devIdx = (devIdx + 1) % devN; cmdIdx = 0; scrollTop = 0; }
  else if (k == ';' || k == ':' || k == 'k') { if (d.n) cmdIdx = (cmdIdx - 1 + d.n) % d.n; }
  else if (k == '.' || k == 'j') { if (d.n) cmdIdx = (cmdIdx + 1) % d.n; }
  else if (k == '\n') { if (d.n) sendCmd(d.cmds[cmdIdx]); }
  else if (k == 'r' || k == 'R') { reload(); setStatus("reloaded"); }
  else if (k == 'p' || k == 'P') {
    sweeping = true; sweepAt = 0; sweepLastMs = millis() - SWEEP_STEP_MS;   // 立刻发第一条
    setStatus("sweep...");
  }

  listClampScroll(cmdIdx, scrollTop, d.n, ROWS_VISIBLE);
  dirty = true;
}

// ---- 绘制 ----

void drawIr() {
  cv.fillScreen(TFT_BLACK);

  // 1. 顶部 Header
  char rightBadge[32];
  uint16_t badgeCol = ACCENT;
  bool isTxActive = (txActiveMs && millis() - txActiveMs < 400);

  if (sweeping) {
    snprintf(rightBadge, sizeof(rightBadge), "SWEEP %d/%d", sweepAt + 1, devN);
    badgeCol = TFT_YELLOW;
  } else if (isTxActive) {
    snprintf(rightBadge, sizeof(rightBadge), "TX BLAST");
    badgeCol = cv.color565(255, 60, 60);
  } else if (!irCtx) {
    snprintf(rightBadge, sizeof(rightBadge), "NO MEM");
    badgeCol = TFT_RED;
  } else if (!irTxReady()) {
    snprintf(rightBadge, sizeof(rightBadge), "HW ERR");
    badgeCol = TFT_RED;
  } else if (devN) {
    snprintf(rightBadge, sizeof(rightBadge), "%d DEV", devN);
    badgeCol = ACCENT;
  } else {
    snprintf(rightBadge, sizeof(rightBadge), "NO CODES");
    badgeCol = TFT_DARKGREY;
  }
  drawPageHeader("IR Blaster", rightBadge, badgeCol);

  if (!irCtx) {
    cv.fillRoundRect(8, 22, SW - 16, 86, 4, cv.color565(35, 12, 12));
    cv.drawRoundRect(8, 22, SW - 16, 86, 4, cv.color565(140, 30, 30));
    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.setTextColor(TFT_RED);
    cv.drawString("[ OUT OF MEMORY ]", SW / 2, 32);
    cv.setTextColor(TFT_WHITE);
    cv.drawString(statusMsg[0] ? statusMsg : "Cannot allocate memory", SW / 2, 50);
    cv.setTextColor(cv.color565(200, 150, 150));
    cv.drawString("Press 'R' to Retry allocation", SW / 2, 70);
    cv.setTextDatum(bottom_center); cv.setTextColor(TFT_DARKGREY);
    cv.drawString("r retry   ` menu", SW / 2, SH - 6);
    return;
  }

  // 2. 硬件不可用时的科技风警告卡片
  if (!irTxReady()) {
    cv.fillRoundRect(8, 22, SW - 16, 82, 4, cv.color565(35, 12, 12));
    cv.drawRoundRect(8, 22, SW - 16, 82, 4, cv.color565(140, 30, 30));
    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.setTextColor(TFT_RED);
    cv.drawString("[ HARDWARE WARNING ]", SW / 2, 32);
    cv.setTextColor(TFT_WHITE);
    cv.drawString(statusMsg[0] ? statusMsg : "IR transmitter not ready", SW / 2, 50);
    cv.setTextColor(cv.color565(200, 150, 150));
    cv.drawString("RMT Ch3 Collision (FastLED / RMT)", SW / 2, 66);
    cv.setTextColor(TFT_DARKGREY);
    cv.drawString("Check LED pin or restart device", SW / 2, 82);
    cv.setTextDatum(bottom_center); cv.setTextColor(TFT_DARKGREY);
    cv.drawString("` Menu", SW / 2, SH - 6);
    return;
  }

  // 3. 空码表提示卡片
  if (devN == 0) {
    cv.fillRoundRect(8, 22, SW - 16, 86, 4, cv.color565(15, 20, 26));
    cv.drawRoundRect(8, 22, SW - 16, 86, 4, cv.color565(30, 50, 70));
    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.setTextColor(ACCENT);
    cv.drawString("[ NO IR CODE TABLE ]", SW / 2, 32);
    cv.setTextColor(TFT_LIGHTGREY);
    cv.drawString("Put /ir/<name>.ir files on SD", SW / 2, 50);
    cv.setTextColor(TFT_DARKGREY);
    cv.drawString("Format: Power nec32 0x20DF10EF", SW / 2, 66);
    cv.setTextColor(cv.color565(120, 180, 240));
    cv.drawString("Press 'R' to Reload SD Cards", SW / 2, 82);
    cv.setTextDatum(bottom_center); cv.setTextColor(TFT_DARKGREY);
    cv.drawString("r reload   ` menu", SW / 2, SH - 6);
    return;
  }

  IrDevice& d = devs[devIdx];

  // 4. 设备选择器胶囊卡片 (Hero Card: y=15..32, h=18)
  cv.fillRoundRect(4, 15, SW - 8, 18, 3, cv.color565(14, 19, 28));
  cv.drawRoundRect(4, 15, SW - 8, 18, 3, cv.color565(35, 48, 68));

  // 左侧导航箭头与设备名称
  cv.setTextDatum(middle_left); cv.setTextSize(1);
  cv.setTextColor(ACCENT);
  cv.drawString("<", 8, 24);
  cv.setTextColor(TFT_WHITE);
  String devTitle = truncPx(d.name, 110);
  cv.drawString(devTitle, 18, 24);
  int devTitlePx = cv.textWidth(devTitle);
  cv.setTextColor(ACCENT);
  cv.drawString(">", 20 + devTitlePx, 24);

  // 右侧设备计数与来源徽章
  // 来源徽章 SD: 24x12, BUILT-IN: 46x12
  int badgeW = d.fromSd ? 24 : 46;
  int badgeX = SW - 8 - badgeW;
  uint16_t bBg = d.fromSd ? cv.color565(0, 45, 35) : cv.color565(35, 20, 48);
  uint16_t bBd = d.fromSd ? ACCENT : cv.color565(130, 80, 180);
  uint16_t bFg = d.fromSd ? ACCENT : cv.color565(190, 150, 240);
  cv.fillRoundRect(badgeX, 18, badgeW, 12, 2, bBg);
  cv.drawRoundRect(badgeX, 18, badgeW, 12, 2, bBd);
  cv.setTextDatum(middle_center);
  cv.setTextColor(bFg);
  cv.drawString(d.fromSd ? "SD" : "BUILT-IN", badgeX + badgeW / 2, 24);

  // 计数器 (例如 "1/3")
  char countBuf[24];
  snprintf(countBuf, sizeof(countBuf), "%d/%d", devIdx + 1, devN);
  cv.setTextDatum(middle_right);
  cv.setTextColor(cv.color565(120, 140, 165));
  cv.drawString(countBuf, badgeX - 5, 24);

  // 5. 5行命令瓷砖列表 (y0 = 36, rowH = 15)
  bool needScrollbar = (d.n > ROWS_VISIBLE);
  int tileW = needScrollbar ? (SW - 16) : (SW - 8);
  int tileX = 4;

  for (int r = 0; r < ROWS_VISIBLE; r++) {
    int i = scrollTop + r;
    if (i >= d.n) break;
    const IrCmdEntry& e = d.cmds[i];
    int y = 36 + r * 15;
    bool sel = (i == cmdIdx);
    bool blasting = (isTxActive && sel);

    // 瓷砖底色与边框
    uint16_t tBg, tBorder;
    if (blasting) {
      tBg = cv.color565(55, 15, 15);
      tBorder = cv.color565(255, 70, 70);
    } else if (sel) {
      tBg = cv.color565(12, 32, 48);
      tBorder = ACCENT;
    } else {
      tBg = cv.color565(11, 14, 19);
      tBorder = cv.color565(24, 30, 40);
    }
    cv.fillRoundRect(tileX, y, tileW, 14, 3, tBg);
    cv.drawRoundRect(tileX, y, tileW, 14, 3, tBorder);

    // 选中时左侧高亮细条
    if (sel) {
      cv.fillRect(tileX + 1, y + 2, 2, 10, blasting ? TFT_RED : ACCENT);
    }

    // 命令名称 (左对齐)
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setTextColor(blasting ? TFT_RED : (sel ? TFT_WHITE : TFT_LIGHTGREY));
    cv.drawString(trunc(e.name, 11), tileX + 8, y + 7);

    // 右侧：协议 + 十六进制码
    char protoStr[12];
    if (e.proto == IR_NEC32)       strcpy(protoStr, "NEC32");
    else if (e.proto == IR_NEC)    strcpy(protoStr, "NEC");
    else if (e.proto == IR_SAMSUNG)strcpy(protoStr, "SAMS");
    else if (e.proto == IR_SONY)   strcpy(protoStr, "SONY");
    else if (e.proto == IR_RC5)    strcpy(protoStr, "RC5");
    else {
      strncpy(protoStr, irProtoName(e.proto), sizeof(protoStr) - 1);
      protoStr[sizeof(protoStr) - 1] = '\0';
    }

    char valStr[20];
    if (e.proto == IR_RAW) {
      snprintf(valStr, sizeof(valStr), "%dseg", e.rawLen);
    } else if (e.proto == IR_SONY) {
      snprintf(valStr, sizeof(valStr), "0x%lX/%d", (unsigned long)e.value, e.bits);
    } else {
      snprintf(valStr, sizeof(valStr), "0x%08lX", (unsigned long)e.value);
    }

    // 渲染右侧参数
    cv.setTextDatum(middle_right);
    // 码值 (最右侧)
    uint16_t valCol = blasting ? TFT_YELLOW : (sel ? cv.color565(255, 215, 100) : cv.color565(140, 130, 90));
    cv.setTextColor(valCol);
    cv.drawString(valStr, tileX + tileW - 6, y + 7);
    int valPx = cv.textWidth(valStr);

    // 协议名 (在码值左侧)
    uint16_t protoCol = blasting ? cv.color565(255, 120, 120) : (sel ? ACCENT : cv.color565(70, 110, 130));
    cv.setTextColor(protoCol);
    cv.drawString(protoStr, tileX + tileW - 10 - valPx, y + 7);
  }

  // 6. 滚动条 (超过 5 行时)
  drawScrollBar(SW - 7, 36, 74, scrollTop, d.n, ROWS_VISIBLE, ACCENT, cv.color565(30, 36, 48));

  // 7. 动态遥测 / 状态光带 (y=112..122, h=10)
  uint16_t stBg;
  if (sweeping) {
    stBg = cv.color565(45, 36, 8);
    cv.fillRoundRect(4, 112, SW - 8, 10, 2, stBg);
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setTextColor(TFT_YELLOW);
    char swBuf[48];
    snprintf(swBuf, sizeof(swBuf), "SWEEP %d/%d: %s", sweepAt + 1, devN, d.name);
    cv.drawString(trunc(swBuf, 22), 8, 117);
    cv.setTextDatum(middle_right);
    cv.setTextColor(cv.color565(255, 200, 50));
    cv.drawString("[P:STOP]", SW - 8, 117);
  } else if (isTxActive) {
    stBg = cv.color565(50, 12, 12);
    cv.fillRoundRect(4, 112, SW - 8, 10, 2, stBg);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.setTextColor(cv.color565(255, 100, 100));
    const char* txName = (d.n && cmdIdx >= 0 && cmdIdx < d.n) ? d.cmds[cmdIdx].name : "";
    char txBuf[48];
    snprintf(txBuf, sizeof(txBuf), ">>> TX BLAST: %s >>>", txName);
    cv.drawString(txBuf, SW / 2, 117);
  } else if (statusMsg[0] && millis() - statusMs < 3000) {
    stBg = cv.color565(12, 28, 38);
    cv.fillRoundRect(4, 112, SW - 8, 10, 2, stBg);
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setTextColor(ACCENT);
    char stBuf[48];
    snprintf(stBuf, sizeof(stBuf), "* %s", statusMsg);
    cv.drawString(trunc(stBuf, 36), 8, 117);
  } else {
    stBg = cv.color565(9, 13, 18);
    cv.fillRoundRect(4, 112, SW - 8, 10, 2, stBg);
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setTextColor(cv.color565(75, 95, 115));
    cv.drawString("TX: GPIO44 (RMT3)", 8, 117);
    cv.setTextDatum(middle_right);
    cv.setTextColor(cv.color565(65, 85, 105));
    cv.drawString("38kHz 33%", SW - 8, 117);
  }

  // 8. 统一底部按键提示 (y=125)
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(ACCENT);
  cv.drawString("Enter", 4, 125);
  cv.setTextColor(TFT_DARKGREY);
  cv.drawString(" TX", 34, 125);

  cv.setTextColor(ACCENT);
  cv.drawString(",/", 54, 125);
  cv.setTextColor(TFT_DARKGREY);
  cv.drawString(" Dev", 66, 125);

  cv.setTextColor(ACCENT);
  cv.drawString(";.", 92, 125);
  cv.setTextColor(TFT_DARKGREY);
  cv.drawString(" Cmd", 104, 125);

  cv.setTextColor(ACCENT);
  cv.drawString("P", 132, 125);
  cv.setTextColor(TFT_DARKGREY);
  cv.drawString(" Swp", 138, 125);

  cv.setTextColor(ACCENT);
  cv.drawString("R", 168, 125);
  cv.setTextColor(TFT_DARKGREY);
  cv.drawString(" Reload", 174, 125);
}
