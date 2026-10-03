#include "radio.h"
#include "wifi_net.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <new>
#include <atomic>
#include <AudioFileSource.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include "ui_common.h"
#include "list_sel.h"
#include "audio_common.h"
#include "sd_files.h"
#include "config.h"

static volatile uint8_t gRadioVuLevel = 0; // 实时电平 (0..100)

// ---- 预置电台（全部为公开 HTTP MP3 直链，无需认证）----
// 内置列表在 config.h（CFG_RADIO_PRESETS）；SD 卡 /radio.txt 存在时整体替换（见 loadStations）
struct Station { const char* name; const char* url; const char* genre; };
static const Station BUILTIN_PRESETS[] = {
  CFG_RADIO_PRESETS
  {"[Custom URL]",   "",                                           "User Defined"},
};
static const int BUILTIN_COUNT = sizeof(BUILTIN_PRESETS) / sizeof(BUILTIN_PRESETS[0]);

static const int SD_ST_MAX = 12;
struct SdStation { char name[20]; char url[88]; char genre[16]; };
static SdStation sdStations[SD_ST_MAX];
static Station presetsSd[SD_ST_MAX + 1];

static const Station* PRESETS = BUILTIN_PRESETS;
static int STATION_COUNT = BUILTIN_COUNT;
static int CUSTOM_IDX    = BUILTIN_COUNT - 1;

// 读 SD 卡 /radio.txt：每行 "名称|URL|风格"（风格可省），# 注释。至少一条有效才替换内置列表。
static void loadStations() {
  PRESETS = BUILTIN_PRESETS; STATION_COUNT = BUILTIN_COUNT; CUSTOM_IDX = BUILTIN_COUNT - 1;
  if (!sdReady() || !SD.exists("/radio.txt")) return;
  File f = SD.open("/radio.txt", FILE_READ);
  if (!f) return;
  int n = 0;
  while (f.available() && n < SD_ST_MAX) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    int p1 = line.indexOf('|');
    if (p1 <= 0) continue;
    int p2 = line.indexOf('|', p1 + 1);
    String name = line.substring(0, p1);
    String url  = (p2 < 0) ? line.substring(p1 + 1) : line.substring(p1 + 1, p2);
    String genre = (p2 < 0) ? String("") : line.substring(p2 + 1);
    name.trim(); url.trim(); genre.trim();
    if (!name.length() || !url.startsWith("http://") || url.length() >= sizeof(sdStations[0].url)) continue;
    snprintf(sdStations[n].name, sizeof(sdStations[n].name), "%s", name.c_str());
    snprintf(sdStations[n].url, sizeof(sdStations[n].url), "%s", url.c_str());
    snprintf(sdStations[n].genre, sizeof(sdStations[n].genre), "%s", genre.length() ? genre.c_str() : "Custom");
    presetsSd[n] = { sdStations[n].name, sdStations[n].url, sdStations[n].genre };
    n++;
  }
  f.close();
  if (n == 0) return;
  presetsSd[n] = BUILTIN_PRESETS[BUILTIN_COUNT - 1];   // 末尾仍是 [Custom URL]
  PRESETS = presetsSd; STATION_COUNT = n + 1; CUSTOM_IDX = n;
}

// ---- 全局状态 ----
String radioCustomUrl  = "";
static volatile bool radioRunning = false;
static int radioStIdx   = 0;   // 列表光标
static int radioStTop   = 0;   // 列表滚动偏移

// gTask 与 radioPlayIdx 由后台音频任务与主循环跨线程共享，
// 必须 volatile 避免编译器寄存器缓存导致主循环无法感知任务退出。
static TaskHandle_t volatile gTask = nullptr;
static volatile int radioPlayIdx = -1;  // 正在播放的台索引（-1 = 停止）

enum RStatus { RS_STOPPED, RS_CONNECTING, RS_PLAYING, RS_STOPPING, RS_ERROR };
static volatile RStatus rStatus = RS_STOPPED;
static char rErrMsg[32] = "";
static portMUX_TYPE radioStateMux = portMUX_INITIALIZER_UNLOCKED;

// 挂起动作：异步停止后在任务彻底注销时由主循环 radioUpdate() 补做。
// 遵循后到覆盖先到原则，同一时刻至多存在一个挂起动作。
enum RadioPendingAction {
  PENDING_NONE = 0,
  PENDING_START_STATION, // 换台：旧任务退出后立即开启新台
  PENDING_TO_LIST,       // 停止并回列表页：旧任务退出后恢复画布并切到 SCREEN_RADIO
  PENDING_TO_URL,        // 停止并去 URL 页：旧任务退出后恢复画布并切到 SCREEN_RADIO_URL
  PENDING_RESTORE_CANVAS // 离开 App (radioExit 后)：旧任务退出后仅恢复画布
};
static volatile RadioPendingAction pendingAction = PENDING_NONE;
static volatile int pendingStationIdx = -1;

// 错误信息会由音频任务写、UI 任务读。String 在这里不能靠 volatile 保证安全，
// 使用固定缓冲区避免后台任务分配堆内存，也避免 UI 读到一半被改写。
static void setRadioError(const char* msg) {
  portENTER_CRITICAL(&radioStateMux);
  strncpy(rErrMsg, msg ? msg : "error", sizeof(rErrMsg) - 1);
  rErrMsg[sizeof(rErrMsg) - 1] = '\0';
  portEXIT_CRITICAL(&radioStateMux);
}

static void getRadioError(char* out, size_t n) {
  if (!out || n == 0) return;
  portENTER_CRITICAL(&radioStateMux);
  strncpy(out, rErrMsg, n - 1);
  out[n - 1] = '\0';
  portEXIT_CRITICAL(&radioStateMux);
}

// ---- M5.Speaker 音频输出适配器 ----
// ESP8266Audio 通过 ConsumeSample() 逐样本回调，我们在此积攒到 FRAMES 个 frame
// 后批量送给 M5.Speaker.playRaw()，用双缓冲避免间隙。
static volatile bool gRadioStopReq = false;  // 供内部类访问的停止标志副本
static const uint8_t RADIO_CH = 0;           // 固定用 0 号虚拟通道，好查它的队列深度

class RadioOut : public AudioOutput {
public:
  RadioOut() { channels = 2; hertz = 44100; wpos_ = 0; flip_ = 0; }

  bool begin() override { wpos_ = 0; return true; }

  bool ConsumeSample(int16_t sample[2]) override {
    buf_[flip_][wpos_ * 2]     = sample[0];
    buf_[flip_][wpos_ * 2 + 1] = (channels == 2) ? sample[1] : sample[0];
    wpos_++;
    if (wpos_ >= FRAMES) flush();
    return !gRadioStopReq;
  }

  bool stop() override {
    if (wpos_ > 0) flush();
    audioDrainAndStop(AUDIO_PLAY_CH, (volatile bool*)&gRadioStopReq, 500);
    gRadioVuLevel = 0;
    return true;
  }

  // ESP8266Audio 的 AudioOutput 只有 hertz/channels/gain 三个量，没有 bitsPerSample——
  // 解码器出来的一律是 int16，所以这里也不需要它。
  bool SetRate(int hz)    override { hertz = hz; return true; }
  bool SetChannels(int c) override { channels = c; return true; }

private:
  // 1024 frame @44.1kHz ≈ 23ms 一块，两块排上队就有 ~46ms 缓冲，够吃解码抖动。
  // 每块 4KB，两块 8KB 堆——这板子没 PSRAM，别再往上加了。
  static const int FRAMES = 1024;
  int16_t buf_[2][FRAMES * 2];     // 两块交替用，配合下面那个 2 格队列
  int wpos_ = 0;
  int flip_ = 0;

  void flush() {
    if (!gRadioStopReq) {
      gRadioVuLevel = audioCalcPeak(buf_[flip_], (size_t)(wpos_ * 2), 16);
      M5.Speaker.playRaw(buf_[flip_], (size_t)(wpos_ * 2),
                         (uint32_t)hertz, true, 1, AUDIO_PLAY_CH, /*stop_current_sound=*/false);
    }
    flip_ ^= 1;
    wpos_  = 0;

    audioWaitQueueSpace(AUDIO_PLAY_CH, (volatile bool*)&gRadioStopReq, pdMS_TO_TICKS(1));
  }
};

// ESP8266Audio 1.9.9 的 AudioGeneratorMP3 构造函数只把 buff 置空，stream/frame/synth 留着野值，
// 析构却四个都 free —— begin() 成功分配之前删掉它（HTTP 打不开、预缓冲超时这些失败路径）
// 就是 free 野指针，assert 重启。低内存下 HTTP 打不开最常见，所以表现成"堆碎了进 Radio 必重启"
// （实测回溯：cleanupPipeline -> ~AudioGeneratorMP3 -> free: pointer outside heap areas）。
// 上游 earlephilhower/ESP8266Audio #715 / PR #716，2.0.0 修复；但 2.3.0 起要 IDF 5，我们锁在 1.9.9，只能自己补。
class SafeMP3 : public AudioGeneratorMP3 {
 public:
  SafeMP3() : AudioGeneratorMP3() { stream = nullptr; frame = nullptr; synth = nullptr; }
  SafeMP3(void *buff, int buffSize, void *stream, int streamSize, void *frame, int frameSize, void *synth, int synthSize)
    : AudioGeneratorMP3(buff, buffSize, stream, streamSize, frame, frameSize, synth, synthSize) {
    this->stream = nullptr;
    this->frame = nullptr;
    this->synth = nullptr;
  }
};

// 自定义 HTTP 流源：基于标准 WiFiClient + HTTPClient 实现 AudioFileSource 接口。
// 库原生的 AudioFileSourceHTTPStream 构造即阻塞且无法配置超时（默认 5s），
// 这里显式设置 1500ms 连接与读取超时，并在握手、数据读取各环节响应 gRadioStopReq，
// 保证停止与切台操作能在 1~2 秒内获得后台任务响应。
class RadioHttpStream : public AudioFileSource {
public:
  explicit RadioHttpStream(const volatile bool* stopFlag = nullptr) : stopFlag_(stopFlag) {
    pos_ = 0;
    size_ = 0;
    saveURL_[0] = '\0';
  }

  virtual ~RadioHttpStream() override {
    close();
  }

  bool open(const char* url) override {
    pos_ = 0;
    size_ = 0;
    if (stopFlag_ && *stopFlag_) return false;
    if (!url || !url[0]) return false;

    strncpy(saveURL_, url, sizeof(saveURL_) - 1);
    saveURL_[sizeof(saveURL_) - 1] = '\0';

    http_.begin(client_, url);
    http_.setReuse(true);
    // 缩短 TCP 连接与读超时为 1500ms，防止网络不通或慢速响应时长时间阻塞后台任务
    http_.setConnectTimeout(1500);
    http_.setTimeout(1500);

    if (stopFlag_ && *stopFlag_) {
      http_.end();
      return false;
    }

    int code = http_.GET();
    if ((stopFlag_ && *stopFlag_) || code != HTTP_CODE_OK) {
      http_.end();
      return false;
    }

    size_ = http_.getSize();
    return true;
  }

  uint32_t read(void* data, uint32_t len) override {
    return readInternal(data, len, false);
  }

  uint32_t readNonBlock(void* data, uint32_t len) override {
    return readInternal(data, len, true);
  }

  bool close() override {
    http_.end();
    return true;
  }

  bool isOpen() override {
    return http_.connected();
  }

  uint32_t getSize() override {
    return (uint32_t)(size_ > 0 ? size_ : 0);
  }

  uint32_t getPos() override {
    return pos_;
  }

  bool seek(int32_t, int) override {
    return false;
  }

private:
  uint32_t readInternal(void* data, uint32_t len, bool nonBlock) {
    if (stopFlag_ && *stopFlag_) return 0;
    if (!http_.connected()) return 0;
    if (size_ > 0 && (int)pos_ >= size_) return 0;
    if (!data || len == 0) return 0;

    WiFiClient* stream = http_.getStreamPtr();
    if (!stream) return 0;

    if (size_ > 0 && len > (uint32_t)(size_ - pos_)) {
      len = (uint32_t)(size_ - pos_);
    }

    if (!nonBlock) {
      uint32_t start = millis();
      while ((stream->available() < (int)len) && (millis() - start < 500)) {
        if (stopFlag_ && *stopFlag_) return 0;
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    }

    size_t avail = stream->available();
    if (!avail) return 0;
    if (avail < len) len = avail;

    int bytesRead = stream->read(reinterpret_cast<uint8_t*>(data), len);
    if (bytesRead > 0) {
      pos_ += bytesRead;
      return (uint32_t)bytesRead;
    }
    return 0;
  }

  WiFiClient client_;
  HTTPClient http_;
  uint32_t pos_;
  int size_;
  char saveURL_[128];
  const volatile bool* stopFlag_;
};

// ---- Audio pipeline 对象（堆分配，避免占用静态 BSS）----
static RadioHttpStream*           gHttp = nullptr;
static AudioFileSourceBuffer*     gBuf  = nullptr;
static AudioGeneratorMP3*         gMp3  = nullptr;
static RadioOut*                  gOut  = nullptr;
// 源缓冲自己申请再交给库：库的 AudioFileSourceBuffer(in, bytes) 构造里 malloc 失败只打一行日志、
// 照样返回对象，内部缓冲是空指针，第一次 loop() 往里写就崩（最大连续块 ~22K 时进 Radio 必重启）。
static uint8_t*                   gBufMem = nullptr;
// MP3 解码器大块外部预分配指针：
// 趁画布刚释放、堆最整洁时按"先大后小"顺序申请（mad_frame 20.8KB 最先），避免被小对象碎化。
static uint8_t*                   gMadFrameMem  = nullptr;
static uint8_t*                   gMadSynthMem  = nullptr;
static uint8_t*                   gMadBuffMem   = nullptr;
static uint8_t*                   gMadStreamMem = nullptr;

static volatile bool              gMp3Started = false;
static char                       gUrl[128] = "";
static const uint32_t RADIO_BUF_BYTES = 16 * 1024;
static const uint32_t RADIO_PREBUFFER_BYTES = 8 * 1024;
static const uint32_t RADIO_PREBUFFER_TIMEOUT_MS = 2500;
static volatile uint8_t gBufferPct = 0;
static bool radioMuted = false;

static void cleanupPipeline(bool stopDecoder) {
  if (gMp3) {
    if (stopDecoder && gMp3Started) gMp3->stop();
    delete gMp3;
    gMp3 = nullptr;
  }
  if (gOut)  { delete gOut;  gOut  = nullptr; }
  if (gBuf)  { gBuf->close(); delete gBuf; gBuf = nullptr; }
  if (gBufMem) { free(gBufMem); gBufMem = nullptr; }   // 预分配构造不归库释放，得在 gBuf 删掉之后自己还
  if (gHttp) { gHttp->close(); delete gHttp; gHttp = nullptr; }
  if (gMadFrameMem)  { free(gMadFrameMem);  gMadFrameMem  = nullptr; }
  if (gMadSynthMem)  { free(gMadSynthMem);  gMadSynthMem  = nullptr; }
  if (gMadBuffMem)   { free(gMadBuffMem);   gMadBuffMem   = nullptr; }
  if (gMadStreamMem) { free(gMadStreamMem); gMadStreamMem = nullptr; }
  gMp3Started = false;
  gBufferPct = 0;
}

// ---- 后台音频任务 ----
static void audioTask(void*) {
  uint32_t prebufferStart = 0;
  // HTTP GET 和 MP3 初始化都放在后台，避免按下播放键时阻塞 UI 主循环。
  // 大块分配顺序调整为"先大后小"：
  // 1. mad_frame 约 20.8KB（最大整块，最先要）
  // 2. 源缓冲 16KB（次大整块）
  // 3. mad_synth 约 6.2KB
  // 4. RadioOut 约 8KB
  // 5. MP3 解码器小缓冲：buff 约 1.5KB，stream 约 0.8KB
  // 等小对象和 HTTP 握手切碎了堆再要大块就拿不到了。new 一律 nothrow。
  gMadFrameMem  = (uint8_t*)malloc(AudioGeneratorMP3::preAllocFrameSize());
  gBufMem       = (uint8_t*)malloc(RADIO_BUF_BYTES);
  gMadSynthMem  = (uint8_t*)malloc(AudioGeneratorMP3::preAllocSynthSize());
  gOut          = new (std::nothrow) RadioOut();
  gMadBuffMem   = (uint8_t*)malloc(AudioGeneratorMP3::preAllocBuffSize());
  gMadStreamMem = (uint8_t*)malloc(AudioGeneratorMP3::preAllocStreamSize());

  if (!gMadFrameMem || !gBufMem || !gMadSynthMem || !gOut || !gMadBuffMem || !gMadStreamMem) {
    setRadioError("low memory");
    rStatus = RS_ERROR;
    goto finished;
  }

  if (gRadioStopReq) {
    goto finished;
  }

  gHttp = new (std::nothrow) RadioHttpStream(&gRadioStopReq);
  if (!gHttp) {
    setRadioError("low memory");
    rStatus = RS_ERROR;
    goto finished;
  }

  if (gRadioStopReq) {
    goto finished;
  }

  if (!gHttp->open(gUrl)) {
    if (!gRadioStopReq) {
      setRadioError("http open failed");
      rStatus = RS_ERROR;
    }
    goto finished;
  }

  if (gRadioStopReq) {
    goto finished;
  }

  gBuf  = new (std::nothrow) AudioFileSourceBuffer(gHttp, gBufMem, RADIO_BUF_BYTES);
  gMp3  = gBuf ? new (std::nothrow) SafeMP3(
            gMadBuffMem,   AudioGeneratorMP3::preAllocBuffSize(),
            gMadStreamMem, AudioGeneratorMP3::preAllocStreamSize(),
            gMadFrameMem,  AudioGeneratorMP3::preAllocFrameSize(),
            gMadSynthMem,  AudioGeneratorMP3::preAllocSynthSize()
          ) : nullptr;

  if (!gBuf || !gMp3) {
    setRadioError("low memory");
    rStatus = RS_ERROR;
    goto finished;
  }

  // 冷启动时先把网络抖动吸收到源缓冲里，再让解码器开始向 Speaker 送数据。
  // 8KB 对 128kbps 电台约等于 0.5 秒的音频，足以覆盖首次 Wi-Fi 调度尖峰。
  prebufferStart = millis();
  while (!gRadioStopReq && gBuf->getFillLevel() < RADIO_PREBUFFER_BYTES &&
         millis() - prebufferStart < RADIO_PREBUFFER_TIMEOUT_MS) {
    if (!gBuf->loop()) {
      if (!gRadioStopReq) {
        setRadioError("stream ended");
        rStatus = RS_ERROR;
      }
      goto finished;
    }
    uint32_t fill = gBuf->getFillLevel();
    uint32_t pct = (fill * 100) / RADIO_BUF_BYTES;
    gBufferPct = (uint8_t)(pct > 100 ? 100 : pct);
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  if (gRadioStopReq) {
    goto finished;
  }

  if (gBuf->getFillLevel() < RADIO_PREBUFFER_BYTES) {
    setRadioError("buffer timeout");
    rStatus = RS_ERROR;
    goto finished;
  }

  if (!gMp3->begin(gBuf, gOut)) {
    setRadioError("decoder init failed");
    rStatus = RS_ERROR;
    goto finished;
  }
  gMp3Started = true;
  rStatus = RS_PLAYING;

  while (!gRadioStopReq && gMp3 && gMp3->isRunning()) {
    if (!gMp3->loop()) {
      if (!gRadioStopReq) { setRadioError("stream ended"); rStatus = RS_ERROR; }
      break;
    }
    if (gBuf) {
      uint32_t fill = gBuf->getFillLevel();
      uint32_t pct = (fill * 100) / RADIO_BUF_BYTES;
      gBufferPct = (uint8_t)(pct > 100 ? 100 : pct);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }

finished:
  // 必须在音频任务内部集中释放，避免与主线程并发析构；
  // 且扬声器频道也在此处停止，音频任务是唯一的写者，不会与自己产生死锁竞争。
  cleanupPipeline(true);
  M5.Speaker.stop(AUDIO_PLAY_CH);
  radioRunning = false;
  radioPlayIdx = -1;
  WiFi.setSleep(true);

  if (rStatus == RS_PLAYING || rStatus == RS_CONNECTING || rStatus == RS_STOPPING) {
    rStatus = RS_STOPPED;
  }
  gRadioStopReq = false;

  // 确保内存回收与状态变更对主核完全可见后，最后清空任务句柄
  std::atomic_thread_fence(std::memory_order_release);
  gTask        = nullptr;
  vTaskDelete(nullptr);
}

// 协作式停止：置停止标志、设置状态为 RS_STOPPING 并立即返回，绝不在主线程自旋等待，
// 也不在此处调 M5.Speaker.stop()（防止与音频任务内部形成死锁竞争）。
static void stopCurrent() {
  if (!gTask) {
    radioRunning = false;
    radioPlayIdx = -1;
    if (rStatus != RS_ERROR) rStatus = RS_STOPPED;
    return;
  }
  gRadioStopReq = true;
  rStatus = RS_STOPPING;
}

// 状态快照与限频控制（直推 M5.Display，不分配 sprite，限频 ≤4Hz）
struct RadioPlaySnapshot {
  RStatus status;
  char errMsg[32];
  int playIdx;
  int vol;
  bool muted;
  uint8_t bufferPct;
  uint8_t vuLevel;
};

static RadioPlaySnapshot lastDrawnSnap = {RS_STOPPED, "", -1, -1, false, 0, 0};
static uint32_t radioPlayLastDrawMs = 0;
static bool radioPlayFirstDraw = true;

static bool startStation(int idx) {
  if (gTask != nullptr) {
    // 任务尚未退出，绝不能启动新任务，也不触碰画布；
    // 挂起切台动作，由 radioUpdate() 在检测到任务彻底注销后执行。
    gRadioStopReq = true;
    rStatus = RS_STOPPING;
    pendingAction = PENDING_START_STATION;
    pendingStationIdx = idx;
    return false;
  }

  const char* url = (idx == CUSTOM_IDX) ? radioCustomUrl.c_str() : PRESETS[idx].url;
  if (!url || strlen(url) == 0) { setRadioError("no URL"); rStatus = RS_ERROR; return false; }
  if (!url[0] || strncmp(url, "http://", 7) != 0) {
    setRadioError("http:// only"); rStatus = RS_ERROR; return false;
  }

  if (WiFi.status() != WL_CONNECTED) { setRadioError("no wifi"); rStatus = RS_ERROR; return false; }

  // 释放 64.8KB cv 画布，把连续内存让给解码器（~29KB）、源缓冲（16KB）和任务栈
  canvasRelease();

  // 复制 URL，避免用户在自定义 URL 页面继续编辑时改动后台任务使用的地址。
  strncpy(gUrl, url, sizeof(gUrl) - 1);
  gUrl[sizeof(gUrl) - 1] = '\0';

  rStatus  = RS_CONNECTING;
  setRadioError("");
  radioPlayIdx = idx;
  pendingAction = PENDING_NONE;

  // 重置播放页直推绘制状态
  radioPlayFirstDraw = true;

  WiFi.setSleep(false);

  gRadioStopReq = false;
  radioRunning  = true;

  if (xTaskCreatePinnedToCore(audioTask, "radio", 8192, nullptr, 3,
                              const_cast<TaskHandle_t*>(&gTask), 1) != pdPASS) {
    radioRunning = false;
    radioPlayIdx = -1;
    gRadioStopReq = false;
    WiFi.setSleep(true);
    setRadioError("task create failed");
    rStatus = RS_ERROR;
    // 创建任务失败兜底：恢复画布
    canvasRestore();
    return false;
  }
  return true;
}

// ---- 公开接口 ----
bool radioIsActive() { return (gTask != nullptr); }
uint8_t radioGetVuLevel() { return gRadioVuLevel; }

void radioEnter() {
  radioStIdx   = 0;
  radioStTop   = 0;
  pendingAction = PENDING_NONE;

  if (gTask == nullptr) {
    loadStations();   // 没在播才重载：播放任务/界面还在引用当前列表
    canvasRestore();
    rStatus      = RS_STOPPED;
    radioPlayIdx = -1;
    setRadioError("");
  }

  if (!wifiEnsureConnected()) {
    setRadioError("no wifi");
    rStatus = RS_ERROR;
  }
}

void radioExit() {
  if (gTask != nullptr) {
    gRadioStopReq = true;
    rStatus = RS_STOPPING;
    pendingAction = PENDING_RESTORE_CANVAS;

    // 适度等待任务注销（上限 200ms），绝大部分情况下任务能在此期间快速退出
    uint32_t t0 = millis();
    while (gTask && millis() - t0 < 200) {
      delay(10);
    }
  }

  if (radioMuted) {
    radioMuted = false;
    M5.Speaker.setVolume(volVal());
  }

  WiFi.setSleep(true);

  if (gTask == nullptr) {
    rStatus = RS_STOPPED;
    pendingAction = PENDING_NONE;
    canvasRestore();
  } else {
    // 任务超时尚未退出：绝不在主线程强行 canvasRestore() 或调 M5.Speaker.stop()，
    // 把画布恢复交给 loop 里的 radioUpdate() 补做。
    // 主菜单在此期间由 render() 中的 !canvasAvailable() 分支提示 "finishing request..."，避免卡死或白屏。
  }
}

static const int VIS = 5;   // 列表可见行数

void radioKey(char k) {
  if (k == '[' || k == ']') {
    if (k == '[') volPct = max(0, volPct - 5);
    else          volPct = min(100, volPct + 5);
    if (!radioMuted) M5.Speaker.setVolume(volVal());
    dirty = true;
  } else if (k == 'm' || k == 'M') {
    radioMuted = !radioMuted;
    M5.Speaker.setVolume(radioMuted ? 0 : volVal());
    dirty = true;
  } else if (k == 'r' || k == 'R') {
    if (!radioIsActive()) {
      if (startStation(radioStIdx)) {
        screen = SCREEN_RADIO_PLAY;
      }
      dirty = true;
    }
  } else if (k == 'l' || k == 'L') {
    if (screen == SCREEN_RADIO_PLAY) {
      // 离开播放页回列表：必须先停掉音频管线并等任务注销，再由 radioUpdate 恢复画布
      if (gTask != nullptr) {
        stopCurrent();
        pendingAction = PENDING_TO_LIST;
      } else {
        canvasRestore();
        screen = SCREEN_RADIO;
        rStatus = RS_STOPPED;
      }
      dirty = true;
    } else if (screen == SCREEN_RADIO) {
      if (radioIsActive() || rStatus != RS_STOPPED) {
        screen = SCREEN_RADIO_PLAY;
        dirty = true;
      }
    }
  } else if (k == ';' || k == ',') {
    listMove(radioStIdx, radioStTop, STATION_COUNT, VIS, -1);
    if (screen == SCREEN_RADIO_PLAY) {
      if (radioStIdx != CUSTOM_IDX) {
        if (gTask != nullptr) {
          stopCurrent();
          pendingAction = PENDING_START_STATION;
          pendingStationIdx = radioStIdx;
        } else {
          startStation(radioStIdx);
        }
      } else {
        if (gTask != nullptr) {
          stopCurrent();
          pendingAction = PENDING_TO_URL;
        } else {
          canvasRestore();
          screen = SCREEN_RADIO_URL;
        }
      }
    }
    dirty = true;
  } else if (k == '.' || k == '/') {
    listMove(radioStIdx, radioStTop, STATION_COUNT, VIS, 1);
    if (screen == SCREEN_RADIO_PLAY) {
      if (radioStIdx != CUSTOM_IDX) {
        if (gTask != nullptr) {
          stopCurrent();
          pendingAction = PENDING_START_STATION;
          pendingStationIdx = radioStIdx;
        } else {
          startStation(radioStIdx);
        }
      } else {
        if (gTask != nullptr) {
          stopCurrent();
          pendingAction = PENDING_TO_URL;
        } else {
          canvasRestore();
          screen = SCREEN_RADIO_URL;
        }
      }
    }
    dirty = true;
  } else if (k == '\n') {
    if (screen == SCREEN_RADIO && radioStIdx == CUSTOM_IDX) {
      screen = SCREEN_RADIO_URL; dirty = true; return;
    }
    if (screen == SCREEN_RADIO_PLAY) {
      // 播放页按 ENTER 停止当前电台并挂起返回列表
      if (gTask != nullptr) {
        stopCurrent();
        pendingAction = PENDING_TO_LIST;
      } else {
        canvasRestore();
        screen = SCREEN_RADIO;
        rStatus = RS_STOPPED;
      }
    } else {
      // 列表页按 ENTER
      if (radioIsActive() && radioPlayIdx == radioStIdx) {
        stopCurrent();
        pendingAction = PENDING_NONE;
      } else if (!radioIsActive()) {
        if (startStation(radioStIdx)) {
          screen = SCREEN_RADIO_PLAY;
        }
      } else {
        stopCurrent();
        pendingAction = PENDING_START_STATION;
        pendingStationIdx = radioStIdx;
      }
    }
    dirty = true;
  } else if (k == 'u' || k == 'U') {
    if (screen == SCREEN_RADIO_PLAY) {
      if (gTask != nullptr) {
        stopCurrent();
        pendingAction = PENDING_TO_URL;
      } else {
        canvasRestore();
        screen = SCREEN_RADIO_URL;
      }
    } else {
      screen = SCREEN_RADIO_URL;
    }
    dirty = true;
  }
}

void radioUrlKey(char k) {
  if (k == '`') { screen = SCREEN_RADIO; dirty = true; return; }
  if (k == ' ' && radioCustomUrl.length() == 0) {
    radioCustomUrl = "http://";
    dirty = true;
    return;
  }
  if (k == '\b') {
    if (radioCustomUrl.length()) radioCustomUrl.remove(radioCustomUrl.length() - 1);
    dirty = true; return;
  }
  if (k == '\n') {
    if (radioCustomUrl.length() > 7) {
      radioStIdx = CUSTOM_IDX;
      if (startStation(CUSTOM_IDX)) screen = SCREEN_RADIO_PLAY;
    } else {
      screen = SCREEN_RADIO;
    }
    dirty = true; return;
  }
  if (k >= 0x20 && k <= 0x7e && radioCustomUrl.length() < 120) {
    radioCustomUrl += k; dirty = true;
  }
}

// ---- 绘制 ----
// 顶栏状态串按等宽字体宽度截断，使用双静态缓冲区防止同一行内多次调用冲突
static const char* clipToWidth(const char* s, int maxW, int textSize = 1) {
  static char bufs[2][64];
  static uint8_t bIdx = 0;
  bIdx ^= 1;
  char* buf = bufs[bIdx];
  if (!s) s = "";
  int chW = 6 * textSize;
  int maxChars = maxW / chW;
  if (maxChars < 1) maxChars = 1;
  if (maxChars > 63) maxChars = 63;
  int n = 0;
  while (n < maxChars && s[n]) { buf[n] = s[n]; n++; }
  buf[n] = '\0';
  return buf;
}

void drawRadio() {
  cv.fillScreen(TFT_BLACK);

  // 标题栏状态
  uint16_t stCol; const char* stStr;
  char errMsg[sizeof(rErrMsg)];
  getRadioError(errMsg, sizeof(errMsg));
  switch (rStatus) {
    case RS_CONNECTING: stStr = "CONNECTING"; stCol = 0xFDA0; break;
    case RS_PLAYING:    stStr = "LIVE";       stCol = 0x07E0; break;
    case RS_STOPPING:   stStr = "STOPPING";   stCol = 0xFDA0; break;
    case RS_ERROR:      stStr = errMsg;       stCol = 0xF800; break;
    default:            stStr = "STANDBY";    stCol = 0x7BEF; break;
  }
  drawPageHeader("Web Radio", clipToWidth(stStr, 90), stCol);

  // 若当前正在播放，顶部画 6-band 动态迷你音柱与正在播放电台卡片
  int listY = 16;
  int vis = 5;
  if (radioPlayIdx >= 0 && rStatus != RS_STOPPED) {
    const int bannerH = 20;
    cv.fillRoundRect(8, 15, SW - 16, bannerH, 3, 0x0821);
    cv.drawRoundRect(8, 15, SW - 16, bannerH, 3, rStatus == RS_PLAYING ? 0x07E0 : 0x18C3);

    // 迷你跳动音柱 (6-band)
    int nowMs = millis();
    for (int i = 0; i < 6; i++) {
      int h = (rStatus == RS_PLAYING) ? (3 + ((nowMs / (80 + i * 20) + i * 2) % 5) * 2) : 3;
      cv.fillRect(14 + i * 4, 15 + (bannerH / 2) + 5 - h, 2, h, 0x07E0);
      cv.drawPixel(14 + i * 4, 15 + (bannerH / 2) + 4 - h, 0x07FF);
    }

    cv.setTextSize(1);
    cv.setTextDatum(middle_left);
    cv.setTextColor(rStatus == RS_PLAYING ? TFT_WHITE : 0xFDA0, 0x0821);
    const char* pName = (radioPlayIdx == CUSTOM_IDX) ? "Custom URL" : PRESETS[radioPlayIdx].name;
    cv.drawString(clipToWidth(pName, 120), 42, 15 + bannerH / 2);

    cv.setTextDatum(middle_right);
    cv.setTextColor(0x07FF, 0x0821);
    cv.drawString("HUD >", SW - 12, 15 + bannerH / 2);

    listY = 38;
    vis = 4;
  }

  // 电台列表卡片
  int availH = (SH - 14) - listY;
  int rowH = availH / vis;
  if (rowH < 18) rowH = 18;

  for (int i = 0; i < vis && radioStTop + i < STATION_COUNT; i++) {
    int idx = radioStTop + i;
    int y   = listY + i * rowH;
    int cardH = rowH - 2;
    bool sel     = (idx == radioStIdx);
    bool playing = (idx == radioPlayIdx && radioRunning);

    uint16_t bg  = sel ? 0x1124 : 0x0821;
    uint16_t bdr = sel ? 0x07FF : (playing ? 0x07E0 : 0x18C3);

    cv.fillRoundRect(8, y, SW - 16, cardH, 3, bg);
    cv.drawRoundRect(8, y, SW - 16, cardH, 3, bdr);
    if (sel) {
      cv.drawFastVLine(8, y + 2, cardH - 4, 0x07FF);
    }

    cv.setTextSize(1);

    // 序号胶囊
    cv.fillRoundRect(12, y + 2, 22, cardH - 4, 2, sel ? 0x0210 : 0x1082);
    cv.setTextDatum(middle_center);
    cv.setTextColor(sel ? 0x07FF : 0x7BEF, sel ? 0x0210 : 0x1082);
    char numBuf[16];
    if (idx == CUSTOM_IDX) snprintf(numBuf, sizeof(numBuf), "URL");
    else                   snprintf(numBuf, sizeof(numBuf), "%02d", idx + 1);
    cv.drawString(numBuf, 23, y + cardH / 2);

    // 电台名称
    cv.setTextDatum(middle_left);
    cv.setTextColor(playing ? 0x07E0 : (sel ? TFT_WHITE : 0xCE79), bg);
    cv.drawString(clipToWidth(PRESETS[idx].name, 76), 38, y + cardH / 2);

    // 流派标签
    cv.setTextColor(sel ? 0x7BEF : 0x4208, bg);
    cv.drawString(clipToWidth(PRESETS[idx].genre, 76), 118, y + cardH / 2);

    // 右侧标签
    cv.setTextDatum(middle_right);
    if (playing) {
      drawMiniVuMeter(SW - 44, y + cardH / 2 - 4, gRadioVuLevel, true, 0x07E0, 0x1082);
      cv.setTextColor(0x07E0, bg);
      cv.drawString("PLAY", SW - 14, y + cardH / 2);
    } else {
      cv.setTextColor(0x4208, bg);
      cv.drawString(idx == CUSTOM_IDX ? "EDIT" : "128k", SW - 14, y + cardH / 2);
    }
  }

  // 滚动条
  drawScrollBar(SW - 6, listY, vis * rowH, radioStTop, STATION_COUNT, vis, 0x07FF, 0x1082);

  // 底部按键提示
  cv.drawFastHLine(0, 121, SW, 0x1082);
  const int footY = SH - 12;
  cv.setTextDatum(top_left);
  cv.setTextSize(1);

  int fx = 6;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("RET", fx, footY); fx += 21;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("PLAY", fx, footY); fx += 29;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString(";.", fx, footY); fx += 15;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("TUNE", fx, footY); fx += 29;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("U", fx, footY); fx += 9;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("URL", fx, footY); fx += 25;
  if (radioRunning) {
    cv.setTextColor(0x07E0, TFT_BLACK); cv.drawString("L", fx, footY); fx += 9;
    cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("HUD", fx, footY); fx += 25;
  }
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", fx, footY); fx += 9;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("MENU", fx, footY);
}

// 紧急变化：状态文字、错误信息、电台、音量、静音（立即重画）
static bool radioPlayUrgentChanged(const RadioPlaySnapshot& a, const RadioPlaySnapshot& b) {
  return a.status != b.status ||
         strncmp(a.errMsg, b.errMsg, sizeof(a.errMsg)) != 0 ||
         a.playIdx != b.playIdx ||
         a.vol != b.vol ||
         a.muted != b.muted;
}

// 高频数值变化：缓冲百分比或电平变化（限频 ≤4Hz）
static bool radioPlayMeterChanged(const RadioPlaySnapshot& a, const RadioPlaySnapshot& b) {
  return a.bufferPct != b.bufferPct || (abs((int)a.vuLevel - (int)b.vuLevel) >= 3);
}

static RadioPlaySnapshot radioPlayCurrentSnapshot() {
  RadioPlaySnapshot s;
  s.status = rStatus;
  getRadioError(s.errMsg, sizeof(s.errMsg));
  s.playIdx = (radioPlayIdx >= 0) ? radioPlayIdx : radioStIdx;
  s.vol = volPct;
  s.muted = radioMuted;
  s.bufferPct = gBufferPct;
  s.vuLevel = gRadioVuLevel;
  return s;
}

void radioUpdate() {
  // 1. 串口诊断：主循环中检测状态、错误信息或台号变更并打印
  char curErr[32];
  getRadioError(curErr, sizeof(curErr));
  RStatus curSt = rStatus;
  int curIdx = (radioPlayIdx >= 0) ? radioPlayIdx : -1;

  static RStatus lastLoggedSt = (RStatus)-1;
  static int lastLoggedIdx = -999;
  static char lastLoggedErr[32] = {1};

  if (curSt != lastLoggedSt || curIdx != lastLoggedIdx || strcmp(curErr, lastLoggedErr) != 0) {
    lastLoggedSt = curSt;
    lastLoggedIdx = curIdx;
    strncpy(lastLoggedErr, curErr, sizeof(lastLoggedErr) - 1);
    lastLoggedErr[sizeof(lastLoggedErr) - 1] = '\0';

    const char* stStr = "UNKNOWN";
    switch (curSt) {
      case RS_STOPPED:    stStr = "STOPPED"; break;
      case RS_CONNECTING: stStr = "CONNECTING"; break;
      case RS_PLAYING:    stStr = "PLAYING"; break;
      case RS_STOPPING:   stStr = "STOPPING"; break;
      case RS_ERROR:      stStr = "ERROR"; break;
    }
    Serial.printf("[radio] status=%s err=\"%s\" idx=%d\n", stStr, curErr, curIdx);
  }

  // 2. 检测后台任务退出与挂起动作执行
  if (gTask == nullptr) {
    if (pendingAction != PENDING_NONE) {
      RadioPendingAction act = pendingAction;
      pendingAction = PENDING_NONE;
      switch (act) {
        case PENDING_START_STATION:
          startStation(pendingStationIdx);
          break;
        case PENDING_TO_LIST:
          canvasRestore();
          screen = SCREEN_RADIO;
          rStatus = RS_STOPPED;
          dirty = true;
          break;
        case PENDING_TO_URL:
          canvasRestore();
          screen = SCREEN_RADIO_URL;
          rStatus = RS_STOPPED;
          dirty = true;
          break;
        case PENDING_RESTORE_CANVAS:
          canvasRestore();
          dirty = true;
          break;
        default:
          break;
      }
    }
  }

  // 3. 列表页播放时维持顶部跳动动效
  if (screen == SCREEN_RADIO && (rStatus == RS_PLAYING || rStatus == RS_CONNECTING)) {
    dirty = true;
  }

  // 4. 播放页直推屏幕限频更新 (≤4Hz)
  if (screen == SCREEN_RADIO_PLAY) {
    uint32_t now = millis();
    RadioPlaySnapshot cur = radioPlayCurrentSnapshot();
    if (radioPlayFirstDraw || radioPlayUrgentChanged(cur, lastDrawnSnap)) {
      dirty = true;
      return;
    }
    if (now - radioPlayLastDrawMs >= 250 && radioPlayMeterChanged(cur, lastDrawnSnap)) {
      dirty = true;
    }
  }
}

void drawRadioPlay() {
  uint32_t now = millis();
  RadioPlaySnapshot cur = radioPlayCurrentSnapshot();

  bool first = radioPlayFirstDraw;
  bool urgent = radioPlayUrgentChanged(cur, lastDrawnSnap);
  bool meter = radioPlayMeterChanged(cur, lastDrawnSnap);

  if (!first && !urgent && (!meter || now - radioPlayLastDrawMs < 250)) {
    return;
  }

  radioPlayLastDrawMs = now;
  radioPlayFirstDraw = false;

  const int cardX = 8, cardW = SW - 16;
  const int topCardY = 15, topCardH = 69;
  const int botCardY = 87, botCardH = 31;

  // 1. 初次绘制或切台：直连 M5.Display 重画全屏静态背景与线框，绝对不分配任何 sprite
  if (first || cur.playIdx != lastDrawnSnap.playIdx) {
    M5.Display.fillScreen(TFT_BLACK);

    // 顶部标题
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(top_left);
    M5.Display.fillRoundRect(8, 2, 4, 10, 1, ACCENT);
    M5.Display.setTextColor(ACCENT, TFT_BLACK);
    M5.Display.drawString("Radio Stream", 16, 3);
    M5.Display.drawFastHLine(8, 14, SW - 16, DIM_BORDER);

    // 主视窗卡片底与边框
    M5.Display.fillRoundRect(cardX, topCardY, cardW, topCardH, 4, 0x0821);
    M5.Display.drawRoundRect(cardX, topCardY, cardW, topCardH, 4,
                             cur.status == RS_PLAYING ? 0x07E0 : (cur.status == RS_STOPPING ? 0xFDA0 : 0x18C3));

    // 边角科技感刻度
    M5.Display.drawFastHLine(cardX, topCardY, 6, 0x07FF);
    M5.Display.drawFastVLine(cardX, topCardY, 6, 0x07FF);
    M5.Display.drawFastHLine(cardX + cardW - 7, topCardY, 6, 0x07FF);
    M5.Display.drawFastVLine(cardX + cardW - 1, topCardY, 6, 0x07FF);

    // 顶行状态标识
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(0x7BEF, 0x0821);
    M5.Display.drawString("SOMAFM STREAM", cardX + 8, topCardY + 4);

    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(0x07FF, 0x0821);
    M5.Display.drawString("MP3 128k / 44k", cardX + cardW - 8, topCardY + 4);

    // 电台名称大字 + 流派副标题
    int idx = cur.playIdx;
    const char* name = (idx == CUSTOM_IDX) ? "Custom URL" : PRESETS[idx].name;
    const char* genre = (idx == CUSTOM_IDX) ? "User Defined Stream" : PRESETS[idx].genre;

    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE, 0x0821);
    M5.Display.setTextSize(2);
    M5.Display.drawString(clipToWidth(name, cardW - 16, 2), SW / 2, topCardY + 23);

    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0x7BEF, 0x0821);
    M5.Display.drawString(clipToWidth(genre, cardW - 20, 1), SW / 2, topCardY + 36);

    // 下方左 Pod 框架: BUFFER LEVEL
    M5.Display.fillRoundRect(cardX, botCardY, 110, botCardH, 3, 0x0821);
    M5.Display.drawRoundRect(cardX, botCardY, 110, botCardH, 3, 0x18C3);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(0x7BEF, 0x0821);
    M5.Display.drawString("BUFFER", 13, botCardY + 3);
    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(0x4208, 0x0821);
    M5.Display.drawString("16KB", 112, botCardY + 3);

    // 下方右 Pod 框架: SPEAKER OUT
    M5.Display.fillRoundRect(122, botCardY, 110, botCardH, 3, 0x0821);
    M5.Display.drawRoundRect(122, botCardY, 110, botCardH, 3, 0x18C3);
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextColor(0x7BEF, 0x0821);
    M5.Display.drawString("SPEAKER", 127, botCardY + 3);
    M5.Display.setTextDatum(top_right);
    M5.Display.setTextColor(0x4208, 0x0821);
    M5.Display.drawString("M5 DAC", 226, botCardY + 3);
    M5.Display.drawString("[] M", 226, botCardY + 21);

    // 底部按键提示
    M5.Display.drawFastHLine(0, 121, SW, 0x1082);
    const int footY = SH - 12;
    M5.Display.setTextDatum(top_left);
    M5.Display.setTextSize(1);
    int fx = 6;
    M5.Display.setTextColor(0x07FF, TFT_BLACK); M5.Display.drawString("RET", fx, footY); fx += 21;
    M5.Display.setTextColor(0x8410, TFT_BLACK); M5.Display.drawString("STOP", fx, footY); fx += 28;
    M5.Display.setTextColor(0x07FF, TFT_BLACK); M5.Display.drawString(";.", fx, footY); fx += 15;
    M5.Display.setTextColor(0x8410, TFT_BLACK); M5.Display.drawString("CH", fx, footY); fx += 18;
    M5.Display.setTextColor(0x07FF, TFT_BLACK); M5.Display.drawString("[]", fx, footY); fx += 15;
    M5.Display.setTextColor(0x8410, TFT_BLACK); M5.Display.drawString("VOL", fx, footY); fx += 24;
    M5.Display.setTextColor(0x07FF, TFT_BLACK); M5.Display.drawString("M", fx, footY); fx += 9;
    M5.Display.setTextColor(0x8410, TFT_BLACK); M5.Display.drawString("MUTE", fx, footY); fx += 28;
    M5.Display.setTextColor(0x07E0, TFT_BLACK); M5.Display.drawString("L", fx, footY); fx += 9;
    M5.Display.setTextColor(0x8410, TFT_BLACK); M5.Display.drawString("LIST", fx, footY); fx += 28;
    M5.Display.setTextColor(0x07FF, TFT_BLACK); M5.Display.drawString("`", fx, footY); fx += 9;
    M5.Display.setTextColor(0x8410, TFT_BLACK); M5.Display.drawString("EXIT", fx, footY);
  }

  // 2. 状态栏文字局部刷新 (首绘或状态改变)
  if (first || cur.status != lastDrawnSnap.status || strncmp(cur.errMsg, lastDrawnSnap.errMsg, sizeof(cur.errMsg)) != 0) {
    uint16_t statusCol;
    const char* statusText;
    switch (cur.status) {
      case RS_CONNECTING: statusText = "BUFFERING"; statusCol = 0xFDA0; break;
      case RS_PLAYING:    statusText = "ON AIR";    statusCol = 0x07E0; break;
      case RS_STOPPING:   statusText = "STOPPING..."; statusCol = 0xFDA0; break;
      case RS_ERROR:      statusText = cur.errMsg[0] ? cur.errMsg : "ERROR"; statusCol = 0xF800; break;
      default:            statusText = "STOPPED";   statusCol = 0x7BEF; break;
    }
    // 擦除旧状态文字区域
    M5.Display.fillRect(SW - 120, 2, 112, 12, TFT_BLACK);
    M5.Display.setTextDatum(top_right);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(statusCol, TFT_BLACK);
    M5.Display.drawString(clipToWidth(statusText, 110), SW - 8, 3);

    // 主卡片外框颜色随播放状态切换
    M5.Display.drawRoundRect(cardX, topCardY, cardW, topCardH, 4,
                             cur.status == RS_PLAYING ? 0x07E0 : (cur.status == RS_STOPPING ? 0xFDA0 : 0x18C3));
  }

  // 3. 动态音柱 / 频谱局部刷新 (限频 ≤4Hz)
  const int cx = SW / 2;
  const int specBaseY = topCardY + 65;
  // 仅在卡片中间清理音柱区域，避免全屏重画
  M5.Display.fillRect(cx - 75, topCardY + 47, 150, 20, 0x0821);

  if (cur.status == RS_PLAYING) {
    float vuScale = (float)cur.vuLevel / 100.0f;
    uint32_t t = now;
    for (int b = -7; b <= 7; b++) {
      int dist = abs(b);
      int bx = cx + b * 10 - 2;
      float base = (sinf(t * 0.008f + dist * 0.7f) * 3.5f
                  + cosf(t * 0.014f + dist * 1.2f) * 2.5f + 5.0f) * (0.35f + 0.65f * vuScale);
      int h = 3 + (int)base;
      if (h < 3) h = 3;
      if (h > 15) h = 15;

      for (int py = 0; py < h; py++) {
        uint16_t c = (py > 11) ? 0xFDA0 : ((py > 6) ? 0x07FF : 0x07E0);
        M5.Display.drawFastHLine(bx, specBaseY - py, 5, c);
      }
      M5.Display.drawFastHLine(bx, specBaseY - h - 1, 5, 0xFFFF);
    }
  } else if (cur.status == RS_CONNECTING || cur.status == RS_STOPPING) {
    int wave = ((now / 70) % 15) - 7;
    for (int b = -7; b <= 7; b++) {
      int bx = cx + b * 10 - 2;
      int h = (abs(b - wave) <= 1) ? 12 : (abs(b - wave) <= 2 ? 6 : 2);
      M5.Display.fillRoundRect(bx, specBaseY - h, 5, h, 1, 0xFDA0);
    }
  } else {
    for (int b = -7; b <= 7; b++) {
      int bx = cx + b * 10 - 2;
      M5.Display.fillRoundRect(bx, specBaseY - 2, 5, 2, 1, 0x18C3);
    }
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(0x4208, 0x0821);
    M5.Display.drawString("-- STREAM IDLE --", cx, specBaseY - 8);
  }

  // 4. 左 Pod: BUFFER LEVEL 局部刷新
  if (first || cur.bufferPct != lastDrawnSnap.bufferPct) {
    // 10 格 LED 分段指示
    for (int s = 0; s < 10; s++) {
      int sx = 13 + s * 10;
      bool active = (s < (cur.bufferPct + 5) / 10);
      uint16_t sc = active ? (cur.bufferPct < 25 ? 0xFDA0 : 0x07E0) : 0x1082;
      M5.Display.fillRect(sx, botCardY + 13, 7, 5, sc);
    }
    // 文字
    M5.Display.fillRect(13, botCardY + 21, 80, 8, 0x0821);
    M5.Display.setTextDatum(top_left);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
    char bufStr[16]; snprintf(bufStr, sizeof(bufStr), "FILL: %d%%", cur.bufferPct);
    M5.Display.setTextColor(0x07FF, 0x0821);
    M5.Display.drawString(bufStr, 13, botCardY + 21);
  }

  // 5. 右 Pod: SPEAKER / 音量局部刷新
  if (first || cur.vol != lastDrawnSnap.vol || cur.muted != lastDrawnSnap.muted) {
    M5.Display.fillRoundRect(127, botCardY + 13, 98, 5, 2, 0x1082);
    int vFill = (96 * cur.vol) / 100;
    if (vFill > 0) {
      M5.Display.fillRoundRect(128, botCardY + 14, vFill, 3, 1, cur.muted ? 0xFDA0 : 0x07E0);
    }
    M5.Display.fillRect(127, botCardY + 21, 70, 8, 0x0821);
    M5.Display.setTextDatum(top_left);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(1);
    if (cur.muted) {
      M5.Display.setTextColor(0xFDA0, 0x0821);
      M5.Display.drawString("MUTED", 127, botCardY + 21);
    } else {
      char volStr[16];
      snprintf(volStr, sizeof(volStr), "VOL: %d%%", cur.vol);
      M5.Display.setTextColor(0x07FF, 0x0821);
      M5.Display.drawString(volStr, 127, botCardY + 21);
    }
  }

  lastDrawnSnap = cur;
}

void drawRadioUrl() {
  cv.fillScreen(TFT_BLACK);
  drawPageHeader("Custom Stream", "HTTP ONLY", 0xFDA0);

  // 输入控制台卡片
  cv.fillRoundRect(8, 16, SW - 16, 35, 4, 0x0821);
  cv.drawRoundRect(8, 16, SW - 16, 35, 4, 0x07FF);

  cv.setTextSize(1);
  cv.setTextDatum(top_left);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("STREAM URL INPUT", 14, 19);

  cv.setTextDatum(top_right);
  bool hasHttp = radioCustomUrl.startsWith("http://");
  char cntStr[32];
  snprintf(cntStr, sizeof(cntStr), "%d/120  %s", (int)radioCustomUrl.length(), hasHttp ? "HTTP OK" : "RAW");
  cv.setTextColor(hasHttp ? 0x07E0 : 0xFDA0, 0x0821);
  cv.drawString(cntStr, SW - 14, 19);

  // 输入行
  cv.setTextDatum(middle_left);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString(">", 14, 37);

  String disp = radioCustomUrl;
  if (disp.length() > 31) disp = "~" + disp.substring(disp.length() - 30);
  bool blink = (millis() / 400) % 2 == 0;
  if (disp.length() == 0) {
    cv.setTextColor(0x632C, 0x0821);
    cv.drawString(blink ? "http://_  [SPACE to fill]" : "http://   [SPACE to fill]", 24, 37);
  } else {
    cv.setTextColor(TFT_WHITE, 0x0821);
    cv.drawString(disp + (blink ? "_" : " "), 24, 37);
  }

  // 示例与说明卡片
  cv.fillRoundRect(8, 54, SW - 16, 64, 4, 0x0821);
  cv.drawRoundRect(8, 54, SW - 16, 64, 4, 0x18C3);

  cv.setTextDatum(top_left);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("CURATED ICECAST STREAMS // HTTP:", 14, 58);

  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("* Groove Salad:", 14, 70);
  cv.setTextColor(0xCE79, 0x0821);
  cv.drawString("ice1.somafm.com/groovesalad-128-mp3", 20, 80);

  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("* Secret Agent:", 14, 91);
  cv.setTextColor(0xCE79, 0x0821);
  cv.drawString("ice1.somafm.com/secretagent-128-mp3", 20, 101);

  cv.setTextColor(0x632C, 0x0821);
  cv.drawString("* HTTP MP3 stream only (no HTTPS)", 14, 111);

  // 底部按键提示
  cv.drawFastHLine(0, 121, SW, 0x1082);
  const int footY = SH - 12;
  cv.setTextDatum(top_left);
  cv.setTextSize(1);

  int fx = 8;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("RET", fx, footY); fx += 20;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("CONNECT", fx, footY); fx += 48;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("BS", fx, footY); fx += 14;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("DEL", fx, footY); fx += 26;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("SPC", fx, footY); fx += 20;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("AUTO-HTTP", fx, footY); fx += 58;
  cv.setTextColor(0x07FF, TFT_BLACK); cv.drawString("`", fx, footY); fx += 8;
  cv.setTextColor(0x8410, TFT_BLACK); cv.drawString("BACK", fx, footY);
}
