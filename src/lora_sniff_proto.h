#pragma once
#include <cstdint>
#include <cstddef>

// LoRa 嗅探配置结构体（纯 C++，不依赖 Arduino 运行时）
struct LoraSniffConfig {
  float mhz;           // 射频工作频率（MHz，SX1262 支持 150.0..960.0）
  uint8_t sf;          // 扩频因子（Spreading Factor，5..12）
  float bw;            // 带宽（kHz，只能是 SX126x 的 10 个离散档位之一：7.8..500）
  uint8_t cr;          // 编码率（Coding Rate，5..8，对应 4/5..4/8）
  uint8_t sync;        // 同步字（Sync Word，0x00..0xFF，Meshtastic 统一为 0x2b 即 43）
  uint16_t preamble;   // 前导码长度（Preamble Length，sync==0x2b 为 16，其余为 8）
};

// 格式化浮点数为 JSON 安全数字字符串，并剔除多余尾随 0 与无意义小数点
// 例如：869.525 -> "869.525", 250.0 -> "250", -92.50 -> "-92.5"
void formatTrimmedFloat(char* out, size_t sz, float val, int maxDecimals);

// 解析 LORA SNIFF 命令行参数
// 支持语法：
//   1. 仅频率：<MHz>
//      默认填入 Meshtastic LongFast（sf=11, bw=250, cr=5, sync=0x2b, pre=16）
//   2. 4 参数：<MHz> <sf> <bw_khz> <cr>
//      sync 默认 0x2b，preamble 根据 sync 规则取 16
//   3. 5 参数：<MHz> <sf> <bw_khz> <cr> <sync_hex>
//      sync 支持 0x2b 或 2b 十六进制输入，sync==0x2b 则 pre=16，否则 pre=8
// 返回值：解析与参数范围校验成功返回 true，否则返回 false 并写入 errReason（可选）
bool loraSniffParseArgs(const char* argStr, LoraSniffConfig& outCfg, const char** errReason = nullptr);

// 格式化输出 JSON 行（写入 out 缓冲区，返回写入字符数，不含结尾 null）
size_t loraSniffFormatStart(char* out, size_t maxLen, const LoraSniffConfig& cfg);
size_t loraSniffFormatEnd(char* out, size_t maxLen);
size_t loraSniffFormatErr(char* out, size_t maxLen, const char* msg);

// 格式化数据包报头（供流式或分段输出复用）
// 输出形如：LORA {"t":"p","ts":123456,"len":37,"rssi":-92.5,"snr":7.25,"fe":-1234,"crc":true,"hex":"
size_t loraSniffFormatPacketHeader(char* out, size_t maxLen, uint32_t ts, size_t len,
                                  float rssi, float snr, long fe, bool crcOk);

// 完整格式化整个数据包 JSON 行（主要用于主机端单元测试验证）
size_t loraSniffFormatPacket(char* out, size_t maxLen, uint32_t ts, size_t len,
                            float rssi, float snr, long fe, bool crcOk,
                            const uint8_t* payload, size_t payloadLen);
