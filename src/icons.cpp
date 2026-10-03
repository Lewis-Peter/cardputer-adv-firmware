#include "icons.h"
#include "power_util.h"
#include <cmath>

static const float DIRS[8][2] = {
  {1,0},{0.707f,0.707f},{0,1},{-0.707f,0.707f},
  {-1,0},{-0.707f,-0.707f},{0,-1},{0.707f,-0.707f}};

void icoClock(int cx, int cy, int r, uint16_t col) {
  cv.drawCircle(cx, cy, r, col);
  cv.drawLine(cx, cy, cx, cy - (int)(r * 0.6f), col);
  cv.drawLine(cx, cy, cx + (int)(r * 0.45f), cy, col);
  cv.fillCircle(cx, cy, 1, col);
}
void icoSun(int cx, int cy, int r, uint16_t col) {
  cv.fillCircle(cx, cy, (int)(r * 0.42f), col);
  for (int i = 0; i < 8; i++) {
    int x1 = cx + (int)(DIRS[i][0] * r * 0.66f), y1 = cy + (int)(DIRS[i][1] * r * 0.66f);
    int x2 = cx + (int)(DIRS[i][0] * r),         y2 = cy + (int)(DIRS[i][1] * r);
    cv.drawLine(x1, y1, x2, y2, col);
  }
}
void icoInfo(int cx, int cy, int r, uint16_t col) {
  cv.drawCircle(cx, cy, r, col);
  cv.fillCircle(cx, cy - (int)(r * 0.42f), 1, col);
  cv.fillRect(cx - 1, cy - (int)(r * 0.12f), 2, (int)(r * 0.6f), col);
}
void icoSliders(int cx, int cy, int r, uint16_t col) {          // Settings
  int kx[3] = {cx - (int)(r * 0.35f), cx + (int)(r * 0.35f), cx - (int)(r * 0.15f)};
  for (int i = 0; i < 3; i++) {
    int yy = cy + (i - 1) * (int)(r * 0.6f);
    cv.drawLine(cx - r, yy, cx + r, yy, col);
    cv.fillRect(kx[i] - 2, yy - 2, 5, 5, col);
  }
}
void icoWifi(int cx, int cy, int r, uint16_t col) {
  int by = cy + (int)(r * 0.6f);                                        // 底部发射点
  cv.fillCircle(cx, by, 1, col);
  for (int k = 1; k <= 3; k++) {
    int rr = (int)(r * 0.34f * k);
    cv.drawArc(cx, by, rr, rr, 215, 325, col);
  }
}
void icoBt(int cx, int cy, int r, uint16_t col) {               // 蓝牙 rune
  int rx = (int)(r * 0.5f), ry = (int)(r * 0.45f);
  cv.drawLine(cx, cy - r, cx, cy + r, col);                            // 竖脊
  cv.drawLine(cx, cy - r, cx + rx, cy - ry, col);
  cv.drawLine(cx + rx, cy - ry, cx - rx, cy + ry, col);
  cv.drawLine(cx, cy + r, cx + rx, cy + ry, col);
  cv.drawLine(cx + rx, cy + ry, cx - rx, cy - ry, col);
}
void icoSpeaker(int cx, int cy, int r, uint16_t col) {
  int bx = cx - (int)(r * 0.75f);
  cv.fillRect(bx, cy - (int)(r * 0.3f), (int)(r * 0.35f), (int)(r * 0.6f) + 1, col);   // 箱体
  cv.fillTriangle(bx, cy - (int)(r * 0.58f), bx, cy + (int)(r * 0.58f),
                  bx + (int)(r * 0.72f), cy, col);                                 // 号角
  cv.drawArc(cx + (int)(r * 0.15f), cy, (int)(r * 0.5f), (int)(r * 0.5f), -55, 55, col);
  cv.drawArc(cx + (int)(r * 0.15f), cy, (int)(r * 0.85f), (int)(r * 0.85f), -55, 55, col);
}
void icoCompass(int cx, int cy, int r, uint16_t col) {
  cv.drawCircle(cx, cy, r, col);
  int ny = cy - (int)(r * 0.62f), sy = cy + (int)(r * 0.62f), hw = (int)(r * 0.24f);
  cv.fillTriangle(cx, ny, cx - hw, cy, cx + hw, cy, col);            // 北半针（实心）
  cv.drawLine(cx, sy, cx - hw, cy, col);                             // 南半针（描边）
  cv.drawLine(cx, sy, cx + hw, cy, col);
  cv.drawLine(cx - hw, cy, cx + hw, cy, col);
  cv.fillCircle(cx, cy, 1, col);
}
void icoFolder(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 1.6f), h = (int)(r * 1.15f);
  int x = cx - w / 2, y = cy - h / 2 + (int)(r * 0.16f);
  cv.drawLine(x, y - (int)(r * 0.3f), x + (int)(w * 0.5f), y - (int)(r * 0.3f), col);  // 标签顶
  cv.drawLine(x + (int)(w * 0.5f), y - (int)(r * 0.3f), x + (int)(w * 0.62f), y, col); // 标签斜边
  cv.drawRect(x, y, w, h, col);
}
void icoGnss(int cx, int cy, int r, uint16_t col) {   // 定位图钉
  int hcy = cy - (int)(r * 0.2f);                                    // 钉头圆心
  cv.fillCircle(cx, hcy, (int)(r * 0.55f), col);
  cv.fillCircle(cx, hcy, (int)(r * 0.22f), TFT_BLACK);
  cv.fillTriangle(cx - (int)(r * 0.4f), hcy + (int)(r * 0.28f),
                  cx + (int)(r * 0.4f), hcy + (int)(r * 0.28f), cx, cy + r, col);
}
// 对话气泡：圆角框 + 左下角小尖角，里面三个点表示"正在说话"
void icoChat(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 1.65f), h = (int)(r * 1.2f);
  int x = cx - w / 2, y = cy - h / 2 - (int)(r * 0.12f);
  cv.drawRoundRect(x, y, w, h, 4, col);
  cv.fillTriangle(x + (int)(w * 0.22f), y + h - 1, x + (int)(w * 0.46f), y + h - 1,
                  x + (int)(w * 0.22f), y + h + (int)(r * 0.42f), col);
  int dotY = y + h / 2;
  for (int i = -1; i <= 1; i++) cv.fillCircle(cx + i * (int)(r * 0.36f), dotY, 1, col);
}
// 均衡器三条柱：中间高两边低，一眼能看出是频谱/音频相关功能
void icoSpectrum(int cx, int cy, int r, uint16_t col) {
  int barW = (int)(r * 0.36f);
  int gap = (int)(r * 0.26f);
  int heights[3] = { (int)(r * 0.75f), (int)(r * 1.35f), (int)(r * 1.0f) };
  int baseY = cy + (int)(r * 0.68f);
  for (int i = 0; i < 3; i++) {
    int x = cx + (i - 1) * (barW + gap) - barW / 2;
    cv.fillRect(x, baseY - heights[i], barW, heights[i], col);
  }
}
// 电源符号（圆环缺口+竖线插入，缺口居于正上方与竖线对齐）：用于 Screen off 设置项
void icoPower(int cx, int cy, int r, uint16_t col) {
  cv.drawArc(cx, cy, r, r, -60, 240, col);
  cv.drawLine(cx, cy - r, cx, cy - (int)(r * 0.2f), col);
}
// 电池外框+正极凸起+约 60% 电量填充（示意图标，不代表实际电量）：用于 Battery 设置项
void icoBattery(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 1.7f), h = (int)(r * 0.95f);
  int x = cx - w / 2, y = cy - h / 2;
  cv.drawRect(x, y, w, h, col);
  cv.fillRect(x + w, y + (int)(h * 0.3f), (int)(r * 0.22f) + 1, (int)(h * 0.4f) + 1, col);
  cv.fillRect(x + 2, y + 2, (int)((w - 4) * 0.6f), h - 4, col);
}
// 热点图标：中心 AP 核心 + 向四周发散的同心圆（区别于 icoWifi 单向的信号扇形——
// 这个是"自己在全向广播"，不是"正在接收信号"）
void icoHotspot(int cx, int cy, int r, uint16_t col) {
  cv.fillCircle(cx, cy, (int)(r * 0.24f), col);
  cv.drawCircle(cx, cy, (int)(r * 0.6f), col);
  cv.drawCircle(cx, cy, r, col);
}
// 计算器：竖长机身，顶部一条显示屏，下面 3x3 按键点阵
void icoCalc(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 1.3f), h = (int)(r * 1.7f);
  int x = cx - w / 2, y = cy - h / 2;
  cv.drawRoundRect(x, y, w, h, 3, col);
  cv.drawRect(x + 3, y + 3, w - 6, (int)(h * 0.24f), col);                 // 显示屏
  int gy0 = y + (int)(h * 0.48f), sx = (int)(w * 0.28f), sy = (int)(h * 0.18f);
  for (int rr = 0; rr < 3; rr++)
    for (int cc = 0; cc < 3; cc++)
      cv.fillCircle(x + sx + cc * (int)(w * 0.22f), gy0 + rr * sy, 1, col); // 按键点阵
}
// 电波塔：塔架（三脚）+ 顶端向两侧发散的电波（区别于 Wi-Fi 单向扇形、热点同心圆——
// 强调"远距离定向发射"）。用于 LoRa 嗅探器
void icoLora(int cx, int cy, int r, uint16_t col) {
  int tip = cy - (int)(r * 0.55f);
  cv.drawLine(cx, tip, cx - (int)(r * 0.55f), cy + (int)(r * 0.85f), col);       // 左腿
  cv.drawLine(cx, tip, cx + (int)(r * 0.55f), cy + (int)(r * 0.85f), col);       // 右腿
  cv.drawLine(cx - (int)(r * 0.28f), cy + (int)(r * 0.15f),
              cx + (int)(r * 0.28f), cy + (int)(r * 0.15f), col);                // 横撑
  cv.fillCircle(cx, tip, 1, col);
  for (int k = 1; k <= 2; k++) {
    int rr = (int)(r * 0.34f * k);
    cv.drawArc(cx, tip, rr, rr, 205, 265, col);                                  // 左上电波
    cv.drawArc(cx, tip, rr, rr, 275, 335, col);                                  // 右上电波
  }
}
// 红外遥控器：机身 + 顶部红外 LED + 向上发散的红外波 + 2x2 按键（区别于计算器/喇叭）
void icoIr(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 0.95f), h = (int)(r * 1.25f);
  int x = cx - w / 2, y = cy - h / 2 + (int)(r * 0.28f);
  cv.drawRoundRect(x, y, w, h, 3, col);
  cv.fillCircle(cx, y + (int)(h * 0.16f), 2, col);                        // 顶部红外 LED
  for (int rr = 0; rr < 2; rr++)
    for (int cc = 0; cc < 2; cc++)
      cv.fillCircle(cx + (cc * 2 - 1) * (int)(w * 0.24f),
                    y + (int)(h * 0.5f) + rr * (int)(h * 0.26f), 1, col);  // 2x2 按键
  int tip = y - (int)(r * 0.15f);
  for (int k = 1; k <= 2; k++) {
    int rr = (int)(r * 0.26f * k);
    cv.drawArc(cx, tip, rr, rr, 235, 305, col);                          // 向上发散的红外波
  }
}
// WiFi 波 + 放大镜：混杂嗅探（区别于 WiFi Chan 的普通扇形）
void icoWsniff(int cx, int cy, int r, uint16_t col) {
  int by = cy - (int)(r * 0.25f);
  cv.fillCircle(cx - (int)(r * 0.15f), by + (int)(r * 0.2f), 1, col);
  for (int k = 1; k <= 2; k++) {
    int rr = (int)(r * 0.33f * k);
    cv.drawArc(cx - (int)(r * 0.15f), by + (int)(r * 0.2f), rr, rr, 215, 325, col);
  }
  int lr = (int)(r * 0.4f), lx = cx + (int)(r * 0.35f), ly = cy + (int)(r * 0.4f);
  cv.drawCircle(lx, ly, lr, col);
  cv.drawLine(lx + (int)(lr * 0.7f), ly + (int)(lr * 0.7f), lx + (int)(lr * 1.5f), ly + (int)(lr * 1.5f), col);
}
// 刻度尺：长方形外框 + 若干刻度线（换算器图标）
void icoRuler(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 1.8f), h = (int)(r * 0.6f);
  int x = cx - w / 2, y = cy - h / 2;
  cv.drawRect(x, y, w, h, col);
  // 5 个刻度：长短交替
  for (int i = 1; i <= 4; i++) {
    int tx = x + (int)(w * i / 5.0f);
    int th = (i == 2 || i == 3) ? h - 2 : (int)(h * 0.55f);
    cv.drawLine(tx, y, tx, y + th, col);
  }
}
// 四分音符：符头椭圆 + 竖茎 + 符尾（音乐播放器图标）
// 靶心+四向刻度：外圈+中圈两层同心圆，中心实心点，四个方向短刻度线戳出圈外——
// 一眼像"雷达/示波器探针打靶"，跟 Settings 复用的 icoSliders 区分开（NetProbe图标）。
void icoProbe(int cx, int cy, int r, uint16_t col) {
  cv.drawCircle(cx, cy, (int)(r * 0.85f), col);
  cv.drawCircle(cx, cy, (int)(r * 0.45f), col);
  cv.fillCircle(cx, cy, (int)(r * 0.12f), col);
  int outer = (int)(r * 1.05f), inner = (int)(r * 0.85f);
  cv.drawLine(cx, cy - outer, cx, cy - inner, col);
  cv.drawLine(cx, cy + inner, cx, cy + outer, col);
  cv.drawLine(cx - outer, cy, cx - inner, cy, col);
  cv.drawLine(cx + inner, cy, cx + outer, cy, col);
}
// 虫子：椭圆身体 + 圆头 + 左右各3条腿斜线戳出去——一眼"bug/fuzz"，跟靶心探针图标区分开。
void icoBug(int cx, int cy, int r, uint16_t col) {
  cv.fillEllipse(cx, cy + (int)(r * 0.1f), (int)(r * 0.5f), (int)(r * 0.65f), col);
  cv.fillCircle(cx, cy - (int)(r * 0.6f), (int)(r * 0.3f), col);
  for (int i = -1; i <= 1; i++) {
    int ly = cy - (int)(r * 0.3f) + i * (int)(r * 0.4f);
    cv.drawLine(cx - (int)(r * 0.5f), ly, cx - (int)(r * 1.0f), ly - (int)(r * 0.25f) * (i == 0 ? 0 : 1), col);
    cv.drawLine(cx + (int)(r * 0.5f), ly, cx + (int)(r * 1.0f), ly - (int)(r * 0.25f) * (i == 0 ? 0 : 1), col);
  }
}
// 车身+两个轮子 + 车顶上方发散的wifi弧——开车扫WiFi(Wardriving)，跟单纯的
// wifi图标(弧从一个点发散)、热点图标(同心圆)区分开：这两个都没有"车"的元素。
void icoWardrive(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 1.6f), h = (int)(r * 0.7f);
  int x = cx - w / 2, y = cy - h / 2 + (int)(r * 0.2f);
  cv.drawRoundRect(x, y, w, h, 3, col);
  cv.fillCircle(x + (int)(w * 0.22f), y + h, (int)(r * 0.16f), col);   // 左轮
  cv.fillCircle(x + (int)(w * 0.78f), y + h, (int)(r * 0.16f), col);   // 右轮
  int wy = y - (int)(r * 0.05f);
  for (int k = 1; k <= 2; k++) {
    int rr = (int)(r * 0.3f * k);
    cv.drawArc(cx, wy, rr, rr, 215, 325, col);                        // 车顶上方的wifi信号弧
  }
}
// 小黄鸭：圆头、鸭嘴、饱满身形与上翘尾羽（呼应 Hak5 Rubber Ducky / BadUSB 文化）
void icoDucky(int cx, int cy, int r, uint16_t col) {
  int by = cy + (int)(r * 0.18f);
  int bodyR = (int)(r * 0.62f);
  int bodyH = (int)(bodyR * 0.68f);
  // 躯干（饱满椭圆）
  cv.fillEllipse(cx + (int)(r * 0.05f), by, bodyR, bodyH, col);
  // 头部（圆球）
  int headCx = cx - (int)(r * 0.28f);
  int headCy = cy - (int)(r * 0.32f);
  int headR = (int)(r * 0.40f);
  cv.fillCircle(headCx, headCy, headR, col);
  // 鸭嘴（向左突出三角形）
  int beakTipX = headCx - headR - (int)(r * 0.36f);
  int beakY = headCy + 1;
  cv.fillTriangle(headCx - headR + 2, headCy - (int)(r * 0.15f),
                  headCx - headR + 2, headCy + (int)(r * 0.18f),
                  beakTipX, beakY, col);
  // 尾羽（右侧上翘三角形）
  int tailX = cx + bodyR + (int)(r * 0.22f);
  int tailY = by - (int)(r * 0.42f);
  cv.fillTriangle(cx + bodyR - 2, by + (int)(r * 0.08f),
                  cx + bodyR - 5, by - (int)(r * 0.18f),
                  tailX, tailY, col);
}
// 中心节点(本机) + 三个外围节点 + 连线——局域网拓扑示意，跟WiFi弧形/热点同心圆/
// NetProbe靶心都区分开：这是唯一画了"多个独立设备互连"这个元素的图标。
void icoLanscan(int cx, int cy, int r, uint16_t col) {
  cv.fillCircle(cx, cy, (int)(r * 0.22f), col);
  static const float A[3] = { -90.0f, 30.0f, 150.0f };
  for (int i = 0; i < 3; i++) {
    float a = A[i] * (float)M_PI / 180.0f;
    int nx = cx + (int)(cosf(a) * r), ny = cy + (int)(sinf(a) * r);
    cv.drawLine(cx, cy, nx, ny, col);
    cv.drawCircle(nx, ny, (int)(r * 0.2f), col);
  }
}
// 秒表：表盘往下挪一点，给顶上的按钮和把手留位置（跟 icoClock 区分开）
void icoTimer(int cx, int cy, int r, uint16_t col) {
  int dr = (int)(r * 0.78f), dy = cy + (int)(r * 0.18f);
  cv.drawCircle(cx, dy, dr, col);
  cv.fillRect(cx - 2, dy - dr - 3, 4, 3, col);                       // 顶部按钮
  cv.drawLine(cx - 4, dy - dr - 4, cx + 4, dy - dr - 4, col);
  cv.drawLine(cx, dy, cx + (int)(dr * 0.5f), dy - (int)(dr * 0.5f), col);   // 指针指向 1~2 点
  cv.fillCircle(cx, dy, 1, col);
}

// 天气：云朵 + 探头出来的半个太阳
void icoCloudSun(int cx, int cy, int r, uint16_t col) {
  int sx = cx + (int)(r * 0.45f), sy = cy - (int)(r * 0.45f);
  cv.drawCircle(sx, sy, (int)(r * 0.32f), col);
  for (int i = 0; i < 8; i += 2) {
    int x1 = sx + (int)(DIRS[i][0] * r * 0.44f), y1 = sy + (int)(DIRS[i][1] * r * 0.44f);
    int x2 = sx + (int)(DIRS[i][0] * r * 0.62f), y2 = sy + (int)(DIRS[i][1] * r * 0.62f);
    cv.drawLine(x1, y1, x2, y2, col);
  }
  int by = cy + (int)(r * 0.3f);
  cv.fillCircle(cx - (int)(r * 0.45f), by, (int)(r * 0.34f), col);
  cv.fillCircle(cx + (int)(r * 0.3f),  by, (int)(r * 0.28f), col);
  cv.fillCircle(cx - (int)(r * 0.05f), by - (int)(r * 0.2f), (int)(r * 0.38f), col);
  cv.fillRect(cx - (int)(r * 0.45f), by, (int)(r * 0.75f), (int)(r * 0.34f), col);
}

// 飞机：机头朝右上的剪影（跟 adsb 页里那个同一个造型，只是这里固定角度）
void icoPlane(int cx, int cy, int r, uint16_t col) {
  float s = r * 0.62f;
  const float a = -0.6f;                       // 稍微斜一点，正着画太像十字
  float fx = sinf(a), fy = -cosf(a), rx = cosf(a), ry = sinf(a);
  #define AX(al, si) (int)(cx + (al) * s * fx + (si) * s * rx)
  #define AY(al, si) (int)(cy + (al) * s * fy + (si) * s * ry)
  cv.fillTriangle(AX(1.5,0),AY(1.5,0), AX(-1.3,-0.22),AY(-1.3,-0.22), AX(-1.3,0.22),AY(-1.3,0.22), col);
  cv.fillTriangle(AX(0.35,0),AY(0.35,0), AX(-0.55,-1.35),AY(-0.55,-1.35), AX(-0.85,-1.25),AY(-0.85,-1.25), col);
  cv.fillTriangle(AX(0.35,0),AY(0.35,0), AX(-0.55,1.35),AY(-0.55,1.35), AX(-0.85,1.25),AY(-0.85,1.25), col);
  cv.fillTriangle(AX(-1.0,0),AY(-1.0,0), AX(-1.35,-0.55),AY(-1.35,-0.55), AX(-1.45,-0.48),AY(-1.45,-0.48), col);
  cv.fillTriangle(AX(-1.0,0),AY(-1.0,0), AX(-1.35,0.55),AY(-1.35,0.55), AX(-1.45,0.48),AY(-1.45,0.48), col);
  #undef AX
  #undef AY
}

// 卫星：机身本体 + 太阳能板（带网格及连杆）+ 向下（地球方向）发射的辐射波束弧线
void icoSat(int cx, int cy, int r, uint16_t col) {
  int b = (int)(r * 0.28f);
  int bodyY = cy - (int)(r * 0.22f);
  cv.drawRoundRect(cx - b, bodyY - b, b * 2, b * 2, 2, col);
  cv.fillCircle(cx, bodyY, 1, col);

  // 两侧太阳能板（带中线格线和连接杆）
  int wingW = (int)(r * 0.65f);
  int wingH = (int)(r * 0.42f);
  for (int i = -1; i <= 1; i += 2) {
    int wx = cx + i * (b + (int)(r * 0.15f));
    if (i == -1) wx -= wingW;
    int wy = bodyY - wingH / 2;
    cv.drawRect(wx, wy, wingW, wingH, col);
    cv.drawLine(wx + wingW / 2, wy, wx + wingW / 2, wy + wingH - 1, col);
    int strutX1 = cx + i * b;
    int strutX2 = wx + (i == -1 ? wingW : 0);
    cv.drawLine(strutX1, bodyY, strutX2, bodyY, col);
  }

  // 向下辐射发射弧线（对地通信波束）
  int beamY = bodyY + b;
  for (int k = 1; k <= 2; k++) {
    int rr = (int)(r * 0.32f * k) + 2;
    cv.drawArc(cx, beamY, rr, rr, 35, 145, col);
  }
}

// 台风：中心风眼 + 两条对称螺旋云带（对数螺线采样成折线）
void icoTyphoon(int cx, int cy, int r, uint16_t col) {
  cv.drawCircle(cx, cy, (int)(r * 0.22f), col);
  const int N = 14;
  for (int arm = 0; arm < 2; arm++) {
    float base = arm * 3.14159265f;
    int px = 0, py = 0;
    for (int i = 0; i <= N; i++) {
      float t   = (float)i / N;
      float ang = base + t * 2.6f;
      float rad = r * (0.28f + 0.72f * t);
      int x = cx + (int)(cosf(ang) * rad);
      int y = cy + (int)(sinf(ang) * rad * 0.86f);
      if (i) cv.drawLine(px, py, x, y, col);
      px = x; py = y;
    }
  }
}

// 地震：一条地震仪记录纸上的波形——左右平静、中间一串越来越大的尖峰。
// 不用"裂开的地面"那种画法：16px 见方画不出裂缝，画出来跟别的图标糊成一团。
void icoQuake(int cx, int cy, int r, uint16_t col) {
  // 每个采样点的相对高度（×r），中间那几个是主震
  static const float A[] = {0.0f, 0.08f, -0.06f, 0.10f, -0.75f, 0.95f, -0.85f,
                            0.45f, -0.30f, 0.14f, -0.09f, 0.0f};
  const int N = (int)(sizeof(A) / sizeof(A[0]));
  int px = 0, py = 0;
  for (int i = 0; i < N; i++) {
    int x = cx - r + (int)((float)i / (N - 1) * 2 * r);
    int y = cy - (int)(A[i] * r);
    if (i) cv.drawLine(px, py, x, y, col);
    px = x; py = y;
  }
  cv.drawFastHLine(cx - r, cy + r, 2 * r + 1, col);   // 记录纸的底边
}

// 汇率：上下两支反向的箭头（⇄）。不画 $ / ¥ 字样——16px 见方里两个货币符号会糊成一团，
// 而且写死符号就把"这是哪两种货币"钉在图标上了，换个币对图标就得跟着改。
void icoExchange(int cx, int cy, int r, uint16_t col) {
  const int half = (int)(r * 0.85f), dy = (int)(r * 0.42f), head = (int)(r * 0.3f);
  // 上面这支往右
  cv.drawFastHLine(cx - half, cy - dy, half * 2, col);
  cv.drawLine(cx + half, cy - dy, cx + half - head, cy - dy - head, col);
  cv.drawLine(cx + half, cy - dy, cx + half - head, cy - dy + head, col);
  // 下面这支往左
  cv.drawFastHLine(cx - half, cy + dy, half * 2, col);
  cv.drawLine(cx - half, cy + dy, cx - half + head, cy + dy - head, col);
  cv.drawLine(cx - half, cy + dy, cx - half + head, cy + dy + head, col);
}

// OKX 的标记：3×3 的方格里填四角和正中，拼成一个 X。
// 用方块拼而不是画两条斜线——斜线在这个尺寸下会有锯齿，而方块阵列在点阵屏上是干净的，
// 而且这正是那个标记本身的构成方式。
void icoOkx(int cx, int cy, int r, uint16_t col) {
  int cell = (int)(r * 0.60f);
  if (cell < 2) cell = 2;
  const int step = cell + (cell >= 6 ? 2 : 1);      // 格与格之间留一点缝，别糊成一块
  for (int gy = -1; gy <= 1; gy++)
    for (int gx = -1; gx <= 1; gx++) {
      const bool corner = (gx != 0 && gy != 0);
      const bool center = (gx == 0 && gy == 0);
      if (!corner && !center) continue;
      cv.fillRect(cx + gx * step - cell / 2, cy + gy * step - cell / 2, cell, cell, col);
    }
}

// 路由器：机箱 + 两根天线 + 指示灯
void icoRouter(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 1.5f), h = (int)(r * 0.55f);
  int top = cy + (int)(r * 0.15f);
  cv.drawRoundRect(cx - w / 2, top, w, h, 3, col);
  cv.drawLine(cx - (int)(r * 0.45f), top, cx - (int)(r * 0.8f), cy - r, col);
  cv.drawLine(cx + (int)(r * 0.45f), top, cx + (int)(r * 0.8f), cy - r, col);
  for (int i = -1; i <= 1; i++) cv.fillCircle(cx + i * 5, top + h / 2, 1, col);
}

// 终端窗口 >_ ：给 Debug 开关用。
// 原来偷懒复用了 icoBug，但那个是 DnsFuzz 的，设置里跟 app 列表撞图标了。
// 这个造型跟现有那二十几个都不像，而且"看日志/调试"的语义一眼就对。
void icoTerminal(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 1.9f), h = (int)(r * 1.45f);
  int x = cx - w / 2, y = cy - h / 2;
  cv.drawRoundRect(x, y, w, h, 2, col);
  cv.drawFastHLine(x + 1, y + 4, w - 2, col);          // 窗口标题栏那条横线
  // 提示符 ">" ：两笔折线，比 drawString 稳（这个尺寸下字体渲染会糊）
  int px = x + 4, py = y + h / 2 + 1;
  cv.drawLine(px, py - 3, px + 3, py, col);
  cv.drawLine(px + 3, py, px, py + 3, col);
  cv.drawFastHLine(px + 5, py + 3, 5, col);             // 光标下划线
}

// 信道占用曲线：基线 + 两个交叠的钟形包络，正是 WiFi Chan 页面画的那个东西。
// 原来这个 app 跟"设置→Wi-Fi"共用 icoWifi，两处撞图标；而且那个扇形本来表达的是
// "正在收信号"，用来指"信道分析"并不贴切。
void icoChanCurves(int cx, int cy, int r, uint16_t col) {
  int by = cy + (int)(r * 0.55f);                       // 基线
  cv.drawFastHLine(cx - r, by, r * 2, col);
  int rr = (int)(r * 0.62f);
  cv.drawArc(cx - (int)(r * 0.42f), by, rr, rr, 180, 360, col);   // 左边那个包络
  cv.drawArc(cx + (int)(r * 0.42f), by, rr, rr, 180, 360, col);   // 右边那个（交叠，正是重叠信道的意思）
}

// GitHub 官方 Octocat 标志。
// 这个形状手画不出来（曲线太多），所以走位图：从 GitHub 官方 favicon.svg
// （https://github.githubassets.com/favicons/favicon.svg，那是"深色圆形挖掉猫"的版本）
// 用 rsvg 渲到 256px，取"圆内 + 亮色"那部分反解出猫的实心轮廓，再缩到图标尺寸。
// 两个尺寸是因为菜单里选中的图标要大一圈(r 15->17)；直接缩放位图会糊，不如各存一份。
// 顺带一提：@primer/octicons 的 mark-github 路径渲染出来是空心的（描边式），
// 不能直接用，所以才绕道 favicon。
// 26x26，每行 4 字节（MSB 在左，跟 LGFX draw_bitmap 的 byteWidth=(w+7)>>3 一致）
static const uint8_t GH_MARK_26[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00,
  0x03, 0xDE, 0x70, 0x00, 0x03, 0xFF, 0xF0, 0x00, 0x03, 0xFF, 0xF0, 0x00,
  0x03, 0xFF, 0xF8, 0x00, 0x07, 0xFF, 0xF8, 0x00, 0x07, 0xFF, 0xF8, 0x00,
  0x07, 0xFF, 0xF8, 0x00, 0x07, 0xFF, 0xF8, 0x00, 0x07, 0xFF, 0xF8, 0x00,
  0x07, 0xFF, 0xF8, 0x00, 0x03, 0xFF, 0xF0, 0x00, 0x01, 0xFF, 0xE0, 0x00,
  0x08, 0x7F, 0x80, 0x00, 0x04, 0x3F, 0x00, 0x00, 0x06, 0x7F, 0x00, 0x00,
  0x03, 0xFF, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00,
  0x00, 0x3F, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00,
};

// 30x30，每行 4 字节（MSB 在左，跟 LGFX draw_bitmap 的 byteWidth=(w+7)>>3 一致）
static const uint8_t GH_MARK_30[] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x80, 0x06, 0x00, 0x01, 0xEF, 0xDE, 0x00, 0x01, 0xFF, 0xFE, 0x00,
  0x01, 0xFF, 0xFE, 0x00, 0x01, 0xFF, 0xFE, 0x00, 0x03, 0xFF, 0xFF, 0x00,
  0x03, 0xFF, 0xFF, 0x00, 0x07, 0xFF, 0xFF, 0x80, 0x07, 0xFF, 0xFF, 0x80,
  0x07, 0xFF, 0xFF, 0x80, 0x03, 0xFF, 0xFF, 0x00, 0x03, 0xFF, 0xFF, 0x00,
  0x03, 0xFF, 0xFF, 0x00, 0x01, 0xFF, 0xFE, 0x00, 0x00, 0xFF, 0xFC, 0x00,
  0x0C, 0x1F, 0xE0, 0x00, 0x06, 0x0F, 0xC0, 0x00, 0x03, 0x1F, 0xE0, 0x00,
  0x03, 0xFF, 0xE0, 0x00, 0x00, 0xFF, 0xE0, 0x00, 0x00, 0x1F, 0xE0, 0x00,
  0x00, 0x1F, 0xE0, 0x00, 0x00, 0x1F, 0xE0, 0x00, 0x00, 0x1F, 0xE0, 0x00,
};

void icoGithub(int cx, int cy, int r, uint16_t col) {
  const bool big = (r >= 16);
  const int w = big ? 30 : 26;
  cv.drawBitmap(cx - w / 2, cy - w / 2, big ? GH_MARK_30 : GH_MARK_26, w, w, col);
}

// 音乐音符：经典连杠双八分音符 (♫)，倾斜实心符头 + 坚实符杆 + 顶部连接梁
void icoNote(int cx, int cy, int r, uint16_t col) {
  int hw = (int)(r * 0.36f);
  int hh = (int)(r * 0.26f);
  int x1 = cx - (int)(r * 0.42f), y1 = cy + (int)(r * 0.42f);
  int x2 = cx + (int)(r * 0.38f), y2 = cy + (int)(r * 0.20f);
  cv.fillEllipse(x1, y1, hw, hh, col);
  cv.fillEllipse(x2, y2, hw, hh, col);
  int s1 = x1 + hw - 1;
  int s2 = x2 + hw - 1;
  int topY1 = cy - (int)(r * 0.65f);
  int topY2 = cy - (int)(r * 0.88f);
  cv.drawLine(s1, y1, s1, topY1, col);
  cv.drawLine(s1 - 1, y1, s1 - 1, topY1, col);
  cv.drawLine(s2, y2, s2, topY2, col);
  cv.drawLine(s2 - 1, y2, s2 - 1, topY2, col);
  cv.drawLine(s1, topY1, s2, topY2, col);
  cv.drawLine(s1, topY1 + 1, s2, topY2 + 1, col);
  cv.drawLine(s1, topY1 + 2, s2, topY2 + 2, col);
}

// 苹果：圆润外轮廓 + 顶部凹陷果蒂与斜叶 + 经典咬痕（呼应 Bad Apple）
void icoApple(int cx, int cy, int r, uint16_t col) {
  int by = cy + (int)(r * 0.12f);
  int br = (int)(r * 0.72f);
  int topY = by - br;

  // 果蒂斜茎
  cv.drawLine(cx, topY + 2, cx + 2, topY - (int)(r * 0.42f), col);
  cv.drawLine(cx + 1, topY + 2, cx + 3, topY - (int)(r * 0.42f), col);
  // 右上实心叶片
  cv.fillEllipse(cx + (int)(r * 0.32f), topY - (int)(r * 0.20f), (int)(r * 0.24f), (int)(r * 0.12f), col);

  // 左半边饱满轮廓
  cv.drawArc(cx, by, br, br, 95, 265, col);
  // 顶部入凹弧
  cv.drawArc(cx, by, br, br, 265, 305, col);
  // 咬痕凹陷（向内凹入的一段弧）
  int biteCx = cx + (int)(br * 0.78f);
  int biteCy = by - (int)(br * 0.10f);
  int biteR = (int)(br * 0.45f);
  cv.drawArc(biteCx, biteCy, biteR, biteR, 135, 275, col);
  // 底部右侧弧
  cv.drawArc(cx, by, br, br, 45, 95, col);
  // 顶底部微凹过渡线
  cv.drawLine(cx - 2, topY + 2, cx, topY, col);
  cv.drawLine(cx - 2, by + br - 1, cx + 2, by + br - 1, col);
}

// 收音机：矩形机身 + 顶部天线 + 左侧旋钮 + 右侧两段弧形电波
void icoRadio(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 1.7f), h = (int)(r * 1.1f);
  int x = cx - w / 2, y = cy - h / 2 + (int)(r * 0.18f);
  cv.drawRoundRect(x, y, w, h, 3, col);
  // 天线
  int ax = x + (int)(w * 0.75f);
  cv.drawLine(ax, y, ax + (int)(r * 0.3f), y - (int)(r * 0.9f), col);
  // 旋钮（左侧小圆）
  cv.drawCircle(x + (int)(w * 0.22f), y + h / 2, (int)(r * 0.22f), col);
  // 右侧两段弧形电波（发射信号）
  int ex = x + (int)(w * 0.72f), ey = y + h / 2;
  for (int k = 1; k <= 2; k++) {
    int rr = (int)(r * 0.3f * k);
    cv.drawArc(ex, ey, rr, rr, -55, 55, col);
  }
}

// 四旋翼：机身 + 四条臂 + 四个桨盘，中间一点表示正在播 Remote ID
void icoDrone(int cx, int cy, int r, uint16_t col) {
  const int a = (int)(r * 0.62f);          // 臂端偏移
  const int pr = (int)(r * 0.30f);         // 桨盘半径
  const int bx = (int)(r * 0.22f);         // 机身半宽
  cv.drawRect(cx - bx, cy - bx, bx * 2, bx * 2, col);
  const int dx[4] = {-a, a, -a, a}, dy[4] = {-a, -a, a, a};
  for (int i = 0; i < 4; i++) {
    cv.drawLine(cx + (dx[i] > 0 ? bx : -bx), cy + (dy[i] > 0 ? bx : -bx),
                cx + dx[i], cy + dy[i], col);
    cv.drawCircle(cx + dx[i], cy + dy[i], pr, col);
  }
  cv.fillCircle(cx, cy, 1, col);
}

// 打开的书本：书脊垂直平分线 + 左右微翘的书页轮廓 + 展开的文字排版
void icoBook(int cx, int cy, int r, uint16_t col) {
  int bw = (int)(r * 0.85f);
  int top_mid_y = cy - (int)(r * 0.25f);
  int bot_mid_y = cy + (int)(r * 0.55f);
  cv.drawFastVLine(cx, top_mid_y, bot_mid_y - top_mid_y + 1, col);

  int left_x = cx - bw;
  int right_x = cx + bw;
  int top_side_y = top_mid_y - (int)(r * 0.15f);
  int bot_side_y = bot_mid_y - (int)(r * 0.15f);

  cv.drawFastVLine(left_x, top_side_y, bot_side_y - top_side_y + 1, col);
  cv.drawFastVLine(right_x, top_side_y, bot_side_y - top_side_y + 1, col);

  int top_arch_y = top_mid_y - (int)(r * 0.28f);
  cv.drawLine(left_x, top_side_y, cx - bw / 2, top_arch_y, col);
  cv.drawLine(cx - bw / 2, top_arch_y, cx, top_mid_y, col);
  cv.drawLine(cx, top_mid_y, cx + bw / 2, top_arch_y, col);
  cv.drawLine(cx + bw / 2, top_arch_y, right_x, top_side_y, col);

  int bot_arch_y = bot_mid_y - (int)(r * 0.28f);
  cv.drawLine(left_x, bot_side_y, cx - bw / 2, bot_arch_y, col);
  cv.drawLine(cx - bw / 2, bot_arch_y, cx, bot_mid_y, col);
  cv.drawLine(cx, bot_mid_y, cx + bw / 2, bot_arch_y, col);
  cv.drawLine(cx + bw / 2, bot_arch_y, right_x, bot_side_y, col);

  if (r >= 10) {
    for (int i = 0; i < 2; i++) {
      int ly = top_side_y + 4 + i * 4;
      cv.drawLine(left_x + 3, ly, cx - 3, ly + 2, col);
      int ry = top_side_y + 4 + i * 4;
      cv.drawLine(cx + 3, ry + 2, right_x - 3, ry, col);
    }
  }
}

void drawAppIcon(int cx, int cy, int r, uint16_t col, AppId id) {
  if      (id == APP_TIME)    icoClock(cx, cy, r, col);
  else if (id == APP_ASTRO)   icoMoon(cx, cy, r, col);
  else if (id == APP_COMPASS) icoCompass(cx, cy, r, col);
  else if (id == APP_FILES)   icoFolder(cx, cy, r, col);
  else if (id == APP_GNSS)    icoGnss(cx, cy, r, col);
  else if (id == APP_MAP)     icoGlobe(cx, cy, r, col);
  else if (id == APP_BTSCAN)  icoBt(cx, cy, r, col);
  else if (id == APP_BTKB)    icoKeyboard(cx, cy, r, col);
  else if (id == APP_BTMEDIA) icoRemote(cx, cy, r, col);
  else if (id == APP_CHAT)    icoChat(cx, cy, r, col);
  else if (id == APP_SPECTRUM) icoSpectrum(cx, cy, r, col);
  else if (id == APP_WIFICHAN) icoChanCurves(cx, cy, r, col);
  else if (id == APP_HOTSPOT) icoHotspot(cx, cy, r, col);
  else if (id == APP_CALC)    icoCalc(cx, cy, r, col);
  else if (id == APP_LORA)    icoLora(cx, cy, r, col);
  else if (id == APP_RID)     icoDrone(cx, cy, r, col);
  else if (id == APP_IR)      icoIr(cx, cy, r, col);
  else if (id == APP_WSNIFF)  icoWsniff(cx, cy, r, col);
  else if (id == APP_CONV)    icoRuler(cx, cy, r, col);
  else if (id == APP_NETPROBE) icoProbe(cx, cy, r, col);
  else if (id == APP_WARDRIVE) icoWardrive(cx, cy, r, col);
  else if (id == APP_DUCKY)   icoDucky(cx, cy, r, col);
  else if (id == APP_LANSCAN) icoLanscan(cx, cy, r, col);
  else if (id == APP_WEATHER) icoCloudSun(cx, cy, r, col);
  else if (id == APP_ADSB)    icoPlane(cx, cy, r, col);
  else if (id == APP_SATS)    icoSat(cx, cy, r, col);
  else if (id == APP_TYPHOON) icoTyphoon(cx, cy, r, col);
  else if (id == APP_QUAKE)   icoQuake(cx, cy, r, col);
  else if (id == APP_FX)      icoExchange(cx, cy, r, col);
  else if (id == APP_OKX)     icoOkx(cx, cy, r, col);
  else if (id == APP_ROUTER)  icoRouter(cx, cy, r, col);
  else if (id == APP_GITHUB)  icoGithub(cx, cy, r, col);
  else if (id == APP_PLAYER)  icoNote(cx, cy, r, col);
  else if (id == APP_BADAPPLE) icoApple(cx, cy, r, col);
  else if (id == APP_RADIO)   icoRadio(cx, cy, r, col);
  else if (id == APP_READER)  icoBook(cx, cy, r, col);
  else if (id == APP_HASHOVEN) icoFlame(cx, cy, r, col);
  else if (id == APP_SSH)      icoTerminal(cx, cy, r, col);
  else                        icoSliders(cx, cy, r, col);
}

void icoFlame(int cx, int cy, int r, uint16_t col) {
  cv.fillTriangle(cx, cy - r, cx - (int)(r * 0.75f), cy + (int)(r * 0.7f), cx + (int)(r * 0.75f), cy + (int)(r * 0.7f), col);
  cv.fillCircle(cx, cy + (int)(r * 0.4f), (int)(r * 0.6f), col);
  cv.fillTriangle(cx, cy - (int)(r * 0.15f), cx - (int)(r * 0.35f), cy + (int)(r * 0.6f), cx + (int)(r * 0.35f), cy + (int)(r * 0.6f), TFT_BLACK);
}
// 蛾眉月：大圆减去一个右移的小圆。逐行算两圆的 x 区间再 drawFastHLine，
// 不逐像素（主菜单一帧要画 8 个图标），也不"先画两个实心圆再抠背景"
// ——图标是画在已有内容上的，抠背景会留下黑洞。
void icoMoon(int cx, int cy, int r, uint16_t col) {
  const float r2 = r * 0.92f;          // 被挖掉的那个圆
  const int   ox = (int)(r * 0.42f);   // 它右移多少，决定月牙多宽
  for (int dy = -r; dy <= r; dy++) {
    float w = sqrtf((float)(r * r - dy * dy));
    float xr = w;                                     // 默认整条弦都要
    float d2 = r2 * r2 - (float)(dy * dy);
    if (d2 > 0) xr = ox - sqrtf(d2);                  // 这一行被小圆截断
    if (xr <= -w) continue;                           // 整条都被吃掉
    int x0 = cx - (int)w, x1 = cx + (int)xr;
    cv.drawFastHLine(x0, cy + dy, x1 - x0 + 1, col);
  }
}

// 地球仪：一个圆 + 赤道 + 一条经线（竖着的椭圆）
void icoGlobe(int cx, int cy, int r, uint16_t col) {
  cv.drawCircle(cx, cy, r, col);
  cv.drawFastHLine(cx - r, cy, 2 * r + 1, col);
  cv.drawEllipse(cx, cy, r / 2, r, col);
}

// 键盘：一个扁框 + 三排键点 + 底下一条空格
void icoKeyboard(int cx, int cy, int r, uint16_t col) {
  const int w = (int)(r * 1.9f), h = (int)(r * 1.25f);
  cv.drawRoundRect(cx - w / 2, cy - h / 2, w, h, 2, col);
  for (int row = 0; row < 2; row++)
    for (int i = 0; i < 5; i++)
      cv.drawPixel(cx - w / 2 + 3 + i * (w - 6) / 4, cy - h / 2 + 3 + row * 3, col);
  cv.drawFastHLine(cx - w / 4, cy + h / 2 - 3, w / 2, col);   // 空格键
}

// 遥控器：竖着的圆角框 + 一个播放三角
void icoRemote(int cx, int cy, int r, uint16_t col) {
  const int w = (int)(r * 1.05f), h = (int)(r * 1.9f);
  cv.drawRoundRect(cx - w / 2, cy - h / 2, w, h, 3, col);
  const int t = (int)(r * 0.42f);
  cv.fillTriangle(cx - t / 2, cy - t, cx - t / 2, cy + t, cx + t, cy, col);
}

void icoTrash(int cx, int cy, int r, uint16_t col) {
  int hw = (int)(r * 0.62f);
  cv.drawLine(cx - r, cy - (int)(r * 0.55f), cx + r, cy - (int)(r * 0.55f), col);  // 盖
  cv.fillRect(cx - (int)(r * 0.28f), cy - (int)(r * 0.82f), (int)(r * 0.56f), (int)(r * 0.28f), col); // 提手
  int by = cy + (int)(r * 0.8f), bhw = (int)(r * 0.48f), ty = cy - (int)(r * 0.45f);
  cv.drawLine(cx - hw, ty, cx - bhw, by, col);                                     // 桶身（上宽下窄）
  cv.drawLine(cx + hw, ty, cx + bhw, by, col);
  cv.drawLine(cx - bhw, by, cx + bhw, by, col);                                    // 桶底
  for (int i = -1; i <= 1; i++)
    cv.drawLine(cx + i * (int)(r * 0.3f), cy - (int)(r * 0.25f),
                cx + i * (int)(r * 0.3f), cy + (int)(r * 0.55f), col);             // 竖纹
}
// 灯泡：玻璃泡 + 灯丝 + 灯座（区别于亮度的太阳）。用于 LED 开关
void icoLed(int cx, int cy, int r, uint16_t col) {
  int gy = cy - (int)(r * 0.2f);
  cv.drawCircle(cx, gy, (int)(r * 0.55f), col);
  cv.drawLine(cx - (int)(r * 0.22f), gy - (int)(r * 0.05f), cx, gy + (int)(r * 0.18f), col);   // 灯丝
  cv.drawLine(cx, gy + (int)(r * 0.18f), cx + (int)(r * 0.22f), gy - (int)(r * 0.05f), col);
  int bw = (int)(r * 0.5f), bx = cx - bw / 2, by = cy + (int)(r * 0.4f);
  cv.drawRect(bx, by, bw, (int)(r * 0.42f), col);
  cv.drawLine(bx, by + (int)(r * 0.2f), bx + bw, by + (int)(r * 0.2f), col);
}
// 温度计：细杆 + 底部实心水银泡 + 饱满上升水银柱 + 侧边刻度（用于温标切换 °C/°F）
void icoThermo(int cx, int cy, int r, uint16_t col) {
  int bulbR = (int)(r * 0.32f);
  int by = cy + (int)(r * 0.55f) - bulbR;
  int stemW = (int)(r * 0.36f);
  if (stemW < 4) stemW = 4;
  int topY = cy - r;
  cv.drawCircle(cx, by, bulbR, col);
  cv.drawRoundRect(cx - stemW / 2, topY, stemW, by - topY + 2, stemW / 2, col);
  cv.fillCircle(cx, by, bulbR - 1, col);
  int mercuryTop = cy - (int)(r * 0.20f);
  cv.fillRect(cx - stemW / 2 + 1, mercuryTop, stemW - 2, by - mercuryTop, col);
  for (int i = 1; i <= 3; i++) {
    int ty = topY + (int)((by - topY) * i / 4.0f);
    cv.drawLine(cx + stemW / 2, ty, cx + stemW / 2 + 3, ty, col);
  }
}

// 调色板：主体轮廓 + 拇指穿孔 + 三个颜料点（用于设置项中的主题切换，避免与主菜单的设置图标撞车）
void icoPalette(int cx, int cy, int r, uint16_t col) {
  int pr = (int)(r * 0.85f);
  cv.drawCircle(cx, cy, pr, col);
  int holeR = (pr >= 10) ? (int)(pr * 0.22f) : 1;
  cv.drawCircle(cx + (int)(pr * 0.45f), cy + (int)(pr * 0.30f), holeR, col);
  int dotR = (r <= 8) ? 1 : 2;
  cv.fillCircle(cx - (int)(pr * 0.45f), cy - (int)(pr * 0.25f), dotR, col);
  cv.fillCircle(cx - (int)(pr * 0.12f), cy - (int)(pr * 0.55f), dotR, col);
  cv.fillCircle(cx + (int)(pr * 0.36f), cy - (int)(pr * 0.45f), dotR, col);
}

// 经纬线时区地球：圆外框 + 本初子午线 + 经线椭圆 + 赤道横线 + 中央时针分针
void icoTimezone(int cx, int cy, int r, uint16_t col) {
  cv.drawCircle(cx, cy, r, col);
  cv.drawLine(cx, cy - r, cx, cy + r, col);
  cv.drawEllipse(cx, cy, r / 2, r, col);
  cv.drawFastHLine(cx - r, cy, 2 * r + 1, col);
  cv.drawLine(cx, cy, cx, cy - (int)(r * 0.55f), col);
  cv.drawLine(cx, cy, cx + (int)(r * 0.42f), cy, col);
  cv.fillCircle(cx, cy, 1, col);
}

// PC/显示器图标：显示屏外框 + 支架底座
void icoPc(int cx, int cy, int r, uint16_t col) {
  int w = (int)(r * 1.6f), h = (int)(r * 1.1f);
  int x = cx - w / 2, y = cy - h / 2 - (int)(r * 0.2f);
  cv.drawRoundRect(x, y, w, h, 2, col);
  int standY = y + h;
  cv.drawFastVLine(cx, standY, (int)(r * 0.35f), col);
  cv.drawFastHLine(cx - (int)(r * 0.45f), standY + (int)(r * 0.35f), (int)(r * 0.9f) + 1, col);
}

void drawSetIcon(int cx, int cy, int r, uint16_t col, SetId id) {
  switch (id) {
    case SET_WIFI:    icoWifi(cx, cy, r, col); break;
    case SET_GNSS:    icoGnss(cx, cy, r, col); break;
    case SET_PCMODE:  icoPc(cx, cy, r, col); break;
    case SET_BRIGHT:  icoSun(cx, cy, r, col); break;
    case SET_LED:     icoLed(cx, cy, r, col); break;
    case SET_VOL:     icoSpeaker(cx, cy, r, col); break;
    case SET_BOOT_SOUND: icoNote(cx, cy, r, col); break;
    case SET_THEME:   icoPalette(cx, cy, r, col); break;
    case SET_SLEEP:   icoPower(cx, cy, r, col); break;
    case SET_TZ:      icoTimezone(cx, cy, r, col); break;
    case SET_WX_UNIT: icoThermo(cx, cy, r, col); break;
    case SET_BATTERY: icoBattery(cx, cy, r, col); break;
    case SET_DEBUG:   icoTerminal(cx, cy, r, col); break;
    case SET_FORMAT:  icoTrash(cx, cy, r, col); break;
    case SET_ABOUT:   icoInfo(cx, cy, r, col); break;
    default: break;
  }
}
void drawBattery(int x, int y) {
  int w = 24, h = 13;
  cv.drawRect(x, y, w, h, TFT_DARKGREY);
  cv.fillRect(x + w, y + 4, 2, h - 8, TFT_DARKGREY);
  int lvl = powerBatteryLevel();
  if (lvl < 0) return;
  uint16_t c = (lvl <= 20) ? TFT_RED : (lvl <= 50 ? TFT_YELLOW : TFT_GREEN);
  cv.fillRect(x + 2, y + 2, (w - 4) * lvl / 100, h - 4, c);

  // 充电中：图标中央叠一个白色闪电（带黑描边保证在任何底色上都看得清）
  // 本机没有充电状态脚，充电与否是靠电压趋势推测的（见 power_util）
  if (powerCharging()) {
    int cx = x + w / 2, cy = y + h / 2;
    for (int i = 0; i < 2; i++) {
      uint16_t col = (i == 0) ? TFT_BLACK : TFT_WHITE;   // 先粗黑描边，再白色本体
      int d = (i == 0) ? 1 : 0;                          // 描边稍微外扩 1px
      cv.fillTriangle(cx + 2 + d, cy - 5 - d, cx - 3 - d, cy + 1, cx + 1, cy, col);
      cv.fillTriangle(cx - 2 - d, cy + 5 + d, cx + 3 + d, cy - 1, cx - 1, cy, col);
    }
  }
}
// Wi-Fi 信号强度：4 格柱状，按 RSSI 点亮
void drawSignal(int x, int y, int rssi, uint16_t on) {
  int lvl = (rssi >= -50) ? 4 : (rssi >= -60) ? 3 : (rssi >= -70) ? 2 : (rssi >= -80) ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    int bh = 3 + i * 2, bx = x + i * 4, by = y - bh;
    cv.fillRect(bx, by, 3, bh, i < lvl ? on : TFT_DARKGREY);
  }
}
// 顶栏用的小 Wi-Fi 图标（跟蜂窝网络那种柱状条区分开，画成无线电波纹样式）：
// 圆点+向上张开的三段弧线，弧线按信号强度分级点亮，弱信号只亮内圈
void drawWifiSignal(int cx, int by, int rssi, uint16_t on) {
  int lvl = (rssi >= -55) ? 3 : (rssi >= -70) ? 2 : (rssi >= -85) ? 1 : 0;
  cv.fillCircle(cx, by, 2, lvl > 0 ? on : TFT_DARKGREY);
  for (int k = 1; k <= 3; k++) {
    int rr = 2 + k * 2;
    cv.drawArc(cx, by, rr, rr, 210, 330, k <= lvl ? on : TFT_DARKGREY);
  }
}
// 加密网络的小挂锁
void drawLock(int cx, int cy, uint16_t col) {
  cv.drawRect(cx - 3, cy - 1, 6, 5, col);       // 锁body
  cv.drawArc(cx, cy - 1, 2, 2, 180, 360, col);  // 锁梁
}
