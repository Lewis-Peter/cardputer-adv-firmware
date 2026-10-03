#include "lora_sniff_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cctype>

void formatTrimmedFloat(char* out, size_t sz, float val, int maxDecimals) {
  if (!out || sz == 0) return;
  char temp[32];
  snprintf(temp, sizeof(temp), "%.*f", maxDecimals, val);
  char* dot = strchr(temp, '.');
  if (dot) {
    char* end = temp + strlen(temp) - 1;
    while (end > dot && *end == '0') {
      *end = '\0';
      end--;
    }
    if (end == dot) {
      *dot = '\0';
    }
  }
  snprintf(out, sz, "%s", temp);
}

bool loraSniffParseArgs(const char* argStr, LoraSniffConfig& outCfg, const char** errReason) {
  if (!argStr) {
    if (errReason) *errReason = "bad args: null input";
    return false;
  }

  // 1. 拆分参数为 tokens（最多允许 5 个参数，多余的作为越界错误拦截）
  const char* tokens[6];
  char tokenBuf[128];
  size_t len = strlen(argStr);
  if (len >= sizeof(tokenBuf)) {
    if (errReason) *errReason = "bad args: command line too long";
    return false;
  }
  memcpy(tokenBuf, argStr, len + 1);

  int tokCount = 0;
  char* p = tokenBuf;
  while (*p) {
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) break;
    if (tokCount >= 5) {
      // 超过 5 个参数属于非法语法
      if (errReason) *errReason = "bad args: too many parameters";
      return false;
    }
    tokens[tokCount++] = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (*p) {
      *p = '\0';
      p++;
    }
  }

  // 仅支持 1 个参数（仅频率）、4 个参数（频率+sf+bw+cr）或 5 个参数（频率+sf+bw+cr+sync）
  if (tokCount != 1 && tokCount != 4 && tokCount != 5) {
    if (errReason) *errReason = "bad args: parameter count must be 1, 4, or 5";
    return false;
  }

  // 2. 解析频率 MHz
  char* endptr = nullptr;
  float mhz = (float)strtod(tokens[0], &endptr);
  if (endptr == tokens[0] || *endptr != '\0' || std::isnan(mhz) || std::isinf(mhz)) {
    if (errReason) *errReason = "bad args: invalid frequency format";
    return false;
  }
  // SX1262 射频频率支持范围 150MHz..960MHz
  if (mhz < 150.0f || mhz > 960.0f) {
    if (errReason) *errReason = "bad args: frequency out of range (150..960 MHz)";
    return false;
  }

  outCfg.mhz = mhz;

  // 3. 单参数模式：默认填入 Meshtastic 官方 LongFast 预设
  // 按照规格：sf=11, bw=250, cr=5(即4/5), sync=0x2b, preamble=16
  if (tokCount == 1) {
    outCfg.sf = 11;
    outCfg.bw = 250.0f;
    outCfg.cr = 5;
    outCfg.sync = 0x2b;
    outCfg.preamble = 16;
    return true;
  }

  // 4. 解析多参数模式：sf, bw_khz, cr
  // 4.1 扩频因子 sf: 5..12
  long sf = strtol(tokens[1], &endptr, 10);
  if (endptr == tokens[1] || *endptr != '\0' || sf < 5 || sf > 12) {
    if (errReason) *errReason = "bad args: sf must be between 5 and 12";
    return false;
  }
  outCfg.sf = (uint8_t)sf;

  // 4.2 带宽 bw_khz：SX126x 只认 10 个离散档位，不是 (0..500] 里随便填。
  // 填个 200 进去 radio.begin() 会回 RADIOLIB_ERR_INVALID_BANDWIDTH，调用方只能报成 "no radio"，
  // PC 端就会以为模块没插——所以必须在这里按 "bad args" 拦下来。
  // 允许 ±0.05 的误差，好让 "41.7" / "41.67"、"7.8" / "7.81" 这种写法都能对上。
  static const float SX126X_BWS[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};
  float bw = (float)strtod(tokens[2], &endptr);
  bool bwOk = false;
  if (endptr != tokens[2] && *endptr == '\0' && !std::isnan(bw) && !std::isinf(bw)) {
    for (float v : SX126X_BWS) {
      if (std::fabs(bw - v) <= 0.05f) { bw = v; bwOk = true; break; }
    }
  }
  if (!bwOk) {
    if (errReason) *errReason = "bad args: bandwidth must be one of 7.8/10.4/15.6/20.8/31.25/41.7/62.5/125/250/500 kHz";
    return false;
  }
  outCfg.bw = bw;

  // 4.3 编码率 cr: 5..8 (对应 RadioLib 4/5..4/8)
  long cr = strtol(tokens[3], &endptr, 10);
  if (endptr == tokens[3] || *endptr != '\0' || cr < 5 || cr > 8) {
    if (errReason) *errReason = "bad args: coding rate cr must be between 5 and 8";
    return false;
  }
  outCfg.cr = (uint8_t)cr;

  // 4.4 同步字 sync_hex（可选）
  if (tokCount == 5) {
    unsigned long syncVal = strtoul(tokens[4], &endptr, 16);
    if (endptr == tokens[4] || *endptr != '\0' || syncVal > 0xFF) {
      if (errReason) *errReason = "bad args: sync word must be 1-byte hex";
      return false;
    }
    outCfg.sync = (uint8_t)syncVal;
  } else {
    // 给了 sf/bw/cr 但没给 sync 时 sync 仍默认 0x2b
    outCfg.sync = 0x2b;
  }

  // 4.5 前导码长度：sync==0x2b 用 16，否则用 8（和 src/lora.cpp 的 PRESETS 一致）
  outCfg.preamble = (outCfg.sync == 0x2b) ? 16 : 8;

  return true;
}

size_t loraSniffFormatStart(char* out, size_t maxLen, const LoraSniffConfig& cfg) {
  if (!out || maxLen == 0) return 0;
  char mhzBuf[16], bwBuf[16];
  formatTrimmedFloat(mhzBuf, sizeof(mhzBuf), cfg.mhz, 4);
  formatTrimmedFloat(bwBuf, sizeof(bwBuf), cfg.bw, 2);

  int written = snprintf(out, maxLen,
                         "LORA {\"t\":\"start\",\"mhz\":%s,\"sf\":%u,\"bw\":%s,\"cr\":%u,\"sync\":%u,\"pre\":%u}",
                         mhzBuf, cfg.sf, bwBuf, cfg.cr, cfg.sync, cfg.preamble);
  return (written > 0 && (size_t)written < maxLen) ? (size_t)written : 0;
}

size_t loraSniffFormatEnd(char* out, size_t maxLen) {
  if (!out || maxLen == 0) return 0;
  int written = snprintf(out, maxLen, "LORA {\"t\":\"end\"}");
  return (written > 0 && (size_t)written < maxLen) ? (size_t)written : 0;
}

size_t loraSniffFormatErr(char* out, size_t maxLen, const char* msg) {
  if (!out || maxLen == 0) return 0;
  int written = snprintf(out, maxLen, "LORA {\"t\":\"err\",\"msg\":\"%s\"}", msg ? msg : "unknown");
  return (written > 0 && (size_t)written < maxLen) ? (size_t)written : 0;
}

size_t loraSniffFormatPacketHeader(char* out, size_t maxLen, uint32_t ts, size_t len,
                                  float rssi, float snr, long fe, bool crcOk) {
  if (!out || maxLen == 0) return 0;
  char rssiBuf[16], snrBuf[16];
  formatTrimmedFloat(rssiBuf, sizeof(rssiBuf), rssi, 2);
  formatTrimmedFloat(snrBuf, sizeof(snrBuf), snr, 2);

  int written = snprintf(out, maxLen,
                         "LORA {\"t\":\"p\",\"ts\":%lu,\"len\":%u,\"rssi\":%s,\"snr\":%s,\"fe\":%ld,\"crc\":%s,\"hex\":\"",
                         (unsigned long)ts, (unsigned int)len, rssiBuf, snrBuf, fe, crcOk ? "true" : "false");
  return (written > 0 && (size_t)written < maxLen) ? (size_t)written : 0;
}

size_t loraSniffFormatPacket(char* out, size_t maxLen, uint32_t ts, size_t len,
                            float rssi, float snr, long fe, bool crcOk,
                            const uint8_t* payload, size_t payloadLen) {
  if (!out || maxLen == 0) return 0;
  size_t hdrLen = loraSniffFormatPacketHeader(out, maxLen, ts, len, rssi, snr, fe, crcOk);
  if (hdrLen == 0 || hdrLen >= maxLen) return 0;

  static const char hexChars[] = "0123456789abcdef";
  size_t pos = hdrLen;
  for (size_t i = 0; i < payloadLen; i++) {
    if (pos + 2 >= maxLen) return 0;
    uint8_t b = payload[i];
    out[pos++] = hexChars[(b >> 4) & 0x0F];
    out[pos++] = hexChars[b & 0x0F];
  }

  // 拼接结尾 "\"}
  if (pos + 3 >= maxLen) return 0;
  out[pos++] = '"';
  out[pos++] = '}';
  out[pos] = '\0';
  return pos;
}
