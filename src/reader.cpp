#include "reader.h"
#include <vector>
#include "gbk_table.h"
#include "ui_common.h"
#include "icons.h"
#include "sd_files.h"
#include "list_sel.h"

bool readerFromFiles = false;

struct BookEntry {
  String path;
  String name;
  uint32_t size;
  uint32_t progress;
};

static BookEntry* shelfBooks = nullptr;
static int shelfCount = 0;
static int shelfIdx = 0;
static int shelfTop = 0;
static const int SHELF_MAX = 48;
static const int SHELF_VIS = 5;

const FontSizeConfig FONT_CONFIGS[FONT_SIZE_COUNT] = {
  { &fonts::efontCN_12, 14, 8, "12px" },
  { &fonts::efontCN_14, 16, 7, "14px" },
  { &fonts::efontCN_16, 19, 6, "16px" }
};

int readerFontSize = FONT_SIZE_14;
static bool readerFontInitDone = false;

static void readerEnsureFontInit() {
  if (!readerFontInitDone) {
    readerFontSize = loadUInt("reader", "fontSize", FONT_SIZE_14);
    if (readerFontSize < 0 || readerFontSize >= FONT_SIZE_COUNT) readerFontSize = FONT_SIZE_14;
    readerFontInitDone = true;
  }
}

static const int MAX_READER_LINES_CAP = 10;   // 支持最多 10 行
File   readerFile;
static String readerPath;
static uint32_t readerPageStart = 0, readerNextStart = 0;
static bool   readerEOF = false;
static bool   readerIsGbk = false;
static String readerLines[MAX_READER_LINES_CAP];
static int    readerLineCount = 0;
static std::vector<uint32_t> readerBack;   // 上一页的起始偏移栈

static uint32_t fnv1a(const String& s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}
void readerSaveProgress() {
  char key[9]; snprintf(key, sizeof(key), "%08x", fnv1a(readerPath));
  saveUInt("reader", key, readerPageStart);
}
uint32_t readerLoadProgress(const String& path) {
  char key[9]; snprintf(key, sizeof(key), "%08x", fnv1a(path));
  return loadUInt("reader", key, 0);
}

// 从 startOffset 开始，按屏幕宽度换行、填满当前字号的 maxLines 行，记下一页起点
static void readerLoadPage(uint32_t startOffset) {
  readerEnsureFontInit();
  readerLineCount = 0;
  readerEOF = false;
  if (!readerFile) return;
  readerFile.seek(startOffset);

  const FontSizeConfig& cfg = FONT_CONFIGS[readerFontSize];
  cv.setFont(cfg.font);
  cv.setTextSize(1);
  const int maxW = SW - 16;
  String line;
  while (readerLineCount < cfg.maxLines) {
    uint32_t savePos = readerFile.position();
    int c = readerFile.read();
    if (c < 0) { readerEOF = true; break; }
    if (c == '\r') continue;
    char buf[5] = {0};
    if (!readerIsGbk) {
      int n = 0;
      buf[n++] = (char)c;
      int extra = (c & 0x80) == 0 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
      for (int i = 0; i < extra; i++) { int c2 = readerFile.read(); if (c2 < 0) break; buf[n++] = (char)c2; }
      buf[n] = 0;
    } else {
      if ((uint8_t)c < 0x80) {
        buf[0] = (char)c;
        buf[1] = 0;
      } else if ((uint8_t)c >= 0x81 && (uint8_t)c <= 0xFE) {
        int c2 = readerFile.read();
        if (c2 < 0) { readerEOF = true; break; }
        uint16_t u = gbkToUnicode((uint8_t)c, (uint8_t)c2);
        if (u) {
          unicodeToUtf8(u, buf);
        } else {
          buf[0] = '?'; buf[1] = 0;
        }
      } else {
        buf[0] = '?'; buf[1] = 0;
      }
    }
    if (c == '\n') { readerLines[readerLineCount++] = line; line = ""; continue; }
    String test = line + buf;
    if (cv.textWidth(test.c_str()) > maxW && line.length() > 0) {
      readerLines[readerLineCount++] = line;
      if (readerLineCount >= cfg.maxLines) { readerFile.seek(savePos); break; }   // 溢出字符留给下一页
      line = String(buf);
    } else {
      line = test;
    }
  }
  if (readerLineCount < cfg.maxLines && line.length() > 0) readerLines[readerLineCount++] = line;
  readerNextStart = readerFile.position();
  cv.setFont(&fonts::Font0);   // 恢复默认字体，别影响其它屏幕
}
void readerOpen(const String& path) {
  if (readerFile) readerFile.close();
  readerFile = SD.open(path, FILE_READ);
  readerPath = path;
  readerBack.clear();
  readerIsGbk = false;

  // 自动探测文本编码（UTF-8 vs GBK）
  if (readerFile) {
    uint8_t sample[512];
    size_t sampleLen = readerFile.read(sample, sizeof(sample));
    readerFile.seek(0);
    if (sampleLen >= 3 && sample[0] == 0xEF && sample[1] == 0xBB && sample[2] == 0xBF) {
      readerIsGbk = false;
    } else {
      bool validUtf8 = true;
      size_t k = 0;
      while (k < sampleLen) {
        uint8_t b = sample[k];
        if (b < 0x80) { k++; }
        else if ((b & 0xE0) == 0xC0) {
          if (k + 1 >= sampleLen) break;
          if ((sample[k + 1] & 0xC0) != 0x80) { validUtf8 = false; break; }
          k += 2;
        } else if ((b & 0xF0) == 0xE0) {
          if (k + 2 >= sampleLen) break;
          if ((sample[k + 1] & 0xC0) != 0x80 || (sample[k + 2] & 0xC0) != 0x80) { validUtf8 = false; break; }
          k += 3;
        } else if ((b & 0xF8) == 0xF0) {
          if (k + 3 >= sampleLen) break;
          if ((sample[k + 1] & 0xC0) != 0x80 || (sample[k + 2] & 0xC0) != 0x80 || (sample[k + 3] & 0xC0) != 0x80) { validUtf8 = false; break; }
          k += 4;
        } else {
          validUtf8 = false; break;
        }
      }
      if (!validUtf8) {
        readerIsGbk = true;
      }
    }
  }

  readerPageStart = readerLoadProgress(path);
  readerLoadPage(readerPageStart);
}
void readerNextPage() {
  if (readerEOF && readerLineCount == 0) return;   // 已经是最后一页
  readerBack.push_back(readerPageStart);
  readerPageStart = readerNextStart;
  readerLoadPage(readerPageStart);
}
void readerPrevPage() {
  if (readerBack.empty()) return;
  readerPageStart = readerBack.back();
  readerBack.pop_back();
  readerLoadPage(readerPageStart);
}
// 快速跳页：按 ~5% 文件长度前后跳，落点对齐到下一行开头（避免从半个字/半行开始）
void readerJump(int dir) {
  if (!readerFile) return;
  uint32_t sz = readerFile.size();
  if (sz == 0) return;
  long off = (long)readerPageStart + (long)dir * (long)(sz / 20);   // 5% = sz/20
  if (off < 0) off = 0;
  if (off >= (long)sz) off = (long)sz - 1;
  readerFile.seek(off);
  if (off > 0) { int c; while ((c = readerFile.read()) >= 0 && c != '\n') {} }   // 跳到下一行开头
  readerPageStart = readerFile.position();
  readerBack.clear();   // 跳转后上一页栈作废（用跳转键往回跳即可）
  readerLoadPage(readerPageStart);
}

void readerCycleFontSize() {
  readerEnsureFontInit();
  readerFontSize = (readerFontSize + 1) % FONT_SIZE_COUNT;
  saveUInt("reader", "fontSize", readerFontSize);
  readerBack.clear();
  if (readerFile) {
    readerLoadPage(readerPageStart);
  }
}

void readerSetFontSize(int sizeIdx) {
  readerEnsureFontInit();
  if (sizeIdx >= 0 && sizeIdx < FONT_SIZE_COUNT) {
    readerFontSize = sizeIdx;
    saveUInt("reader", "fontSize", readerFontSize);
    readerBack.clear();
    if (readerFile) {
      readerLoadPage(readerPageStart);
    }
  }
}

void drawReader() {
  readerEnsureFontInit();
  cv.fillScreen(TFT_BLACK);
  const FontSizeConfig& cfg = FONT_CONFIGS[readerFontSize];
  cv.setFont(cfg.font); cv.setTextSize(1);
  cv.setTextColor(TFT_WHITE, TFT_BLACK); cv.setTextDatum(top_left);
  int y = 2;
  for (int i = 0; i < readerLineCount; i++) {
    cv.drawString(readerLines[i].c_str(), 6, y);
    y += cfg.lineH;
  }
  cv.setFont(&fonts::Font0);   // 底部提示用回默认字体
  cv.setTextSize(1);

  // 底部进度条 + 字号 + 编码状态 + 百分比
  uint32_t sz = readerFile ? readerFile.size() : 0;
  float prog = sz ? (float)readerPageStart / sz : 0.0f;
  const int barX = 6, barY = SH - 4, barW = SW - 88;
  cv.drawFastHLine(barX, barY, barW, DIM_BORDER);
  if (prog > 0) cv.fillRect(barX, barY - 1, (int)(barW * prog), 3, ACCENT);
  char pb[32];
  snprintf(pb, sizeof(pb), "%s %s %d%%", cfg.label, readerIsGbk ? "GBK" : "UTF8", (int)(prog * 100 + 0.5f));
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK); cv.setTextDatum(bottom_right);
  cv.drawString(pb, SW - 4, SH - 1);

  if (readerLineCount == 0 && readerEOF) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK); cv.setTextDatum(bottom_left);
    cv.drawString("-- end --", barX, SH - 1);
  }
}

static void readerScanDir(const String& dirPath) {
  if (shelfCount >= SHELF_MAX) return;
  sdScanFiles(dirPath.c_str(), ".txt", [&](File& e, const String& bareName, const String& fullPath) -> bool {
    if (shelfCount >= SHELF_MAX) return false;
    bool exists = false;
    for (int i = 0; i < shelfCount; i++) {
      if (shelfBooks[i].path == fullPath) { exists = true; break; }
    }
    if (!exists) {
      shelfBooks[shelfCount].path = fullPath;
      shelfBooks[shelfCount].name = bareName;
      shelfBooks[shelfCount].size = e.size();
      shelfBooks[shelfCount].progress = readerLoadProgress(fullPath);
      shelfCount++;
    }
    return true;
  });
}

static void readerShelfScan() {
  if (!shelfBooks) {
    shelfBooks = new (std::nothrow) BookEntry[SHELF_MAX];
    if (!shelfBooks) return;
  }
  shelfCount = 0;
  if (!sdMounted) return;
  if (SD.exists("/books")) {
    readerScanDir("/books");
  } else if (SD.exists("/Books")) {
    readerScanDir("/Books");
  }
  readerScanDir("/");

  // 排序：按文件名升序
  for (int i = 1; i < shelfCount; i++) {
    BookEntry key = shelfBooks[i];
    int j = i - 1;
    while (j >= 0 && shelfBooks[j].name > key.name) {
      shelfBooks[j + 1] = shelfBooks[j];
      j--;
    }
    shelfBooks[j + 1] = key;
  }
  listClampScroll(shelfIdx, shelfTop, shelfCount, SHELF_VIS);
}

void readerEnter() {
  screen = SCREEN_READER_SHELF;
  readerShelfScan();
}

void readerShelfExit() {
  if (shelfBooks) {
    delete[] shelfBooks;
    shelfBooks = nullptr;
  }
  shelfCount = 0;
  shelfIdx = 0;
  shelfTop = 0;
}

void readerShelfKey(char k) {
  if (k == '`') {
    cleanupApp(SCREEN_READER_SHELF);
    screen = SCREEN_MENU;
    dirty = true;
    return;
  }
  if (k == 'f' || k == 'F') {
    readerCycleFontSize();
    dirty = true;
    return;
  }
  if (k == ';' || k == ',') {
    listMove(shelfIdx, shelfTop, shelfCount, SHELF_VIS, -1);
    dirty = true;
  } else if (k == '.' || k == '/') {
    listMove(shelfIdx, shelfTop, shelfCount, SHELF_VIS, 1);
    dirty = true;
  } else if (k == '\n') {
    if (shelfCount > 0 && shelfBooks) {
      readerFromFiles = false;
      readerOpen(shelfBooks[shelfIdx].path);
      screen = SCREEN_READER;
      dirty = true;
    }
  }
}

void drawReaderShelf() {
  readerEnsureFontInit();
  cv.fillScreen(TFT_BLACK);
  char title[32];
  if (shelfCount > 0) {
    snprintf(title, sizeof(title), "Bookshelf (%d)", shelfCount);
  } else {
    snprintf(title, sizeof(title), "Bookshelf");
  }
  char rightHdr[16];
  snprintf(rightHdr, sizeof(rightHdr), "Font:%s", FONT_CONFIGS[readerFontSize].label);
  drawPageHeader(title, rightHdr, ICON_DIM);

  if (!sdMounted) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString("no SD card inserted", SW / 2, SH / 2);
    return;
  }
  if (shelfCount == 0) {
    cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString("No .txt books found", SW / 2, SH / 2 - 8);
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("Put .txt files into / or /books", SW / 2, SH / 2 + 10);
    return;
  }

  const int startY = 18, rowH = 20;
  for (int p = 0; p < SHELF_VIS; p++) {
    int i = shelfTop + p;
    if (i >= shelfCount) break;
    int y = startY + p * rowH;
    bool sel = (i == shelfIdx);
    if (sel) {
      cv.fillRoundRect(4, y, SW - 8, rowH - 2, 4, CARD_BG);
      cv.fillRect(4, y, 3, rowH - 2, ACCENT);
    }
    int midY = y + (rowH - 2) / 2;
    icoBook(18, midY, 6, sel ? ACCENT : ICON_DIM);

    cv.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? CARD_BG : TFT_BLACK);
    cv.setTextDatum(middle_left); cv.setTextSize(1);
    cv.setFont(&fonts::efontCN_14);
    String displayName = ensureUtf8(shelfBooks[i].name);
    cv.drawString(truncPx(displayName, SW - 80), 30, midY);
    cv.setFont(&fonts::Font0);

    char progBuf[16];
    uint32_t bsz = shelfBooks[i].size;
    uint32_t bprog = shelfBooks[i].progress;
    if (bprog == 0) {
      snprintf(progBuf, sizeof(progBuf), "new");
    } else {
      int pct = (int)((float)bprog * 100.0f / (bsz ? bsz : 1) + 0.5f);
      if (pct > 100) pct = 100;
      snprintf(progBuf, sizeof(progBuf), "%d%%", pct);
    }
    cv.setTextColor(sel ? ACCENT : (bprog > 0 ? TFT_CYAN : TFT_DARKGREY), sel ? CARD_BG : TFT_BLACK);
    cv.setTextDatum(middle_right);
    cv.drawString(progBuf, SW - 8, midY);
  }

  drawScrollBar(SW - 4, startY, SHELF_VIS * rowH, shelfTop, shelfCount, SHELF_VIS);

  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_center); cv.setTextSize(1);
  cv.drawString("Enter:Read  ;/.Move  f:Font  `:Back", SW / 2, SH - 2);
}

