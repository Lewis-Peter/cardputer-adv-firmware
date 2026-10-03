#!/usr/bin/env bash
# 桌面跑一遍 ram_profile 的输出。见 main.cpp 顶上那段：这一步验的是"表长什么样"，
# 跟 uisim 的 -fsyntax-only（验"编得过"）互补。
set -e
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
EXTRA=""
[ "${SAN:-0}" = "1" ] && EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer"
"$CXX" -std=c++17 -O1 -Wall -Wextra $EXTRA -I stubs -I ../../src -o ramtest main.cpp
./ramtest
