#pragma once
#include <M5Unified.h>
#include "globals.h"

// 音乐播放器 (player) 与网络收音机 (radio) 共享的音频底层抽象与流控工具

static const uint8_t AUDIO_PLAY_CH = 0;   // 统一使用 M5.Speaker 0 号虚拟通道

// 等待 M5.Speaker 播放通道出现空位（队列深度 < 2），防止覆写播放中的缓冲区
inline bool audioWaitQueueSpace(uint8_t ch = AUDIO_PLAY_CH, volatile bool* stopReq = nullptr, uint32_t delayTicks = 1, uint32_t timeoutMs = 500) {
  uint32_t t0 = millis();
  while (M5.Speaker.isPlaying(ch) >= 2) {
    if (stopReq && *stopReq) return false;
    if (timeoutMs > 0 && millis() - t0 >= timeoutMs) return false;
    vTaskDelay(delayTicks);
  }
  return true;
}

// 等待尾部残留音频帧播放完毕，并在超时或停止请求时关闭扬声器通道
inline void audioDrainAndStop(uint8_t ch = AUDIO_PLAY_CH, volatile bool* stopReq = nullptr, uint32_t timeoutMs = 4000) {
  uint32_t t0 = millis();
  while (M5.Speaker.isPlaying(ch) && (!stopReq || !*stopReq) && millis() - t0 < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  M5.Speaker.stop(ch);
}

// 快速抽样提取 16-bit PCM 音频幅值峰值，归一化至 0..100，用于驱动 UI 电平仪表
inline uint8_t audioCalcPeak(const int16_t* samples, size_t numSamples, size_t stride = 16) {
  if (!samples || numSamples == 0) return 0;
  int32_t peak = 0;
  for (size_t i = 0; i < numSamples; i += stride) {
    int32_t s = samples[i];
    int32_t v = (s < 0) ? -s : s;
    if (v > peak) peak = v;
  }
  if (peak > 32767) peak = 32767;
  return (uint8_t)((uint32_t)peak * 100 / 32767);
}
