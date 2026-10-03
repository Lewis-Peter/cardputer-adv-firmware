#pragma once
#include <stddef.h>
#include <stdint.h>

#define MBEDTLS_AES_ENCRYPT 1
#define MBEDTLS_AES_DECRYPT 0

typedef struct {
  uint32_t erk[64];
  int nr;
} mbedtls_aes_context;

#ifdef __cplusplus
extern "C" {
#endif
void mbedtls_aes_init(mbedtls_aes_context* ctx);
void mbedtls_aes_free(mbedtls_aes_context* ctx);
int mbedtls_aes_setkey_enc(mbedtls_aes_context* ctx, const unsigned char* key, unsigned int keybits);
int mbedtls_aes_setkey_dec(mbedtls_aes_context* ctx, const unsigned char* key, unsigned int keybits);
int mbedtls_aes_crypt_cbc(mbedtls_aes_context* ctx, int mode, size_t length, unsigned char iv[16], const unsigned char* input, unsigned char* output);
#ifdef __cplusplus
}
#endif
