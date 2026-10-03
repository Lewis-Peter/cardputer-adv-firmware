#!/usr/bin/env bash
# 主机端编译运行文字表盘 (QlockTwo) 的取词与边界测试。
# 验证 24 小时 1440 分钟每一分钟的高亮词组合，特别是正午、午夜、HALF PAST、QUARTER TO 等边界。
#
# 用法：
#   cd tools/clocktest && ./build.sh
#   SAN=1 ./build.sh
set -e
cd "$(dirname "$0")"

: "${CXX:=g++}"
SANFLAGS=""
[ "${SAN:-0}" = "1" ] && SANFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

"$CXX" -std=c++17 -w -g $SANFLAGS -DARDUINO=100 \
  -I stubs \
  -I ../../src \
  main.cpp \
  ../../src/clock.cpp \
  -o clocktest

./clocktest
