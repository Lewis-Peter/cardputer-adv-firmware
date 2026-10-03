// 音乐播放器：从 SD 卡流式播放 WAV 文件（16-bit PCM，单声道/立体声，任意采样率）。
// 使用双缓冲 + FreeRTOS 后台任务实现无缝流式播放，主循环不阻塞。
// 导航：;. 上下选文件；Enter 播放/暂停；` 返回（自动停止）。
// 文件位置：/music/*.wav 优先，若无则扫描根目录 /*.wav
#pragma once
#include "globals.h"

void playerEnter();   // 扫描 WAV 文件，进入屏幕
void playerExit();    // 停止播放，关闭文件，删除任务
void playerKey(char k);
void drawPlayer();
void playerUpdate();

bool playerIsPlaying();  // 外部查询：后台任务是否还在播放
uint8_t playerGetVuLevel(); // 外部查询：当前播放电平 (0..100)
