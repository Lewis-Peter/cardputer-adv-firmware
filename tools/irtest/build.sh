#!/usr/bin/env bash
# 把 src/ir_proto.cpp 那份**真代码**编出来跑，逐段核对红外波形。
#
# 为什么必须有这个：这块板子的红外**只有发射、没有接收**（HARDWARE.md），
# 手上又没有示波器——发出去的波形对不对，在实机上根本观测不到。
# 按一下电视没反应，你分不清是编码错了、载波错了、还是没对准。
# 把纯编码那一半拉到桌面上验，至少能把问题范围压到"硬件那一半"。
#
# 解码器/编码器都是按位算偏移的，越界是这类代码的典型坑，能开 sanitizer 就开：
#   SAN=1 ./build.sh
# 用法： cd tools/irtest && ./build.sh
set -e
cd "$(dirname "$0")"
: "${CXX:=c++}"
SANFLAGS=""
[ -n "${SAN:-}" ] && SANFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
"$CXX" -std=c++17 -Wall -Wextra -g $SANFLAGS -I ../../src \
  main.cpp ../../src/ir_proto.cpp -o irtest
./irtest
