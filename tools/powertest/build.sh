#!/usr/bin/env bash
# 在 Mac 上把 src/power_util.cpp 那份**真代码**编出来跑，用合成的电压/时间序列
# 验证充电判定和电量推算。用法： cd tools/powertest && ./build.sh
set -e
cd "$(dirname "$0")"
# CXX 可覆盖：Mac 上默认 clang++，Linux 上是 g++。原来写死 /usr/bin/clang++，
# 在 Linux 上直接 "No such file or directory"。
: "${CXX:=c++}"
"$CXX" -std=c++17 -w -I stubs -I ../../src \
  main.cpp ../../src/power_util.cpp battlog_stub.cpp -o powertest
./powertest 1
echo
./powertest 2
