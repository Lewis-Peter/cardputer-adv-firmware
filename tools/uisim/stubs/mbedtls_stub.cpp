#include "mbedtls/aes.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"
#include <cstring>
#include <string>

// Simple FNV-1a / polynomial hash based mock for UI simulator rendering
extern "C" {

void mbedtls_aes_init(mbedtls_aes_context* ctx) {
  if (ctx) memset(ctx, 0, sizeof(*ctx));
}

void mbedtls_aes_free(mbedtls_aes_context* ctx) {
  if (ctx) memset(ctx, 0, sizeof(*ctx));
}

int mbedtls_aes_setkey_enc(mbedtls_aes_context* ctx, const unsigned char* key, unsigned int keybits) {
  if (!ctx || !key) return -1;
  ctx->nr = 10;
  memcpy(ctx->erk, key, keybits / 8 < 32 ? keybits / 8 : 32);
  return 0;
}

int mbedtls_aes_setkey_dec(mbedtls_aes_context* ctx, const unsigned char* key, unsigned int keybits) {
  return mbedtls_aes_setkey_enc(ctx, key, keybits);
}

int mbedtls_aes_crypt_cbc(mbedtls_aes_context* ctx, int mode, size_t length, unsigned char iv[16], const unsigned char* input, unsigned char* output) {
  if (!output || !input) return -1;
  for (size_t i = 0; i < length; i++) {
    output[i] = input[i] ^ (iv ? iv[i % 16] : 0x5a) ^ 0x3c;
  }
  return 0;
}

// Simple MD5 mock producing 16 deterministic bytes
void mbedtls_md5_init(mbedtls_md5_context* ctx) {
  if (ctx) memset(ctx, 0, sizeof(*ctx));
}
void mbedtls_md5_free(mbedtls_md5_context* ctx) {
  if (ctx) memset(ctx, 0, sizeof(*ctx));
}
int mbedtls_md5_starts_ret(mbedtls_md5_context* ctx) {
  if (ctx) ctx->state[0] = 0x67452301;
  return 0;
}
int mbedtls_md5_update_ret(mbedtls_md5_context* ctx, const unsigned char* input, size_t ilen) {
  if (!ctx || !input) return 0;
  for (size_t i = 0; i < ilen; i++) {
    ctx->state[0] = (ctx->state[0] * 33) ^ input[i];
  }
  return 0;
}
int mbedtls_md5_finish_ret(mbedtls_md5_context* ctx, unsigned char output[16]) {
  if (!output) return -1;
  uint32_t h = ctx ? ctx->state[0] : 0x12345678;
  for (int i = 0; i < 16; i++) {
    output[i] = (unsigned char)(h >> ((i % 4) * 8)) ^ (unsigned char)(i * 17 + 0x2b);
    h = (h * 1103515245 + 12345);
  }
  return 0;
}

// Simple SHA-256 mock producing 32 deterministic bytes
void mbedtls_sha256_init(mbedtls_sha256_context* ctx) {
  if (ctx) memset(ctx, 0, sizeof(*ctx));
}
void mbedtls_sha256_free(mbedtls_sha256_context* ctx) {
  if (ctx) memset(ctx, 0, sizeof(*ctx));
}
int mbedtls_sha256_starts_ret(mbedtls_sha256_context* ctx, int) {
  if (ctx) ctx->state[0] = 0x6a09e667;
  return 0;
}
int mbedtls_sha256_update_ret(mbedtls_sha256_context* ctx, const unsigned char* input, size_t ilen) {
  if (!ctx || !input) return 0;
  for (size_t i = 0; i < ilen; i++) {
    ctx->state[0] = (ctx->state[0] * 31) ^ input[i];
  }
  return 0;
}
int mbedtls_sha256_finish_ret(mbedtls_sha256_context* ctx, unsigned char output[32]) {
  if (!output) return -1;
  uint32_t h = ctx ? ctx->state[0] : 0x87654321;
  for (int i = 0; i < 32; i++) {
    output[i] = (unsigned char)(h >> ((i % 4) * 8)) ^ (unsigned char)(i * 23 + 0x4f);
    h = (h * 1664525 + 1013904223);
  }
  return 0;
}

} // extern "C"
