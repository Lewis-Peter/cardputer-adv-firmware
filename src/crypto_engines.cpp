#include "crypto_engines.h"
#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <cstring>
#include <cstdio>
#include <cctype>

namespace crypto {

namespace {

static const char HEX_CHARS[] = "0123456789abcdef";

static inline int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 固定 IV（16 字节），保证对同一个 key + plaintext 的 CBC 输出确定且可逆
static const unsigned char FIXED_IV[16] = {
  'c', 'a', 'r', 'd', 'p', 'u', 't', 'e',
  'r', '_', 'c', 'b', 'c', '_', 'i', 'v'
};

// 通过 MD5 将任意长度的字符串 Key 散列为 16 字节（128 位）AES 密钥
static void derive128Key(const char* key, unsigned char outKey[16]) {
  size_t len = strlen(key);
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts_ret(&ctx);
  mbedtls_md5_update_ret(&ctx, (const unsigned char*)key, len);
  mbedtls_md5_finish_ret(&ctx, outKey);
  mbedtls_md5_free(&ctx);
}

} // namespace

// ---- AES-128-CBC 加密 (带 PKCS#7 Padding) ----
bool aes128_cbc_encrypt(const char* key, const char* plaintext, char* outHex, size_t outHexMax) {
  if (!key || !plaintext || !outHex) return false;
  size_t plainLen = strlen(plaintext);
  if (plainLen > 128) plainLen = 128; // 防护

  size_t padLen = 16 - (plainLen % 16);
  size_t totalLen = plainLen + padLen;
  if (outHexMax < totalLen * 2 + 1) return false;

  unsigned char buf[144];
  memcpy(buf, plaintext, plainLen);
  memset(buf + plainLen, (unsigned char)padLen, padLen);

  unsigned char key128[16];
  derive128Key(key, key128);

  unsigned char iv[16];
  memcpy(iv, FIXED_IV, 16);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key128, 128);

  unsigned char cipher[144];
  int ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, totalLen, iv, buf, cipher);
  mbedtls_aes_free(&aes);
  if (ret != 0) return false;

  for (size_t i = 0; i < totalLen; i++) {
    outHex[i * 2]     = HEX_CHARS[(cipher[i] >> 4) & 0x0F];
    outHex[i * 2 + 1] = HEX_CHARS[cipher[i] & 0x0F];
  }
  outHex[totalLen * 2] = '\0';
  return true;
}

// ---- AES-128-CBC 解密 (剥离 PKCS#7 Padding) ----
bool aes128_cbc_decrypt(const char* key, const char* hexCipher, char* outPlain, size_t outPlainMax) {
  if (!key || !hexCipher || !outPlain || outPlainMax < 1) return false;
  size_t hexLen = strlen(hexCipher);
  if (hexLen % 32 != 0 || hexLen == 0 || hexLen > 288) return false;

  size_t cipherLen = hexLen / 2;
  unsigned char cipher[144];
  for (size_t i = 0; i < cipherLen; i++) {
    int h1 = hexVal(hexCipher[i * 2]);
    int h2 = hexVal(hexCipher[i * 2 + 1]);
    if (h1 < 0 || h2 < 0) return false;
    cipher[i] = (unsigned char)((h1 << 4) | h2);
  }

  unsigned char key128[16];
  derive128Key(key, key128);

  unsigned char iv[16];
  memcpy(iv, FIXED_IV, 16);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, key128, 128);

  unsigned char plain[144];
  int ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipherLen, iv, cipher, plain);
  mbedtls_aes_free(&aes);
  if (ret != 0) return false;

  unsigned char pad = plain[cipherLen - 1];
  if (pad == 0 || pad > 16 || pad > cipherLen) return false;
  for (size_t i = cipherLen - pad; i < cipherLen; i++) {
    if (plain[i] != pad) return false;
  }

  size_t validLen = cipherLen - pad;
  if (validLen >= outPlainMax) validLen = outPlainMax - 1;
  memcpy(outPlain, plain, validLen);
  outPlain[validLen] = '\0';
  return true;
}

// ---- RC4 (ARC4) 流密码 ----
static void rc4_process(const unsigned char* key, size_t keyLen,
                        const unsigned char* in, unsigned char* out, size_t len) {
  unsigned char s[256];
  for (int i = 0; i < 256; i++) s[i] = (unsigned char)i;
  int j = 0;
  for (int i = 0; i < 256; i++) {
    j = (j + s[i] + key[i % keyLen]) & 255;
    unsigned char tmp = s[i]; s[i] = s[j]; s[j] = tmp;
  }
  int i = 0; j = 0;
  for (size_t k = 0; k < len; k++) {
    i = (i + 1) & 255;
    j = (j + s[i]) & 255;
    unsigned char tmp = s[i]; s[i] = s[j]; s[j] = tmp;
    out[k] = in[k] ^ s[(s[i] + s[j]) & 255];
  }
}

bool rc4_encrypt(const char* key, const char* plaintext, char* outHex, size_t outHexMax) {
  if (!key || !plaintext || !outHex) return false;
  size_t kLen = strlen(key);
  if (kLen == 0) return false;
  size_t pLen = strlen(plaintext);
  if (pLen > 128) pLen = 128;
  if (outHexMax < pLen * 2 + 1) return false;

  unsigned char outBytes[144];
  rc4_process((const unsigned char*)key, kLen, (const unsigned char*)plaintext, outBytes, pLen);

  for (size_t i = 0; i < pLen; i++) {
    outHex[i * 2]     = HEX_CHARS[(outBytes[i] >> 4) & 0x0F];
    outHex[i * 2 + 1] = HEX_CHARS[outBytes[i] & 0x0F];
  }
  outHex[pLen * 2] = '\0';
  return true;
}

bool rc4_decrypt(const char* key, const char* hexCipher, char* outPlain, size_t outPlainMax) {
  if (!key || !hexCipher || !outPlain || outPlainMax < 1) return false;
  size_t kLen = strlen(key);
  if (kLen == 0) return false;
  size_t hexLen = strlen(hexCipher);
  if (hexLen % 2 != 0 || hexLen == 0) return false;

  size_t byteLen = hexLen / 2;
  if (byteLen > 128) byteLen = 128;
  unsigned char inBytes[144];
  for (size_t i = 0; i < byteLen; i++) {
    int h1 = hexVal(hexCipher[i * 2]);
    int h2 = hexVal(hexCipher[i * 2 + 1]);
    if (h1 < 0 || h2 < 0) return false;
    inBytes[i] = (unsigned char)((h1 << 4) | h2);
  }

  unsigned char plainBytes[144];
  rc4_process((const unsigned char*)key, kLen, inBytes, plainBytes, byteLen);

  size_t copyLen = (byteLen < outPlainMax - 1) ? byteLen : outPlainMax - 1;
  memcpy(outPlain, plainBytes, copyLen);
  outPlain[copyLen] = '\0';
  return true;
}

// ---- XOR (Repeating Key) ----
bool xor_encrypt(const char* key, const char* plaintext, char* outHex, size_t outHexMax) {
  if (!key || !plaintext || !outHex) return false;
  size_t kLen = strlen(key);
  if (kLen == 0) return false;
  size_t pLen = strlen(plaintext);
  if (pLen > 128) pLen = 128;
  if (outHexMax < pLen * 2 + 1) return false;

  for (size_t i = 0; i < pLen; i++) {
    unsigned char b = (unsigned char)(plaintext[i] ^ key[i % kLen]);
    outHex[i * 2]     = HEX_CHARS[(b >> 4) & 0x0F];
    outHex[i * 2 + 1] = HEX_CHARS[b & 0x0F];
  }
  outHex[pLen * 2] = '\0';
  return true;
}

bool xor_decrypt(const char* key, const char* hexCipher, char* outPlain, size_t outPlainMax) {
  if (!key || !hexCipher || !outPlain || outPlainMax < 1) return false;
  size_t kLen = strlen(key);
  if (kLen == 0) return false;
  size_t hexLen = strlen(hexCipher);
  if (hexLen % 2 != 0 || hexLen == 0) return false;

  size_t byteLen = hexLen / 2;
  if (byteLen >= outPlainMax) byteLen = outPlainMax - 1;

  for (size_t i = 0; i < byteLen; i++) {
    int h1 = hexVal(hexCipher[i * 2]);
    int h2 = hexVal(hexCipher[i * 2 + 1]);
    if (h1 < 0 || h2 < 0) return false;
    unsigned char b = (unsigned char)((h1 << 4) | h2);
    outPlain[i] = (char)(b ^ key[i % kLen]);
  }
  outPlain[byteLen] = '\0';
  return true;
}

// ---- 凯撒密码 (Caesar / ROT13) ----
void caesar_shift(const char* inStr, int shift, char* outStr, size_t outMax) {
  if (!inStr || !outStr || outMax < 1) return;
  shift = ((shift % 26) + 26) % 26;
  size_t i = 0;
  for (; i < outMax - 1 && inStr[i]; i++) {
    char c = inStr[i];
    if (c >= 'a' && c <= 'z') {
      outStr[i] = (char)('a' + (c - 'a' + shift) % 26);
    } else if (c >= 'A' && c <= 'Z') {
      outStr[i] = (char)('A' + (c - 'A' + shift) % 26);
    } else {
      outStr[i] = c;
    }
  }
  outStr[i] = '\0';
}

// ---- 维吉尼亚密码 (Vigenère) ----
void vigenere_encrypt(const char* key, const char* plaintext, char* outStr, size_t outMax) {
  if (!key || !plaintext || !outStr || outMax < 1) return;
  size_t kLen = strlen(key);
  if (kLen == 0) {
    snprintf(outStr, outMax, "%s", plaintext);
    return;
  }
  size_t ki = 0;
  size_t i = 0;
  for (; i < outMax - 1 && plaintext[i]; i++) {
    char c = plaintext[i];
    char k = tolower((unsigned char)key[ki % kLen]);
    int shift = (k >= 'a' && k <= 'z') ? (k - 'a') : 0;
    if (c >= 'a' && c <= 'z') {
      outStr[i] = (char)('a' + (c - 'a' + shift) % 26);
      ki++;
    } else if (c >= 'A' && c <= 'Z') {
      outStr[i] = (char)('A' + (c - 'A' + shift) % 26);
      ki++;
    } else {
      outStr[i] = c;
    }
  }
  outStr[i] = '\0';
}

void vigenere_decrypt(const char* key, const char* ciphertext, char* outStr, size_t outMax) {
  if (!key || !ciphertext || !outStr || outMax < 1) return;
  size_t kLen = strlen(key);
  if (kLen == 0) {
    snprintf(outStr, outMax, "%s", ciphertext);
    return;
  }
  size_t ki = 0;
  size_t i = 0;
  for (; i < outMax - 1 && ciphertext[i]; i++) {
    char c = ciphertext[i];
    char k = tolower((unsigned char)key[ki % kLen]);
    int shift = (k >= 'a' && k <= 'z') ? (k - 'a') : 0;
    if (c >= 'a' && c <= 'z') {
      outStr[i] = (char)('a' + (c - 'a' - shift + 26) % 26);
      ki++;
    } else if (c >= 'A' && c <= 'Z') {
      outStr[i] = (char)('A' + (c - 'A' - shift + 26) % 26);
      ki++;
    } else {
      outStr[i] = c;
    }
  }
  outStr[i] = '\0';
}

// ---- Base64 ----
static const char B64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool base64_encode(const char* inStr, char* outStr, size_t outMax) {
  if (!inStr || !outStr || outMax < 1) return false;
  size_t inLen = strlen(inStr);
  size_t outLen = 4 * ((inLen + 2) / 3);
  if (outMax < outLen + 1) return false;

  size_t i = 0, j = 0;
  while (i < inLen) {
    size_t rem = inLen - i;
    uint32_t oct_a = (unsigned char)inStr[i++];
    uint32_t oct_b = (rem > 1) ? (unsigned char)inStr[i++] : 0;
    uint32_t oct_c = (rem > 2) ? (unsigned char)inStr[i++] : 0;
    uint32_t triple = (oct_a << 16) | (oct_b << 8) | oct_c;

    outStr[j++] = B64_CHARS[(triple >> 18) & 0x3F];
    outStr[j++] = B64_CHARS[(triple >> 12) & 0x3F];
    outStr[j++] = (rem < 2) ? '=' : B64_CHARS[(triple >> 6) & 0x3F];
    outStr[j++] = (rem < 3) ? '=' : B64_CHARS[triple & 0x3F];
  }
  outStr[j] = '\0';
  return true;
}

static inline int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool base64_decode(const char* inStr, char* outStr, size_t outMax) {
  if (!inStr || !outStr || outMax < 1) return false;
  size_t inLen = strlen(inStr);
  if (inLen % 4 != 0) return false;

  size_t j = 0;
  for (size_t i = 0; i < inLen; i += 4) {
    int v0 = b64Val(inStr[i]);
    int v1 = b64Val(inStr[i + 1]);
    int v2 = (inStr[i + 2] == '=') ? 0 : b64Val(inStr[i + 2]);
    int v3 = (inStr[i + 3] == '=') ? 0 : b64Val(inStr[i + 3]);
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return false;
    if (inStr[i + 2] == '=' && inStr[i + 3] != '=') return false;

    uint32_t triple = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;
    if (j < outMax - 1) outStr[j++] = (char)((triple >> 16) & 0xFF);
    if (inStr[i + 2] != '=' && j < outMax - 1) outStr[j++] = (char)((triple >> 8) & 0xFF);
    if (inStr[i + 3] != '=' && j < outMax - 1) outStr[j++] = (char)(triple & 0xFF);
  }
  outStr[j] = '\0';
  return true;
}

// ---- Hex ----
bool hex_encode(const char* inStr, char* outHex, size_t outMax) {
  if (!inStr || !outHex) return false;
  size_t len = strlen(inStr);
  if (outMax < len * 2 + 1) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char b = (unsigned char)inStr[i];
    outHex[i * 2]     = HEX_CHARS[(b >> 4) & 0x0F];
    outHex[i * 2 + 1] = HEX_CHARS[b & 0x0F];
  }
  outHex[len * 2] = '\0';
  return true;
}

bool hex_decode(const char* inHex, char* outStr, size_t outMax) {
  if (!inHex || !outStr || outMax < 1) return false;
  size_t len = strlen(inHex);
  if (len % 2 != 0) return false;
  size_t byteLen = len / 2;
  if (byteLen >= outMax) byteLen = outMax - 1;
  for (size_t i = 0; i < byteLen; i++) {
    int h1 = hexVal(inHex[i * 2]);
    int h2 = hexVal(inHex[i * 2 + 1]);
    if (h1 < 0 || h2 < 0) return false;
    outStr[i] = (char)((h1 << 4) | h2);
  }
  outStr[byteLen] = '\0';
  return true;
}

// ---- MD5 & SHA-256 散列 ----
void md5_hex(const char* inStr, char* outHex33) {
  if (!inStr || !outHex33) return;
  unsigned char digest[16];
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts_ret(&ctx);
  mbedtls_md5_update_ret(&ctx, (const unsigned char*)inStr, strlen(inStr));
  mbedtls_md5_finish_ret(&ctx, digest);
  mbedtls_md5_free(&ctx);

  for (int i = 0; i < 16; i++) {
    outHex33[i * 2]     = HEX_CHARS[(digest[i] >> 4) & 0x0F];
    outHex33[i * 2 + 1] = HEX_CHARS[digest[i] & 0x0F];
  }
  outHex33[32] = '\0';
}

void sha256_hex(const char* inStr, char* outHex65) {
  if (!inStr || !outHex65) return;
  unsigned char digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0); // 0 = SHA-256
  mbedtls_sha256_update_ret(&ctx, (const unsigned char*)inStr, strlen(inStr));
  mbedtls_sha256_finish_ret(&ctx, digest);
  mbedtls_sha256_free(&ctx);

  for (int i = 0; i < 32; i++) {
    outHex65[i * 2]     = HEX_CHARS[(digest[i] >> 4) & 0x0F];
    outHex65[i * 2 + 1] = HEX_CHARS[digest[i] & 0x0F];
  }
  outHex65[64] = '\0';
}

// ---- Linux $1$ MD5-crypt (Poul-Henning Kamp) ----
static const char MD5_B64T[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

#define B64_24BIT(B2, B1, B0, N) \
  do { \
    unsigned int w = (((unsigned int)(uint8_t)(B2)) << 16) | (((unsigned int)(uint8_t)(B1)) << 8) | ((unsigned int)(uint8_t)(B0)); \
    int _n = (N); \
    while (_n-- > 0) { \
      *cp++ = MD5_B64T[w & 0x3f]; \
      w >>= 6; \
    } \
  } while (0)

bool md5_crypt_calc(const char* key, const char* salt, char* outBuf, size_t outBufMax) {
  if (!key || !salt || !outBuf || outBufMax < 40) return false;
  if (strncmp(salt, "$1$", 3) == 0) salt += 3;
  size_t saltLen = 0;
  while (salt[saltLen] && salt[saltLen] != '$' && saltLen < 8) saltLen++;
  size_t keyLen = strlen(key);
  if (keyLen > 128) keyLen = 128;

  unsigned char alt[16];
  mbedtls_md5_context ctx1, ctx2;

  mbedtls_md5_init(&ctx1);
  mbedtls_md5_starts_ret(&ctx1);
  mbedtls_md5_update_ret(&ctx1, (const unsigned char*)key, keyLen);
  mbedtls_md5_update_ret(&ctx1, (const unsigned char*)"$1$", 3);
  mbedtls_md5_update_ret(&ctx1, (const unsigned char*)salt, saltLen);

  mbedtls_md5_init(&ctx2);
  mbedtls_md5_starts_ret(&ctx2);
  mbedtls_md5_update_ret(&ctx2, (const unsigned char*)key, keyLen);
  mbedtls_md5_update_ret(&ctx2, (const unsigned char*)salt, saltLen);
  mbedtls_md5_update_ret(&ctx2, (const unsigned char*)key, keyLen);
  mbedtls_md5_finish_ret(&ctx2, alt);
  mbedtls_md5_free(&ctx2);

  for (size_t i = keyLen; i > 0; i = (i > 16) ? i - 16 : 0) {
    mbedtls_md5_update_ret(&ctx1, alt, (i > 16) ? 16 : i);
  }

  for (size_t i = keyLen; i > 0; i >>= 1) {
    if (i & 1) {
      unsigned char z = 0;
      mbedtls_md5_update_ret(&ctx1, &z, 1);
    } else {
      mbedtls_md5_update_ret(&ctx1, (const unsigned char*)key, 1);
    }
  }
  mbedtls_md5_finish_ret(&ctx1, alt);
  mbedtls_md5_free(&ctx1);

  for (int cnt = 0; cnt < 1000; ++cnt) {
    mbedtls_md5_init(&ctx1);
    mbedtls_md5_starts_ret(&ctx1);
    if (cnt & 1) mbedtls_md5_update_ret(&ctx1, (const unsigned char*)key, keyLen);
    else mbedtls_md5_update_ret(&ctx1, alt, 16);

    if (cnt % 3) mbedtls_md5_update_ret(&ctx1, (const unsigned char*)salt, saltLen);
    if (cnt % 7) mbedtls_md5_update_ret(&ctx1, (const unsigned char*)key, keyLen);

    if (cnt & 1) mbedtls_md5_update_ret(&ctx1, alt, 16);
    else mbedtls_md5_update_ret(&ctx1, (const unsigned char*)key, keyLen);

    mbedtls_md5_finish_ret(&ctx1, alt);
    mbedtls_md5_free(&ctx1);
  }

  char* cp = outBuf;
  cp += snprintf(cp, outBufMax, "$1$%.*s$", (int)saltLen, salt);
  B64_24BIT(alt[0], alt[6], alt[12], 4);
  B64_24BIT(alt[1], alt[7], alt[13], 4);
  B64_24BIT(alt[2], alt[8], alt[14], 4);
  B64_24BIT(alt[3], alt[9], alt[15], 4);
  B64_24BIT(alt[4], alt[10], alt[5], 4);
  B64_24BIT(0, 0, alt[11], 2);
  *cp = '\0';
  return true;
}
#undef B64_24BIT

} // namespace crypto
