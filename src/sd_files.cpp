#include "sd_files.h"
#include <SPI.h>
#include "icons.h"
#include "ui_common.h"
#include "gbk_table.h"

bool sdMounted = false;
uint64_t sdSizeMB = 0, sdUsedMB = 0;

void sdInit() {
  SPI.begin(40, 39, 14, 12);              // SCK, MISO, MOSI, SS
  sdMounted = SD.begin(12, SPI, 20000000);
  if (sdMounted) {
    sdSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
    sdUsedMB = SD.usedBytes() / (1024ULL * 1024ULL);
  }
}
// 递归删除某目录下全部内容（用于"格式化/清空"）
static void rmrf(const char* path) {
  File dir = SD.open(path);
  if (!dir) return;
  File e;
  while ((e = dir.openNextFile())) {
    String p = e.path();
    bool isdir = e.isDirectory();
    e.close();
    if (isdir) { rmrf(p.c_str()); SD.rmdir(p); }
    else       { SD.remove(p); }
  }
  dir.close();
}
// 清空整张卡（无真正 mkfs，等效于删掉所有文件/目录——f_mkfs 方案在实机上把卡搞成
// 分区表/引导扇区错乱、设备和电脑都读不出来，回退为更安全的"清空文件"做法）
bool formatSD() {
  if (!sdMounted) return false;
  rmrf("/");
  sdUsedMB = SD.usedBytes() / (1024ULL * 1024ULL);
  return true;
}

const int FILES_MAX = 64;      // 单目录最多列出的条目数
const int FILES_VIS = 4;       // 一屏可见条目数
FileEntry* fileList = nullptr;
int fileCount = 0;
int fileIdx = 0, fileTop = 0;
String curPath = "/";
bool fileDelConfirm = false;   // 删除二次确认

void filesExit() {
  if (fileList) {
    delete[] fileList;
    fileList = nullptr;
  }
  fileCount = 0;
  fileIdx = 0;
  fileTop = 0;
  fileDelConfirm = false;
}

void loadDir(const String& path) {
  fileCount = 0; fileIdx = 0; fileTop = 0; fileDelConfirm = false;
  if (!sdMounted) return;
  if (!fileList) {
    fileList = new (std::nothrow) FileEntry[FILES_MAX];
    if (!fileList) return;
  }
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File e;
  while (fileCount < FILES_MAX && (e = dir.openNextFile())) {
    fileList[fileCount].name  = sdBaseName(e.name());
    fileList[fileCount].isDir = e.isDirectory();
    fileList[fileCount].size  = e.size();
    fileCount++;
    e.close();
  }
  dir.close();
  // 排序：目录优先，然后按文件名
  for (int i = 1; i < fileCount; i++) {
    FileEntry key = fileList[i]; int j = i - 1;
    while (j >= 0 && (fileList[j].isDir < key.isDir ||
           (fileList[j].isDir == key.isDir && fileList[j].name > key.name))) {
      fileList[j + 1] = fileList[j]; j--;
    }
    fileList[j + 1] = key;
  }
}

void drawFiles() {
  cv.fillScreen(TFT_BLACK);
  drawPageHeader(trunc(curPath, 34).c_str());

  if (!sdMounted) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString("no SD card", SW / 2, SH / 2);
    return;
  }
  if (!fileList || fileCount == 0) {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString("(empty)", SW / 2, SH / 2);
  } else {
    const int startY = 18, rowH = 20;
    for (int p = 0; p < FILES_VIS; p++) {
      int i = fileTop + p;
      if (i >= fileCount) break;
      int y = startY + p * rowH;
      bool sel = (i == fileIdx);
      if (sel) {
        cv.fillRoundRect(4, y, SW - 8, rowH - 2, 4, CARD_BG);
        cv.fillRect(4, y, 3, rowH - 2, ACCENT);
      }
      int midY = y + (rowH - 2) / 2;
      FileEntry& fe = fileList[i];
      if (fe.isDir) icoFolder(20, midY, 7, sel ? ACCENT : TFT_LIGHTGREY);
      cv.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? CARD_BG : TFT_BLACK);
      cv.setTextDatum(middle_left); cv.setTextSize(1);
      cv.setFont(&fonts::efontCN_14);
      String displayName = ensureUtf8(fe.name);
      int maxW = fe.isDir ? (SW - 44) : 152;
      cv.drawString(truncPx(displayName, maxW), 34, midY);
      cv.setFont(&fonts::Font0);
      if (!fe.isDir) {
        cv.setTextColor(sel ? TFT_CYAN : TFT_DARKGREY, sel ? CARD_BG : TFT_BLACK);
        cv.setTextDatum(middle_right); cv.drawString(fmtBytes(fe.size), SW - 8, midY);
      }
    }
    drawScrollBar(SW - 4, startY, FILES_VIS * rowH, fileTop, fileCount, FILES_VIS);
  }

  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_center); cv.setTextSize(1);
  if (fileDelConfirm) cv.drawString("delete this file? Enter=yes  ` cancel", SW / 2, SH - 2);
}
