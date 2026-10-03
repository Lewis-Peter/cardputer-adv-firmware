#include "Arduino.h"
#include "M5Unified.h"
#include "hash_oven.h"
#include "sha512_crypt.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <cassert>
#include <unordered_set>

// ---- 替身全局状态 ----
uint32_t g_fakeNowMs = 0;
SimSerial Serial;
SimM5 M5;
M5Canvas cv;
Preferences prefs;
bool dirty = false;
int SW = 240, SH = 135;
uint16_t ACCENT = 0, CARD_BG = 0, DIM_BORDER = 0, ICON_DIM = 0;

void drawPageDots() {}
void drawMiniVuMeter(int, int, uint8_t, bool, uint16_t, uint16_t) {}
void drawPageHeader(const char*, const char*, uint16_t) {}
String trunc(const String& s, int) { return s; }
int powerBatteryVoltage() { return 4000; }
int powerBatteryCurrent() { return 0; }
int powerBatteryLevel() { return 80; }
int powerBatteryMv() { return 4000; }

extern "C" bool sha512_crypt_calc(const char*, const char*, int, char*, size_t, volatile int*, volatile bool*) {
  return true;
}

namespace crypto {
bool aes128_cbc_encrypt(const char*, const char*, char*, size_t) { return true; }
bool aes128_cbc_decrypt(const char*, const char*, char*, size_t) { return true; }
bool rc4_encrypt(const char*, const char*, char*, size_t) { return true; }
bool rc4_decrypt(const char*, const char*, char*, size_t) { return true; }
bool xor_encrypt(const char*, const char*, char*, size_t) { return true; }
bool xor_decrypt(const char*, const char*, char*, size_t) { return true; }
bool caesar_shift(const char*, int, char*, size_t) { return true; }
bool vigenere_encrypt(const char*, const char*, char*, size_t) { return true; }
bool vigenere_decrypt(const char*, const char*, char*, size_t) { return true; }
bool base64_encode(const char*, char*, size_t) { return true; }
bool base64_decode(const char*, char*, size_t) { return true; }
bool hex_encode(const char*, char*, size_t) { return true; }
bool hex_decode(const char*, char*, size_t) { return true; }
bool md5_crypt_calc(const char*, const char*, char*, size_t) { return true; }
bool sha256_hex(const char*, char*) { return true; }
bool md5_hex(const char*, char*) { return true; }
}

// ---- 测试计数与断言 ----
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* name, const char* detail = "") {
  if (cond) {
    g_pass++;
    printf("  [PASS] %s\n", name);
  } else {
    g_fail++;
    printf("  [FAIL] %s %s\n", name, detail);
  }
}

// 计算某字符串在指定 base 字符集下的整数索引
static uint64_t decodeGuessIndex(const char* str, const char* charset) {
  int base = (int)strlen(charset);
  uint64_t idx = 0;
  for (int i = 0; str[i] != '\0'; i++) {
    const char* p = strchr(charset, str[i]);
    if (!p) return UINT64_MAX;
    idx = idx * (uint64_t)base + (uint64_t)(p - charset);
  }
  return idx;
}

// 计算 base^exp 的 64 位无符号整数
static uint64_t power64(uint64_t base, int exp) {
  uint64_t res = 1;
  for (int i = 0; i < exp; i++) res *= base;
  return res;
}

// =================================================================
// 1. 各 keyspace 字符集、长度与总空间大小验证
// =================================================================
void testKeyspaceMetadata() {
  printf("--- 1. Keyspace 预设字符集、长度与总空间数学核对 ---\n");

  check(KEYSPACE_COUNT == 5, "Keyspace 预设档位数等于 5");

  for (int i = 0; i < KEYSPACE_COUNT; i++) {
    const KeyspacePreset& kp = KEYSPACES[i];
    int charCount = (int)strlen(kp.charset);
    int solLen = (int)strlen(kp.solution);

    // 检查字符集无重复字符
    bool charsetNoDup = true;
    for (int c1 = 0; c1 < charCount; c1++) {
      for (int c2 = c1 + 1; c2 < charCount; c2++) {
        if (kp.charset[c1] == kp.charset[c2]) { charsetNoDup = false; break; }
      }
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Keyspace %d [%s]: 字符集无重复字符 (base=%d)", i, kp.name, charCount);
    check(charsetNoDup, msg);

    // 检查预设总空间 total 是否等于 base^len
    uint64_t calculatedTotal = power64(charCount, solLen);
    snprintf(msg, sizeof(msg), "Keyspace %d [%s]: 总空间 total == %d^%d", i, kp.name, charCount, solLen);
    check(calculatedTotal == kp.total, msg);

    // 检查 solution 中的每个字符都在 charset 中
    bool solInCharset = true;
    for (int s = 0; s < solLen; s++) {
      if (!strchr(kp.charset, kp.solution[s])) { solInCharset = false; break; }
    }
    snprintf(msg, sizeof(msg), "Keyspace %d [%s]: 答案 \"%s\" 所有字符均在字符集中", i, kp.name, kp.solution);
    check(solInCharset, msg);
  }
}

// =================================================================
// 2. 索引覆盖全空间与双向一一映射 (无重复/无遗漏) 测试
// =================================================================
void testExhaustiveCoverage() {
  printf("--- 2. 索引全空间覆盖与单射无碰撞测试 ---\n");

  // 2.1 4-Digits: 全量 10,000 个索引遍历，验证无重复且一一对应
  {
    const KeyspacePreset& kp = KEYSPACES[0];
    const int N = 10000;
    std::vector<bool> seen(N, false);
    bool allValid = true;
    char guess[16];

    for (int idx = 0; idx < N; idx++) {
      generateCrackGuess(0, (uint64_t)idx, guess, sizeof(guess));
      if ((int)strlen(guess) != 4) { allValid = false; break; }

      uint64_t dec = decodeGuessIndex(guess, kp.charset);
      if (dec != (uint64_t)idx || seen[dec]) { allValid = false; break; }
      seen[dec] = true;
    }
    check(allValid, "4-Digits: 0..9999 全量 10000 个猜测严格一一映射、无碰撞、无遗漏");
  }

  // 2.2 4-Lower: 全量 456,976 个索引遍历，验证 26^4 空间全覆盖
  {
    const KeyspacePreset& kp = KEYSPACES[1];
    const uint32_t N = 456976;
    std::vector<bool> seen(N, false);
    bool allValid = true;
    char guess[16];

    for (uint32_t idx = 0; idx < N; idx++) {
      generateCrackGuess(1, (uint64_t)idx, guess, sizeof(guess));
      if ((int)strlen(guess) != 4) { allValid = false; break; }

      uint64_t dec = decodeGuessIndex(guess, kp.charset);
      if (dec != (uint64_t)idx || seen[dec]) { allValid = false; break; }
      seen[dec] = true;
    }
    check(allValid, "4-Lower: 0..456975 全量 456,976 个猜测严格一一映射、无碰撞、无遗漏");
  }

  // 2.3 针对大 keyspace (6-Lower, 8-Lower, 8-Alnum) 进行步进采样与双向自洽验证
  for (int ks = 2; ks < KEYSPACE_COUNT; ks++) {
    const KeyspacePreset& kp = KEYSPACES[ks];
    int len = (int)strlen(kp.solution);
    bool stepOk = true;
    char guess[32];

    // 检查首末端点
    generateCrackGuess(ks, 0, guess, sizeof(guess));
    for (int i = 0; i < len; i++) if (guess[i] != kp.charset[0]) stepOk = false;

    generateCrackGuess(ks, kp.total - 1, guess, sizeof(guess));
    int lastCharIdx = (int)strlen(kp.charset) - 1;
    for (int i = 0; i < len; i++) if (guess[i] != kp.charset[lastCharIdx]) stepOk = false;

    // 步进跨度采样 100,000 点
    uint64_t step = kp.total / 100000ULL;
    if (step == 0) step = 1;
    for (uint64_t idx = 0; idx < kp.total; idx += step) {
      generateCrackGuess(ks, idx, guess, sizeof(guess));
      if ((int)strlen(guess) != len) { stepOk = false; break; }
      uint64_t dec = decodeGuessIndex(guess, kp.charset);
      if (dec != idx) { stepOk = false; break; }
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Keyspace %d [%s]: 10万点跨空间大步进采样，双向编码反解 100%% 恒等", ks, kp.name);
    check(stepOk, msg);
  }
}

// =================================================================
// 3. 验证各档 solution 均能在对应索引处生成
// =================================================================
void testSolutionGeneration() {
  printf("--- 3. 各档预设 solution 命中生成测试 ---\n");

  for (int ks = 0; ks < KEYSPACE_COUNT; ks++) {
    const KeyspacePreset& kp = KEYSPACES[ks];
    uint64_t solIdx = decodeGuessIndex(kp.solution, kp.charset);

    char msg[128];
    snprintf(msg, sizeof(msg), "Keyspace %d [%s]: 答案 \"%s\" 索引 (0x%llx) < total (0x%llx)",
             ks, kp.name, kp.solution, (unsigned long long)solIdx, (unsigned long long)kp.total);
    check(solIdx < kp.total, msg);

    char guess[32];
    generateCrackGuess(ks, solIdx, guess, sizeof(guess));
    snprintf(msg, sizeof(msg), "Keyspace %d [%s]: generateCrackGuess(idx=%llu) 成功生成 solution \"%s\"",
             ks, kp.name, (unsigned long long)solIdx, kp.solution);
    check(strcmp(guess, kp.solution) == 0, msg, guess);
  }
}

// =================================================================
// 4. 边界与异常入参保护
// =================================================================
void testEdgeCases() {
  printf("--- 4. 边界与入参异常保护测试 ---\n");

  char buf[32];

  // 4.1 超小缓冲区截断保护（如 buffer 只有 3 字节，对 4 字节的生成不能越界写）
  memset(buf, 'X', sizeof(buf));
  generateCrackGuess(0, 1234, buf, 3);
  check(buf[2] == '\0' && buf[3] == 'X', "缓冲区过小时截断并写终结符，不发生越界写");

  // 4.2 非法 keyspace 下标保护
  memset(buf, 0, sizeof(buf));
  generateCrackGuess(-1, 0, buf, sizeof(buf));
  generateCrackGuess(KEYSPACE_COUNT, 0, buf, sizeof(buf));
  check(true, "非法 keyspace 下标安全返回，不崩溃");

  // 4.3 outSize == 0 保护
  generateCrackGuess(0, 0, buf, 0);
  check(true, "outSize == 0 安全返回，不崩溃");
}

int main() {
  printf("==========================================\n");
  printf("  Cardputer ADV: Hash Oven Host Tests\n");
  printf("==========================================\n");

  testKeyspaceMetadata();
  testExhaustiveCoverage();
  testSolutionGeneration();
  testEdgeCases();

  printf("==========================================\n");
  printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
  printf("==========================================\n");

  return g_fail > 0 ? 1 : 0;
}
