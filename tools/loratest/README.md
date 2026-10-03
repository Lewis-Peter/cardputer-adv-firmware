# LoRa 嗅探协议与参数解析测试台

`src/lora_sniff_proto.h` 和 `src/lora_sniff_proto.cpp` 封装了固件 LoRa 嗅探能力的协议层与命令行解析逻辑：
- 零依赖 Arduino 运行时的纯 C++ 实现，方便跨平台编译与主机端全量单测。
- 命令行参数解析：
  - 单参数：`LORA SNIFF <MHz>` 自动填充 Meshtastic 默认 LongFast 参数（SF11, BW250, CR5, sync 0x2b, preamble 16）。
  - 多参数：`LORA SNIFF <MHz> [sf bw_khz cr [sync_hex]]`，支持十进制或十六进制 sync 字，根据 sync 自动推导 preamble（0x2b 为 16，其余为 8）。
  - 参数边界校验：SX1262 射频频率（150..960 MHz）、扩频因子（5..12）、带宽（(0..500] kHz）、编码率（5..8）、同步字（0..255）。
- 协议格式化：
  - `start` / `p` / `end` / `err` 纯文本 JSON 流格式输出。
  - 浮点数安全格式化与尾随 0 剔除（如 `869.525`、`250`、`-92.5`）。
  - 数据包 HEX 编码转换与 CRC 状态上报。

## 运行方法

```bash
cd tools/loratest && ./build.sh
# 开启 AddressSanitizer 和 UndefinedBehaviorSanitizer:
SAN=1 ./build.sh
```
