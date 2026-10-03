#!/usr/bin/env bash
# 主机端编译运行天文计算（solarElev / sunCross / subsolarPoint / moonPhase）测试台。
# 直接引入 src/astro.cpp 和 src/moon.cpp 的真实代码，不允许复制逻辑。
# -DHOST_TEST 激活 astro.cpp / moon.cpp 末尾的 wrapper 出口，让 static 函数可被外部访问。
#
# 用法：
#   cd tools/astrotest && ./build.sh
#   SAN=1 ./build.sh
set -e
cd "$(dirname "$0")"

: "${CXX:=g++}"
SANFLAGS=""
[ "${SAN:-0}" = "1" ] && SANFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

"$CXX" -std=c++17 -w -g $SANFLAGS \
  -DARDUINO=100 \
  -DHOST_TEST \
  -I stubs \
  -I ../../src \
  main.cpp \
  ../../src/astro.cpp \
  ../../src/moon.cpp \
  -o astrotest

./astrotest
