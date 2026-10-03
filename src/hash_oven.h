// SHA-512-crypt 赛博暖手宝 & 算力基准测试 / 荒谬穷举挑战 (Hash Oven)
// Mode 0: OVEN / BENCHMARK —— 双核压力烘焙，实时 H/s 测速、毫秒延迟、电池功耗遥测（方案 A）
// Mode 1: SINGLE HASH     —— 真实 Linux /etc/shadow $6$ 计算器，实时轮数进度条，串口导出
// Mode 2: FUTILITY CRACK  —— 荒谬穷举挑战与算力现实度量器，展示 MCU 暴力破解的渺小与绝望（方案 B）
#pragma once
#include "globals.h"

void hashOvenEnter();
void hashOvenExit();
void hashOvenUpdate();
bool hashOvenKey(char k);
void drawHashOven();

// ---- Mode 4: 荒谬穷举预设与猜测生成（供主机端测试）----
struct KeyspacePreset {
  const char* name;
  uint64_t total;
  const char* etaText;
  const char* targetHash;
  const char* solution;
  const char* charset;
};

extern const KeyspacePreset KEYSPACES[];
extern const int KEYSPACE_COUNT;

void generateCrackGuess(int ksIdx, uint64_t idx, char* out, size_t outSize);
