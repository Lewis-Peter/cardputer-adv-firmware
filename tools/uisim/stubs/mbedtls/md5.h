#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t state[4];
  uint32_t count[2];
  unsigned char buffer[64];
} mbedtls_md5_context;

#ifdef __cplusplus
extern "C" {
#endif
void mbedtls_md5_init(mbedtls_md5_context* ctx);
void mbedtls_md5_free(mbedtls_md5_context* ctx);
int mbedtls_md5_starts_ret(mbedtls_md5_context* ctx);
int mbedtls_md5_update_ret(mbedtls_md5_context* ctx, const unsigned char* input, size_t ilen);
int mbedtls_md5_finish_ret(mbedtls_md5_context* ctx, unsigned char output[16]);
#ifdef __cplusplus
}
#endif
