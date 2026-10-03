#include "player.h"
#include <SD.h>
#include <M5Unified.h>
#include "sd_files.h"
#include "ui_common.h"
#include "list_sel.h"
#include "audio_common.h"

// ---- 文件列表 ----
static const int MAX_WAV  = 48;
static const int PLAYER_VIS = 4;   // 屏幕上最多同时显示的文件行数

static String wavPaths[MAX_WAV];   // 完整路径
static String wavNames[MAX_WAV];   // 仅文件名（显示用）
static int    wavCount = 0;
static int    wavIdx   = 0;   // 当前光标位置
static int    wavTop   = 0;   // 列表滚动偏移

// ---- 流式播放状态 ----
// 8KB 单块双缓冲（16KB 堆空间，退出时释放），在 44.1kHz 双声道下单块裕量达 ~46.4ms，
// 配合底层 2 格队列可吸纳近 93ms 的 SD 卡 SPI 读取抖动，彻底消除偶发爆音/卡顿。
static const int  BUF_BYTES   = 8192;
static uint8_t    (*audioBuf)[BUF_BYTES] = nullptr;
static const uint8_t PLAYER_CH = 0;     // 固定 0 号虚拟通道，好查它的队列深度

static File       wavFile;
static uint32_t   wavSampleRate  = 44100;
static bool       wavStereo      = false;
static uint32_t   wavBytesPerFrame = 2;  // channels * 2 (16-bit)
static uint32_t   wavDataLen     = 0;    // data chunk 字节数
static uint32_t   wavDataPos     = 0;    // 已读字节数（进度）
static int        wavPlayingIdx  = -1;   // 当前正在播放的文件索引（-1=未播放）

static volatile bool     playerRunning   = false;
static volatile bool     playerStopReq   = false;
static volatile bool     playerAutoNext  = false; // 自然播完自动连播标记
static volatile uint8_t  playerVuLevel   = 0;     // 实时抽样电平 (0..100)
static bool              playerMuted     = false;
static TaskHandle_t volatile playerTaskH = nullptr;

// ---- WAV header 解析 ----
static bool parseWav(File& f) {
  uint8_t hdr[12];
  if (f.read(hdr, 12) < 12) return false;
  if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

  uint16_t channels = 1, bitsPerSample = 16;
  uint32_t sampleRate = 44100, dataLen = 0, dataStart = 0;

  // 逐块扫描，直到找到 data chunk
  while (f.available()) {
    uint8_t ch[8];
    if (f.read(ch, 8) < 8) break;
    uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                  ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);

    if (memcmp(ch, "fmt ", 4) == 0 && sz >= 16) {
      uint8_t fmt[16];
      if (f.read(fmt, 16) < 16) return false;
      uint16_t fmt_tag = fmt[0] | ((uint16_t)fmt[1] << 8);
      if (fmt_tag != 1) return false;   // PCM only
      channels      = fmt[2] | ((uint16_t)fmt[3] << 8);
      sampleRate    = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                      ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      bitsPerSample = fmt[14] | ((uint16_t)fmt[15] << 8);
      if (sz > 16) f.seek(f.position() + sz - 16);
    } else if (memcmp(ch, "data", 4) == 0) {
      dataStart = f.position();
      dataLen   = sz;
      break;
    } else {
      // 跳过未知 chunk（chunk 大小按字对齐）
      f.seek(f.position() + sz + (sz & 1));
    }
  }
  if (dataStart == 0 || dataLen == 0) return false;
  if (bitsPerSample != 16) return false;   // 只支持 16-bit PCM
  if (channels < 1 || channels > 2) return false;

  // 边界防御：部分录音文件 dataLen 会被填为 0xFFFFFFFF，按物理文件实际剩余大小夹紧
  uint32_t maxPossibleData = (f.size() > dataStart) ? (f.size() - dataStart) : 0;
  if (dataLen > maxPossibleData) dataLen = maxPossibleData;
  if (dataLen == 0) return false;

  wavSampleRate    = sampleRate;
  wavStereo        = (channels == 2);
  wavBytesPerFrame = channels * 2;
  wavDataLen       = dataLen;
  wavDataPos       = 0;
  return true;
}

// ---- 后台流式任务（双缓冲 + Core 0 独立亲和性）----
static void wavStreamTask(void* arg) {
  int readBuf  = 0;
  bool started = false;

  // 按缓冲播放时长计算超时（至少 2 倍缓冲时长），避免 8kHz 等低码率下一个缓冲播 512ms 超出默认 500ms
  uint32_t bytesPerSec = wavSampleRate * wavBytesPerFrame;
  uint32_t bufDurationMs = bytesPerSec ? ((uint32_t)BUF_BYTES * 1000UL / bytesPerSec) : 500;
  uint32_t queueTimeout = bufDurationMs * 2 + 500;

  while (!playerStopReq) {
    uint32_t remaining = (wavDataLen > wavDataPos) ? (wavDataLen - wavDataPos) : 0;
    if (remaining == 0) break;

    // 等待 M5Unified 播放通道有空位（队列深度 < 2）
    if (started) {
      if (!audioWaitQueueSpace(AUDIO_PLAY_CH, (volatile bool*)&playerStopReq, pdMS_TO_TICKS(2), queueTimeout)) break;
    }
    if (playerStopReq) break;

    // 读入缓冲区（此时 audioBuf[readBuf] 已确认不在硬件队列中）
    uint32_t toRead = (remaining < BUF_BYTES) ? remaining : BUF_BYTES;
    int n = wavFile.read(audioBuf[readBuf], (size_t)toRead);
    if (n <= 0) break;
    wavDataPos += (uint32_t)n;

    // 提取当前音频振幅峰值 (0..100) 用于实时动效
    playerVuLevel = audioCalcPeak((const int16_t*)audioBuf[readBuf], (size_t)(n / 2), 16);

    // 追加到播放队列，无缝播放
    M5.Speaker.playRaw((const int16_t*)audioBuf[readBuf],
                       (size_t)(n / 2),
                       wavSampleRate,
                       wavStereo,
                       1, AUDIO_PLAY_CH, /*stop_current_sound=*/false);
    started  = true;
    readBuf ^= 1;
  }

  // 等待尾部残留缓冲区播完
  audioDrainAndStop(AUDIO_PLAY_CH, (volatile bool*)&playerStopReq, 5000);
  wavFile.close();
  // 自然播完且未被用户手动中断：请求自动连播下一首。必须在交还句柄之前判：
  // 句柄一清空，主线程就会把 playerStopReq 复位成 false，再判就会把"手动停"误当成"播完"。
  bool finishedNaturally = !playerStopReq && (wavDataLen == 0 || wavDataPos >= wavDataLen);
  playerRunning = false;
  playerVuLevel = 0;
  if (finishedNaturally) playerAutoNext = true;
  dirty = true;
  playerTaskH = nullptr;   // 最后一步：主线程看到 nullptr 才接管 wavFile/audioBuf
  vTaskDelete(nullptr);
}

// ---- 扫描目录 ----
static void scanDir(const char* path) {
  sdScanFiles(path, ".wav", [](File& /*f*/, const String& baseName, const String& fullPath) -> bool {
    if (wavCount >= MAX_WAV) return false;
    wavPaths[wavCount] = fullPath;
    wavNames[wavCount] = baseName;
    wavCount++;
    return true;
  });
}

// 协作式停止：只等，不强删。任务可能正卡在 wavFile.read() 里持有 FATFS 锁，强删之后
// 整张 SD 卡的访问都会永久卡死。超时返回 false，调用方就不能再碰 wavFile/audioBuf。
static bool stopPlayerTask(uint32_t timeoutMs) {
  playerStopReq = true;
  playerAutoNext = false;
  uint32_t t0 = millis();
  while (playerTaskH && millis() - t0 < timeoutMs) delay(5);
  if (playerTaskH) return false;
  playerAutoNext = false;   // 任务收尾时可能刚判完"自然播完"，这里再清一次
  return true;
}

// ---- 公开接口 ----
void playerEnter() {
  wavCount = 0;
  wavIdx   = 0;
  wavTop   = 0;
  playerMuted = false;
  if (!sdReady()) return;
  if (!audioBuf) audioBuf = (uint8_t(*)[BUF_BYTES])malloc(2 * BUF_BYTES);
  // 先找 /music/，再找根目录
  scanDir("/music");
  if (wavCount == 0) scanDir("/");
  // 按文件名自然排序
  for (int i = 0; i < wavCount - 1; i++) {
    for (int j = i + 1; j < wavCount; j++) {
      if (wavNames[i] > wavNames[j]) {
        String tn = wavNames[i]; wavNames[i] = wavNames[j]; wavNames[j] = tn;
        String tp = wavPaths[i]; wavPaths[i] = wavPaths[j]; wavPaths[j] = tp;
      }
    }
  }
}

void playerExit() {
  bool gone = stopPlayerTask(1500);
  M5.Speaker.stop();
  if (playerMuted) {
    playerMuted = false;
    M5.Speaker.setVolume(volVal());
  }
  wavPlayingIdx = -1;
  if (!gone) return;   // 任务还在收尾：文件和缓冲区留给它，audioBuf 下次进来复用
  playerRunning  = false;
  playerStopReq  = false;
  playerVuLevel  = 0;
  if (wavFile) wavFile.close();
  wavPlayingIdx  = -1;
  if (audioBuf) {
    free(audioBuf);
    audioBuf = nullptr;
  }
}

bool playerIsPlaying() { return playerRunning; }
uint8_t playerGetVuLevel() { return playerVuLevel; }

static void startPlayback(int idx) {
  // 停止当前播放（等待正在运行的任务退出，杜绝文件句柄与任务竞态）
  if (playerRunning || playerTaskH) {
    if (!stopPlayerTask(1000)) return;   // 旧任务没退出就不能复用 wavFile/audioBuf
    M5.Speaker.stop();
    playerRunning = false;
    playerStopReq = false;
    playerVuLevel = 0;
    if (wavFile) wavFile.close();
  }

  if (!audioBuf) audioBuf = (uint8_t(*)[BUF_BYTES])malloc(2 * BUF_BYTES);
  if (!audioBuf) return;

  wavFile = SD.open(wavPaths[idx].c_str(), FILE_READ);
  if (!wavFile) return;
  if (!parseWav(wavFile)) { wavFile.close(); return; }

  wavPlayingIdx = idx;
  playerRunning = true;
  playerStopReq = false;
  playerAutoNext = false;

  // 物理双核硬隔离：固定在 Core 0 运行，优先级 3（高于 Core 1 上的 UI 渲染与按键轮询）
  if (xTaskCreatePinnedToCore(wavStreamTask, "wav", 4096, nullptr, 3, const_cast<TaskHandle_t*>(&playerTaskH), 0) != pdPASS) {
    playerRunning = false;
    playerTaskH = nullptr;
    wavPlayingIdx = -1;
    wavFile.close();
    M5.Speaker.stop();
  }
}

void playerUpdate() {
  if (playerAutoNext) {
    playerAutoNext = false;
    if (wavCount > 0 && wavPlayingIdx >= 0) {
      int nextIdx = (wavPlayingIdx + 1) % wavCount;
      wavIdx = nextIdx;
      listClampScroll(wavIdx, wavTop, wavCount, 3);
      startPlayback(nextIdx);
      dirty = true;
      return;
    }
  }
  if (playerRunning) {
    static uint32_t lastUpdateMs = 0;
    if (millis() - lastUpdateMs >= 80) { // ~12.5 FPS 平滑进度与 VU 表
      lastUpdateMs = millis();
      dirty = true;
    }
  }
}

void playerKey(char k) {
  // 音量快速调节（5% 步进）
  if (k == '[' || k == ']') {
    int v = volPct + (k == ']' ? 5 : -5);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (playerMuted) playerMuted = false;
    volSet(v);
    dirty = true;
    return;
  }
  // 静音快速切换
  if (k == 'm' || k == 'M') {
    playerMuted = !playerMuted;
    M5.Speaker.setVolume(playerMuted ? 0 : volVal());
    dirty = true;
    return;
  }

  if (wavCount == 0) return;

  int vis = (wavPlayingIdx >= 0) ? 3 : 4;
  if (k == ';' || k == ',') {
    listMove(wavIdx, wavTop, wavCount, vis, -1);
    dirty = true;
  } else if (k == '.' || k == '/') {
    listMove(wavIdx, wavTop, wavCount, vis, 1);
    dirty = true;
  } else if (k == '\n') {
    if (playerRunning && wavPlayingIdx == wavIdx) {
      // 再次 Enter：停止当前播放
      bool gone = stopPlayerTask(1000);
      M5.Speaker.stop();
      wavPlayingIdx = -1;
      dirty = true;
      if (!gone) return;
      playerRunning = false;
      playerStopReq = false;
      playerVuLevel = 0;
      if (wavFile) wavFile.close();
      wavPlayingIdx = -1;
    } else {
      startPlayback(wavIdx);
    }
    dirty = true;
  }
}

void drawPlayer() {
  cv.fillScreen(TFT_BLACK);

  // 1. 顶栏标准化
  const char* st = playerRunning ? "PLAYING" : "STOPPED";
  uint16_t stCol = playerRunning ? ACCENT : 0x9CD3;
  drawPageHeader("Player", st, stCol);

  // 2. 空状态 / 无 SD 卡 / 无 WAV
  if (!sdReady() || wavCount == 0) {
    const int cx = SW / 2;
    cv.fillRoundRect(16, 28, 208, 76, 5, 0x0821);
    cv.drawRoundRect(16, 28, 208, 76, 5, 0x18C3);

    cv.setTextDatum(middle_center);
    cv.setTextSize(2);
    if (!sdReady()) {
      cv.setTextColor(0xFBE0, 0x0821);
      cv.drawString("NO SD CARD", cx, 48);
      cv.setTextSize(1);
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString("Insert FAT32 MicroSD card", cx, 70);
      cv.setTextColor(0x4208, 0x0821);
      cv.drawString("Place 16-bit PCM .wav on SD root", cx, 86);
    } else {
      cv.setTextColor(0x07FF, 0x0821);
      cv.drawString("NO AUDIO", cx, 48);
      cv.setTextSize(1);
      cv.setTextColor(0x7BEF, 0x0821);
      cv.drawString("Copy .wav files to SD card root", cx, 70);
      cv.setTextColor(0x4208, 0x0821);
      cv.drawString("Supports 16-bit PCM (mono/stereo)", cx, 86);
    }

    // 底部按键提示
    cv.setTextDatum(top_left);
    cv.setTextSize(1);
    cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 10, SH - 14);
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" back", 18, SH - 14);
    return;
  }

  // 3. 正在播放时的顶部状态卡片
  int listY = 16;
  int visCount = (wavPlayingIdx >= 0) ? 3 : 4;
  if (wavPlayingIdx >= 0) {
    const int cardX = 8, cardW = 224;
    const int playH = 43;
    cv.fillRoundRect(cardX, 16, cardW, playH, 4, 0x0821);
    cv.drawRoundRect(cardX, 16, cardW, playH, 4, playerRunning ? 0x2492 : 0x18C3);

    // 文件名与格式
    cv.setTextSize(1);
    cv.setTextDatum(top_left);
    cv.setTextColor(playerRunning ? ACCENT : 0x9CD3, 0x0821);
    String fn = wavNames[wavPlayingIdx];
    if (fn.endsWith(".wav") || fn.endsWith(".WAV")) fn = fn.substring(0, fn.length() - 4);
    if (fn.length() > 22) fn = fn.substring(0, 21) + "~";
    cv.drawString((playerRunning ? "> " : "|| ") + fn, cardX + 6, 20);

    // 右侧音频参数角标 (如 44kHz 2ch)
    cv.setTextDatum(top_right);
    char fmtBuf[16];
    snprintf(fmtBuf, sizeof(fmtBuf), "%ukHz %s", (unsigned)(wavSampleRate / 1000), wavStereo ? "2ch" : "1ch");
    cv.setTextColor(0x7BEF, 0x0821);
    cv.drawString(fmtBuf, cardX + cardW - 6, 20);

    // 进度条
    const int bx = cardX + 8, bw = cardW - 16, by = 32, bh = 5;
    cv.fillRoundRect(bx, by, bw, bh, 2, 0x1082);
    if (wavDataLen > 0) {
      int fill = (int)((float)wavDataPos / wavDataLen * (bw - 2));
      if (fill > 0) cv.fillRoundRect(bx + 1, by + 1, fill, bh - 2, 2, playerRunning ? ACCENT : 0x07FF);
    }

    // 时间显示
    cv.setTextDatum(top_left);
    cv.setTextColor(0x7BEF, 0x0821);
    if (wavBytesPerFrame > 0 && wavSampleRate > 0) {
      uint32_t elapsedSec = wavDataPos / wavBytesPerFrame / wavSampleRate;
      uint32_t totalSec   = wavDataLen  / wavBytesPerFrame / wavSampleRate;
      char tb[32];
      snprintf(tb, sizeof(tb), "%u:%02u/%u:%02u", (unsigned)(elapsedSec / 60), (unsigned)(elapsedSec % 60),
               (unsigned)(totalSec / 60), (unsigned)(totalSec % 60));
      cv.drawString(tb, cardX + 8, 41);

      // 音量状态
      char vb[16];
      snprintf(vb, sizeof(vb), "V:%d%%", volPct);
      cv.drawString(vb, cardX + 78, 41);

      // 实时动态微型 VU 电平柱 (6 柱，响应音乐峰值跳动)
      drawMiniVuMeter(cardX + 120, 41, playerVuLevel, playerRunning, ACCENT, 0x2104);

      // 百分比
      cv.setTextDatum(top_right);
      int pct = (wavDataLen > 0) ? (int)((uint64_t)wavDataPos * 100 / wavDataLen) : 0;
      char pb[12]; snprintf(pb, sizeof(pb), "%d%%", pct);
      cv.drawString(pb, cardX + cardW - 8, 41);
    }

    listY = 62;
  }

  // 4. 文件列表卡片
  int availH = (SH - 16) - listY;
  int rowH = availH / visCount;
  if (rowH < 14) rowH = 14;

  for (int i = 0; i < visCount && (wavTop + i) < wavCount; i++) {
    int idx = wavTop + i;
    int y   = listY + i * rowH;
    bool sel     = (idx == wavIdx);
    bool playing = (idx == wavPlayingIdx && playerRunning);

    uint16_t bg  = sel ? 0x1124 : 0x0821;
    uint16_t bdr = sel ? 0x07FF : 0x18C3;

    cv.fillRoundRect(8, y, SW - 16, rowH - 2, 3, bg);
    cv.drawRoundRect(8, y, SW - 16, rowH - 2, 3, bdr);

    cv.setTextSize(1);
    cv.setTextDatum(middle_left);

    // 编号胶囊
    char numBuf[8];
    snprintf(numBuf, sizeof(numBuf), "%02d", idx + 1);
    cv.setTextColor(sel ? 0x07FF : 0x632C, bg);
    cv.drawString(numBuf, 14, y + rowH / 2 - 1);

    // 文件名
    String name = wavNames[idx];
    if (name.endsWith(".wav") || name.endsWith(".WAV")) name = name.substring(0, name.length() - 4);
    if (name.length() > 24) name = name.substring(0, 23) + "~";

    cv.setTextColor(playing ? ACCENT : (sel ? TFT_WHITE : 0xCE79), bg);
    cv.drawString(name, 36, y + rowH / 2 - 1);

    // 状态标记
    if (playing) {
      cv.setTextDatum(middle_right);
      cv.setTextColor(ACCENT, bg);
      cv.drawString("PLAY", SW - 14, y + rowH / 2 - 1);
    }
  }

  // 滚动条
  drawScrollBar(SW - 6, listY, visCount * rowH, wavTop, wavCount, visCount, ACCENT, 0x1082);

  // 5. 底部按键提示
  cv.setTextDatum(top_left);
  cv.setTextSize(1);
  const int footY = SH - 14;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("Enter", 6, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" play", 34, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString(";.", 66, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" sel", 78, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("[]", 108, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" vol", 120, footY);

  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", 152, footY);
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString(" back", 158, footY);
}
