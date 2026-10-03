#include "lora_sniff_proto.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

#define TEST_ASSERT(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
    exit(1); \
  } \
} while(0)

static void test_parse_args_defaults() {
  printf("[TEST] test_parse_args_defaults...\n");
  LoraSniffConfig cfg;
  const char* err = nullptr;

  // 1. 标准 Meshtastic 默认频点
  TEST_ASSERT(loraSniffParseArgs("869.525", cfg, &err));
  TEST_ASSERT(cfg.mhz == 869.525f);
  TEST_ASSERT(cfg.sf == 11);
  TEST_ASSERT(cfg.bw == 250.0f);
  TEST_ASSERT(cfg.cr == 5);
  TEST_ASSERT(cfg.sync == 0x2b);
  TEST_ASSERT(cfg.preamble == 16);

  // 2. 带有多余前后空格与制表符
  TEST_ASSERT(loraSniffParseArgs("   906.875 \t ", cfg, &err));
  TEST_ASSERT(cfg.mhz == 906.875f);
  TEST_ASSERT(cfg.sf == 11);
  TEST_ASSERT(cfg.bw == 250.0f);
  TEST_ASSERT(cfg.cr == 5);
  TEST_ASSERT(cfg.sync == 0x2b);
  TEST_ASSERT(cfg.preamble == 16);

  // 3. 边界频率 150.0 和 960.0
  TEST_ASSERT(loraSniffParseArgs("150.0", cfg, &err));
  TEST_ASSERT(cfg.mhz == 150.0f);
  TEST_ASSERT(loraSniffParseArgs("960.0", cfg, &err));
  TEST_ASSERT(cfg.mhz == 960.0f);
}

static void test_parse_args_custom() {
  printf("[TEST] test_parse_args_custom...\n");
  LoraSniffConfig cfg;
  const char* err = nullptr;

  // 4 参数：未提供 sync 时，sync 默认 0x2b，preamble 取 16
  TEST_ASSERT(loraSniffParseArgs("868.0 7 125 5", cfg, &err));
  TEST_ASSERT(cfg.mhz == 868.0f);
  TEST_ASSERT(cfg.sf == 7);
  TEST_ASSERT(cfg.bw == 125.0f);
  TEST_ASSERT(cfg.cr == 5);
  TEST_ASSERT(cfg.sync == 0x2b);
  TEST_ASSERT(cfg.preamble == 16);

  // 5 参数：自定义 sync 0x12 -> preamble 自动使用 8
  TEST_ASSERT(loraSniffParseArgs("868.0 7 125 5 0x12", cfg, &err));
  TEST_ASSERT(cfg.sync == 0x12);
  TEST_ASSERT(cfg.preamble == 8);

  // 5 参数：不带 0x 前缀的十六进制
  TEST_ASSERT(loraSniffParseArgs("868.0 7 125 5 12", cfg, &err));
  TEST_ASSERT(cfg.sync == 0x12);
  TEST_ASSERT(cfg.preamble == 8);

  // 5 参数：显式给 0x2b，preamble 为 16
  TEST_ASSERT(loraSniffParseArgs("915.0 12 500 8 0x2b", cfg, &err));
  TEST_ASSERT(cfg.mhz == 915.0f);
  TEST_ASSERT(cfg.sf == 12);
  TEST_ASSERT(cfg.bw == 500.0f);
  TEST_ASSERT(cfg.cr == 8);
  TEST_ASSERT(cfg.sync == 0x2b);
  TEST_ASSERT(cfg.preamble == 16);

  // 浮点带宽支持（如 7.8, 31.25, 62.5）
  TEST_ASSERT(loraSniffParseArgs("433.175 5 7.8 5 0x12", cfg, &err));
  TEST_ASSERT(cfg.mhz == 433.175f);
  TEST_ASSERT(cfg.sf == 5);
  TEST_ASSERT(fabsf(cfg.bw - 7.8f) < 0.01f);
  TEST_ASSERT(cfg.cr == 5);
  TEST_ASSERT(cfg.sync == 0x12);
  TEST_ASSERT(cfg.preamble == 8);
}

static void test_parse_args_invalid() {
  printf("[TEST] test_parse_args_invalid...\n");
  LoraSniffConfig cfg;
  const char* err = nullptr;

  // 空输入
  TEST_ASSERT(!loraSniffParseArgs(nullptr, cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("   ", cfg, &err));

  // 频率越界
  TEST_ASSERT(!loraSniffParseArgs("149.9", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("960.1", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("-10", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("0", cfg, &err));

  // SF 越界
  TEST_ASSERT(!loraSniffParseArgs("868.0 4 125 5", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("868.0 13 125 5", cfg, &err));

  // BW 越界
  TEST_ASSERT(!loraSniffParseArgs("868.0 7 0 5", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("868.0 7 -10 5", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("868.0 7 500.1 5", cfg, &err));
  // 不在 SX126x 离散档位上的带宽（radio.begin 会失败，不能被当成 "no radio"）
  TEST_ASSERT(!loraSniffParseArgs("868.0 7 200 5", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("868.0 7 100 5", cfg, &err));
  // 离散档位允许小误差并吸附到标准值
  TEST_ASSERT(loraSniffParseArgs("868.0 7 41.67 5", cfg, &err) && cfg.bw == 41.7f);
  TEST_ASSERT(loraSniffParseArgs("868.0 7 62.5 5", cfg, &err) && cfg.bw == 62.5f);

  // CR 越界
  TEST_ASSERT(!loraSniffParseArgs("868.0 7 125 4", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("868.0 7 125 9", cfg, &err));

  // sync 越界或非法
  TEST_ASSERT(!loraSniffParseArgs("868.0 7 125 5 0x100", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("868.0 7 125 5 zzz", cfg, &err));

  // 参数个数非法（2 或 3 个参数）
  TEST_ASSERT(!loraSniffParseArgs("869.525 11", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("869.525 11 250", cfg, &err));

  // 多余参数（超过 5 个）
  TEST_ASSERT(!loraSniffParseArgs("869.525 11 250 5 0x2b extra", cfg, &err));

  // 非数值乱码
  TEST_ASSERT(!loraSniffParseArgs("freq", cfg, &err));
  TEST_ASSERT(!loraSniffParseArgs("869.525 bad 250 5", cfg, &err));
}

static void test_format_start() {
  printf("[TEST] test_format_start...\n");
  LoraSniffConfig cfg = { 869.525f, 11, 250.0f, 5, 0x2b, 16 };
  char buf[256];
  size_t n = loraSniffFormatStart(buf, sizeof(buf), cfg);
  TEST_ASSERT(n > 0);
  // 核对与规格书完全一致：LORA {"t":"start","mhz":869.525,"sf":11,"bw":250,"cr":5,"sync":43,"pre":16}
  TEST_ASSERT(strcmp(buf, "LORA {\"t\":\"start\",\"mhz\":869.525,\"sf\":11,\"bw\":250,\"cr\":5,\"sync\":43,\"pre\":16}") == 0);

  // 整数频率与带小数带宽格式化
  cfg = { 915.0f, 7, 62.5f, 5, 18, 8 };
  n = loraSniffFormatStart(buf, sizeof(buf), cfg);
  TEST_ASSERT(n > 0);
  TEST_ASSERT(strcmp(buf, "LORA {\"t\":\"start\",\"mhz\":915,\"sf\":7,\"bw\":62.5,\"cr\":5,\"sync\":18,\"pre\":8}") == 0);
}

static void test_format_end() {
  printf("[TEST] test_format_end...\n");
  char buf[64];
  size_t n = loraSniffFormatEnd(buf, sizeof(buf));
  TEST_ASSERT(n > 0);
  TEST_ASSERT(strcmp(buf, "LORA {\"t\":\"end\"}") == 0);
}

static void test_format_err() {
  printf("[TEST] test_format_err...\n");
  char buf[128];
  size_t n = loraSniffFormatErr(buf, sizeof(buf), "no radio");
  TEST_ASSERT(n > 0);
  TEST_ASSERT(strcmp(buf, "LORA {\"t\":\"err\",\"msg\":\"no radio\"}") == 0);

  n = loraSniffFormatErr(buf, sizeof(buf), "busy: LoRa app open");
  TEST_ASSERT(n > 0);
  TEST_ASSERT(strcmp(buf, "LORA {\"t\":\"err\",\"msg\":\"busy: LoRa app open\"}") == 0);

  n = loraSniffFormatErr(buf, sizeof(buf), "bad args");
  TEST_ASSERT(n > 0);
  TEST_ASSERT(strcmp(buf, "LORA {\"t\":\"err\",\"msg\":\"bad args\"}") == 0);
}

static void test_format_packet() {
  printf("[TEST] test_format_packet...\n");
  uint8_t payload[37];
  memset(payload, 0xFF, sizeof(payload));

  char buf[1024];
  size_t n = loraSniffFormatPacket(buf, sizeof(buf), 123456, 37, -92.5f, 7.25f, -1234, true, payload, sizeof(payload));
  TEST_ASSERT(n > 0);

  // 构建期望字符串进行全匹配
  std::string expected = "LORA {\"t\":\"p\",\"ts\":123456,\"len\":37,\"rssi\":-92.5,\"snr\":7.25,\"fe\":-1234,\"crc\":true,\"hex\":\"";
  for (int i = 0; i < 37; i++) expected += "ff";
  expected += "\"}";
  TEST_ASSERT(std::string(buf) == expected);

  // 验证 CRC 错误标记 crc:false
  n = loraSniffFormatPacket(buf, sizeof(buf), 200000, 4, -110.0f, -5.5f, 456, false, (const uint8_t*)"\x12\x34\xab\xcd", 4);
  TEST_ASSERT(n > 0);
  expected = "LORA {\"t\":\"p\",\"ts\":200000,\"len\":4,\"rssi\":-110,\"snr\":-5.5,\"fe\":456,\"crc\":false,\"hex\":\"1234abcd\"}";
  TEST_ASSERT(std::string(buf) == expected);

  // 验证空载荷 len=0
  n = loraSniffFormatPacket(buf, sizeof(buf), 300000, 0, -80.0f, 10.0f, 0, true, nullptr, 0);
  TEST_ASSERT(n > 0);
  expected = "LORA {\"t\":\"p\",\"ts\":300000,\"len\":0,\"rssi\":-80,\"snr\":10,\"fe\":0,\"crc\":true,\"hex\":\"\"}";
  TEST_ASSERT(std::string(buf) == expected);

  // 验证最大 255 字节满载荷
  uint8_t maxPayload[255];
  for (int i = 0; i < 255; i++) maxPayload[i] = (uint8_t)i;
  n = loraSniffFormatPacket(buf, sizeof(buf), 400000, 255, -75.0f, 8.0f, 100, true, maxPayload, sizeof(maxPayload));
  TEST_ASSERT(n > 0);
  TEST_ASSERT(strstr(buf, "\"crc\":true,\"hex\":\"00010203") != nullptr);
  TEST_ASSERT(strstr(buf, "fcfdfe\"}") != nullptr);
}

int main() {
  printf("===== Running LoRa Sniff Unit Tests =====\n");
  test_parse_args_defaults();
  test_parse_args_custom();
  test_parse_args_invalid();
  test_format_start();
  test_format_end();
  test_format_err();
  test_format_packet();
  printf("===== All LoRa Sniff Unit Tests Passed! =====\n");
  return 0;
}
