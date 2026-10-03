// Crypto Lab & SHA-512-crypt 赛博暖手宝 / 多算法密码工坊 (Hash Oven & Crypto Suite)
#include "hash_oven.h"
#include "sha512_crypt.h"
#include "crypto_engines.h"
#include "ui_common.h"
#include "power_util.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>
#include <cmath>
static const char CS_DIGIT[] = "0123456789";
static const char CS_LOWER[] = "abcdefghijklmnopqrstuvwxyz";
static const char CS_ALNUM[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

const KeyspacePreset KEYSPACES[] = {
  { "4-Digits [0-9]", 10000ULL, "~12 Mins",
    "$6$card$y4rEwZJgG1k3Xj9A7pZfM2qN0L5sK8uV1bC9xD4eT7aP2oI5mY8wQ1rT4uI7oP0aS3dF6gH9jK2lZ5xC8vB1n.", "0123", CS_DIGIT },
  { "4-Lower  [a-z]", 456976ULL, "~9.5 Hours",
    "$6$card$dummy4lower......................................................................", "card", CS_LOWER },
  { "6-Lower  [a-z]", 308915776ULL, "~270 Days",
    "$6$card$dummy6lower......................................................................", "cardpt", CS_LOWER },
  { "8-Lower  [a-z]", 208827064576ULL, "~490,000 Yrs",
    "$6$card$dummy8lower......................................................................", "infinity", CS_LOWER },
  { "8-Alnum  [a-Z0-9]", 218340105584896ULL, "~520M Years",
    "$6$card$dummy8alnum......................................................................", "entropy9", CS_ALNUM }
};
const int KEYSPACE_COUNT = sizeof(KEYSPACES) / sizeof(KEYSPACES[0]);

void generateCrackGuess(int ksIdx, uint64_t idx, char* out, size_t outSize) {
  if (ksIdx < 0 || ksIdx >= KEYSPACE_COUNT || outSize == 0) return;
  const KeyspacePreset& kp = KEYSPACES[ksIdx];
  int len = (int)strlen(kp.solution);
  if (len >= (int)outSize) len = (int)outSize - 1;
  const char* cs = kp.charset;
  int base = (int)strlen(cs);
  if (base <= 0) base = 1;

  out[len] = '\0';
  uint64_t n = idx;
  for (int i = len - 1; i >= 0; i--) {
    out[i] = cs[n % base];
    n /= base;
  }
}

namespace {

enum OvenMode {
  MODE_OVEN = 0,     // 算力烤炉 & 双核跑分
  MODE_CIPHER = 1,   // 对称加密/解密 (AES-128 / RC4 / XOR)
  MODE_CLASSIC = 2,  // 古典/CTF工具 (Caesar / Vigenere / Base64 / Hex)
  MODE_HASH = 3,     // 哈希与 Linux Shadow ($6$ SHA-512, $1$ MD5, SHA-256, MD5)
  MODE_CRACK = 4,    // 荒谬穷举挑战 (Futility Cracker)
  MODE__MAX = 5
};

static OvenMode curMode = MODE_OVEN;

// ---- 预设文本与密钥 ----
static const char* TEXT_PRESETS[] = {
  "Cardputer ADV 2026",
  "Hello World!",
  "Attack at dawn!",
  "flag{cyber_esp32_crypto}",
  "admin:secret_pass_99",
  "The quick brown fox jumps"
};
static const int TEXT_PRESET_COUNT = sizeof(TEXT_PRESETS) / sizeof(TEXT_PRESETS[0]);
static int textPresetIdx = 0;

static const char* KEY_PRESETS[] = {
  "cardputer", "secret123", "m5stack", "cyberkey", "omega99", "admin"
};
static const int KEY_PRESET_COUNT = sizeof(KEY_PRESETS) / sizeof(KEY_PRESETS[0]);
static int keyPresetIdx = 0;

static const char* SALT_PRESETS[] = {
  "salt1234", "adv2026", "root", "m5stack", "cypher"
};
static const int SALT_PRESET_COUNT = sizeof(SALT_PRESETS) / sizeof(SALT_PRESETS[0]);
static int saltPresetIdx = 0;

// ---- Mode 0: 烤炉跑分 ----
static volatile bool baking = false;
static volatile bool dualCore = false;
static volatile uint32_t totalHashes = 0;
static volatile float liveHps = 0.0f;
static volatile uint32_t lastHashDurationMs = 0;
static uint32_t bakeStartMs = 0;

// ---- Mode 1: 对称密码 (AES / RC4 / XOR) ----
enum CipherAlgo { CIPHER_AES128 = 0, CIPHER_RC4 = 1, CIPHER_XOR = 2, CIPHER__MAX = 3 };
static CipherAlgo cipherAlgo = CIPHER_AES128;
static bool cipherDecrypt = false; // false = Encrypt, true = Decrypt
static char cipherResult[192] = "";
static bool cipherSuccess = true;

// ---- Mode 2: 古典/CTF密码 ----
enum ClassicAlgo { CLASSIC_CAESAR = 0, CLASSIC_VIGENERE = 1, CLASSIC_BASE64 = 2, CLASSIC_HEX = 3, CLASSIC__MAX = 4 };
static ClassicAlgo classicAlgo = CLASSIC_CAESAR;
static int caesarShift = 13; // 默认 ROT13
static bool classicDecrypt = false;
static char classicResult[192] = "";

// ---- Mode 3: 哈希与 Shadow ----
enum HashAlgo { HASH_SHA512_CRYPT = 0, HASH_MD5_CRYPT = 1, HASH_SHA256 = 2, HASH_MD5 = 3, HASH__MAX = 4 };
static HashAlgo hashAlgo = HASH_SHA512_CRYPT;
static volatile int hashProgressRounds = 0;
static volatile bool hashRunning = false;
static char hashResult[192] = "";
static uint32_t hashElapsedMs = 0;

// ---- Mode 4: 荒谬穷举 (Scheme B) ----
static int curKeyspaceIdx = 0;

static volatile bool crackRunning = false;
static volatile uint32_t crackAttempts = 0;
static volatile bool crackSuccess = false;
static char crackCurrentGuess[16] = "0000";
// 重置 / 切档时 +1。worker 每轮开头快照一次，算完发现变了就整轮作废——
// 否则在途那一轮会把计数从 0 顶成 1，还拿旧档的猜测去比新档的答案。
static volatile uint32_t crackGen = 0;
static uint32_t crackStartMs = 0;

// ---- FreeRTOS 后台 Core 0 任务句柄 ----
static TaskHandle_t volatile ovenTaskHandle = nullptr;
static volatile bool taskKeepAlive = true;
static volatile bool taskCancelReq = false;

// 更新对称密码计算结果
static void updateCipherCalc() {
  const char* text = TEXT_PRESETS[textPresetIdx];
  const char* key  = KEY_PRESETS[keyPresetIdx];
  cipherSuccess = true;

  if (!cipherDecrypt) {
    // 加密
    if (cipherAlgo == CIPHER_AES128) {
      cipherSuccess = crypto::aes128_cbc_encrypt(key, text, cipherResult, sizeof(cipherResult));
    } else if (cipherAlgo == CIPHER_RC4) {
      cipherSuccess = crypto::rc4_encrypt(key, text, cipherResult, sizeof(cipherResult));
    } else {
      cipherSuccess = crypto::xor_encrypt(key, text, cipherResult, sizeof(cipherResult));
    }
  } else {
    // 解密（先取加密密文，再使用当前 key 尝试还原）
    char tempHex[192];
    if (cipherAlgo == CIPHER_AES128) {
      crypto::aes128_cbc_encrypt(key, text, tempHex, sizeof(tempHex));
      cipherSuccess = crypto::aes128_cbc_decrypt(key, tempHex, cipherResult, sizeof(cipherResult));
    } else if (cipherAlgo == CIPHER_RC4) {
      crypto::rc4_encrypt(key, text, tempHex, sizeof(tempHex));
      cipherSuccess = crypto::rc4_decrypt(key, tempHex, cipherResult, sizeof(cipherResult));
    } else {
      crypto::xor_encrypt(key, text, tempHex, sizeof(tempHex));
      cipherSuccess = crypto::xor_decrypt(key, tempHex, cipherResult, sizeof(cipherResult));
    }
  }
  if (!cipherSuccess) {
    snprintf(cipherResult, sizeof(cipherResult), "[ERROR: BAD PADDING / KEY]");
  }
}

// 更新古典密码计算结果
static void updateClassicCalc() {
  const char* text = TEXT_PRESETS[textPresetIdx];
  const char* key  = KEY_PRESETS[keyPresetIdx];

  if (classicAlgo == CLASSIC_CAESAR) {
    crypto::caesar_shift(text, caesarShift, classicResult, sizeof(classicResult));
  } else if (classicAlgo == CLASSIC_VIGENERE) {
    if (!classicDecrypt) crypto::vigenere_encrypt(key, text, classicResult, sizeof(classicResult));
    else                 crypto::vigenere_decrypt(key, text, classicResult, sizeof(classicResult));
  } else if (classicAlgo == CLASSIC_BASE64) {
    if (!classicDecrypt) {
      crypto::base64_encode(text, classicResult, sizeof(classicResult));
    } else {
      char enc[192];
      crypto::base64_encode(text, enc, sizeof(enc));
      crypto::base64_decode(enc, classicResult, sizeof(classicResult));
    }
  } else if (classicAlgo == CLASSIC_HEX) {
    if (!classicDecrypt) {
      crypto::hex_encode(text, classicResult, sizeof(classicResult));
    } else {
      char enc[192];
      crypto::hex_encode(text, enc, sizeof(enc));
      crypto::hex_decode(enc, classicResult, sizeof(classicResult));
    }
  }
}

// Core 0 工作线程
static void ovenWorkerTask(void* param) {
  char hashBuf[128];
  uint32_t lastReportMs = millis();
  uint32_t batchCount = 0;

  while (taskKeepAlive) {
    if (baking && curMode == MODE_OVEN) {
      uint32_t t0 = millis();
      sha512_crypt_calc("bake_stress_benchmark", "salt1234", 5000, hashBuf, sizeof(hashBuf), nullptr, &taskCancelReq);
      uint32_t dur = millis() - t0;
      if (!taskKeepAlive || taskCancelReq) break;
      lastHashDurationMs = dur;
      totalHashes++;
      batchCount++;

      uint32_t now = millis();
      if (now - lastReportMs >= 500) {
        liveHps = (float)batchCount * 1000.0f / (float)(now - lastReportMs);
        lastReportMs = now;
        batchCount = 0;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    } else if (hashRunning && curMode == MODE_HASH) {
      hashProgressRounds = 0;
      uint32_t t0 = millis();
      const char* pass = TEXT_PRESETS[textPresetIdx];
      const char* salt = SALT_PRESETS[saltPresetIdx];

      if (hashAlgo == HASH_SHA512_CRYPT) {
        sha512_crypt_calc(pass, salt, 5000, hashResult, sizeof(hashResult), &hashProgressRounds, &taskCancelReq);
      } else if (hashAlgo == HASH_MD5_CRYPT) {
        crypto::md5_crypt_calc(pass, salt, hashResult, sizeof(hashResult));
        hashProgressRounds = 1000;
      } else if (hashAlgo == HASH_SHA256) {
        crypto::sha256_hex(pass, hashResult);
        hashProgressRounds = 1;
      } else if (hashAlgo == HASH_MD5) {
        crypto::md5_hex(pass, hashResult);
        hashProgressRounds = 1;
      }
      hashElapsedMs = millis() - t0;
      hashRunning = false;
      vTaskDelay(pdMS_TO_TICKS(2));
    } else if (crackRunning && curMode == MODE_CRACK) {
      uint32_t gen = crackGen;
      int ks = curKeyspaceIdx;
      uint32_t idx = crackAttempts;
      char guess[sizeof(crackCurrentGuess)];
      generateCrackGuess(ks, idx, guess, sizeof(guess));
      memcpy(crackCurrentGuess, guess, sizeof(guess));

      sha512_crypt_calc(guess, "card", 5000, hashBuf, sizeof(hashBuf), nullptr, &taskCancelReq);
      if (!taskKeepAlive || taskCancelReq) break;

      if (gen == crackGen) {
        crackAttempts = idx + 1;
        if (strcmp(guess, KEYSPACES[ks].solution) == 0) {
          crackSuccess = true;
          crackRunning = false;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    } else {
      vTaskDelay(pdMS_TO_TICKS(40));
    }
  }

  ovenTaskHandle = nullptr;
  vTaskDelete(NULL);
}

} // namespace

// 上一次退出时 worker 可能还没来得及结束（正卡在一轮 SHA-512-crypt 里）；
// 它还在循环里就会被 taskKeepAlive 救回来继续用，已经退出了就补建一个。
static void ensureOvenWorker() {
  if (!ovenTaskHandle && taskKeepAlive) {
    xTaskCreatePinnedToCore(ovenWorkerTask, "hash_oven", 10240, nullptr, 1, const_cast<TaskHandle_t*>(&ovenTaskHandle), 0);
  }
}

void hashOvenEnter() {
  taskKeepAlive = true;
  taskCancelReq = false;
  updateCipherCalc();
  updateClassicCalc();
  generateCrackGuess(curKeyspaceIdx, crackAttempts, crackCurrentGuess, sizeof(crackCurrentGuess));
  ensureOvenWorker();
}

void hashOvenExit() {
  baking = false;
  hashRunning = false;
  crackRunning = false;
  taskCancelReq = true;
  taskKeepAlive = false;

  // 只等不删：sha512_crypt_calc 会看 taskCancelReq 尽快返回，worker 随后自己 vTaskDelete。
  // 强删会跟它自己的退出撞车（删一个已经删掉的任务）。
  uint32_t t0 = millis();
  while (ovenTaskHandle && (millis() - t0 < 800)) {
    delay(10);
  }
}

void hashOvenUpdate() {
  ensureOvenWorker();
  if (baking && dualCore && curMode == MODE_OVEN && !taskCancelReq) {
    char dummy[128];
    sha512_crypt_calc("core1_turbo", "salt1234", 5000, dummy, sizeof(dummy), nullptr, &taskCancelReq);
    if (!taskCancelReq) totalHashes++;
  }
  dirty = true;
}

bool hashOvenKey(char k) {
  if (k == '`') return true;

  if (k == 'm' || k == 'M') {
    baking = false;
    hashRunning = false;
    crackRunning = false;
    curMode = (OvenMode)((curMode + 1) % MODE__MAX);
    if (curMode == MODE_CIPHER) updateCipherCalc();
    if (curMode == MODE_CLASSIC) updateClassicCalc();
    if (curMode == MODE_CRACK) generateCrackGuess(curKeyspaceIdx, crackAttempts, crackCurrentGuess, sizeof(crackCurrentGuess));
    dirty = true;
    return false;
  }

  if (curMode == MODE_OVEN) {
    if (k == '\r' || k == '\n') {
      baking = !baking;
      if (baking) bakeStartMs = millis();
      dirty = true;
    } else if (k == 'd' || k == 'D') {
      dualCore = !dualCore;
      dirty = true;
    } else if (k == 'r' || k == 'R') {
      totalHashes = 0;
      liveHps = 0.0f;
      dirty = true;
    }
  } else if (curMode == MODE_CIPHER) {
    if (k == 'a' || k == 'A') {
      cipherAlgo = (CipherAlgo)((cipherAlgo + 1) % CIPHER__MAX);
      updateCipherCalc();
      dirty = true;
    } else if (k == 't' || k == 'T' || k == '\t') {
      cipherDecrypt = !cipherDecrypt;
      updateCipherCalc();
      dirty = true;
    } else if (k == ';' || k == '.') {
      textPresetIdx = (k == ';') ? (textPresetIdx + TEXT_PRESET_COUNT - 1) % TEXT_PRESET_COUNT
                                 : (textPresetIdx + 1) % TEXT_PRESET_COUNT;
      updateCipherCalc();
      dirty = true;
    } else if (k == ',' || k == '/') {
      keyPresetIdx = (k == ',') ? (keyPresetIdx + KEY_PRESET_COUNT - 1) % KEY_PRESET_COUNT
                                : (keyPresetIdx + 1) % KEY_PRESET_COUNT;
      updateCipherCalc();
      dirty = true;
    } else if (k == 's' || k == 'S') {
      Serial.printf("[CIPHER] Algo:%d Dec:%d Key:%s Res:%s\n",
                    cipherAlgo, cipherDecrypt, KEY_PRESETS[keyPresetIdx], cipherResult);
    }
  } else if (curMode == MODE_CLASSIC) {
    if (k == 'a' || k == 'A') {
      classicAlgo = (ClassicAlgo)((classicAlgo + 1) % CLASSIC__MAX);
      updateClassicCalc();
      dirty = true;
    } else if (k == 't' || k == 'T' || k == '\t') {
      classicDecrypt = !classicDecrypt;
      updateClassicCalc();
      dirty = true;
    } else if (k == '[' || k == ']') {
      if (classicAlgo == CLASSIC_CAESAR) {
        caesarShift = (k == '[') ? (caesarShift + 25) % 26 : (caesarShift + 1) % 26;
        updateClassicCalc();
        dirty = true;
      }
    } else if (k == ';' || k == '.') {
      textPresetIdx = (k == ';') ? (textPresetIdx + TEXT_PRESET_COUNT - 1) % TEXT_PRESET_COUNT
                                 : (textPresetIdx + 1) % TEXT_PRESET_COUNT;
      updateClassicCalc();
      dirty = true;
    } else if (k == ',' || k == '/') {
      keyPresetIdx = (k == ',') ? (keyPresetIdx + KEY_PRESET_COUNT - 1) % KEY_PRESET_COUNT
                                : (keyPresetIdx + 1) % KEY_PRESET_COUNT;
      updateClassicCalc();
      dirty = true;
    } else if (k == 's' || k == 'S') {
      Serial.printf("[CLASSIC] Algo:%d Res:%s\n", classicAlgo, classicResult);
    }
  } else if (curMode == MODE_HASH) {
    if (k == 'a' || k == 'A') {
      hashAlgo = (HashAlgo)((hashAlgo + 1) % HASH__MAX);
      hashResult[0] = '\0';
      dirty = true;
    } else if (k == ';' || k == '.') {
      textPresetIdx = (k == ';') ? (textPresetIdx + TEXT_PRESET_COUNT - 1) % TEXT_PRESET_COUNT
                                 : (textPresetIdx + 1) % TEXT_PRESET_COUNT;
      hashResult[0] = '\0';
      dirty = true;
    } else if (k == ',' || k == '/') {
      saltPresetIdx = (k == ',') ? (saltPresetIdx + SALT_PRESET_COUNT - 1) % SALT_PRESET_COUNT
                                 : (saltPresetIdx + 1) % SALT_PRESET_COUNT;
      hashResult[0] = '\0';
      dirty = true;
    } else if (k == '\r' || k == '\n') {
      if (!hashRunning) {
        hashRunning = true;
        dirty = true;
      }
    } else if (k == 's' || k == 'S') {
      if (hashResult[0]) {
        Serial.printf("[HASH] Algo:%d Output:%s\n", hashAlgo, hashResult);
      }
    }
  } else if (curMode == MODE_CRACK) {
    if (k == '[' || k == ']') {
      if (!crackRunning) {
        crackGen = crackGen + 1;
        curKeyspaceIdx = (k == '[') ? (curKeyspaceIdx + KEYSPACE_COUNT - 1) % KEYSPACE_COUNT
                                    : (curKeyspaceIdx + 1) % KEYSPACE_COUNT;
        crackAttempts = 0;
        crackSuccess = false;
        generateCrackGuess(curKeyspaceIdx, 0, crackCurrentGuess, sizeof(crackCurrentGuess));
        dirty = true;
      }
    } else if (k == '\r' || k == '\n') {
      crackRunning = !crackRunning;
      if (crackRunning) {
        crackSuccess = false;
        crackStartMs = millis();
      }
      dirty = true;
    } else if (k == 'r' || k == 'R') {
      crackGen = crackGen + 1;
      crackAttempts = 0;
      crackSuccess = false;
      crackRunning = false;
      generateCrackGuess(curKeyspaceIdx, 0, crackCurrentGuess, sizeof(crackCurrentGuess));
      dirty = true;
    }
  }
  return false;
}

void drawHashOven() {
  cv.fillScreen(TFT_BLACK);

  // 顶栏 5 个大子模块
  const char* modeNames[] = {
    "1/5 BENCH OVEN", "2/5 SYMMETRIC CIPHER", "3/5 CLASSIC & CTF", "4/5 HASH & SHADOW", "5/5 FUTILITY CRACK"
  };
  uint16_t modeColors[] = {
    TFT_ORANGE, TFT_GREEN, TFT_CYAN, TFT_YELLOW, TFT_MAGENTA
  };
  drawPageHeader("CRYPTO SUITE", modeNames[curMode], modeColors[curMode]);

  int y = PAGE_HDR_BOTTOM + 3;

  if (curMode == MODE_OVEN) {
    // Mode 0: 赛博烤炉 HUD
    char bufHps[24], bufTotal[32], bufLatency[24];
    snprintf(bufHps, sizeof(bufHps), "%.1f H/s", (double)liveHps);
    snprintf(bufTotal, sizeof(bufTotal), "COUNT: %lu", (unsigned long)totalHashes);
    snprintf(bufLatency, sizeof(bufLatency), "LAT: %lums", (unsigned long)lastHashDurationMs);

    cv.setTextColor(baking ? TFT_GREEN : ICON_DIM, TFT_BLACK);
    cv.setTextSize(2);
    cv.setTextDatum(top_left);
    cv.drawString(bufHps, 6, y);

    cv.setTextSize(1);
    cv.setTextDatum(top_right);
    if (baking) {
      uint16_t badgeCol = dualCore ? TFT_RED : TFT_ORANGE;
      cv.setTextColor(badgeCol, TFT_BLACK);
      cv.drawString(dualCore ? "[DUAL TURBO]" : "[CORE0 ROAST]", SW - 6, y + 2);
    } else {
      cv.setTextColor(ICON_DIM, TFT_BLACK);
      cv.drawString("[STANDBY]", SW - 6, y + 2);
    }

    int ovenY = y + 20;
    cv.drawRoundRect(6, ovenY, SW - 12, 16, 3, DIM_BORDER);
    int barW = SW - 16;
    if (baking) {
      uint32_t t = millis() / 150;
      for (int i = 0; i < 8; ++i) {
        uint16_t filColor = ((i + t) % 3 == 0) ? TFT_RED : (((i + t) % 3 == 1) ? TFT_ORANGE : TFT_YELLOW);
        int fx = 10 + i * (barW / 8);
        cv.drawLine(fx, ovenY + 3, fx + 8, ovenY + 12, filColor);
        cv.drawLine(fx + 8, ovenY + 12, fx + 16, ovenY + 3, filColor);
      }
    } else {
      cv.setTextDatum(middle_center);
      cv.setTextColor(ICON_DIM, TFT_BLACK);
      cv.drawString("OVEN COLD - PRESS ENTER TO BAKE", SW / 2, ovenY + 8);
    }

    int infoY = ovenY + 22;
    cv.setTextDatum(top_left);
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.drawString(bufTotal, 6, infoY);

    cv.setTextDatum(top_right);
    cv.setTextColor(TFT_CYAN, TFT_BLACK);
    cv.drawString(bufLatency, SW - 6, infoY);

    int bmv = powerBatteryMv();
    char pwrBuf[48];
    snprintf(pwrBuf, sizeof(pwrBuf), "BAT: %dmV | DRAIN: ~%dmA",
             bmv > 0 ? bmv : 0, baking ? (dualCore ? 210 : 165) : 85);
    cv.setTextDatum(top_left);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString(pwrBuf, 6, infoY + 13);

    cv.setTextDatum(bottom_left);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("Enter:Bake d:Dual r:Clr m:Mode", 4, SH - 2);

  } else if (curMode == MODE_CIPHER) {
    // Mode 1: 对称加解密 (AES-128 / RC4 / XOR)
    cv.setTextDatum(top_left); cv.setTextSize(1);

    const char* cAlgoNames[] = { "AES-128-CBC", "RC4 (ARC4)", "XOR (OTP)" };
    char hBuf[64];
    snprintf(hBuf, sizeof(hBuf), "ALGO (a): %s", cAlgoNames[cipherAlgo]);
    cv.setTextColor(TFT_GREEN, TFT_BLACK);
    cv.drawString(hBuf, 6, y);

    cv.setTextDatum(top_right);
    uint16_t dirCol = cipherDecrypt ? TFT_ORANGE : TFT_CYAN;
    cv.setTextColor(dirCol, TFT_BLACK);
    cv.drawString(cipherDecrypt ? "[DIR: DECRYPT]" : "[DIR: ENCRYPT]", SW - 6, y);

    cv.setTextDatum(top_left);
    char keyBuf[64], textBuf[64];
    snprintf(keyBuf, sizeof(keyBuf), "KEY (,//): \"%s\"", KEY_PRESETS[keyPresetIdx]);
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString(keyBuf, 6, y + 12);

    snprintf(textBuf, sizeof(textBuf), "TXT (;/.): \"%s\"", TEXT_PRESETS[textPresetIdx]);
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.drawString(trunc(textBuf, 36), 6, y + 23);

    // 结果卡片
    int outY = y + 36;
    cv.drawRoundRect(6, outY, SW - 12, 44, 3, DIM_BORDER);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString(cipherDecrypt ? "DECRYPTED PLAIN TEXT:" : "CIPHER TEXT (HEX ENCODED):", 10, outY + 4);

    cv.setTextColor(cipherSuccess ? (cipherDecrypt ? TFT_GREEN : TFT_CYAN) : TFT_RED, TFT_BLACK);
    char l1[36], l2[36];
    snprintf(l1, sizeof(l1), "%.34s", cipherResult);
    snprintf(l2, sizeof(l2), "%.34s", (strlen(cipherResult) > 34) ? cipherResult + 34 : "");
    cv.drawString(l1, 10, outY + 16);
    if (l2[0]) cv.drawString(l2, 10, outY + 28);

    cv.setTextDatum(bottom_left);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("a:Algo t:Dir ;/.Txt ,//Key m:Mode", 4, SH - 2);

  } else if (curMode == MODE_CLASSIC) {
    // Mode 2: 古典与CTF密码
    cv.setTextDatum(top_left); cv.setTextSize(1);

    const char* clNames[] = { "Caesar", "Vigenere", "Base64", "Hex" };
    char hBuf[64];
    snprintf(hBuf, sizeof(hBuf), "TOOL (a): %s", clNames[classicAlgo]);
    cv.setTextColor(TFT_CYAN, TFT_BLACK);
    cv.drawString(hBuf, 6, y);

    cv.setTextDatum(top_right);
    if (classicAlgo == CLASSIC_CAESAR) {
      char cTag[24];
      snprintf(cTag, sizeof(cTag), "[SHIFT: +%d%s]", caesarShift, (caesarShift == 13 ? " ROT13" : ""));
      cv.setTextColor(TFT_YELLOW, TFT_BLACK);
      cv.drawString(cTag, SW - 6, y);
    } else {
      cv.setTextColor(classicDecrypt ? TFT_ORANGE : TFT_GREEN, TFT_BLACK);
      cv.drawString(classicDecrypt ? "[DECODE]" : "[ENCODE]", SW - 6, y);
    }

    cv.setTextDatum(top_left);
    if (classicAlgo == CLASSIC_VIGENERE) {
      char keyBuf[48];
      snprintf(keyBuf, sizeof(keyBuf), "KEY (,//): \"%s\"", KEY_PRESETS[keyPresetIdx]);
      cv.setTextColor(TFT_YELLOW, TFT_BLACK);
      cv.drawString(keyBuf, 6, y + 12);
    } else if (classicAlgo == CLASSIC_CAESAR) {
      cv.setTextColor(ICON_DIM, TFT_BLACK);
      cv.drawString("Press [ / ] to shift alphabet 1..25", 6, y + 12);
    } else {
      cv.setTextColor(ICON_DIM, TFT_BLACK);
      cv.drawString("Instant bidirectional encoder/decoder", 6, y + 12);
    }

    char inBuf[64];
    snprintf(inBuf, sizeof(inBuf), "INPUT (;/.): \"%s\"", TEXT_PRESETS[textPresetIdx]);
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.drawString(trunc(inBuf, 36), 6, y + 23);

    int outY = y + 36;
    cv.drawRoundRect(6, outY, SW - 12, 44, 3, DIM_BORDER);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("RESULT:", 10, outY + 4);

    cv.setTextColor(TFT_GREEN, TFT_BLACK);
    char l1[36], l2[36];
    snprintf(l1, sizeof(l1), "%.34s", classicResult);
    snprintf(l2, sizeof(l2), "%.34s", (strlen(classicResult) > 34) ? classicResult + 34 : "");
    cv.drawString(l1, 10, outY + 16);
    if (l2[0]) cv.drawString(l2, 10, outY + 28);

    cv.setTextDatum(bottom_left);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    if (classicAlgo == CLASSIC_CAESAR) {
      cv.drawString("a:Algo [ ]:Shift ;/.Txt m:Mode", 4, SH - 2);
    } else {
      cv.drawString("a:Algo t:Dir ;/.Txt ,//Key m:Mode", 4, SH - 2);
    }

  } else if (curMode == MODE_HASH) {
    // Mode 3: 哈希与 Shadow
    cv.setTextDatum(top_left); cv.setTextSize(1);

    const char* hNames[] = { "$6$ SHA-512-crypt", "$1$ MD5-crypt", "SHA-256 Digest", "MD5 Digest" };
    char hBuf[64];
    snprintf(hBuf, sizeof(hBuf), "ALGO (a): %s", hNames[hashAlgo]);
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString(hBuf, 6, y);

    char selBuf[64];
    snprintf(selBuf, sizeof(selBuf), "PASS (;/.): %s", TEXT_PRESETS[textPresetIdx]);
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.drawString(selBuf, 6, y + 12);

    if (hashAlgo == HASH_SHA512_CRYPT || hashAlgo == HASH_MD5_CRYPT) {
      char saltBuf[64];
      snprintf(saltBuf, sizeof(saltBuf), "SALT (,//): %s (%s rounds)",
               SALT_PRESETS[saltPresetIdx], (hashAlgo == HASH_SHA512_CRYPT ? "5000" : "1000"));
      cv.setTextColor(TFT_CYAN, TFT_BLACK);
      cv.drawString(saltBuf, 6, y + 23);
    } else {
      cv.setTextColor(ICON_DIM, TFT_BLACK);
      cv.drawString("Standard unkeyed cryptographic hash", 6, y + 23);
    }

    int progY = y + 36;
    if (hashAlgo == HASH_SHA512_CRYPT || hashAlgo == HASH_MD5_CRYPT) {
      int maxRounds = (hashAlgo == HASH_SHA512_CRYPT) ? 5000 : 1000;
      cv.drawRoundRect(6, progY, SW - 12, 8, 2, DIM_BORDER);
      int pWidth = (SW - 14) * hashProgressRounds / maxRounds;
      if (pWidth > SW - 14) pWidth = SW - 14;
      if (pWidth > 0) cv.fillRect(7, progY + 1, pWidth, 6, TFT_GREEN);
    }

    int outY = progY + 12;
    if (hashResult[0]) {
      cv.setTextColor(TFT_GREEN, TFT_BLACK);
      char l1[36], l2[36], l3[36];
      snprintf(l1, sizeof(l1), "%.34s", hashResult);
      snprintf(l2, sizeof(l2), "%.34s", (strlen(hashResult) > 34) ? hashResult + 34 : "");
      snprintf(l3, sizeof(l3), "%.34s", (strlen(hashResult) > 68) ? hashResult + 68 : "");
      cv.drawString(l1, 6, outY);
      if (l2[0]) cv.drawString(l2, 6, outY + 10);
      if (l3[0]) cv.drawString(l3, 6, outY + 20);

      char timeBuf[24];
      snprintf(timeBuf, sizeof(timeBuf), "%lums", (unsigned long)hashElapsedMs);
      cv.setTextDatum(top_right);
      cv.setTextColor(TFT_ORANGE, TFT_BLACK);
      cv.drawString(timeBuf, SW - 6, y);
    } else if (hashRunning) {
      cv.setTextColor(TFT_YELLOW, TFT_BLACK);
      cv.drawString("CALCULATING HASH...", 6, outY + 4);
    } else {
      cv.setTextColor(ICON_DIM, TFT_BLACK);
      cv.drawString("Press Enter to compute hash", 6, outY + 4);
    }

    cv.setTextDatum(bottom_left);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("Enter:Calc a:Algo ;/.Pass s:Ser m:Mode", 4, SH - 2);

  } else if (curMode == MODE_CRACK) {
    // Mode 4: 荒谬穷举挑战 (Futility Cracker)
    cv.setTextDatum(top_left); cv.setTextSize(1);

    const KeyspacePreset& kp = KEYSPACES[curKeyspaceIdx];

    char ksBuf[48];
    snprintf(ksBuf, sizeof(ksBuf), "KEYSPACE [[]]: %s", kp.name);
    cv.setTextColor(TFT_MAGENTA, TFT_BLACK);
    cv.drawString(ksBuf, 6, y);

    char etaBuf[48];
    snprintf(etaBuf, sizeof(etaBuf), "ETA: %s (%llu combos)", kp.etaText, (unsigned long long)kp.total);
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString(etaBuf, 6, y + 12);

    int boxY = y + 26;
    cv.drawRoundRect(6, boxY, SW - 12, 42, 3, DIM_BORDER);

    if (crackSuccess) {
      cv.setTextDatum(middle_center);
      cv.setTextColor(TFT_GREEN, TFT_BLACK);
      cv.setTextSize(2);
      cv.drawString("TARGET PWNED!", SW / 2, boxY + 14);
      cv.setTextSize(1);
      char winBuf[48];
      snprintf(winBuf, sizeof(winBuf), "PASSWORD: \"%s\" IN %lu ATTEMPTS",
               kp.solution, (unsigned long)crackAttempts);
      cv.setTextColor(TFT_WHITE, TFT_BLACK);
      cv.drawString(winBuf, SW / 2, boxY + 30);
    } else {
      cv.setTextDatum(top_left);
      cv.setTextColor(TFT_WHITE, TFT_BLACK);
      char tryBuf[32];
      snprintf(tryBuf, sizeof(tryBuf), "TRYING: \"%s\"", crackCurrentGuess);
      cv.drawString(tryBuf, 12, boxY + 4);

      char attBuf[32];
      snprintf(attBuf, sizeof(attBuf), "TESTED: %lu", (unsigned long)crackAttempts);
      cv.setTextColor(TFT_CYAN, TFT_BLACK);
      cv.drawString(attBuf, 12, boxY + 16);

      float pct = (kp.total > 0) ? ((float)crackAttempts * 100.0f / (float)kp.total) : 0.0f;
      char pctBuf[32];
      snprintf(pctBuf, sizeof(pctBuf), "PROGRESS: %.4f%%", pct);
      cv.setTextColor(TFT_ORANGE, TFT_BLACK);
      cv.drawString(pctBuf, 12, boxY + 28);
    }

    cv.setTextDatum(bottom_left);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("Enter:Start/Stop r:Reset m:Mode", 4, SH - 2);
  }
}
