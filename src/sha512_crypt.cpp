/* SHA512-based Unix crypt implementation.
   Adapted from the Public Domain reference implementation by Ulrich Drepper <drepper@redhat.com>.
   Zero heap allocation, safe for embedded Xtensa dual-core execution.  */

#include "sha512_crypt.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

namespace {

struct sha512_ctx {
  uint64_t H[8];
  uint64_t total[2];
  uint64_t buflen;
  char buffer[256];
};

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(__LITTLE_ENDIAN__) || defined(ESP32)
# define SWAP(n) \
  (((n) << 56) \
   | (((n) & 0xff00ULL) << 40) \
   | (((n) & 0xff0000ULL) << 24) \
   | (((n) & 0xff000000ULL) << 8) \
   | (((n) >> 8) & 0xff000000ULL) \
   | (((n) >> 24) & 0xff0000ULL) \
   | (((n) >> 40) & 0xff00ULL) \
   | ((n) >> 56))
#else
# define SWAP(n) (n)
#endif

static const unsigned char fillbuf[128] = { 0x80, 0 };

static const uint64_t K[80] = {
  UINT64_C (0x428a2f98d728ae22), UINT64_C (0x7137449123ef65cd),
  UINT64_C (0xb5c0fbcfec4d3b2f), UINT64_C (0xe9b5dba58189dbbc),
  UINT64_C (0x3956c25bf348b538), UINT64_C (0x59f111f1b605d019),
  UINT64_C (0x923f82a4af194f9b), UINT64_C (0xab1c5ed5da6d8118),
  UINT64_C (0xd807aa98a3030242), UINT64_C (0x12835b0145706fbe),
  UINT64_C (0x243185be4ee4b28c), UINT64_C (0x550c7dc3d5ffb4e2),
  UINT64_C (0x72be5d74f27b896f), UINT64_C (0x80deb1fe3b1696b1),
  UINT64_C (0x9bdc06a725c71235), UINT64_C (0xc19bf174cf692694),
  UINT64_C (0xe49b69c19ef14ad2), UINT64_C (0xefbe4786384f25e3),
  UINT64_C (0x0fc19dc68b8cd5b5), UINT64_C (0x240ca1cc77ac9c65),
  UINT64_C (0x2de92c6f592b0275), UINT64_C (0x4a7484aa6ea6e483),
  UINT64_C (0x5cb0a9dcbd41fbd4), UINT64_C (0x76f988da831153b5),
  UINT64_C (0x983e5152ee66dfab), UINT64_C (0xa831c66d2db43210),
  UINT64_C (0xb00327c898fb213f), UINT64_C (0xbf597fc7beef0ee4),
  UINT64_C (0xc6e00bf33da88fc2), UINT64_C (0xd5a79147930aa725),
  UINT64_C (0x06ca6351e003826f), UINT64_C (0x142929670a0e6e70),
  UINT64_C (0x27b70a8546d22ffc), UINT64_C (0x2e1b21385c26c926),
  UINT64_C (0x4d2c6dfc5ac42aed), UINT64_C (0x53380d139d95b3df),
  UINT64_C (0x650a73548baf63de), UINT64_C (0x766a0abb3c77b2a8),
  UINT64_C (0x81c2c92e47edaee6), UINT64_C (0x92722c851482353b),
  UINT64_C (0xa2bfe8a14cf10364), UINT64_C (0xa81a664bbc423001),
  UINT64_C (0xc24b8b70d0f89791), UINT64_C (0xc76c51a30654be30),
  UINT64_C (0xd192e819d6ef5218), UINT64_C (0xd69906245565a910),
  UINT64_C (0xf40e35855771202a), UINT64_C (0x106aa07032bbd1b8),
  UINT64_C (0x19a4c116b8d2d0c8), UINT64_C (0x1e376c085141ab53),
  UINT64_C (0x2748774cdf8eeb99), UINT64_C (0x34b0bcb5e19b48a8),
  UINT64_C (0x391c0cb3c5c95a63), UINT64_C (0x4ed8aa4ae3418acb),
  UINT64_C (0x5b9cca4f7763e373), UINT64_C (0x682e6ff3d6b2b8a3),
  UINT64_C (0x748f82ee5defb2fc), UINT64_C (0x78a5636f43172f60),
  UINT64_C (0x84c87814a1f0ab72), UINT64_C (0x8cc702081a6439ec),
  UINT64_C (0x90befffa23631e28), UINT64_C (0xa4506cebde82bde9),
  UINT64_C (0xbef9a3f7b2c67915), UINT64_C (0xc67178f2e372532b),
  UINT64_C (0xca273eceea26619c), UINT64_C (0xd186b8c721c0c207),
  UINT64_C (0xeada7dd6cde0eb1e), UINT64_C (0xf57d4f7fee6ed178),
  UINT64_C (0x06f067aa72176fba), UINT64_C (0x0a637dc5a2c898a6),
  UINT64_C (0x113f9804bef90dae), UINT64_C (0x1b710b35131c471b),
  UINT64_C (0x28db77f523047d84), UINT64_C (0x32caab7b40c72493),
  UINT64_C (0x3c9ebe0a15c9bebc), UINT64_C (0x431d67c49c100d4c),
  UINT64_C (0x4cc5d4becb3e42b6), UINT64_C (0x597f299cfc657e2a),
  UINT64_C (0x5fcb6fab3ad6faec), UINT64_C (0x6c44198c4a475817)
};

static void sha512_process_block(const void *buffer, size_t len, struct sha512_ctx *ctx) {
  const uint64_t *words = (const uint64_t *)buffer;
  size_t nwords = len / sizeof(uint64_t);
  uint64_t a = ctx->H[0], b = ctx->H[1], c = ctx->H[2], d = ctx->H[3];
  uint64_t e = ctx->H[4], f = ctx->H[5], g = ctx->H[6], h = ctx->H[7];

  ctx->total[0] += len;
  if (ctx->total[0] < len) ++ctx->total[1];

  while (nwords > 0) {
    uint64_t W[80];
    uint64_t a_save = a, b_save = b, c_save = c, d_save = d;
    uint64_t e_save = e, f_save = f, g_save = g, h_save = h;

#define Ch(x, y, z) ((x & y) ^ (~x & z))
#define Maj(x, y, z) ((x & y) ^ (x & z) ^ (y & z))
#define S0(x) (CYCLIC (x, 28) ^ CYCLIC (x, 34) ^ CYCLIC (x, 39))
#define S1(x) (CYCLIC (x, 14) ^ CYCLIC (x, 18) ^ CYCLIC (x, 41))
#define R0(x) (CYCLIC (x, 1) ^ CYCLIC (x, 8) ^ (x >> 7))
#define R1(x) (CYCLIC (x, 19) ^ CYCLIC (x, 61) ^ (x >> 6))
#define CYCLIC(w, s) ((w >> s) | (w << (64 - s)))

    for (unsigned int t = 0; t < 16; ++t) {
      W[t] = SWAP(*words);
      ++words;
    }
    for (unsigned int t = 16; t < 80; ++t) {
      W[t] = R1(W[t - 2]) + W[t - 7] + R0(W[t - 15]) + W[t - 16];
    }

    for (unsigned int t = 0; t < 80; ++t) {
      uint64_t T1 = h + S1(e) + Ch(e, f, g) + K[t] + W[t];
      uint64_t T2 = S0(a) + Maj(a, b, c);
      h = g;
      g = f;
      f = e;
      e = d + T1;
      d = c;
      c = b;
      b = a;
      a = T1 + T2;
    }

#undef Ch
#undef Maj
#undef S0
#undef S1
#undef R0
#undef R1
#undef CYCLIC

    a += a_save; b += b_save; c += c_save; d += d_save;
    e += e_save; f += f_save; g += g_save; h += h_save;
    nwords -= 16;
  }

  ctx->H[0] = a; ctx->H[1] = b; ctx->H[2] = c; ctx->H[3] = d;
  ctx->H[4] = e; ctx->H[5] = f; ctx->H[6] = g; ctx->H[7] = h;
}

static void sha512_init_ctx(struct sha512_ctx *ctx) {
  ctx->H[0] = UINT64_C(0x6a09e667f3bcc908);
  ctx->H[1] = UINT64_C(0xbb67ae8584caa73b);
  ctx->H[2] = UINT64_C(0x3c6ef372fe94f82b);
  ctx->H[3] = UINT64_C(0xa54ff53a5f1d36f1);
  ctx->H[4] = UINT64_C(0x510e527fade682d1);
  ctx->H[5] = UINT64_C(0x9b05688c2b3e6c1f);
  ctx->H[6] = UINT64_C(0x1f83d9abfb41bd6b);
  ctx->H[7] = UINT64_C(0x5be0cd19137e2179);
  ctx->total[0] = ctx->total[1] = 0;
  ctx->buflen = 0;
}

static void* sha512_finish_ctx(struct sha512_ctx *ctx, void *resbuf) {
  uint64_t bytes = ctx->buflen;
  ctx->total[0] += bytes;
  if (ctx->total[0] < bytes) ++ctx->total[1];

  size_t pad = (bytes >= 112) ? (128 + 112 - bytes) : (112 - bytes);
  memcpy(&ctx->buffer[bytes], fillbuf, pad);

  *(uint64_t *)&ctx->buffer[bytes + pad + 8] = SWAP(ctx->total[0] << 3);
  *(uint64_t *)&ctx->buffer[bytes + pad] = SWAP((ctx->total[1] << 3) | (ctx->total[0] >> 61));

  sha512_process_block(ctx->buffer, bytes + pad + 16, ctx);

  for (unsigned int i = 0; i < 8; ++i) {
    ((uint64_t *)resbuf)[i] = SWAP(ctx->H[i]);
  }
  return resbuf;
}

static void sha512_process_bytes(const void *buffer, size_t len, struct sha512_ctx *ctx) {
  if (ctx->buflen > 0) {
    size_t left_over = ctx->buflen;
    size_t add = 128 - left_over;
    if (len < add) add = len;

    memcpy(&ctx->buffer[left_over], buffer, add);
    ctx->buflen += add;

    if (ctx->buflen > 128) {
      sha512_process_block(ctx->buffer, ctx->buflen & ~127, ctx);
      ctx->buflen &= 127;
      memcpy(ctx->buffer, &ctx->buffer[(left_over + add) & ~127], ctx->buflen);
    }
    buffer = (const char *)buffer + add;
    len -= add;
  }

  if (len >= 128) {
    if (((uintptr_t)buffer % sizeof(uint64_t)) != 0) {
      while (len > 128) {
        sha512_process_block(memcpy(ctx->buffer, buffer, 128), 128, ctx);
        buffer = (const char *)buffer + 128;
        len -= 128;
      }
    } else {
      sha512_process_block(buffer, len & ~127, ctx);
      buffer = (const char *)buffer + (len & ~127);
      len &= 127;
    }
  }

  if (len > 0) {
    size_t left_over = ctx->buflen;
    memcpy(&ctx->buffer[left_over], buffer, len);
    left_over += len;
    if (left_over >= 128) {
      sha512_process_block(ctx->buffer, 128, ctx);
      left_over -= 128;
      memcpy(ctx->buffer, &ctx->buffer[128], left_over);
    }
    ctx->buflen = left_over;
  }
}

static const char b64t[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

} // namespace

bool sha512_crypt_calc(const char *key, const char *salt, int rounds,
                       char *buffer, size_t buflen,
                       volatile int *progress_rounds,
                       volatile bool *cancel_flag) {
  if (!key || !salt || !buffer || buflen < 128) return false;

  unsigned char alt_result[64] __attribute__((aligned(8)));
  unsigned char temp_result[64] __attribute__((aligned(8)));
  struct sha512_ctx ctx;
  struct sha512_ctx alt_ctx;
  size_t salt_len;
  size_t key_len;
  size_t cnt;
  char *cp;
  bool rounds_custom = (rounds != 5000);

  if (strncmp("$6$", salt, 3) == 0) salt += 3;
  if (strncmp("rounds=", salt, 7) == 0) {
    const char *p = strchr(salt, '$');
    if (p) salt = p + 1;
  }

  salt_len = 0;
  while (salt[salt_len] && salt[salt_len] != '$' && salt_len < 16) {
    salt_len++;
  }
  key_len = strlen(key);
  if (key_len > 128) key_len = 128; // Cardputer 键盘输入防护

  if (rounds < 1000) rounds = 1000;
  if (rounds > 999999999) rounds = 999999999;

  // Step 1: Context A and B
  sha512_init_ctx(&ctx);
  sha512_process_bytes(key, key_len, &ctx);
  sha512_process_bytes(salt, salt_len, &ctx);

  sha512_init_ctx(&alt_ctx);
  sha512_process_bytes(key, key_len, &alt_ctx);
  sha512_process_bytes(salt, salt_len, &alt_ctx);
  sha512_process_bytes(key, key_len, &alt_ctx);
  sha512_finish_ctx(&alt_ctx, alt_result);

  for (cnt = key_len; cnt > 64; cnt -= 64)
    sha512_process_bytes(alt_result, 64, &ctx);
  sha512_process_bytes(alt_result, cnt, &ctx);

  for (cnt = key_len; cnt > 0; cnt >>= 1) {
    if ((cnt & 1) != 0)
      sha512_process_bytes(alt_result, 64, &ctx);
    else
      sha512_process_bytes(key, key_len, &ctx);
  }
  sha512_finish_ctx(&ctx, alt_result);

  // Step 2: P-sequence
  sha512_init_ctx(&alt_ctx);
  for (cnt = 0; cnt < key_len; ++cnt)
    sha512_process_bytes(key, key_len, &alt_ctx);
  sha512_finish_ctx(&alt_ctx, temp_result);

  char p_bytes[144];
  cp = p_bytes;
  for (cnt = key_len; cnt >= 64; cnt -= 64) {
    memcpy(cp, temp_result, 64);
    cp += 64;
  }
  memcpy(cp, temp_result, cnt);

  // Step 3: S-sequence
  sha512_init_ctx(&alt_ctx);
  for (cnt = 0; cnt < (size_t)(16 + alt_result[0]); ++cnt)
    sha512_process_bytes(salt, salt_len, &alt_ctx);
  sha512_finish_ctx(&alt_ctx, temp_result);

  char s_bytes[32];
  cp = s_bytes;
  for (cnt = salt_len; cnt >= 64; cnt -= 64) {
    memcpy(cp, temp_result, 64);
    cp += 64;
  }
  memcpy(cp, temp_result, cnt);

  // Step 4: Rounds loop
  for (cnt = 0; cnt < (size_t)rounds; ++cnt) {
    if (cancel_flag && *cancel_flag) return false;

    sha512_init_ctx(&ctx);
    if ((cnt & 1) != 0)
      sha512_process_bytes(p_bytes, key_len, &ctx);
    else
      sha512_process_bytes(alt_result, 64, &ctx);

    if (cnt % 3 != 0)
      sha512_process_bytes(s_bytes, salt_len, &ctx);

    if (cnt % 7 != 0)
      sha512_process_bytes(p_bytes, key_len, &ctx);

    if ((cnt & 1) != 0)
      sha512_process_bytes(alt_result, 64, &ctx);
    else
      sha512_process_bytes(p_bytes, key_len, &ctx);

    sha512_finish_ctx(&ctx, alt_result);

    if (progress_rounds && ((cnt & 0x1f) == 0 || cnt == (size_t)rounds - 1)) {
      *progress_rounds = (int)(cnt + 1);
    }
  }

  // Step 5: Base64
  cp = buffer;
  int n = 0;
  if (!rounds_custom) {
    n = snprintf(cp, buflen, "$6$%.*s$", (int)salt_len, salt);
  } else {
    n = snprintf(cp, buflen, "$6$rounds=%zu$%.*s$", (size_t)rounds, (int)salt_len, salt);
  }
  if (n <= 0 || (size_t)n >= buflen) return false;
  cp += n;
  buflen -= n;
  if (buflen < 87) return false; // 86 字节 Base64 + 1 字节 '\0'

#define b64_from_24bit(B2, B1, B0, N) \
  do { \
    unsigned int w = (((unsigned int)(uint8_t)(B2)) << 16) | (((unsigned int)(uint8_t)(B1)) << 8) | ((unsigned int)(uint8_t)(B0)); \
    int _n = (N); \
    while (_n-- > 0 && buflen > 0) { \
      *cp++ = b64t[w & 0x3f]; \
      --buflen; \
      w >>= 6; \
    } \
  } while (0)

  b64_from_24bit(alt_result[0], alt_result[21], alt_result[42], 4);
  b64_from_24bit(alt_result[22], alt_result[43], alt_result[1], 4);
  b64_from_24bit(alt_result[44], alt_result[2], alt_result[23], 4);
  b64_from_24bit(alt_result[3], alt_result[24], alt_result[45], 4);
  b64_from_24bit(alt_result[25], alt_result[46], alt_result[4], 4);
  b64_from_24bit(alt_result[47], alt_result[5], alt_result[26], 4);
  b64_from_24bit(alt_result[6], alt_result[27], alt_result[48], 4);
  b64_from_24bit(alt_result[28], alt_result[49], alt_result[7], 4);
  b64_from_24bit(alt_result[50], alt_result[8], alt_result[29], 4);
  b64_from_24bit(alt_result[9], alt_result[30], alt_result[51], 4);
  b64_from_24bit(alt_result[31], alt_result[52], alt_result[10], 4);
  b64_from_24bit(alt_result[53], alt_result[11], alt_result[32], 4);
  b64_from_24bit(alt_result[12], alt_result[33], alt_result[54], 4);
  b64_from_24bit(alt_result[34], alt_result[55], alt_result[13], 4);
  b64_from_24bit(alt_result[56], alt_result[14], alt_result[35], 4);
  b64_from_24bit(alt_result[15], alt_result[36], alt_result[57], 4);
  b64_from_24bit(alt_result[37], alt_result[58], alt_result[16], 4);
  b64_from_24bit(alt_result[59], alt_result[17], alt_result[38], 4);
  b64_from_24bit(alt_result[18], alt_result[39], alt_result[60], 4);
  b64_from_24bit(alt_result[40], alt_result[61], alt_result[19], 4);
  b64_from_24bit(alt_result[62], alt_result[20], alt_result[41], 4);
  b64_from_24bit(0, 0, alt_result[63], 2);
#undef b64_from_24bit

  if (buflen == 0) return false;
  *cp = '\0';
  return true;
}
