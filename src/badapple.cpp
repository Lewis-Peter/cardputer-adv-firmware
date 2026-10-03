#include "badapple.h"
#include "sd_files.h"
#include <SD.h>

static const char* VIDEO_PATH = "/video/badapple.dat";

struct BadAppleHeader {
  char     magic[4];
  uint16_t width;
  uint16_t height;
  uint16_t fps;
  uint16_t reserved;
  uint32_t frameCount;
};
// 跟 tools/badapple_pack.py 里 struct.pack("<4sHHHHI", ...) 是同一份布局约定，两边各写各的，
// 没有共享头文件校验——万一编译器给这个结构体塞了 padding，读出来的字段就全错位，
// 还是"能编译、能跑，数值全是垃圾"这种最难查的错法。钉死在这，编译期就能发现。
static_assert(sizeof(BadAppleHeader) == 16, "BadAppleHeader 有隐藏 padding，跟打包脚本的字节布局对不上了");

enum State { ST_ERROR, ST_IDLE, ST_PLAYING, ST_PAUSED, ST_FINISHED };

static State    state        = ST_IDLE;
static String   errMsg       = "";
static File     vidFile;
static uint16_t vidW = 0, vidH = 0, vidFps = 30;
static uint32_t vidFrameCount = 0;
static uint32_t vidFrameBytes = 0;   // (W/8)*H
static uint32_t headerSize    = sizeof(BadAppleHeader);

static uint32_t frameIdx      = 0;
static uint32_t playStartMs   = 0;   // millis() 对应 frameIdx==0 那一刻（seek/resume 时回拨）

static M5Canvas vid(&cv);   // 240x135 1bpp，跟主 UI 的 cv 一样是"先画进内存，render() 时统一推屏"

static void closeFile() {
  if (vidFile) vidFile.close();
}

// frameIdx -> millis() 基准：resume/seek 之后要让"目标帧号"的计算继续跟真实时间对齐，
// 不然每次暂停/快进都会在时间轴上留一段空当或叠一段重复。
static void rebasePlayStart() {
  playStartMs = millis() - (uint32_t)((uint64_t)frameIdx * 1000 / vidFps);
}

static bool seekToFrame(uint32_t f) {
  if (f >= vidFrameCount) f = vidFrameCount - 1;
  if (!vidFile.seek(headerSize + (uint64_t)f * vidFrameBytes)) return false;
  frameIdx = f;
  rebasePlayStart();
  return true;
}

// 读一帧到 vid 的 Sprite 缓冲区。缓冲区布局跟文件里存的完全一致（MSB-first、行字节对齐），
// 所以直接 memcpy，不用逐像素转——这是能撑住 30fps 的关键（读+推屏都不经过 CPU 端位操作）。
static bool readFrameInto(void* buf) {
  int n = vidFile.read((uint8_t*)buf, vidFrameBytes);
  return n == (int)vidFrameBytes;
}

void badappleEnter() {
  state  = ST_IDLE;
  errMsg = "";

  if (!sdReady()) { state = ST_ERROR; errMsg = "No SD card"; return; }

  vidFile = SD.open(VIDEO_PATH, FILE_READ);
  if (!vidFile) { state = ST_ERROR; errMsg = "Missing " + String(VIDEO_PATH); return; }

  BadAppleHeader hdr;
  if (vidFile.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      memcmp(hdr.magic, "BAP1", 4) != 0) {
    errMsg = "Bad file header";
    closeFile();
    state = ST_ERROR;
    return;
  }
  if (hdr.width != 240 || hdr.height != 135 || hdr.fps == 0 || hdr.frameCount == 0) {
    errMsg = "Unsupported format";
    closeFile();
    state = ST_ERROR;
    return;
  }

  vidW          = hdr.width;
  vidH          = hdr.height;
  vidFps        = hdr.fps;
  vidFrameCount = hdr.frameCount;
  vidFrameBytes = ((uint32_t)vidW / 8) * vidH;
  headerSize    = sizeof(BadAppleHeader);
  frameIdx      = 0;

  // ⚠️ 调色板存储是 createSprite() 内部才分配的（成功拿到 buffer 才建），createSprite()
  // 之前调 setPaletteColor() 会被它自己的 `if (!_palette) return;` 静默吃掉——现在能显示
  // 对黑白纯属巧合：createSprite() 内部会自动套一次灰阶默认调色板，2 级灰阶正好算出
  // 0=黑/1=白。setBitmapColor() 才是库里标了"For 1bpp sprites"的正经 API，创建之后调。
  vid.setColorDepth(1);
  if (!vid.createSprite(vidW, vidH)) {
    errMsg = "No RAM for frame buffer";
    closeFile();
    state = ST_ERROR;
    return;
  }
  vid.setBitmapColor(TFT_BLACK, TFT_WHITE);   // 实机测出跟注释写的 (fg,bg)->[1],[0] 相反，先按实测来

  state = ST_IDLE;
  dirty = true;
}

bool badappleIsPlaying() {
  return state == ST_PLAYING || state == ST_PAUSED;
}

void badappleExit() {
  vid.deleteSprite();
  closeFile();
  state = ST_IDLE;
  canvasRestore();
}

static void startPlayback() {
  canvasRelease();   // 播放视频期间直接释放 64.8KB cv 画布，直推硬件屏
  seekToFrame(0);
  readFrameInto(vid.getBuffer());
  state = ST_PLAYING;
  dirty = true;
}

void badappleUpdate() {
  if (state != ST_PLAYING) return;

  uint32_t targetFrame = (uint32_t)((uint64_t)(millis() - playStartMs) * vidFps / 1000);
  if (targetFrame <= frameIdx) return;   // 还没到下一帧该出现的时刻

  if (targetFrame >= vidFrameCount) {
    state = ST_FINISHED;
    canvasRestore();
    dirty = true;
    return;
  }

  if (targetFrame > frameIdx + 1) {
    // 落后超过 1 帧直接 seek 丢帧追赶，保证与真实时间同步播放
    if (!vidFile.seek(headerSize + (uint64_t)targetFrame * vidFrameBytes)) {
      state = ST_FINISHED;
      canvasRestore();
      dirty = true;
      return;
    }
    frameIdx = targetFrame;
  } else {
    frameIdx++;
  }

  if (!readFrameInto(vid.getBuffer())) {
    // SD 偶发读失败：当这一帧丢了，往前挪一格再继续，别整个卡死
    state = ST_FINISHED;
    canvasRestore();
    dirty = true;
    return;
  }
  dirty = true;
}

void badappleKey(char k) {
  if (state == ST_ERROR) return;

  if (k == '\n') {
    if (state == ST_IDLE || state == ST_FINISHED) {
      startPlayback();
    } else if (state == ST_PLAYING) {
      state = ST_PAUSED;
      dirty = true;
    } else if (state == ST_PAUSED) {
      rebasePlayStart();
      state = ST_PLAYING;
      dirty = true;
    }
    return;
  }

  // 快进/快退 10 秒：暂停和播放中都能用，播放中直接续播
  if ((state == ST_PLAYING || state == ST_PAUSED) &&
      (k == '.' || k == '/' || k == ';' || k == ',')) {
    int32_t deltaFrames = (int32_t)vidFps * 10;
    if (k == ';' || k == ',') deltaFrames = -deltaFrames;
    int64_t nf = (int64_t)frameIdx + deltaFrames;
    if (nf < 0) nf = 0;
    seekToFrame((uint32_t)nf);
    readFrameInto(vid.getBuffer());
    dirty = true;
  }
}

// 秒数转 m:ss
static String fmtTime(uint32_t secs) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u:%02u", secs / 60, secs % 60);
  return String(buf);
}

static void drawCenterMsg(const String& msg, uint16_t color) {
  cv.fillScreen(TFT_BLACK);
  cv.setTextSize(1);
  cv.setTextDatum(middle_center);
  cv.setTextColor(color);
  cv.drawString(msg, SW / 2, SH / 2);
  cv.setTextDatum(top_left);
}

void drawBadApple() {
  if (state == ST_ERROR) {
    drawCenterMsg(errMsg, TFT_RED);
    cv.setTextSize(1);
    cv.setTextColor(DIM_BORDER);
    cv.setTextDatum(bottom_left);
    cv.drawString("` back", 6, SH - 1);
    cv.setTextDatum(top_left);
    return;
  }

  if (state == ST_IDLE) {
    cv.fillScreen(TFT_BLACK);
    cv.setTextSize(1);
    cv.setTextColor(ACCENT);
    cv.setTextDatum(top_left);
    cv.drawString("BAD APPLE", 6, 2);
    cv.setTextColor(TFT_WHITE);
    cv.setTextDatum(middle_center);
    char info[48];
    snprintf(info, sizeof(info), "%ux%u  %ufps  %s",
             vidW, vidH, vidFps, fmtTime(vidFrameCount / vidFps).c_str());
    cv.drawString(info, SW / 2, SH / 2 - 6);
    cv.setTextColor(DIM_BORDER);
    cv.drawString("Enter to play", SW / 2, SH / 2 + 10);
    cv.setTextDatum(bottom_left);
    cv.drawString("` back", 6, SH - 1);
    cv.setTextDatum(top_left);
    return;
  }

  if (state == ST_FINISHED) {
    drawCenterMsg("Done — Enter to replay", ACCENT);
    cv.setTextSize(1);
    cv.setTextColor(DIM_BORDER);
    cv.setTextDatum(bottom_left);
    cv.drawString("` back", 6, SH - 1);
    cv.setTextDatum(top_left);
    return;
  }

  // PLAYING / PAUSED：1bpp 视频帧直接硬件 DMA 直推屏幕（避开 64.8KB cv 双重搬移）
  vid.pushSprite(&M5.Display, 0, 0);

  if (state == ST_PAUSED) {
    int barY = SH - 11;
    M5.Display.fillRect(0, barY, SW, 11, TFT_BLACK);
    M5.Display.drawFastHLine(0, barY, SW, DIM_BORDER);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(ACCENT, TFT_BLACK);
    M5.Display.setTextDatum(bottom_left);
    String t = "PAUSED  " + fmtTime(frameIdx / vidFps) + "/" + fmtTime(vidFrameCount / vidFps);
    M5.Display.drawString(t, 4, SH - 1);
    M5.Display.setTextColor(DIM_BORDER, TFT_BLACK);
    M5.Display.setTextDatum(bottom_right);
    M5.Display.drawString("Enter resume  ` back", SW - 4, SH - 1);
    M5.Display.setTextDatum(top_left);
  }
  // 播放中（非暂停）：视频帧已经铺满全屏，没有别的要画了。
  // 曾经在右下角常驻一个实测 fps 读数——测过关掉它对帧率没有可感知的影响（瓶颈在整帧
  // SPI 推屏，不在这几个 drawString），纯粹是加了个字看，应用户要求去掉了。
}
