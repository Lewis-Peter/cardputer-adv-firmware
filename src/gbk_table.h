#pragma once
#include <stdint.h>
#if defined(ARDUINO)
  #include <pgmspace.h>
#else
  #ifndef PROGMEM
    #define PROGMEM
  #endif
  #ifndef pgm_read_word
    #define pgm_read_word(p) (*(const uint16_t*)(p))
  #endif
#endif

// GBK 双字节转 Unicode 映射表声明
// 存储于 Flash (PROGMEM)，占用 47.8KB Flash，0 字节 SRAM
extern const uint16_t GBK_TO_UNICODE[23940] PROGMEM;

inline uint16_t gbkToUnicode(uint8_t b1, uint8_t b2) {
  if (b1 < 0x81 || b1 > 0xFE) return 0;
  if (b2 < 0x40 || b2 > 0xFE || b2 == 0x7F) return 0;
  int idx = (b1 - 0x81) * 190 + (b2 < 0x7F ? b2 - 0x40 : b2 - 0x41);
  return pgm_read_word(&GBK_TO_UNICODE[idx]);
}

// 将 Unicode 码点转为 UTF-8 字节串存入 out（至少 4 字节），返回写入字节数
inline int unicodeToUtf8(uint16_t u, char out[4]) {
  if (u == 0) return 0;
  if (u < 0x80) {
    out[0] = (char)u;
    out[1] = 0;
    return 1;
  }
  if (u < 0x800) {
    out[0] = (char)(0xC0 | (u >> 6));
    out[1] = (char)(0x80 | (u & 0x3F));
    out[2] = 0;
    return 2;
  }
  out[0] = (char)(0xE0 | (u >> 12));
  out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
  out[2] = (char)(0x80 | (u & 0x3F));
  out[3] = 0;
  return 3;
}

#include <WString.h>
String ensureUtf8(const String& s);

