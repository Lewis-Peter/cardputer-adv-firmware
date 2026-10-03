// TF/SD 卡挂载 + 文件管理器（浏览目录、删除单个文件）。
// SPI: CS=12 SCK=40 MISO=39 MOSI=14
#pragma once
#include "globals.h"
#include <SD.h>

extern bool sdMounted;
// 卡挂上了没有。ir.cpp 读 /ir/*.ir 之前问一句，省得每个调用点各判一次 sdMounted。
static inline bool sdReady() { return sdMounted; }
extern uint64_t sdSizeMB, sdUsedMB;

void sdInit();
bool formatSD();   // 清空整张卡（不是真正 mkfs，等效于删掉所有文件/目录）

struct FileEntry { String name; bool isDir; uint32_t size; };
extern const int FILES_MAX;
extern const int FILES_VIS;
extern FileEntry* fileList;
extern int fileCount;
extern int fileIdx, fileTop;
extern String curPath;
extern bool fileDelConfirm;   // 删除二次确认

void filesExit();

void loadDir(const String& path);

// 提取路径中的文件名（去掉目录部分）
inline String sdBaseName(const char* path) {
  if (!path) return String();
  const char* slash = strrchr(path, '/');
  return slash ? String(slash + 1) : String(path);
}
inline String sdBaseName(const String& path) {
  return sdBaseName(path.c_str());
}

// 拼接目录与文件名，自动保证单斜杠分隔
inline String joinPath(const String& dir, const String& name) {
  if (dir.endsWith("/")) return dir + name;
  return dir + "/" + name;
}

// 扫描指定目录下符合扩展名（不区分大小写，如 ".wav"、".txt"、".ir"；若为 nullptr 则不过滤）的文件。
// 自动跳过目录及以 '.' 开头的隐藏/元数据文件。
// callback 签名为 bool(File& f, const String& baseName, const String& fullPath)
// 返回 false 表示终止扫描，返回 true 继续。
template <typename F>
void sdScanFiles(const char* dirPath, const char* extFilter, F&& callback) {
  if (!sdMounted) return;
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  String filterLower = extFilter ? extFilter : "";
  filterLower.toLowerCase();

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String baseName = sdBaseName(f.name());
      if (!baseName.startsWith(".")) {
        bool match = true;
        if (filterLower.length() > 0) {
          String nameLower = baseName;
          nameLower.toLowerCase();
          match = nameLower.endsWith(filterLower);
        }
        if (match) {
          String fullPath = joinPath(dirPath, baseName);
          bool cont = callback(f, baseName, fullPath);
          f.close();
          if (!cont) break;
          continue;
        }
      }
    }
    f.close();
  }
  dir.close();
}

#include "ui_common.h"
static inline String fmtSize(uint32_t sz) { return fmtBytes(sz); }

void drawFiles();


