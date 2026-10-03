#!/usr/bin/env bash
# 主机端编译运行 http_json 流式 JSON 解析器测试。
# 验证 fetchJsonStreamArray 和 fetchJsonHttp：
# 正常数组、空数组、截断数据、异常分隔符、流超时、maxItems 截断、回调提前中止、HTTP 状态码等。
#
# 用法：
#   cd tools/jsontest && ./build.sh
#   SAN=1 ./build.sh
set -e
cd "$(dirname "$0")"

: "${CXX:=g++}"
SANFLAGS=""
[ "${SAN:-0}" = "1" ] && SANFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

"$CXX" -std=c++17 -w -g $SANFLAGS -DARDUINO=100 \
  -DARDUINOJSON_ENABLE_PROGMEM=0 \
  -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 \
  -DARDUINOJSON_ENABLE_ARDUINO_STREAM=1 \
  -I stubs \
  -I ../../src \
  -I ../../.pio/libdeps/cardputer-adv/ArduinoJson/src \
  main.cpp \
  -o jsontest

./jsontest
