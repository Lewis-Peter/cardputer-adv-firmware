// 小说阅读器（支持 UTF-8 及 GBK/GB2312 编码的 .txt 自动识别与实时流式解码）。
// 流式分页 + 按文件路径的 NVS 断点续读 + 多档字号自适应重排。
#pragma once
#include "globals.h"
#include <SD.h>

extern File readerFile;
extern bool readerFromFiles;

enum ReaderFontSize {
  FONT_SIZE_12 = 0, // 12px (紧凑/小)
  FONT_SIZE_14 = 1, // 14px (标准/中，默认)
  FONT_SIZE_16 = 2, // 16px (大字/大)
  FONT_SIZE_COUNT = 3
};

struct FontSizeConfig {
  const lgfx::U8g2font* font;
  int lineH;
  int maxLines;
  const char* label;
};

extern const FontSizeConfig FONT_CONFIGS[FONT_SIZE_COUNT];
extern int readerFontSize;

void readerCycleFontSize();
void readerSetFontSize(int sizeIdx);

void readerEnter();
void readerShelfExit();
void readerShelfKey(char k);
void drawReaderShelf();
uint32_t readerLoadProgress(const String& path);

void readerOpen(const String& path);
void readerNextPage();
void readerPrevPage();
void readerJump(int dir);   // 快速跳页：dir=+1/-1，按 ~5% 文件长度跳
void readerSaveProgress();
void drawReader();
