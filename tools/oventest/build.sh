#!/usr/bin/env bash
# 主机端编译运行 Hash Oven 穷举模式猜测生成测试。
# 验证各个 keyspace 的字符集、总空间数学正确性、索引空间覆盖与单射无碰撞、以及各档预设 solution 的生成。
#
# 用法：
#   cd tools/oventest && ./build.sh
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
  ../../src/hash_oven.cpp \
  -o oventest

./oventest
