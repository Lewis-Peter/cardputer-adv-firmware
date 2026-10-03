#!/usr/bin/env bash
# 在 Mac 上把 src/odid.cpp 那份**真代码**编出来跑，验证 Remote ID 解码。
# 手上没有会播 RID 的无人机，这是唯一能证明字段偏移没错的办法。
#
# 解析器全是按外来字节算偏移，越界读是这类代码最典型的坑，所以能开 sanitizer 就开：
#   SAN=1 ./build.sh
# ⚠️ Darwin 27 上 ASan 版本一启动就空转卡死、一行输出都没有（只开 address 也一样），
# 是那个环境的问题、不是被测代码的问题——所以 Mac 上别开 SAN。Linux + g++ 实测正常，
# 全部用例通过、零报错。不开 sanitizer 时越界靠用例覆盖：见 main.cpp 第 11 组里那个
# "把真机帧从 0 截到全长逐个喂一遍"。
# 用法： cd tools/odidtest && ./build.sh   或   SAN=1 ./build.sh
set -e
cd "$(dirname "$0")"
# CXX 可覆盖：Mac 上默认 clang++，Linux 上是 g++。原来写死 /usr/bin/clang++，
# 在 Linux 上直接 "No such file or directory"。
: "${CXX:=c++}"
SANFLAGS=""
[ -n "${SAN:-}" ] && SANFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
"$CXX" -std=c++17 -w -g $SANFLAGS -I ../../src \
  main.cpp ../../src/odid.cpp -o odidtest
./odidtest
