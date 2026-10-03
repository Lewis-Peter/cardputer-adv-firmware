#include "Arduino.h"
#include "M5Unified.h"
#include "WiFi.h"
#include "gnss.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <cassert>
#include <ArduinoJson.h>
#include <deque>

// ---- 替身全局状态 ----
uint32_t g_fakeNowMs = 1000;
std::deque<char> g_simGpsRxBuf;
SimSerial Serial;

static void feedNmea(const char* s) {
  if (!s) return;
  if (*s == '$') s++;
  std::string body;
  while (*s && *s != '*' && *s != '\r' && *s != '\n') {
    body += *s++;
  }
  uint8_t sum = 0;
  for (char c : body) sum ^= (uint8_t)c;
  char line[256];
  snprintf(line, sizeof(line), "$%s*%02X\r\n", body.c_str(), sum);
  for (char* p = line; *p; p++) g_simGpsRxBuf.push_back(*p);
  gnssPoll();
}
SimM5 M5;
M5Canvas cv;
SimWiFi WiFi;
SimEsp ESP;

bool dirty = false;
int SW = 240, SH = 135;
uint16_t ACCENT = 0x07E0, CARD_BG = 0x18C3, DIM_BORDER = 0x2965, ICON_DIM = 0x4A69;

void centerMsg(const char*, uint16_t) {}
void drawPageDots() {}
void drawPageDots(int, int) {}
void drawPageHeader(const char*, const char*, uint16_t) {}
void drawScrollBar(int, int, int, int, int, int, uint16_t, uint16_t) {}
void drawMiniVuMeter(int, int, uint8_t, bool, uint16_t, uint16_t) {}
String fmtBytes(uint64_t) { return ""; }
String trunc(const String& s, int) { return s; }
bool wifiConnected() { return false; }
String wifiMac() { return ""; }
void geolocRequest() {}
void netJobSubmit(void (*)(void*), void*, const char*) {}
void drawWorldMap(int, int, int, int, double, double, double, double) {}
bool wifiEnsureConnected() { return false; }

struct GeoFix;
bool geoGet(GeoFix&, String*) { return false; }
bool canvasAvailable() { return true; }

extern const uint8_t WORLD_MASK[3270] = {0};
bool timeSynced = false;
bool timeFromGps = false;
bool debugOn = false;
bool sdMounted = false;   // 离线地图瓦片走 sdReady()；测试里当没插卡
const char* TZ_INFO = "UTC0";

// ---- 测试计数与断言 ----
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* name, const char* detail = "") {
  if (cond) {
    g_pass++;
    printf("  [PASS] %s\n", name);
  } else {
    g_fail++;
    printf("  [FAIL] %s %s\n", name, detail);
  }
}

// =================================================================
// 1. 坐标换算测试 (formatDms, toMaidenhead, toUtm)
// =================================================================
void testFormatDms() {
  printf("--- 1.1 formatDms 度分秒换算测试 ---\n");
  char buf[64];

  // 基础经纬度与符号
  formatDms(31.2304, true, buf, sizeof(buf));
  check(strcmp(buf, "31d13'49.4\"N") == 0, "DMS: 31.2304N -> 31d13'49.4\"N", buf);

  formatDms(-31.2304, true, buf, sizeof(buf));
  check(strcmp(buf, "31d13'49.4\"S") == 0, "DMS: -31.2304S -> 31d13'49.4\"S", buf);

  formatDms(121.4737, false, buf, sizeof(buf));
  check(strcmp(buf, "121d28'25.3\"E") == 0, "DMS: 121.4737E -> 121d28'25.3\"E", buf);

  formatDms(-121.4737, false, buf, sizeof(buf));
  check(strcmp(buf, "121d28'25.3\"W") == 0, "DMS: -121.4737W -> 121d28'25.3\"W", buf);

  // 0度与边界
  formatDms(0.0, true, buf, sizeof(buf));
  check(strcmp(buf, "0d00'00.0\"N") == 0, "DMS: 0.0 lat -> 0d00'00.0\"N", buf);

  formatDms(0.0, false, buf, sizeof(buf));
  check(strcmp(buf, "0d00'00.0\"E") == 0, "DMS: 0.0 lon -> 0d00'00.0\"E", buf);

  formatDms(180.0, false, buf, sizeof(buf));
  check(strcmp(buf, "180d00'00.0\"E") == 0, "DMS: 180.0 lon -> 180d00'00.0\"E", buf);

  formatDms(-180.0, false, buf, sizeof(buf));
  check(strcmp(buf, "180d00'00.0\"W") == 0, "DMS: -180.0 lon -> 180d00'00.0\"W", buf);

  // 59.96" 进位测试：绝不能出现 60.0"
  // 59.96 秒 = 59.96 / 3600 = 0.016655555... 度
  double carrySecVal = 10.0 + (30.0 / 60.0) + (59.96 / 3600.0);
  formatDms(carrySecVal, true, buf, sizeof(buf));
  check(strcmp(buf, "10d31'00.0\"N") == 0, "DMS: 59.96\" 秒进位到分，不能输出 60.0\"", buf);

  // 秒+分同时进位到度：10° 59' 59.96" -> 11° 00' 00.0"
  double carryMinSecVal = 10.0 + (59.0 / 60.0) + (59.96 / 3600.0);
  formatDms(carryMinSecVal, true, buf, sizeof(buf));
  check(strcmp(buf, "11d00'00.0\"N") == 0, "DMS: 59'59.96\" 连续进位到度，不能输出 60' 或 60.0\"", buf);

  // 负纬度连续进位
  formatDms(-carryMinSecVal, true, buf, sizeof(buf));
  check(strcmp(buf, "11d00'00.0\"S") == 0, "DMS: 负纬度连续进位为 11d00'00.0\"S", buf);
}

void testToMaidenhead() {
  printf("--- 1.2 toMaidenhead 6位网格换算测试 ---\n");
  char buf[64];

  // 1) 上海人民广场 (31.2304 N, 121.4737 E) -> 任务要求已知参考值 PM01rf
  toMaidenhead(31.2304, 121.4737, buf, sizeof(buf));
  check(strcmp(buf, "PM01rf") == 0, "Maidenhead: 上海 (31.2304N, 121.4737E) -> PM01rf", buf);

  // 2) 英国格林尼治皇家天文台本初子午线 (51.4769 N, -0.0005 W)
  // 参考来源：IARU / WSPRNet 网格库，格林尼治标准网格为 IO91xl
  toMaidenhead(51.4769, -0.0005, buf, sizeof(buf));
  check(strcmp(buf, "IO91xl") == 0, "Maidenhead: 格林尼治天文台 (51.4769N, -0.0005W) -> IO91xl", buf);

  // 3) 澳大利亚悉尼歌剧院 (-33.8568 S, 151.2153 E)
  // 参考来源：ARRL 业余无线电网格数据库，悉尼港为 QF56od
  toMaidenhead(-33.8568, 151.2153, buf, sizeof(buf));
  check(strcmp(buf, "QF56od") == 0, "Maidenhead: 悉尼歌剧院 (-33.8568S, 151.2153E) -> QF56od", buf);

  // 4) 美国纽约时代广场 (40.7580 N, -73.9855 W)
  // 参考来源：HamQTH / QRZ.com 网格库，时代广场为 FN30as
  toMaidenhead(40.7580, -73.9855, buf, sizeof(buf));
  check(strcmp(buf, "FN30as") == 0, "Maidenhead: 纽约时代广场 (40.7580N, -73.9855W) -> FN30as", buf);

  // 5) 原点 (0, 0) -> JJ00aa
  toMaidenhead(0.0, 0.0, buf, sizeof(buf));
  check(strcmp(buf, "JJ00aa") == 0, "Maidenhead: 原点 (0.0, 0.0) -> JJ00aa", buf);

  // 6) 边界极值：南极点与西极点 (-90, -180) -> AA00aa
  toMaidenhead(-90.0, -180.0, buf, sizeof(buf));
  check(strcmp(buf, "AA00aa") == 0, "Maidenhead: 极值 (-90.0, -180.0) -> AA00aa", buf);

  // 7) 边界极值：北极点与东极点 (+90, +180) -> RR99xx
  toMaidenhead(90.0, 180.0, buf, sizeof(buf));
  check(strcmp(buf, "RR99xx") == 0, "Maidenhead: 极值 (90.0, 180.0) -> RR99xx", buf);

  // 8) 越界坐标输入保护：不溢出崩溃
  toMaidenhead(95.0, 190.0, buf, sizeof(buf));
  check(strcmp(buf, "RR99xx") == 0, "Maidenhead: 越界输入 (+95, +190) 截断为 RR99xx", buf);

  toMaidenhead(-95.0, -190.0, buf, sizeof(buf));
  check(strcmp(buf, "AA00aa") == 0, "Maidenhead: 越界输入 (-95, -190) 截断为 AA00aa", buf);
}

void testToUtm() {
  printf("--- 1.3 toUtm 投影换算测试 ---\n");
  char buf[64];

  // 1) 适用范围外纬度：UTM 标准只定义在 80°S 到 84°N 之间
  toUtm(84.1, 120.0, buf, sizeof(buf));
  check(strcmp(buf, "UTM Lat Out") == 0, "UTM: 纬度 84.1°N 超出上限 -> UTM Lat Out", buf);

  toUtm(-80.1, 120.0, buf, sizeof(buf));
  check(strcmp(buf, "UTM Lat Out") == 0, "UTM: 纬度 80.1°S 超出下限 -> UTM Lat Out", buf);

  // 2) 上海人民广场 (31.2304 N, 121.4737 E)
  // WGS-84 UTM Zone 51N, Easting: 354634m, Northing: 3456141m
  toUtm(31.2304, 121.4737, buf, sizeof(buf));
  int zone = 0; char hemi = 0; double east = 0, north = 0;
  sscanf(buf, "%02d%c %lf %lf", &zone, &hemi, &east, &north);
  check(zone == 51 && hemi == 'N', "UTM 上海: Zone 51N", buf);
  check(fabs(east - 354634.0) <= 1.0, "UTM 上海: 东距误差 <= 1 米", buf);
  check(fabs(north - 3456141.0) <= 1.0, "UTM 上海: 北距误差 <= 1 米", buf);

  // 3) 悉尼歌剧院 (-33.8568 S, 151.2153 E)
  // 南半球需加 10,000,000m 假北距。
  // WGS-84 UTM Zone 56S, Easting: 334901m, Northing: 6252289m
  toUtm(-33.8568, 151.2153, buf, sizeof(buf));
  sscanf(buf, "%02d%c %lf %lf", &zone, &hemi, &east, &north);
  check(zone == 56 && hemi == 'S', "UTM 悉尼: Zone 56S", buf);
  check(fabs(east - 334901.0) <= 1.0, "UTM 悉尼: 东距误差 <= 1 米", buf);
  check(fabs(north - 6252289.0) <= 1.0, "UTM 悉尼: 南半球假北距误差 <= 1 米", buf);

  // 4) 格林尼治皇家天文台 (51.4769 N, -0.0005 W)
  // Zone 30N (中央经线 -3°), Easting: 708287m, Northing: 5707127m
  toUtm(51.4769, -0.0005, buf, sizeof(buf));
  sscanf(buf, "%02d%c %lf %lf", &zone, &hemi, &east, &north);
  check(zone == 30 && hemi == 'N', "UTM 格林尼治: Zone 30N", buf);
  check(fabs(east - 708287.0) <= 1.0, "UTM 格林尼治: 东距误差 <= 1 米", buf);
  check(fabs(north - 5707127.0) <= 1.0, "UTM 格林尼治: 北距误差 <= 1 米", buf);

  // 5) 带号边界测试：0度经线附近（西侧 Zone 30, 东侧 Zone 31）
  toUtm(30.0, -0.0001, buf, sizeof(buf));
  sscanf(buf, "%02d%c", &zone, &hemi);
  check(zone == 30, "UTM 带号边界: -0.0001° 在 Zone 30", buf);

  toUtm(30.0, 0.0001, buf, sizeof(buf));
  sscanf(buf, "%02d%c", &zone, &hemi);
  check(zone == 31, "UTM 带号边界: +0.0001° 在 Zone 31", buf);

  // 6) ±180 度边界
  toUtm(0.0, -180.0, buf, sizeof(buf));
  sscanf(buf, "%02d%c", &zone, &hemi);
  check(zone == 1, "UTM 经度 -180° -> Zone 01", buf);

  toUtm(0.0, 180.0, buf, sizeof(buf));
  sscanf(buf, "%02d%c", &zone, &hemi);
  check(zone == 60, "UTM 经度 180° -> Zone 60", buf);
}

// =================================================================
// 2. GNSS 行程统计与跳点过滤测试 (gnssTripProcessPoint)
// =================================================================
void testGnssTripFilter() {
  printf("--- 2. GNSS 行程统计与跳点过滤测试 ---\n");

  // 2.1 静止漂移场景：点在原地轻微漂移（速度 < 3.0 km/h）
  // 应当被过滤掉，移动时长保持为 0，累加里程保持为 0
  gnssTripReset();
  g_fakeNowMs = 1000;
  double baseLat = 31.2304, baseLng = 121.4737;

  for (int i = 0; i < 20; i++) {
    g_fakeNowMs += 1000;
    // 模拟 1~2 米微小漂移，上报速度 0.5 ~ 1.5 km/h
    double driftLat = baseLat + ((i % 3) - 1) * 0.00001;
    double driftLng = baseLng + ((i % 2) - 0.5) * 0.00001;
    float jitterSpd = 0.8f + (i % 5) * 0.2f; // 最高 1.6 km/h < 3.0 km/h
    gnssTripProcessPoint(g_fakeNowMs, true, 0, driftLat, driftLng, jitterSpd, true, 15.0f);
  }

  check(gnssTripGetMovingMs() == 0, "Trip 静止漂移: 移动时长应为 0 ms");
  check(gnssTripGetDistMeters() == 0.0, "Trip 静止漂移: 累计里程应为 0.0 m");

  // 2.2 匀速运动场景：36 km/h (10 m/s) 向北移动
  // 每秒 1 个点，持续 10 秒，理论行驶 90 米（从第 1 个点到第 10 个点共 9 段，每段 10m）
  gnssTripReset();
  g_fakeNowMs = 20000;
  double curLat = baseLat;
  // 纬度每度约 111139 米，10 米约 0.00008997 度
  const double DEG_PER_10M = 10.0 / 111139.0;

  for (int i = 0; i < 10; i++) {
    g_fakeNowMs += 1000;
    curLat += DEG_PER_10M;
    gnssTripProcessPoint(g_fakeNowMs, true, 0, curLat, baseLng, 36.0f, true, 20.0f + i);
  }

  uint32_t moveMs = gnssTripGetMovingMs();
  double dist = gnssTripGetDistMeters();
  float maxSpd = gnssTripGetMaxSpeed();

  check(moveMs >= 8900 && moveMs <= 9100, "Trip 匀速运动: 移动时长约 9000 ms");
  check(dist >= 85.0 && dist <= 95.0, "Trip 匀速运动: 累计里程约 90 米");
  check(fabs(maxSpd - 36.0f) < 0.1f, "Trip 匀速运动: 最高速度记录为 36 km/h");

  // 2.3 跳点过滤场景 (Glitch rejection)
  // 在 36 km/h (10 m/s) 运动中突然出现一个偏离 1000 米的野点
  // 1 秒内允许最大距离为 (36/3.6)*1.0*1.5 + 5 = 20 米，1000 米超出允许范围，必须被过滤
  double beforeDist = gnssTripGetDistMeters();
  g_fakeNowMs += 1000;
  double glitchLat = curLat + (1000.0 / 111139.0);
  gnssTripProcessPoint(g_fakeNowMs, true, 0, glitchLat, baseLng, 36.0f, true, 30.0f);

  check(fabs(gnssTripGetDistMeters() - beforeDist) < 0.1, "Trip 跳点过滤: 1000m 瞬时跳点被成功拦截，不计入里程");

  // 下一个点恢复正常航线（距跳点前的位置移动了 20 米，经过 2 秒，允许 10*2*1.5 + 5 = 35 米）
  g_fakeNowMs += 1000;
  curLat += DEG_PER_10M * 2;
  gnssTripProcessPoint(g_fakeNowMs, true, 0, curLat, baseLng, 36.0f, true, 31.0f);

  check(gnssTripGetDistMeters() > beforeDist + 15.0, "Trip 跳点过滤: 恢复正常航线后继续正确统计里程");

  // 2.4 丢星后重新捕获耗时 (Reacquisition)
  g_fakeNowMs += 1000;
  // 连续 4 秒丢失定位 (fix = false)
  gnssTripProcessPoint(g_fakeNowMs, false, 0, curLat, baseLng, 0.0f, false, 0.0f);
  g_fakeNowMs += 4000;
  // 重新获得定位
  gnssTripProcessPoint(g_fakeNowMs, true, 0, curLat, baseLng, 10.0f, true, 31.0f);
  float reacq = gnssTripGetReacqSec();
  check(fabs(reacq - 4.0f) < 0.1f, "Trip 重新定位耗时: 4 秒失锁后恢复，reacq 记录为 4.0 秒");

  // 2.5 首次定位耗时 (TTFF)
  gnssTripReset();
  gnssTripSetFirstByteMs(1000);
  g_fakeNowMs = 4500;
  gnssTripProcessPoint(g_fakeNowMs, true, 0, baseLat, baseLng, 10.0f, true, 30.0f);
  float ttff = gnssTripGetTtffSec();
  check(fabs(ttff - 3.5f) < 0.1f, "Trip 首次定位耗时: 1000ms到4500ms，TTFF 记录为 3.5 秒");
}

// =================================================================
// 3. GSA NMEA 语句解析测试 (gnssProcessGSA)
// =================================================================
void testGnssGSA() {
  printf("--- 3. GSA NMEA 语句解析测试 ---\n");

  g_fakeNowMs = 10000;

  // 3.1 标准 GPGSA 语句（GPS 单星座 3D 定位）
  char gsaGps[] = "$GPGSA,A,3,04,05,09,12,,,,,,,,,2.5,1.3,2.1*39";
  gnssProcessGSA(gsaGps);

  check(gnssUsedSatCount() == 4, "GSA: GPGSA 解析出 4 颗在用卫星");
  check(gnssIsSatUsed('G', 4) && gnssIsSatUsed('G', 5) &&
        gnssIsSatUsed('G', 9) && gnssIsSatUsed('G', 12),
        "GSA: PRN 4, 5, 9, 12 在用状态为 true");
  check(!gnssIsSatUsed('G', 1), "GSA: 未上报的 PRN 1 为 false");
  check(fabs(gnssGetPdop() - 2.5f) < 0.01f, "GSA: PDOP 为 2.5");
  check(fabs(gnssGetHdop() - 1.3f) < 0.01f, "GSA: HDOP 为 1.3");
  check(fabs(gnssGetVdop() - 2.1f) < 0.01f, "GSA: VDOP 为 2.1");

  // 3.2 北斗 BDGSA 语句（北斗单星座）
  char gsaBds[] = "$BDGSA,A,3,01,02,03,,,,,,,,,,2.0,1.1,1.7*28";
  gnssProcessGSA(gsaBds);

  check(gnssUsedSatCount() == 7, "GSA: GPS(4) + 北斗(3) 累计共 7 颗在用卫星");
  check(gnssIsSatUsed('C', 1) && gnssIsSatUsed('C', 2) && gnssIsSatUsed('C', 3),
        "GSA: 北斗 PRN 1, 2, 3 为 true");

  // 3.3 带 NMEA 4.1 systemId 扩展字段的 GNGSA
  // systemId 1 = GPS, systemId 4 = BDS
  // GSA 共有 12 个 PRN 槽位 (字段 3..14)，字段 15=PDOP, 16=HDOP, 17=VDOP, 18=systemId
  char gngsaGps[] = "$GNGSA,A,3,10,12,24,25,32,,,,,,,,1.5,0.9,1.2,1*1E";
  char gngsaBds[] = "$GNGSA,A,3,01,02,03,04,,,,,,,,,1.5,0.9,1.2,4*1B";
  gnssProcessGSA(gngsaGps);
  gnssProcessGSA(gngsaBds);

  check(gnssIsSatUsed('G', 10) && gnssIsSatUsed('G', 32), "GSA 4.1: systemId 1 正确映射到 GPS 槽位");
  check(gnssIsSatUsed('C', 4), "GSA 4.1: systemId 4 正确映射到北斗槽位");

  // 3.4 无定位状态 (fixType = 1)
  char gsaNoFix[] = "$GPGSA,A,1,,,,,,,,,,,,,99.9,99.9,99.9*12";
  gnssProcessGSA(gsaNoFix);

  check(gnssUsedSatCount() == 0, "GSA: 无定位(fix=1)时在用卫星数返回 0");
  check(!gnssIsSatUsed('G', 10), "GSA: 无定位(fix=1)时 gnssIsSatUsed 返回 false");

  // 3.5 健壮性：空指针、不带 $、不带校验和、畸变空串
  gnssProcessGSA(nullptr);
  char malformed1[] = "GPGSA,A,3,01,,,,,,,,,,,,1.0,1.0,1.0";
  gnssProcessGSA(malformed1);
  check(gnssIsSatUsed('G', 1), "GSA 健壮性: 不带 $ 和校验和的正常语句可正常解析");

  char malformed2[] = ",,,,";
  gnssProcessGSA(malformed2);
  check(true, "GSA 健壮性: 极端畸变空字段输入不崩溃");
}

// =================================================================
// 4. GNSS 串口流测试 (PC 主导模式)
// =================================================================
void testGnssStream() {
  printf("--- 4. GNSS 串口流测试 (PC 主导模式) ---\n");

  gnssIoOk = true;
  // 喂入无定位 GSA 语句，清空之前测试残留的在用卫星及 fixType 状态
  feedNmea("GPGSA,A,1,,,,,,,,,,,,,99.9,99.9,99.9");
  Serial.clear();

  // 4.1 GNSS ON 指令与 hz 参数夹值测试
  gnssStreamSet(true, 10); // 超出 5，应夹到 5
  auto lines = Serial.getLines();
  check(lines.size() == 1, "Stream 启动: 产生 1 行响应");
  check(lines[0] == "GNSS {\"t\":\"start\",\"hz\":5}", "Stream 启动: hz 自动夹到 1..5 范围 (hz=5)", lines[0].c_str());

  Serial.clear();
  gnssStreamSet(true, 0); // 低于 1，应夹到 1
  lines = Serial.getLines();
  check(lines.size() == 1 && lines[0] == "GNSS {\"t\":\"start\",\"hz\":1}", "Stream 启动: hz 自动夹到 1..5 范围 (hz=1)");

  // 4.2 无定位状态（刚开机 / 室内未搜到星）
  Serial.clear();
  g_fakeNowMs += 1000;
  gnssStreamTick();
  lines = Serial.getLines();
  check(lines.size() == 2, "Stream 无定位: 周期 0 输出 fix 与 sats (降频前第一帧包含sats)");

  // 验证第 1 行 fix JSON
  JsonDocument docFix;
  DeserializationError errFix = deserializeJson(docFix, lines[0].substr(5)); // 跳过 "GNSS "
  check(errFix == DeserializationError::Ok, "Stream 无定位: fix 为合法 JSON");
  check(docFix["t"] == "fix", "Stream 无定位: t 为 fix");
  check(docFix["valid"] == false, "Stream 无定位: valid 为 false");
  check(docFix["lat"].isNull() && docFix["lon"].isNull(), "Stream 无定位: lat/lon 为 null");
  check(docFix["alt"].isNull(), "Stream 无定位: alt 为 null");
  check(docFix["spd"].isNull() && docFix["crs"].isNull(), "Stream 无定位: spd/crs 为 null");
  check(docFix["spd_ok"] == false, "Stream 无定位: spd_ok 为 false");
  check(docFix["fix"] == 1, "Stream 无定位: fixType 为 1 (无定位)");
  check(docFix["q"].isNull(), "Stream 无定位: q 为 null");
  check(docFix["used"] == 0, "Stream 无定位: used 为 0");
  check(docFix["vis"] == 0, "Stream 无定位: vis 为 0");
  check(docFix["hdop"].isNull() && docFix["pdop"].isNull() && docFix["vdop"].isNull(), "Stream 无定位: DOP 为 null");
  check(docFix["geoid"].isNull(), "Stream 无定位: geoid 为 null");
  check(docFix["age"].isNull(), "Stream 无定位: age 为 null");
  check(docFix["utc"].isNull(), "Stream 无定位: utc 为 null");

  // 验证第 2 行 sats JSON
  JsonDocument docSats;
  DeserializationError errSats = deserializeJson(docSats, lines[1].substr(5));
  check(errSats == DeserializationError::Ok, "Stream 无定位: sats 为合法 JSON");
  check(docSats["t"] == "sats", "Stream 无定位: t 为 sats");
  check(docSats["list"].is<JsonArray>(), "Stream 无定位: list 为数组");
  check(docSats["list"].size() == 0, "Stream 无定位: list 为空数组");

  // 验证降频：周期 1 只有 fix，没有 sats
  Serial.clear();
  g_fakeNowMs += 1000;
  gnssStreamTick();
  lines = Serial.getLines();
  check(lines.size() == 1, "Stream 降频: 周期 1 仅输出 1 行 fix，不输出 sats");

  // 验证降频：周期 2 又有 fix 和 sats
  Serial.clear();
  g_fakeNowMs += 1000;
  gnssStreamTick();
  lines = Serial.getLines();
  check(lines.size() == 2, "Stream 降频: 周期 2 再次输出 fix 与 sats");

  // 4.3 喂入正常定位报文 (GPS + 北斗 3D 定位，航向速度均正常)
  // 经度 118°29.99424'E = 118.499904, 纬度 36°41.39196'N = 36.689866, 速度 1.2 节 ≈ 2.2 km/h, 航向 142.0°
  feedNmea("$GNRMC,123456.00,A,3641.39196,N,11829.99424,E,001.2,142.0,250926,,,A*78\r\n");
  feedNmea("$GNGGA,123456.00,3641.39196,N,11829.99424,E,1,08,1.2,107.0,M,-2.3,M,,*50\r\n");
  feedNmea("$GNGSA,A,3,04,05,09,12,,,,,,,,,2.4,1.2,1.3,1*1A\r\n");
  feedNmea("$GNGSA,A,3,01,02,03,04,,,,,,,,,2.4,1.2,1.3,4*1E\r\n");
  feedNmea("$GPGSV,1,1,04,04,45,120,35,05,30,060,30,09,37,070,37,12,60,240,40*7E\r\n");
  feedNmea("$BDGSV,1,1,04,01,50,110,38,02,40,150,36,03,30,200,34,04,60,250,42*6C\r\n");

  Serial.clear();
  g_fakeNowMs += 1000;
  gnssStreamTick();
  lines = Serial.getLines();
  check(lines.size() >= 1, "Stream 有定位: 输出流报文");

  JsonDocument docFix2;
  deserializeJson(docFix2, lines[0].substr(5));
  check(docFix2["valid"] == true, "Stream 有定位: valid 为 true");
  check(fabs(docFix2["lat"].as<double>() - 36.689866) < 0.0001, "Stream 有定位: lat 精度符合 36.689866");
  check(fabs(docFix2["lon"].as<double>() - 118.499904) < 0.0001, "Stream 有定位: lon 精度符合 118.499904");
  check(fabs(docFix2["alt"].as<double>() - 107.0) < 0.1, "Stream 有定位: alt 为 107.0");
  check(fabs(docFix2["spd"].as<double>() - 2.2) < 0.2, "Stream 有定位: spd 正确换算为 km/h");
  check(fabs(docFix2["crs"].as<double>() - 142.0) < 0.1, "Stream 有定位: crs 为 142.0");
  check(docFix2["spd_ok"] == true, "Stream 有定位: 定位新鲜且 HDOP<=8 时 spd_ok 为 true");
  check(docFix2["fix"] == 3, "Stream 有定位: fix 为 3 (3D 定位)");
  check(docFix2["q"] == 1, "Stream 有定位: q 为 1 (GPS)");
  check(docFix2["used"] == 8, "Stream 有定位: used 统计出 8 颗在用卫星");
  check(docFix2["vis"] == 8, "Stream 有定位: vis 统计出 8 颗可见卫星");
  check(fabs(docFix2["hdop"].as<double>() - 1.2) < 0.1, "Stream 有定位: hdop 为 1.2");
  check(fabs(docFix2["pdop"].as<double>() - 2.4) < 0.1, "Stream 有定位: pdop 为 2.4");
  check(fabs(docFix2["vdop"].as<double>() - 1.3) < 0.1, "Stream 有定位: vdop 为 1.3");
  check(fabs(docFix2["geoid"].as<double>() - (-2.3)) < 0.1, "Stream 有定位: geoid 为 -2.3");
  check(!docFix2["age"].isNull(), "Stream 有定位: age 为非空数字");
  check(docFix2["utc"] == "2026-09-25T12:34:56Z", "Stream 有定位: utc 格式化为 ISO 8601 2026-09-25T12:34:56Z");

  // 4.4 检查 sats 卫星列表字段
  if (lines.size() < 2) {
    Serial.clear();
    g_fakeNowMs += 1000;
    gnssStreamTick();
    lines = Serial.getLines();
  }
  check(lines.size() >= 2, "Stream 有定位: sats 报文成功生成");
  JsonDocument docSats2;
  deserializeJson(docSats2, lines[1].substr(5));
  JsonArray satArr = docSats2["list"].as<JsonArray>();
  check(satArr.size() == 8, "Stream 有定位: sats 列表包含 8 颗卫星");

  bool foundG9 = false;
  bool foundC1 = false;
  for (JsonObject s : satArr) {
    if (s["sys"] == "G" && s["prn"] == 9) {
      foundG9 = true;
      check(s["snr"] == 37 && s["el"] == 37 && s["az"] == 70 && s["used"] == true,
            "Stream 卫星项: GPS PRN 9 (snr=37, el=37, az=70, used=true)");
    }
    if (s["sys"] == "C" && s["prn"] == 1) {
      foundC1 = true;
      check(s["snr"] == 38 && s["el"] == 50 && s["az"] == 110 && s["used"] == true,
            "Stream 卫星项: 北斗 PRN 1 (snr=38, el=50, az=110, used=true)");
    }
  }
  check(foundG9 && foundC1, "Stream 卫星项: 成功索引到 GPS PRN 9 与北斗 PRN 1");

  // 4.5 定位失锁 / 数据陈旧场景 (进隧道)
  // 时间向前推进 4000ms，定位不再新鲜
  g_fakeNowMs += 4000;
  Serial.clear();
  gnssStreamTick();
  lines = Serial.getLines();
  check(lines.size() >= 1, "Stream 失锁: 输出流报文");
  JsonDocument docStale;
  deserializeJson(docStale, lines[0].substr(5));
  check(docStale["valid"] == false, "Stream 失锁: 定位超过 3 秒未更新，valid 翻转为 false");
  check(fabs(docStale["lat"].as<double>() - 36.689866) < 0.0001, "Stream 失锁: valid=false 时仍保留最后一次坐标");
  check(fabs(docStale["lon"].as<double>() - 118.499904) < 0.0001, "Stream 失锁: valid=false 时仍保留最后一次坐标");
  check(docStale["spd_ok"] == false, "Stream 失锁: spd_ok 变为 false，通知 PC 端不可信");
  check(docStale["utc"].isNull(), "Stream 失锁: 时间超过 3 秒未更新，utc 变为 null");
  check(docStale["age"].as<unsigned long>() >= 4000, "Stream 失锁: age 正确反映数据陈旧毫秒数");

  // 4.6 GNSS 硬件未接入或断开场景
  gnssIoOk = false;
  Serial.clear();
  g_fakeNowMs += 1000;
  gnssStreamTick();
  lines = Serial.getLines();
  check(lines.size() >= 1, "Stream 模块未接入: 正常输出不崩溃");
  JsonDocument docNoHw;
  deserializeJson(docNoHw, lines[0].substr(5));
  check(docNoHw["fix"] == 1, "Stream 模块未接入: fixType 为 1");
  check(docNoHw["valid"] == false, "Stream 模块未接入: valid 为 false");
  gnssIoOk = true;

  // 4.7 GNSS OFF 停止流指令
  Serial.clear();
  gnssStreamSet(false);
  lines = Serial.getLines();
  check(lines.size() == 1 && lines[0] == "GNSS {\"t\":\"end\"}", "Stream 停止: 打印 end 报文");

  // 再次 tick 不应输出任何内容
  Serial.clear();
  g_fakeNowMs += 2000;
  gnssStreamTick();
  lines = Serial.getLines();
  check(lines.empty(), "Stream 停止后: gnssStreamTick 不再输出任何内容");
}


// 真机抓到的一个历元（ATGM336H，2026-09-25，串口 NMEA 命令回显）。这组数据同时带着两个坑：
// 1) GSV 里常有空字段（"04,,,27"：仰角/方位角为空），老代码用 strtok 会整体错位；
// 2) NMEA 4.1 每条 GSV 末尾多一个信号编号字段（",1"），按字段数解析会多出半颗假星。
// 修复前的表现：GSA 说 18 颗在用，卫星列表里只有 12 颗对得上；可见数报 29~30（实际 28）。
static void testGnssRealCapture() {
  printf("--- testGnssRealCapture ---\n");
  const char* epoch[] = {
    "GNGSA,A,3,01,02,07,08,10,14,27,30,,,,,1.5,0.9,1.3,1",
    "GNGSA,A,3,76,,,,,,,,,,,,1.5,0.9,1.3,2",
    "GNGSA,A,3,13,21,26,,,,,,,,,,1.5,0.9,1.3,3",
    "GNGSA,A,3,01,07,26,35,,,,,,,,,1.5,0.9,1.3,4",
    "GNGSA,A,3,02,03,,,,,,,,,,,1.5,0.9,1.3,5",
    "GPGSV,3,1,11,01,41,166,26,02,63,138,26,07,66,264,23,08,63,032,31,1",
    "GPGSV,3,2,11,09,07,223,,10,05,060,35,14,16,298,28,16,15,098,,1",
    "GPGSV,3,3,11,27,26,053,37,30,36,307,15,43,42,147,32,1",
    "GLGSV,1,1,04,74,14,121,,75,59,086,23,76,47,336,26,85,47,036,31,1",
    "GAGSV,1,1,04,13,31,053,36,14,,,32,21,36,081,26,26,49,121,24,7",
    "BDGSV,2,1,07,01,42,147,32,04,,,27,07,61,162,21,10,73,165,,1",
    "BDGSV,2,2,07,24,69,319,,26,59,126,18,35,41,048,30,1",
    "GQGSV,1,1,02,02,68,077,33,03,44,133,26,1",
  };
  for (const char* l : epoch) feedNmea(l);

  gnssStreamSet(true, 1);
  Serial.clear();
  g_fakeNowMs += 1000;
  gnssStreamTick();
  auto lines = Serial.getLines();
  check(lines.size() == 2, "真机历元: 输出 fix + sats 两行");
  if (lines.size() < 2) { gnssStreamSet(false, 0); return; }

  JsonDocument fix, sats;
  check(!deserializeJson(fix, lines[0].substr(5)), "真机历元: fix 是合法 JSON");
  check(!deserializeJson(sats, lines[1].substr(5)), "真机历元: sats 是合法 JSON");
  JsonArray list = sats["list"].as<JsonArray>();

  check(fix["vis"] == 28, "真机历元: 可见 28 颗（末尾信号编号不再算成假星）");
  check(list.size() == 28, "真机历元: 卫星列表 28 项");
  check(fix["used"] == 18, "真机历元: GSA 在用 18 颗");

  int usedInList = 0;
  bool bogus = false;
  for (JsonObject o : list) {
    if (o["used"].as<bool>()) usedInList++;
    int prn = o["prn"];
    // 错位时会读出 162/165/121/81/59/41 这类卫星号
    if (prn == 162 || prn == 165 || prn == 121 || prn == 81 || prn == 59) bogus = true;
  }
  check(usedInList == 18, "真机历元: 列表里标为在用的正好 18 颗，跟 GSA 一致");
  check(!bogus, "真机历元: 没有字段错位读出的假卫星号");

  // 仰角/方位角为空的星按协议输出 null；信号为空的输出 0
  bool e14 = false, c04 = false, g09 = false;
  for (JsonObject o : list) {
    const char* sys = o["sys"];
    int prn = o["prn"];
    if (!strcmp(sys, "E") && prn == 14) e14 = o["el"].isNull() && o["az"].isNull() && o["snr"] == 32;
    if (!strcmp(sys, "C") && prn == 4)  c04 = o["el"].isNull() && o["az"].isNull() && o["snr"] == 27;
    if (!strcmp(sys, "G") && prn == 9)  g09 = o["el"] == 7 && o["az"] == 223 && o["snr"] == 0;
  }
  check(e14, "真机历元: Galileo 14 仰角/方位角为空 -> null，信号 32");
  check(c04, "真机历元: 北斗 04 仰角/方位角为空 -> null，信号 27");
  check(g09, "真机历元: GPS 09 信号为空 -> 0，仰角/方位角正常");

  gnssStreamSet(false, 0);
}

int main() {
  printf("==========================================\n");
  printf("  Cardputer ADV: GNSS Host Tests\n");
  printf("==========================================\n");

  testFormatDms();
  testToMaidenhead();
  testToUtm();
  testGnssTripFilter();
  testGnssGSA();
  testGnssStream();
  testGnssRealCapture();

  printf("==========================================\n");
  printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
  printf("==========================================\n");

  return g_fail > 0 ? 1 : 0;
}
