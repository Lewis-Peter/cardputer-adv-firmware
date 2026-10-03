#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

namespace crypto {

// ---- 对称密码: AES-128-CBC ----
// key: 字符串密钥（内部通过 MD5 或 16 字节填充产生 128 位密钥）
// iv: 固定为 16 字节向量
bool aes128_cbc_encrypt(const char* key, const char* plaintext, char* outHex, size_t outHexMax);
bool aes128_cbc_decrypt(const char* key, const char* hexCipher, char* outPlain, size_t outPlainMax);

// ---- 对称密码: RC4 (ARC4) ----
bool rc4_encrypt(const char* key, const char* plaintext, char* outHex, size_t outHexMax);
bool rc4_decrypt(const char* key, const char* hexCipher, char* outPlain, size_t outPlainMax);

// ---- 对称密码: XOR ----
bool xor_encrypt(const char* key, const char* plaintext, char* outHex, size_t outHexMax);
bool xor_decrypt(const char* key, const char* hexCipher, char* outPlain, size_t outPlainMax);

// ---- 古典密码: 凯撒密码 / ROT13 ----
void caesar_shift(const char* inStr, int shift, char* outStr, size_t outMax);

// ---- 古典密码: 维吉尼亚密码 (Vigenère) ----
void vigenere_encrypt(const char* key, const char* plaintext, char* outStr, size_t outMax);
void vigenere_decrypt(const char* key, const char* ciphertext, char* outStr, size_t outMax);

// ---- 编码: Base64 ----
bool base64_encode(const char* inStr, char* outStr, size_t outMax);
bool base64_decode(const char* inStr, char* outStr, size_t outMax);

// ---- 编码: Hex ----
bool hex_encode(const char* inStr, char* outHex, size_t outMax);
bool hex_decode(const char* inHex, char* outStr, size_t outMax);

// ---- 散列: MD5 与 SHA-256 ----
void md5_hex(const char* inStr, char* outHex33);
void sha256_hex(const char* inStr, char* outHex65);

// ---- Linux $1$ MD5-crypt ----
bool md5_crypt_calc(const char* key, const char* salt, char* outBuf, size_t outBufMax);

} // namespace crypto
