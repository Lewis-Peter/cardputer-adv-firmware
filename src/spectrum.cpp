#include "spectrum.h"
#include "led.h"
#include <cmath>
#include <cstring>
#include "ui_common.h"

static const int FFT_BITS = 8;
static const int FFT_SIZE = 1 << FFT_BITS;         // 256
static const uint32_t SAMPLE_RATE = 16000;
static const int MAG_COUNT = (FFT_SIZE >> 1) + 1;  // 129，到奈奎斯特频率
static const int BAR_COUNT = 32;

struct SpectrumContext {
  int16_t sampleBuf[FFT_SIZE];
  float fr[FFT_SIZE], fi[FFT_SIZE];
  uint16_t bitrev[FFT_SIZE];
  float twiddle[FFT_SIZE];
  float magnitude[MAG_COUNT];
  uint8_t specDb[MAG_COUNT];       // 每 bin 的 dB，0.5dB 一格
};
static SpectrumContext* specCtx = nullptr;

static float barVal[BAR_COUNT];    // 归一化到大致 0..1 的柱高，涨快落慢
static float peakVal[BAR_COUNT];   // 峰值保持，缓慢衰减

// ---- 频谱流式输出（按 s 切换）----
// 设备只管采集，语义留给电脑：把每帧频谱吐到串口，Mac 端 tools/spec_view.py 存成
// ndjson + 渲染语谱图，之后爱怎么分析怎么分析（跑模型、找周期、比对样本都行）。
//
// ⚠️ 这里存的是**自动增益之前**的原始幅度。屏幕上那套 AGC 是为了让柱子好看，
// 它会把安静和响亮的场景都拉到满量程——绝对电平一丢，"这个声音有多大"就没了，
// 而那恰恰是分析时最基本的一个量。
//
// 带宽：129 个 bin × 1 字节，hex 化 258 字符 + json 头约 320 字节/帧。
// 限速 5 帧/秒 ≈ 1.6KB/s，而 115200 实际约 11.5KB/s，留了充足余量。
// （原始音频是传不了的：16kHz×16bit = 256kbit/s，比整条串口还宽。）
static bool     specStream = false;
static float    specRms = 0;             // 这一帧的宽带 RMS（原始量纲）
static uint32_t specLastMs = 0;
static const uint32_t SPEC_MIN_MS = 200; // 5 帧/秒

static bool fftTablesReady = false;
static bool micActive = false;
static bool vuLedMode = false;   // L 键切换：板载LED跟着整体响度换色，接管 led.cpp 的电量指示
static bool waterfallMode = false;  // W 键切换：瀑布图 (语谱图) / 柱状条形图
static bool waterfallReset = true;  // 模式切换或初次进入时重置画布背景

// ---- 爆音抑制：直接操作 ES8311 编解码器（内部 I2C 0x18）----
// M5Unified 切 Mic/Speaker 时会把编解码器模拟部分整体下电再上电，爆音就来自这个模拟
// 上下电瞬态（跟数字音量无关，所以 setVolume(0) 压不住）。这里在切换前后自己给 DAC
// 静音 + 音量缓升，软化"解除静音"那一下咔哒（HP 驱动上电的纯直流爆音受限于芯片软启动
// 未被 M5Unified 暴露，只能减小不保证消掉）。
static const uint8_t ES8311_ADDR = 0x18;
static const uint8_t ES8311_DAC_VOL = 0x32;   // 0xBF = 0dB（M5 默认），0x00 = 静音
static inline void es8311Reg(uint8_t reg, uint8_t val) {
  M5.In_I2C.writeRegister8(ES8311_ADDR, reg, val, 100000);
}

// ---- FFT 表初始化：位反转表 + 旋转因子表（原理见官方示例 fft_function_t::_init）----
static void fftInitTables() {
  if (!specCtx) return;
  auto& bitrev = specCtx->bitrev;
  auto& twiddle = specCtx->twiddle;

  bitrev[0] = 0;
  bitrev[1] = FFT_SIZE >> 1;
  size_t je = 1;
  for (size_t i = 0; i < (size_t)(FFT_BITS - 1); ++i) {
    bitrev[je << 1] = bitrev[je] >> 1;
    je <<= 1;
    for (size_t j = 1; j < je; ++j) bitrev[je + j] = bitrev[je] + bitrev[j];
  }
  float omega = 2.0f * (float)M_PI / FFT_SIZE;
  int s2 = FFT_SIZE >> 1, s4 = FFT_SIZE >> 2;
  twiddle[0] = 0; twiddle[s2] = 0; twiddle[s4] = 1;
  for (int i = 1; i < s4; ++i) {
    float f = cosf(omega * i);
    twiddle[s4 + i] = f;
    twiddle[s4 - i] = f;
    twiddle[s4 + s2 - i] = -f;
  }
}

// ---- 原地 FFT + 幅度谱（原理见官方示例 fft_function_t::update）----
static void fftCompute() {
  if (!specCtx) return;
  auto& fr = specCtx->fr;
  auto& fi = specCtx->fi;
  auto& bitrev = specCtx->bitrev;
  auto& twiddle = specCtx->twiddle;
  auto& sampleBuf = specCtx->sampleBuf;
  auto& magnitude = specCtx->magnitude;
  auto& specDb = specCtx->specDb;

  for (int i = 0; i < FFT_SIZE; ++i) fr[bitrev[i]] = (float)sampleBuf[i];
  memset(fi, 0, sizeof(fi));

  int s4 = FFT_SIZE / 4;
  size_t s = 1, i = 0, je = FFT_SIZE;
  do {
    size_t ke = s;
    s <<= 1;
    je >>= 1;
    size_t j = 0;
    do {
      size_t k = 0;
      size_t m = ke * ((j << 1) + 1);
      size_t l = s * j;
      float* frm_p = &fr[m]; float* fim_p = &fi[m];
      float* frl_p = &fr[l]; float* fil_p = &fi[l];
      const float* wi_p = &twiddle[0];
      const float* wr_p = &twiddle[s4];
      do {
        float wiv = *wi_p, wrv = *wr_p;
        float frm = *frm_p, fim = *fim_p;
        float Wxmr = frm * wrv + fim * wiv;
        float Wxmi = fim * wrv - frm * wiv;
        float frl = *frl_p, fil = *fil_p;
        *frm_p++ = frl - Wxmr;
        *frl_p++ = frl + Wxmr;
        *fim_p++ = fil - Wxmi;
        *fil_p++ = fil + Wxmi;
        wi_p += je; wr_p += je;
      } while (++k < ke);
    } while (++j < je);
  } while (++i < (size_t)FFT_BITS);

  float maxVal = 0.0f;
  for (int b = 0; b < MAG_COUNT; ++b) {
    float v = sqrtf(fr[b] * fr[b] + fi[b] * fi[b]);
    magnitude[b] = v;
    if (v > maxVal) maxVal = v;
    // 趁自动增益还没动手，先把原始电平量化下来（串口流和瀑布图都用它，见上面 specStream 那段注释）
    float db = 20.0f * log10f(v + 1.0f);        // +1 防 log(0)
    int q = (int)(db * 2.0f + 0.5f);            // 0.5dB 一格
    specDb[b] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
  }
  // 自动增益：响的时候把峰值拉到接近满量程，安静的时候不放大（避免噪声被当成信号）
  if (maxVal > 0) {
    float gain = 65536.0f / maxVal;
    if (gain <= 0.03125f) {
      for (int b = 0; b < MAG_COUNT; ++b) magnitude[b] *= gain;
    }
  }
}

static void captureSamples() {
  if (!specCtx) return;
  auto& sampleBuf = specCtx->sampleBuf;
  M5.Mic.record(sampleBuf, FFT_SIZE, SAMPLE_RATE, false);
  uint32_t start = millis();
  while (M5.Mic.isRecording() && millis() - start < 200) delay(1);
  if (specStream) {                      // 宽带 RMS：分析时最基本的"这一帧有多响"
    double acc = 0;
    for (int i = 0; i < FFT_SIZE; ++i) acc += (double)sampleBuf[i] * sampleBuf[i];
    specRms = (float)sqrt(acc / FFT_SIZE);
  }
}

// 一行一帧频谱。⚠️ 只在主循环调用——USB-CDC 在主机不读时会阻塞。
static void specStreamPump() {
  if (!specStream || !specCtx) return;
  auto& specDb = specCtx->specDb;
  const uint32_t now = millis();
  if (now - specLastMs < SPEC_MIN_MS) return;
  specLastMs = now;

  Serial.printf("SPEC {\"ms\":%lu,\"sr\":%u,\"n\":%d,\"hz\":%.2f,\"rms\":%.1f,\"db\":\"",
                (unsigned long)now, (unsigned)SAMPLE_RATE, MAG_COUNT,
                (float)SAMPLE_RATE / FFT_SIZE, specRms);
  // 每 bin 两个 hex 字符；值 = dB×2，电脑那头除以 2 还原
  char hex[3];
  for (int b = 0; b < MAG_COUNT; ++b) {
    snprintf(hex, sizeof(hex), "%02X", specDb[b]);
    Serial.print(hex);
  }
  Serial.println("\"}");
}

static void updateBars() {
  if (!specCtx) return;
  auto& magnitude = specCtx->magnitude;
  for (int i = 0; i < BAR_COUNT; ++i) {
    int idx0 = i * MAG_COUNT / BAR_COUNT;
    int idx1 = (i + 1) * MAG_COUNT / BAR_COUNT;
    if (idx1 <= idx0) idx1 = idx0 + 1;
    float m = 0;
    for (int b = idx0; b < idx1 && b < MAG_COUNT; ++b) if (magnitude[b] > m) m = magnitude[b];
    float target = m / 65536.0f;
    if (target > 1.0f) target = 1.0f;

    if (target > barVal[i]) barVal[i] = target;               // 涨快
    else barVal[i] = barVal[i] * 0.6f + target * 0.4f;         // 落慢

    if (barVal[i] >= peakVal[i]) peakVal[i] = barVal[i];
    else { peakVal[i] -= 0.02f; if (peakVal[i] < 0) peakVal[i] = 0; }
  }
}

void spectrumEnter() {
  if (!specCtx) {
    specCtx = (SpectrumContext*)malloc(sizeof(SpectrumContext));
    if (!specCtx) return;
    fftTablesReady = false;
  }
  if (!fftTablesReady) { fftInitTables(); fftTablesReady = true; }
  waterfallReset = true;
  if (micActive) return;

  // 切走喇叭前：先在编解码器里把 DAC 拉到静音，再软件静音、下电，减小切换爆音。
  es8311Reg(ES8311_DAC_VOL, 0x00);
  M5.Speaker.setVolume(0);
  delay(30);
  M5.Speaker.end();
  delay(30);

  auto cfg = M5.Mic.config();
  cfg.sample_rate = SAMPLE_RATE;
  cfg.dma_buf_len = FFT_SIZE;
  cfg.dma_buf_count = 3;
  cfg.over_sampling = 1;
  M5.Mic.config(cfg);
  M5.Mic.begin();
  micActive = M5.Mic.isEnabled();

  for (int i = 0; i < BAR_COUNT; ++i) { barVal[i] = 0; peakVal[i] = 0; }
}

// 整体响度取所有柱子里最响的一根（跟条形图的红/橙/绿三档配色对齐，视觉上一致）
static void updateVuLed() {
  float level = 0;
  for (int i = 0; i < BAR_COUNT; ++i) if (barVal[i] > level) level = barVal[i];
  uint8_t v = (uint8_t)(40 + level * 215);   // 留个底色，安静时不全黑
  if (level > 0.75f)      ledShowRGB(v, 0, 0);
  else if (level > 0.45f) ledShowRGB(v, v / 2, 0);
  else                    ledShowRGB(0, v, 0);
}

void spectrumKey(char k) {
  if (k == 's' || k == 'S') {            // 切换频谱流式输出
    specStream = !specStream;
    specLastMs = 0;
    Serial.printf("[spec] stream %s\n", specStream ? "on" : "off");
    dirty = true;
    return;
  }
  if (k == 'w' || k == 'W') {            // 切换瀑布图 / 柱状条形图
    waterfallMode = !waterfallMode;
    waterfallReset = true;
    dirty = true;
    return;
  }
  if (k != 'l' && k != 'L') return;
  vuLedMode = !vuLedMode;
  ledSetOverride(vuLedMode);
  dirty = true;
}

void spectrumExit() {
  if (vuLedMode) { vuLedMode = false; ledSetOverride(false); }   // 交还LED给正常电量指示
  if (specCtx) {
    free(specCtx);
    specCtx = nullptr;
    fftTablesReady = false;
  }
  if (!micActive) return;
  M5.Mic.end();
  delay(30);

  // 回到喇叭：begin() 会把 DAC 音量猛地写回 0xBF(0dB)，"解除静音"那一下最响。
  // 所以 begin 之后立刻把编解码器 DAC 压回静音，等 HP 输出直流稳定，再一档档缓升到 0dB。
  M5.Speaker.begin();
  M5.Speaker.setVolume(volVal());       // 软件音量正常给回；下面单独软化编解码器 DAC
  es8311Reg(ES8311_DAC_VOL, 0x00);      // 立刻压回静音（盖掉 begin 写的 0xBF）
  delay(40);                            // 等模拟输出直流稳定
  for (uint8_t v = 0x30; v < 0xBF; v += 0x18) { es8311Reg(ES8311_DAC_VOL, v); delay(8); }
  es8311Reg(ES8311_DAC_VOL, 0xBF);      // 收尾到 M5 默认的 0dB
  micActive = false;
}

// ---- 瀑布图 ----
// 用 AGC 之前的 dB（specDb）而不是 AGC 之后的线性幅度：线性映射下只有最响的那个 bin 能亮，
// 其余几乎全黑；AGC 又是逐帧变的，同一个声音在相邻两行里亮度会跳。
// 窗口上沿跟着最近的峰值走（涨快落慢），下沿 = 上沿 - WF_RANGE_DB。
static const float WF_RANGE_DB = 60.0f;
static const float WF_TOP_MIN_DB = 90.0f;   // 上沿下限：安静时别把本底噪声拉成满屏亮色
static const float WF_TOP_DECAY_DB = 0.3f;  // 每帧回落量
static const int WF_ROW_MAX = 320;
static uint16_t wfRow[WF_ROW_MAX];
static float wfTopDb = WF_TOP_MIN_DB;
static bool wfNewFrame = false;   // 一帧 FFT 只进一行；render() 被额外调用（串口 SHOT）时不重复滚动

static uint16_t waterfallColor(float v);

static void buildWaterfallRow() {
  const auto& specDb = specCtx->specDb;
  float frameMax = 0;
  for (int b = 2; b < MAG_COUNT; ++b) {       // 跳过 bin 0-1：直流和麦克风偏置
    float db = specDb[b] * 0.5f;
    if (db > frameMax) frameMax = db;
  }
  if (frameMax > wfTopDb) wfTopDb = frameMax;
  else wfTopDb -= WF_TOP_DECAY_DB;
  if (wfTopDb < WF_TOP_MIN_DB) wfTopDb = WF_TOP_MIN_DB;
  const float floorDb = wfTopDb - WF_RANGE_DB;

  const int cols = SW < WF_ROW_MAX ? SW : WF_ROW_MAX;
  for (int x = 0; x < cols; ++x) {
    float u = (float)x * (float)(MAG_COUNT - 1) / (float)(cols - 1);
    int b0 = (int)u;
    if (b0 < 2) { wfRow[x] = TFT_BLACK; continue; }
    int b1 = b0 + 1 < MAG_COUNT ? b0 + 1 : MAG_COUNT - 1;
    float f = u - (float)b0;
    float db = ((1.0f - f) * specDb[b0] + f * specDb[b1]) * 0.5f;
    wfRow[x] = waterfallColor((db - floorDb) / WF_RANGE_DB);
  }
  wfNewFrame = true;
}

void spectrumUpdate() {
  if (!micActive || !specCtx) return;
  captureSamples();
  fftCompute();
  updateBars();
  if (waterfallMode) buildWaterfallRow();
  if (vuLedMode) updateVuLed();
  specStreamPump();
}

// 伪彩色渐变映射：黑(0.0) -> 蓝(0.2) -> 青(0.4) -> 绿/黄(0.6) -> 橙/红(0.8) -> 白(1.0)
static uint16_t waterfallColor(float v) {
  if (v <= 0.0f) return TFT_BLACK;
  if (v >= 1.0f) return TFT_WHITE;
  uint8_t r = 0, g = 0, b = 0;
  if (v < 0.20f) {
    // 0.00 ~ 0.20: 黑色 -> 纯蓝
    float t = v * 5.0f;
    b = (uint8_t)(t * 255.0f);
  } else if (v < 0.40f) {
    // 0.20 ~ 0.40: 纯蓝 -> 青色 (Cyan)
    float t = (v - 0.20f) * 5.0f;
    g = (uint8_t)(t * 255.0f);
    b = 255;
  } else if (v < 0.60f) {
    // 0.40 ~ 0.60: 青色 -> 亮绿 -> 亮黄
    float t = (v - 0.40f) * 5.0f;
    r = (uint8_t)(t * 255.0f);
    g = 255;
    b = (uint8_t)((1.0f - t) * 255.0f);
  } else if (v < 0.80f) {
    // 0.60 ~ 0.80: 亮黄 -> 橙色 -> 纯红
    float t = (v - 0.60f) * 5.0f;
    r = 255;
    g = (uint8_t)((1.0f - t) * 255.0f);
    b = 0;
  } else {
    // 0.80 ~ 1.00: 纯红 -> 纯白 (峰值高亮)
    float t = (v - 0.80f) * 5.0f;
    r = 255;
    g = (uint8_t)(t * 255.0f);
    b = (uint8_t)(t * 255.0f);
  }
  return cv.color565(r, g, b);
}

void drawSpectrum() {
  const int top = 26, bottom = SH - 12;
  const int h = bottom - top;

  if (!micActive || !specCtx) {
    cv.fillScreen(TFT_BLACK);
    drawPageHeader("Spectrum", specStream ? "REC->serial" : nullptr,
                   specStream ? TFT_RED : 0);
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString("microphone unavailable", SW / 2, SH / 2);
    return;
  }

  if (waterfallMode) {
    if (waterfallReset) {
      cv.fillScreen(TFT_BLACK);
      waterfallReset = false;
    }

    // 有新的一帧 FFT 才滚一行：局部滚屏限制在 top ~ bottom，向上滚 1 像素，新行画在 bottom - 1
    if (wfNewFrame) {
      wfNewFrame = false;
      cv.setScrollRect(0, top, SW, h);
      cv.scroll(0, -1);
      cv.clearScrollRect();
      const int drawY = bottom - 1;
      const int cols = SW < WF_ROW_MAX ? SW : WF_ROW_MAX;
      for (int x = 0; x < cols; ++x) cv.drawPixel(x, drawY, wfRow[x]);
    }

    // 顶栏局部刷新（防止流式录音等状态变化时留有残影）
    cv.fillRect(0, 0, SW, top, TFT_BLACK);
    drawPageHeader("Spectrum", specStream ? "REC->serial" : nullptr,
                   specStream ? TFT_RED : 0);
  } else {
    cv.fillScreen(TFT_BLACK);
    drawPageHeader("Spectrum", specStream ? "REC->serial" : nullptr,
                   specStream ? TFT_RED : 0);

    const int barW = SW / BAR_COUNT;
    for (int i = 0; i < BAR_COUNT; ++i) {
      int bh = (int)(barVal[i] * h);
      if (bh < 1) bh = 1;
      int x = i * barW + 1;
      int w = barW - 2;
      if (w < 1) w = 1;
      uint16_t col = barVal[i] > 0.75f ? TFT_RED : (barVal[i] > 0.45f ? TFT_ORANGE : ACCENT);
      cv.fillRect(x, bottom - bh, w, bh, col);

      int py = bottom - (int)(peakVal[i] * h);
      if (py < top) py = top;
      if (py >= bottom) py = bottom - 1;
      cv.drawFastHLine(x, py, w, TFT_WHITE);
    }
  }

  // 底栏提示行（两模式公用统一位置与样式，左侧显示切换提示，右侧显示 LED VU 状态）
  cv.fillRect(0, bottom, SW, SH - bottom, TFT_BLACK);
  cv.setTextSize(1);

  cv.setTextDatum(bottom_left);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.drawString(waterfallMode ? "W=bars" : "W=waterfall", 6, SH - 2);

  cv.setTextDatum(bottom_right);
  cv.setTextColor(vuLedMode ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  cv.drawString(vuLedMode ? "L=led vu ON" : "L=led vu", SW - 6, SH - 2);
}
