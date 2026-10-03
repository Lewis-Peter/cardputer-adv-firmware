**English** | [简体中文](README.zh-CN.md)

# LoRa Sniffer Protocol and Parameter Parsing Testbed

`src/lora_sniff_proto.h` and `src/lora_sniff_proto.cpp` encapsulate the protocol layer and CLI parsing logic for firmware LoRa sniffing:
- Pure C++ implementation with zero Arduino runtime dependencies, enabling cross-platform compilation and host-side unit testing.
- Command-line argument parsing:
  - Single parameter: `LORA SNIFF <MHz>` automatically populates Meshtastic default LongFast parameters (SF11, BW250, CR5, sync 0x2b, preamble 16).
  - Multiple parameters: `LORA SNIFF <MHz> [sf bw_khz cr [sync_hex]]`, supporting decimal or hexadecimal sync words, automatically deducing preamble from sync (16 for 0x2b, 8 for others).
  - Parameter boundary validation: SX1262 RF frequency (150..960 MHz), spreading factor (5..12), bandwidth ((0..500] kHz), coding rate (5..8), sync word (0..255).
- Protocol formatting:
  - Plaintext JSON streaming output: `start` / `p` / `end` / `err`.
  - Safe floating-point formatting with trailing zero stripping (e.g. `869.525`, `250`, `-92.5`).
  - Packet HEX encoding conversion and CRC status reporting.

## How to Run

```bash
cd tools/loratest && ./build.sh
# Enable AddressSanitizer and UndefinedBehaviorSanitizer:
SAN=1 ./build.sh
```
