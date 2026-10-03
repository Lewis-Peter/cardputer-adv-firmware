// 空壳：ram_profile.h 要 #include <Arduino.h>，但这个试跑台需要的 millis()/Stream
// 已经由 main.cpp 在包含真代码**之前**自己给了（假的堆脚本也是同理）。
// 这里给个空文件，就是为了不把整个 Arduino 拖进来。
#pragma once
