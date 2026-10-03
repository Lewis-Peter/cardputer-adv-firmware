#!/usr/bin/env bash
# 主机端编译运行 LoRa 嗅探协议与参数解析测试：
# 命令行参数解析、默认值填充、边界条件校验、JSON 输出格式化。
#
# 用法：
#   cd tools/loratest && ./build.sh
#   SAN=1 ./build.sh
set -e
cd "$(dirname "$0")"

: "${CXX:=g++}"
SANFLAGS=""
[ "${SAN:-0}" = "1" ] && SANFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

"$CXX" -std=c++17 -w -g $SANFLAGS \
  -I ../../src \
  main.cpp \
  ../../src/lora_sniff_proto.cpp \
  -o loratest

./loratest
