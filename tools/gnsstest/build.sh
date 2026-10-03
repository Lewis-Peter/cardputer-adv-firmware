#!/usr/bin/env bash
# 主机端编译运行 GNSS 模块的真逻辑测试：
# 坐标转换 (DMS / Maidenhead / UTM)、行程统计与跳点过滤、GSA 卫星解析。
#
# 用法：
#   cd tools/gnsstest && ./build.sh
#   SAN=1 ./build.sh
set -e
cd "$(dirname "$0")"

: "${CXX:=g++}"
SANFLAGS=""
[ "${SAN:-0}" = "1" ] && SANFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

"$CXX" -std=c++17 -w -g $SANFLAGS -DARDUINO=100 \
  -I stubs \
  -I ../../src \
  -I ../../.pio/libdeps/cardputer-adv/TinyGPSPlus/src \
  -I ../../.pio/libdeps/cardputer-adv/ArduinoJson/src \
  main.cpp \
  ../../src/gnss.cpp \
  ../../src/skyview.cpp \
  ../../.pio/libdeps/cardputer-adv/TinyGPSPlus/src/TinyGPS++.cpp \
  -o gnsstest

./gnsstest
