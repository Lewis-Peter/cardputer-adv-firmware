#include "imu.h"
#include "ui_common.h"
#include <cmath>

static float imuAx = 0.0f, imuAy = 0.0f, imuAz = 1.0f;
static float imuGx = 0.0f, imuGy = 0.0f, imuGz = 0.0f;
static float imuPanelAngle = 0.0f;   // 旋转角度（弧度）
static float imuYaw = 0.0f;          // 陀螺仪 Z 轴积分累加值（度）
static uint32_t imuYawTime = 0;

// ---- PC 主导模式：IMU 串口流式输出 ----
static bool imuStreamActive = false;
static int  imuStreamHz = 50;
static uint32_t imuStreamNextMs = 0;

// 从底层 BMI270 读取最新数据并执行姿态解算。
// 返回是否有新样本就绪（用于状态判断与复用）。
static bool imuUpdate() {
  if (!M5.Imu.update()) return false;
  M5.Imu.getAccel(&imuAx, &imuAy, &imuAz);
  M5.Imu.getGyro(&imuGx, &imuGy, &imuGz);

  uint32_t now = millis();
  if (imuYawTime == 0) { imuYawTime = now; return true; }
  float dt = (now - imuYawTime) / 1000.0f;
  imuYawTime = now;
  if (dt <= 0.0f || dt > 0.5f) dt = 0.02f;

  // 陀螺仪死区滤波：静态噪声（< 0.4 dps）不积分，消除静止飘移；
  // 取消原版 200ms 静止暴力清零，转角稳定保持；需归零按 Enter
  if (fabsf(imuGz) > 0.4f) {
    imuYaw += imuGz * dt;
    imuPanelAngle = imuYaw * (float)M_PI / 180.0f;
  }
  return true;
}

void imuSample() {
  // 当 IMU 串口流开启时，由 loop() 里的 imuStreamTick 负责驱动 M5.Imu.update()，
  // UI 页面直接复用缓存值（imuAx/imuAy/imuAz/imuGx/imuGy/imuGz/imuYaw/imuPanelAngle）。
  // 依据：M5Unified 的 BMI270_Class 在读取 INT_STATUS_1 或数据寄存器后会清除硬件数据就绪标志，
  // 若两处同时调用 update() 会发生竞态抢样本导致一方读到 0。
  if (imuStreamActive) return;

  imuUpdate();
}

void imuStreamSet(bool on, int hz) {
  if (on) {
    if (hz < 10) hz = 10;
    if (hz > 100) hz = 100;
    imuStreamActive = true;
    imuStreamHz = hz;
    imuUpdate();
    Serial.printf("IMU {\"t\":\"start\",\"hz\":%d}\n", hz);
    imuStreamNextMs = millis() + (1000 / hz);
  } else {
    imuStreamActive = false;
    Serial.println("IMU {\"t\":\"end\"}");
  }
}

bool imuStreamIsActive() {
  return imuStreamActive;
}

int imuStreamGetHz() {
  return imuStreamHz;
}

static void fmtVal(char* out, size_t sz, float v, int prec) {
  if (std::isnan(v) || std::isinf(v)) {
    snprintf(out, sz, "null");
  } else {
    if (prec == 3 && fabsf(v) < 0.0005f) v = 0.0f;
    else if (prec == 2 && fabsf(v) < 0.005f) v = 0.0f;
    snprintf(out, sz, "%.*f", prec, v);
  }
}

void imuStreamTick() {
  if (!imuStreamActive) return;

  // 只要流开着，每轮循环都尝试捕获底层硬件产生的新数据并更新缓存
  imuUpdate();

  uint32_t now = millis();
  if ((int32_t)(now - imuStreamNextMs) < 0) return;

  uint32_t interval = 1000 / imuStreamHz;
  // 丢样本不补发：如果主循环卡顿延迟超过一个采样周期，
  // 直接跳过积压，重置下一次采样时刻为 now + interval，防止连发过时数据
  if ((int32_t)(now - imuStreamNextMs) >= (int32_t)interval) {
    imuStreamNextMs = now + interval;
  } else {
    imuStreamNextMs += interval;
  }

  float ax = M5.Imu.isEnabled() ? imuAx : NAN;
  float ay = M5.Imu.isEnabled() ? imuAy : NAN;
  float az = M5.Imu.isEnabled() ? imuAz : NAN;
  float gx = M5.Imu.isEnabled() ? imuGx : NAN;
  float gy = M5.Imu.isEnabled() ? imuGy : NAN;
  float gz = M5.Imu.isEnabled() ? imuGz : NAN;

  char axBuf[16], ayBuf[16], azBuf[16];
  char gxBuf[16], gyBuf[16], gzBuf[16];
  fmtVal(axBuf, sizeof(axBuf), ax, 3);
  fmtVal(ayBuf, sizeof(ayBuf), ay, 3);
  fmtVal(azBuf, sizeof(azBuf), az, 3);
  fmtVal(gxBuf, sizeof(gxBuf), gx, 2);
  fmtVal(gyBuf, sizeof(gyBuf), gy, 2);
  fmtVal(gzBuf, sizeof(gzBuf), gz, 2);

  // 严格在栈上构建，零堆分配
  char line[160];
  snprintf(line, sizeof(line),
           "IMU {\"t\":\"s\",\"ts\":%lu,\"a\":[%s,%s,%s],\"g\":[%s,%s,%s]}",
           (unsigned long)now, axBuf, ayBuf, azBuf, gxBuf, gyBuf, gzBuf);
  Serial.println(line);
}

void imuTare() {
  imuYaw = 0.0f;
  imuPanelAngle = 0.0f;
}

static uint16_t rgb(uint32_t c) { return cv.color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }
#define IMU_ACCEL_COL rgb(0x8FC8AA)
#define IMU_GYRO_COL  rgb(0x88AED9)
#define APPLE_ORANGE  rgb(0xFF9F0A)

// 绘制双向居中条形表 (Center-zero bidirectional bar)
static void drawCenterBar(int x, int y, int w, int h, float val, float maxVal, uint16_t barCol) {
  cv.fillRect(x, y, w, h, 0x1082);
  int cx = x + w / 2;
  cv.drawFastVLine(cx, y - 1, h + 2, 0x8410);

  float norm = constrain(val / maxVal, -1.0f, 1.0f);
  int barW = (int)(fabsf(norm) * (w / 2));
  if (barW > 0) {
    if (norm > 0) {
      cv.fillRect(cx + 1, y, barW, h, barCol);
    } else {
      cv.fillRect(cx - barW, y, barW, h, barCol);
    }
  }
}

// 绘制单向条形表 (0..maxVal)
static void drawMeterBar(int x, int y, int w, int h, float val, float maxVal, uint16_t barCol) {
  cv.fillRect(x, y, w, h, 0x1082);
  float norm = constrain(val / maxVal, 0.0f, 1.0f);
  int barW = (int)(norm * w);
  if (barW > 0) {
    cv.fillRect(x, y, barW, h, barCol);
  }
}

// 主界面：航空姿态与水平仪 (Primary Flight / Level HUD)
void drawCompass() {
  cv.fillScreen(TFT_BLACK);

  float pitch = atan2f(-imuAx, sqrtf(imuAy * imuAy + imuAz * imuAz)) * 180.0f / (float)M_PI;
  float roll  = atan2f(imuAy, imuAz) * 180.0f / (float)M_PI;
  const bool level = fabsf(pitch) < 2.0f && fabsf(roll) < 2.0f;
  float g = sqrtf(imuAx * imuAx + imuAy * imuAy + imuAz * imuAz);

  drawPageHeader("IMU Horizon", level ? "LEVEL 0.0" : nullptr, level ? 0x07E0 : 0x8410);

  // ---- 左侧：360°旋转罗盘与水平仪靶心 ----
  const int cx = 58, cy = 73, R = 44;

  // 1. 外表圈与分度刻度
  for (int i = 0; i < 36; i++) {
    float a = imuPanelAngle + i * (float)M_PI / 18.0f;
    bool major = (i % 3 == 0);
    int r1 = R - (major ? 9 : 5), r2 = R - 2;
    int x1 = cx + (int)(sinf(a) * r1), y1 = cy - (int)(cosf(a) * r1);
    int x2 = cx + (int)(sinf(a) * r2), y2 = cy - (int)(cosf(a) * r2);
    cv.drawLine(x1, y1, x2, y2, major ? TFT_WHITE : 0x31A6);
  }
  cv.drawCircle(cx, cy, R, 0x2124);
  cv.drawCircle(cx, cy, R - 1, 0x18E3);

  // 2. 旋转四方位文字
  static const char* CARD_NAMES[] = {"N", "E", "S", "W"};
  static const uint16_t CARD_COLS[] = {0xF800, 0xCE79, 0x07FF, 0xCE79};
  cv.setTextSize(1);
  cv.setTextDatum(middle_center);
  for (int k = 0; k < 4; k++) {
    float a = imuPanelAngle + k * (float)M_PI / 2.0f;
    int lx = cx + (int)(sinf(a) * (R - 15));
    int ly = cy - (int)(cosf(a) * (R - 15));
    cv.setTextColor(CARD_COLS[k], TFT_BLACK);
    cv.drawString(CARD_NAMES[k], lx, ly);
  }

  // 3. 中心水平仪靶心环与十字准星
  uint16_t tgtCol = level ? 0x07E0 : 0x2945;
  cv.drawCircle(cx, cy, 12, tgtCol);
  cv.drawCircle(cx, cy, 22, 0x18E3);
  cv.drawFastHLine(cx - 16, cy, 5, tgtCol);
  cv.drawFastHLine(cx + 11, cy, 5, tgtCol);
  cv.drawFastVLine(cx, cy - 16, 5, tgtCol);
  cv.drawFastVLine(cx, cy + 11, 5, tgtCol);

  // 4. 浮动水平气泡 (越水平越接近圆心，水平合拢变绿)
  int bx = -constrain((int)(roll  * 1.0f), -(R - 16), R - 16);
  int by =  constrain((int)(pitch * 1.0f), -(R - 16), R - 16);
  int mx = cx + bx, my = cy + by;
  uint16_t bCol = level ? 0x07E0 : APPLE_ORANGE;
  uint16_t bBg  = level ? 0x0280 : 0x3100;
  cv.fillCircle(mx, my, 5, bBg);
  cv.drawCircle(mx, my, 5, bCol);
  cv.drawFastHLine(mx - 2, my, 5, bCol);
  cv.drawFastVLine(mx, my - 2, 5, bCol);

  // 5. 顶部固定红色科技三角指针
  cv.fillTriangle(cx - 4, cy - R - 5, cx + 4, cy - R - 5, cx, cy - R + 3, TFT_RED);
  cv.drawTriangle(cx - 4, cy - R - 5, cx + 4, cy - R - 5, cx, cy - R + 3, 0xFBE0);

  // ---- 右侧：飞行遥测卡 (Avionics Telemetry HUD Card) ----
  const int cardX = 118, cardY = 15, cardW = 118, cardH = 108;
  cv.fillRoundRect(cardX, cardY, cardW, cardH, 3, 0x0821);
  cv.drawRoundRect(cardX, cardY, cardW, cardH, 3, 0x18E3);
  cv.fillRect(cardX, cardY, 2, cardH, 0x07FF);   // 青色边缘条

  char b[24];
  int deg = ((int)imuYaw) % 360;
  if (deg < 0) deg += 360;

  // 相对航向角 (TURN)
  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("TURN", cardX + 6, cardY + 4);

  snprintf(b, sizeof(b), "%03d", deg);
  cv.setTextDatum(top_right); cv.setTextSize(2);
  cv.setTextColor(TFT_WHITE, 0x0821);
  cv.drawString(b, cardX + cardW - 12, cardY + 2);
  cv.drawCircle(cardX + cardW - 8, cardY + 4, 2, 0x07FF); // 精致度数圈

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x632C, 0x0821);
  cv.drawString("TARE: Enter", cardX + 6, cardY + 18);
  cv.setTextDatum(top_right);
  cv.drawString("rel", cardX + cardW - 6, cardY + 18);

  cv.drawFastHLine(cardX + 4, cardY + 28, cardW - 8, 0x18E3);

  // 俯仰角 PITCH 与双向条形表
  cv.setTextDatum(top_left);
  cv.setTextColor(0x9CD3, 0x0821);
  cv.drawString("PITCH", cardX + 6, cardY + 31);
  cv.setTextDatum(top_right);
  cv.setTextColor(fabsf(pitch) < 2.0f ? 0x07E0 : TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%+.1f", pitch);
  cv.drawString(b, cardX + cardW - 10, cardY + 31);
  cv.drawCircle(cardX + cardW - 7, cardY + 32, 1, fabsf(pitch) < 2.0f ? 0x07E0 : 0x9CD3);
  drawCenterBar(cardX + 6, cardY + 41, cardW - 12, 3, pitch, 45.0f, fabsf(pitch) < 2.0f ? 0x07E0 : 0x07FF);

  // 横滚角 ROLL 与双向条形表
  cv.setTextDatum(top_left);
  cv.setTextColor(0x9CD3, 0x0821);
  cv.drawString("ROLL", cardX + 6, cardY + 48);
  cv.setTextDatum(top_right);
  cv.setTextColor(fabsf(roll) < 2.0f ? 0x07E0 : TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%+.1f", roll);
  cv.drawString(b, cardX + cardW - 10, cardY + 48);
  cv.drawCircle(cardX + cardW - 7, cardY + 49, 1, fabsf(roll) < 2.0f ? 0x07E0 : 0x9CD3);
  drawCenterBar(cardX + 6, cardY + 58, cardW - 12, 3, roll, 45.0f, fabsf(roll) < 2.0f ? 0x07E0 : 0x07FF);

  cv.drawFastHLine(cardX + 4, cardY + 65, cardW - 8, 0x18E3);

  // 合成加速度 TOTAL G 与 G-meter
  cv.setTextDatum(top_left);
  cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("TOTAL G", cardX + 6, cardY + 68);
  cv.setTextDatum(top_right);
  cv.setTextColor(TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%.2fg", g);
  cv.drawString(b, cardX + cardW - 6, cardY + 68);
  drawMeterBar(cardX + 6, cardY + 78, cardW - 12, 3, g, 2.5f, g < 0.3f ? 0x07FF : (g > 1.8f ? 0xF800 : 0x07E0));

  // 实时姿态状态徽章
  const int badgeW = cardW - 12, badgeH = 13, badgeX = cardX + 6, badgeY = cardY + 88;
  if (level) {
    cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x0280);
    cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x04A0);
    cv.setTextDatum(middle_center);
    cv.setTextColor(0x07E0, 0x0280);
    cv.drawString("[o] LEVEL 0.0", badgeX + badgeW / 2, badgeY + badgeH / 2);
  } else if (g < 0.3f) {
    cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x0114);
    cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x033F);
    cv.setTextDatum(middle_center);
    cv.setTextColor(0x07FF, 0x0114);
    cv.drawString("[!] FREE FALL", badgeX + badgeW / 2, badgeY + badgeH / 2);
  } else if (g > 1.8f) {
    cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x3800);
    cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x7800);
    cv.setTextDatum(middle_center);
    cv.setTextColor(0xF800, 0x3800);
    cv.drawString("[!] HIGH G-LOAD", badgeX + badgeW / 2, badgeY + badgeH / 2);
  } else {
    cv.fillRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x31E0);
    cv.drawRoundRect(badgeX, badgeY, badgeW, badgeH, 2, 0x63E0);
    cv.setTextDatum(middle_center);
    cv.setTextColor(APPLE_ORANGE, 0x31E0);
    cv.drawString("[*] ATTITUDE TILT", badgeX + badgeW / 2, badgeY + badgeH / 2);
  }

  drawPageDots();
}

// 详情页：六轴传感器遥测实验台 (Sensor Lab)
void drawImuDetail() {
  cv.fillScreen(TFT_BLACK);

  float imuTemp = 0.0f;
  M5.Imu.getTemp(&imuTemp);
  char hdrR[24];
  snprintf(hdrR, sizeof(hdrR), "%.1fC  100Hz", imuTemp);
  drawPageHeader("IMU Data", hdrR, 0x07FF);

  const int y = 15, h = 91, w = (SW - 12) / 2; // w = 114
  char b[24];

  // 1. 加速度计卡片 (ACCELEROMETER)
  const int axX = 4;
  cv.fillRoundRect(axX, y, w, h, 3, 0x0821);
  cv.drawRoundRect(axX, y, w, h, 3, 0x18E3);
  cv.fillRect(axX, y, 2, h, IMU_ACCEL_COL);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(IMU_ACCEL_COL, 0x0821);
  cv.drawString("ACCEL g", axX + 6, y + 4);
  cv.setTextDatum(top_right);
  cv.setTextColor(0x632C, 0x0821);
  cv.drawString("+-4g", axX + w - 6, y + 4);

  cv.drawFastHLine(axX + 4, y + 14, w - 8, 0x18E3);

  // X 轴
  cv.setTextDatum(top_left); cv.setTextColor(0x9CD3, 0x0821);
  cv.drawString("X", axX + 6, y + 17);
  cv.setTextDatum(top_right); cv.setTextColor(TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%+.2f", imuAx); cv.drawString(b, axX + w - 6, y + 17);
  drawCenterBar(axX + 6, y + 26, w - 12, 3, imuAx, 2.0f, IMU_ACCEL_COL);

  // Y 轴
  cv.setTextDatum(top_left); cv.setTextColor(0x9CD3, 0x0821);
  cv.drawString("Y", axX + 6, y + 32);
  cv.setTextDatum(top_right); cv.setTextColor(TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%+.2f", imuAy); cv.drawString(b, axX + w - 6, y + 32);
  drawCenterBar(axX + 6, y + 41, w - 12, 3, imuAy, 2.0f, IMU_ACCEL_COL);

  // Z 轴
  cv.setTextDatum(top_left); cv.setTextColor(0x9CD3, 0x0821);
  cv.drawString("Z", axX + 6, y + 47);
  cv.setTextDatum(top_right); cv.setTextColor(TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%+.2f", imuAz); cv.drawString(b, axX + w - 6, y + 47);
  drawCenterBar(axX + 6, y + 56, w - 12, 3, imuAz, 2.0f, IMU_ACCEL_COL);

  cv.drawFastHLine(axX + 4, y + 62, w - 8, 0x18E3);

  // 加速度总模长 |a|
  float g = sqrtf(imuAx * imuAx + imuAy * imuAy + imuAz * imuAz);
  cv.setTextDatum(top_left); cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("|a|", axX + 6, y + 66);
  cv.setTextDatum(top_right); cv.setTextColor(TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%.2f g", g); cv.drawString(b, axX + w - 6, y + 66);
  drawMeterBar(axX + 6, y + 78, w - 12, 3, g, 2.5f, 0x07E0);

  // 2. 陀螺仪卡片 (GYROSCOPE)
  const int gyX = 122;
  cv.fillRoundRect(gyX, y, w, h, 3, 0x0821);
  cv.drawRoundRect(gyX, y, w, h, 3, 0x18E3);
  cv.fillRect(gyX, y, 2, h, IMU_GYRO_COL);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(IMU_GYRO_COL, 0x0821);
  cv.drawString("GYRO dps", gyX + 6, y + 4);
  cv.setTextDatum(top_right);
  cv.setTextColor(0x632C, 0x0821);
  cv.drawString("+-2000", gyX + w - 6, y + 4);

  cv.drawFastHLine(gyX + 4, y + 14, w - 8, 0x18E3);

  // X 轴
  cv.setTextDatum(top_left); cv.setTextColor(0x9CD3, 0x0821);
  cv.drawString("X", gyX + 6, y + 17);
  cv.setTextDatum(top_right); cv.setTextColor(TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%+.1f", imuGx); cv.drawString(b, gyX + w - 6, y + 17);
  drawCenterBar(gyX + 6, y + 26, w - 12, 3, imuGx, 150.0f, IMU_GYRO_COL);

  // Y 轴
  cv.setTextDatum(top_left); cv.setTextColor(0x9CD3, 0x0821);
  cv.drawString("Y", gyX + 6, y + 32);
  cv.setTextDatum(top_right); cv.setTextColor(TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%+.1f", imuGy); cv.drawString(b, gyX + w - 6, y + 32);
  drawCenterBar(gyX + 6, y + 41, w - 12, 3, imuGy, 150.0f, IMU_GYRO_COL);

  // Z 轴
  cv.setTextDatum(top_left); cv.setTextColor(0x9CD3, 0x0821);
  cv.drawString("Z", gyX + 6, y + 47);
  cv.setTextDatum(top_right); cv.setTextColor(TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%+.1f", imuGz); cv.drawString(b, gyX + w - 6, y + 47);
  drawCenterBar(gyX + 6, y + 56, w - 12, 3, imuGz, 150.0f, IMU_GYRO_COL);

  cv.drawFastHLine(gyX + 4, y + 62, w - 8, 0x18E3);

  // 角速度总模长 |w|
  float wMag = sqrtf(imuGx * imuGx + imuGy * imuGy + imuGz * imuGz);
  cv.setTextDatum(top_left); cv.setTextColor(0x7BEF, 0x0821);
  cv.drawString("|w|", gyX + 6, y + 66);
  cv.setTextDatum(top_right); cv.setTextColor(TFT_WHITE, 0x0821);
  snprintf(b, sizeof(b), "%.1f dps", wMag); cv.drawString(b, gyX + w - 6, y + 66);
  drawMeterBar(gyX + 6, y + 78, w - 12, 3, wMag, 200.0f, 0x07FF);

  // 3. 底部芯片硬件检视条
  const int botY = 109, botH = 14;
  cv.fillRoundRect(4, botY, SW - 8, botH, 2, 0x0821);
  cv.drawRoundRect(4, botY, SW - 8, botH, 2, 0x18E3);

  cv.setTextDatum(top_left); cv.setTextSize(1);
  cv.setTextColor(0x632C, 0x0821);
  cv.drawString("HW:", 10, botY + 3);
  cv.setTextColor(0x07FF, 0x0821);
  cv.drawString("BMI270", 30, botY + 3);

  cv.setTextDatum(top_center);
  cv.setTextColor(0xFBE0, 0x0821);
  snprintf(b, sizeof(b), "DIE %.1fC", imuTemp);
  cv.drawString(b, SW / 2, botY + 3);

  cv.setTextDatum(top_right);
  cv.setTextColor(0x07E0, 0x0821);
  cv.drawString("ACTIVE 100Hz", SW - 10, botY + 3);

  drawPageDots();
}
