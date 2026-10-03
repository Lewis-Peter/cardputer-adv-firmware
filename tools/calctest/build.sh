#!/usr/bin/env bash
# 主机端编译运行计算器（calc.cpp）和单位换算（conv.cpp）的测试台。
# 直接编译 src/ 里的原文件，不复制逻辑。
#
# 用法：
#   cd tools/calctest && ./build.sh
#   SAN=1 ./build.sh        # 开启 AddressSanitizer + UBSan
set -e
cd "$(dirname "$0")"

: "${CXX:=g++}"
SANFLAGS=""
[ "${SAN:-0}" = "1" ] && SANFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

"$CXX" -std=c++17 -w -g $SANFLAGS -DARDUINO=100 \
  -I stubs \
  -I ../../src \
  main.cpp \
  ../../src/calc.cpp \
  ../../src/conv.cpp \
  -o calctest

./calctest
