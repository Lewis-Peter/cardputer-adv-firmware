// astrotest：主机端跑 src/astro.cpp（太阳仰角 / 日出日落 / 日下点）和 src/moon.cpp（月相）的真代码。
//
// 参考值从哪来——这是这个测试台唯一要紧的事：
//   期望值一律出自本文件里**独立实现**的 Jean Meeus《Astronomical Algorithms》（第 2 版）算法，
//   不拿被测代码自己的输出当答案。被测代码用的是另一套公式（太阳：天文年历低精度式 + NOAA 分数年式；
//   月相：从一次朔按平均朔望月线性外推），两边算法不同，对得上才说明被测代码对。
//   参考实现本身先用书上的例题数值校一遍（第 1 节），例题数值抄自书中原文：
//     例 12.a / 12.b（格林尼治平恒星时）、例 25.a（太阳视位置）、例 49.a（1977 年 2 月的朔）。
//
// 用法：cd tools/astrotest && ./build.sh   （SAN=1 ./build.sh 带 ASan/UBSan）
#include "Arduino.h"
#include "M5Unified.h"
#include "Preferences.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <ctime>

// ---- 替身全局状态（与 clocktest 同样的写法）----
uint32_t g_fakeNowMs = 0;
SimM5    M5;
M5Canvas cv;
Preferences prefs;

// globals.h 需要的全局变量
bool     dirty      = false;
bool     screenOff  = false;
int      SW = 240, SH = 135;
uint16_t ACCENT = 0x07E0, CARD_BG = 0x18C3, DIM_BORDER = 0x2965, ICON_DIM = 0x4A69;
bool     timeSynced   = false;
bool     timeFromGps  = false;
bool     debugOn      = false;
bool     bootSoundOn  = false;
uint8_t  themeMode    = 0;
int      menuIndex    = 0;
float    menuSelX = 0, menuSelY = 0;
int      settingsIndex = 0;
int      lastSecond    = -1;
int      formatStep    = 0;
int      brightPct     = 80;
int      volPct        = 50;
const char* TZ_INFO    = "UTC0";
#include "bg_fetch.h"
// 真身在 bg_fetch.cpp（要 FreeRTOS）。测试台里同步跑完，等同于"起不来任务"时的退回路径。
bool bgFetchRun(BgFetchFn fn) { fn(); return false; }

// ---- 世界地图掩码（晨昏线页才需要，测试不调绘制路径，给个空数组）----
extern const uint8_t WORLD_MASK[3270] = {0};

// ---- UI/网络 函数打桩 ----
void drawPageDots()   {}
void drawPageDots(int, int) {}
void drawPageHeader(const char*, const char* = nullptr, uint16_t = 0) {}
String trunc(const String& s, int) { return s; }
bool   wifiConnected() { return false; }

// geoloc 打桩（drawAstroSun 用到，但测试不调绘图路径）
struct GeoFix { double lat=0, lon=0; String name=""; bool fromGps=false; bool valid=false; };
bool geoGet(GeoFix&, String* = nullptr) { return false; }
bool geoGetOffline(GeoFix&) { return false; }
void geoInvalidate() {}

// 节能/持久化相关打桩
void   sleepInit()  {}
void   sleepSet(int){}
void   tzInit()     {}
void   tzSet(int)   {}
void   brightInit() {}
void   brightSet(int){}
void   volInit()    {}
void   volSet(int)  {}
void   debugInit()  {}
void   debugSet(bool){}
void   bootSoundInit(){}
void   bootSoundSet(bool){}
void   themeSet(uint8_t){}
const char* themeName(uint8_t) { return ""; }
void   canvasRelease()  {}
void   canvasRestore()  {}
bool   canvasAvailable(){ return true; }
bool   radioIsActive()  { return false; }
bool   nowHM(int& h, int& m, int& s) { h=12; m=0; s=0; return true; }
int    powerBatteryLevel() { return 80; }
int    loadInt(const char*, const char*, int d) { return d; }
bool   loadBool(const char*, const char*, bool d) { return d; }
void   saveInt(const char*, const char*, int)  {}
void   saveBool(const char*, const char*, bool){}
uint32_t loadUInt(const char*, const char*, uint32_t d){ return d; }
void   saveUInt(const char*, const char*, uint32_t)  {}
double loadDouble(const char*, const char*, double d)  { return d; }
void   saveDouble(const char*, const char*, double)    {}
String loadString(const char*, const char*, const String& d){ return d; }
void   saveString(const char*, const char*, const String&)  {}
uint8_t loadUChar(const char*, const char*, uint8_t d) { return d; }
void    saveUChar(const char*, const char*, uint8_t)   {}
void    bootSelfTest() {}
void    cleanupApp(int) {}
void    menuSnapSelection() {}
void    menuNextGroup()    {}
bool    menuUpdateAnim()   { return false; }
void    menuMove(int, int) {}
void    drawTopBar() {}
void    drawMenu()   {}
void    drawClock()  {}
void    bootAnim()   {}
void    drawBootFrame(uint32_t) {}
void    termLogReset() {}
void    termLogLine(const char*, uint16_t = 0) {}
void    termLogDraw(bool) {}
int     menuGroupOf(int) { return 0; }

// ---- src/ 里 #ifdef HOST_TEST 的出口 ----
float  hostSolarElev(time_t t, double lat, double lon);
int    hostSunCross(time_t midnight, double lat, double lon, int dir);
void   hostSubsolarPointAt(time_t t, double& lat0, double& lon0Deg);
double hostMoonPhaseAt(time_t t);
const char* hostPhaseName(double p);

// ---- 断言 ----
static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", buf);
  ok ? g_pass++ : g_fail++;
}

static time_t mkutc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
  return timegm(&t);
}
static double jdOf(double unixSec) { return unixSec / 86400.0 + 2440587.5; }
static double unixOf(double jd) { return (jd - 2440587.5) * 86400.0; }
static double norm360(double a) { a = fmod(a, 360.0); return a < 0 ? a + 360.0 : a; }
static double wrap180(double a) { a = norm360(a); return a > 180.0 ? a - 360.0 : a; }
static const double D2R = M_PI / 180.0;

// =====================================================================================
// 参考实现（Meeus）
// =====================================================================================

// 格林尼治平恒星时（度），Meeus 式 12.4。jd 为 UT 儒略日。
static double refGmst(double jd) {
  double T = (jd - 2451545.0) / 36525.0;
  return norm360(280.46061837 + 360.98564736629 * (jd - 2451545.0)
                 + 0.000387933 * T * T - T * T * T / 38710000.0);
}

// 太阳视赤经 / 赤纬（度），Meeus 第 25 章"低精度"法——视黄经带章动与光行差修正，
// 黄赤交角用式 22.2 + 章动项。书上给的精度是 0.01°。这里拿 UT 当 TD 用：ΔT≈69 s
// 只让太阳挪 0.0008°，远小于要测的量。
static void refSunRaDec(double jd, double& raDeg, double& decDeg) {
  double T  = (jd - 2451545.0) / 36525.0;
  double L0 = 280.46646 + 36000.76983 * T + 0.0003032 * T * T;
  double M  = (357.52911 + 35999.05029 * T - 0.0001537 * T * T) * D2R;
  double C  = (1.914602 - 0.004817 * T - 0.000014 * T * T) * sin(M)
            + (0.019993 - 0.000101 * T) * sin(2 * M) + 0.000289 * sin(3 * M);
  double om = (125.04 - 1934.136 * T) * D2R;
  double lam = (L0 + C - 0.00569 - 0.00478 * sin(om)) * D2R;
  double eps0 = 23.0 + 26.0 / 60.0 + 21.448 / 3600.0
              - (46.8150 * T + 0.00059 * T * T - 0.001813 * T * T * T) / 3600.0;
  double eps = (eps0 + 0.00256 * cos(om)) * D2R;
  raDeg  = norm360(atan2(cos(eps) * sin(lam), cos(lam)) / D2R);
  decDeg = asin(sin(eps) * sin(lam)) / D2R;
}

// 几何高度角（不含大气折射——被测的 solarElev 也不含，折射由日出日落判据 -0.833° 吸收）。
static double refSolarElev(double unixSec, double lat, double lon) {
  double jd = jdOf(unixSec), ra, dec;
  refSunRaDec(jd, ra, dec);
  double H = (refGmst(jd) + lon - ra) * D2R, phi = lat * D2R;
  return asin(sin(phi) * sin(dec * D2R) + cos(phi) * cos(dec * D2R) * cos(H)) / D2R;
}

// 日出(dir=+1)/日落(dir=-1)：在 [midnight, midnight+86400] 里找参考高度角穿过 -0.833° 的**第一个**
// 对应方向的时刻——和被测 sunCross 的定义一致。按 30 s 细扫（比被测代码的 5 min 细得多，
// 能抓到它可能漏掉的短暂穿越），再二分到毫秒级。没有穿越返回 -1。
static double refSunCross(time_t midnight, double lat, double lon, int dir) {
  const double H0 = -0.833, STEP = 30.0;
  double prev = refSolarElev(midnight, lat, lon) - H0;
  for (double t = STEP; t <= 86400.0; t += STEP) {
    double cur = refSolarElev(midnight + t, lat, lon) - H0;
    bool up = prev < 0 && cur >= 0, down = prev >= 0 && cur < 0;
    if ((dir > 0 && up) || (dir < 0 && down)) {
      double lo = t - STEP, hi = t;
      for (int i = 0; i < 30; i++) {
        double mid = (lo + hi) / 2, e = refSolarElev(midnight + mid, lat, lon) - H0;
        if ((dir > 0) ? (e >= 0) : (e < 0)) hi = mid; else lo = mid;
      }
      return hi;
    }
    prev = cur;
  }
  return -1;
}

// 真朔 / 真望的时刻（儒略历书日 JDE），Meeus 第 49 章，含全部周期项和 14 个行星附加项。
// k 为整数 → 朔，k 为整数 + 0.5 → 望。书上给的精度是几秒到十几秒。
static double refLunationJde(double k) {
  const bool full = fmod(fabs(k), 1.0) > 0.25;
  double T = k / 1236.85, T2 = T * T, T3 = T2 * T, T4 = T3 * T;
  double jde = 2451550.09766 + 29.530588861 * k + 0.00015437 * T2 - 0.000000150 * T3 + 0.00000000073 * T4;
  double E  = 1 - 0.002516 * T - 0.0000074 * T2;
  double M  = (2.5534 + 29.10535670 * k - 0.0000014 * T2 - 0.00000011 * T3) * D2R;
  double Mp = (201.5643 + 385.81693528 * k + 0.0107582 * T2 + 0.00001238 * T3 - 0.000000058 * T4) * D2R;
  double F  = (160.7108 + 390.67050284 * k - 0.0016118 * T2 - 0.00000227 * T3 + 0.000000011 * T4) * D2R;
  double Om = (124.7746 - 1.56375588 * k + 0.0020672 * T2 + 0.00000215 * T3) * D2R;
  double c;
  if (!full) {
    c = -0.40720 * sin(Mp) + 0.17241 * E * sin(M) + 0.01608 * sin(2 * Mp) + 0.01039 * sin(2 * F)
      + 0.00739 * E * sin(Mp - M) - 0.00514 * E * sin(Mp + M) + 0.00208 * E * E * sin(2 * M);
  } else {
    c = -0.40614 * sin(Mp) + 0.17302 * E * sin(M) + 0.01614 * sin(2 * Mp) + 0.01043 * sin(2 * F)
      + 0.00734 * E * sin(Mp - M) - 0.00515 * E * sin(Mp + M) + 0.00209 * E * E * sin(2 * M);
  }
  c += -0.00111 * sin(Mp - 2 * F) - 0.00057 * sin(Mp + 2 * F) + 0.00056 * E * sin(2 * Mp + M)
     - 0.00042 * sin(3 * Mp) + 0.00042 * E * sin(M + 2 * F) + 0.00038 * E * sin(M - 2 * F)
     - 0.00024 * E * sin(2 * Mp - M) - 0.00017 * sin(Om) - 0.00007 * sin(Mp + 2 * M)
     + 0.00004 * sin(2 * Mp - 2 * F) + 0.00004 * sin(3 * M) + 0.00003 * sin(Mp + M - 2 * F)
     + 0.00003 * sin(2 * Mp + 2 * F) - 0.00003 * sin(Mp + M + 2 * F) + 0.00003 * sin(Mp - M + 2 * F)
     - 0.00002 * sin(Mp - M - 2 * F) - 0.00002 * sin(3 * Mp + M) + 0.00002 * sin(4 * Mp);
  static const double AC[14][3] = {   // 行星附加项：系数、A 的常数项、A 的 k 系数（A1 另有 T² 项）
    {0.000325, 299.77, 0.107408}, {0.000165, 251.88, 0.016321}, {0.000164, 251.83, 26.651886},
    {0.000126, 349.42, 36.412478}, {0.000110,  84.66, 18.206239}, {0.000062, 141.74, 53.303771},
    {0.000060, 207.14,  2.453732}, {0.000056, 154.84,  7.306860}, {0.000047,  34.52, 27.261239},
    {0.000042, 207.19,  0.121824}, {0.000040, 291.34,  1.844379}, {0.000037, 161.72, 24.198154},
    {0.000035, 239.56, 25.513099}, {0.000023, 331.55,  3.592518}};
  for (int i = 0; i < 14; i++) {
    double A = AC[i][1] + AC[i][2] * k - (i == 0 ? 0.009173 * T2 : 0.0);
    c += AC[i][0] * sin(A * D2R);
  }
  return jde + c;
}
static const double DELTA_T_2026 = 69.0;   // TT−UT（秒），IERS 公报量级；只影响秒级

// =====================================================================================
// 测试
// =====================================================================================

static void testReferenceAgainstBook() {
  printf("--- 1. 参考实现对照 Meeus 书中例题 ---\n");
  // 例 12.a：1987-04-10 0h UT，θ0 = 13h10m46.3668s = 197.693195°
  check(fabs(refGmst(2446895.5) - 197.693195) < 1e-5, "例12.a 平恒星时 %.6f°", refGmst(2446895.5));
  // 例 12.b：1987-04-10 19:21:00 UT，θ0 = 128.7378734°
  double jd12b = 2446895.5 + (19 + 21 / 60.0) / 24.0;
  check(fabs(refGmst(jd12b) - 128.7378734) < 1e-5, "例12.b 平恒星时 %.7f°", refGmst(jd12b));
  // 例 25.a：1992-10-13 0h TD（JDE 2448908.5），α = 198.38083°，δ = −7.78507°
  double ra, dec; refSunRaDec(2448908.5, ra, dec);
  check(fabs(ra - 198.38083) < 2e-5 && fabs(dec + 7.78507) < 2e-5,
        "例25.a 太阳视赤经 %.5f° 赤纬 %.5f°", ra, dec);
  // 例 49.a：1977 年 2 月的朔，k = −283，JDE = 2443192.65118
  double jde = refLunationJde(-283);
  check(fabs(jde - 2443192.65118) < 2e-5, "例49.a 朔 JDE %.5f（差 %.1f s）", jde, (jde - 2443192.65118) * 86400);
}

static void testSolarElev() {
  printf("--- 2. solarElev：跟参考星历逐点比 ---\n");
  // 被测代码注释声称"误差 ~0.01°"。在 2020–2035 年里伪随机撒 20000 个 (时刻, 纬度, 经度)，
  // 取最大绝对误差。门槛放在 0.02°：两边各有约 0.01° 的误差预算，加起来的上限。
  uint32_t seed = 12345;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
  time_t t0 = mkutc(2020, 1, 1), t1 = mkutc(2036, 1, 1);
  double worst = 0; time_t wt = 0; double wlat = 0, wlon = 0;
  for (int i = 0; i < 20000; i++) {
    time_t t = t0 + (time_t)(rnd() * (double)(t1 - t0));
    double lat = rnd() * 180.0 - 90.0, lon = rnd() * 360.0 - 180.0;
    double d = fabs(hostSolarElev(t, lat, lon) - refSolarElev(t, lat, lon));
    if (d > worst) { worst = d; wt = t; wlat = lat; wlon = lon; }
  }
  check(worst < 0.02, "2020–2035 两万个样本最大误差 %.4f°（t=%ld lat=%.1f lon=%.1f）",
        worst, (long)wt, wlat, wlon);
  // 几个人能直觉核对的点：春分正午赤道太阳几乎在头顶，夏至北京正午仰角≈90−39.9+23.44
  double e1 = hostSolarElev(mkutc(2026, 3, 20, 12, 7), 0.0, 0.0);
  check(e1 > 89.0, "2026 春分 12:07 UTC 赤道 0°E 仰角 %.2f°（应近 90°）", e1);
  time_t bjNoon = mkutc(2026, 6, 21, 4, 14);   // 北京真太阳时正午 ≈ 12:14 CST（经度 116.4°E、夏至时差 ≈ −1.7 min）
  double e2 = hostSolarElev(bjNoon, 39.9042, 116.4074), r2 = refSolarElev(bjNoon, 39.9042, 116.4074);
  check(fabs(e2 - r2) < 0.02 && fabs(e2 - (90 - 39.9042 + 23.44)) < 0.1,
        "2026 夏至北京正午仰角 %.3f°（参考 %.3f°，90−φ+ε≈%.3f°）", e2, r2, 90 - 39.9042 + 23.44);
}

struct Place { const char* name; double lat, lon; int tzHours; };

static void testSunCross() {
  printf("--- 3. sunCross：日出日落跟参考星历比 ---\n");
  // 容差 60 s：被测高度角误差 ≤0.02°，太阳在地平线附近每分钟升降 0.1°–0.25°（中低纬），
  // 折成时间是几秒到十几秒；高纬斜着出地平线会放大，60 s 留足余量又不至于掩盖真问题（比如漏掉一次穿越）。
  static const Place P[] = {
    {"北京",       39.9042, 116.4074,   8}, {"赤道 0°E",    0.0,      0.0,       0},
    {"悉尼",      -33.8688, 151.2093,  11}, {"雷克雅未克", 64.1466, -21.9426,   0},
    {"苏瓦",      -18.1416, 178.4419,  12}, {"火奴鲁鲁",   21.3069, -157.8583, -10},
    {"特罗姆瑟",   69.6492,  18.9553,   1},
  };
  static const int DAYS[][3] = {{2026, 3, 20}, {2026, 6, 21}, {2026, 9, 23}, {2026, 12, 21}};
  double worst = 0; int agreeExist = 0, total = 0;
  for (const Place& p : P) {
    for (auto& d : DAYS) {
      time_t midnight = mkutc(d[0], d[1], d[2]) - p.tzHours * 3600;   // 当地 0 点的 UTC epoch
      for (int dir = +1; dir >= -1; dir -= 2) {
        int got = hostSunCross(midnight, p.lat, p.lon, dir);
        double ref = refSunCross(midnight, p.lat, p.lon, dir);
        total++;
        bool bothNone = got < 0 && ref < 0, bothSome = got >= 0 && ref >= 0;
        if (bothNone || bothSome) agreeExist++;
        if (bothSome) {
          double diff = fabs(got - ref);
          if (diff > worst) worst = diff;
          if (diff >= 60)
            check(false, "%s %04d-%02d-%02d %s 被测 %d s 参考 %.0f s 差 %.0f s",
                  p.name, d[0], d[1], d[2], dir > 0 ? "日出" : "日落", got, ref, diff);
        } else if (!bothNone) {
          check(false, "%s %04d-%02d-%02d %s 有无不一致：被测 %d 参考 %.0f",
                p.name, d[0], d[1], d[2], dir > 0 ? "日出" : "日落", got, ref);
        }
      }
    }
  }
  check(agreeExist == total, "%d/%d 个 (地点, 日期, 日出/日落) 在'有没有穿越'上跟参考一致", agreeExist, total);
  check(worst < 60, "有穿越的样本里最大时间差 %.1f s", worst);

  // 极昼 / 极夜：特罗姆瑟（69.65°N）夏至太阳整天在地平线上，冬至整天在下面——两边都应该返回 -1，
  // 页面据此显示"极昼/极夜"而不是一个编出来的时刻。
  time_t tromsoSummer = mkutc(2026, 6, 21) - 3600, tromsoWinter = mkutc(2026, 12, 21) - 3600;
  check(hostSunCross(tromsoSummer, 69.6492, 18.9553, +1) == -1 && hostSunCross(tromsoSummer, 69.6492, 18.9553, -1) == -1,
        "特罗姆瑟夏至（极昼）日出日落都返回 -1");
  check(hostSunCross(tromsoWinter, 69.6492, 18.9553, +1) == -1 && hostSunCross(tromsoWinter, 69.6492, 18.9553, -1) == -1,
        "特罗姆瑟冬至（极夜）日出日落都返回 -1");
  check(refSolarElev(tromsoSummer, 69.6492, 18.9553) > -0.833 && refSolarElev(tromsoWinter + 43200, 69.6492, 18.9553) < -0.833,
        "参考星历也确认：夏至午夜太阳在判据线上、冬至正午在判据线下");
}

static void testSubsolar() {
  printf("--- 4. subsolarPointAt：日下点跟参考星历比 ---\n");
  // 日下点纬度 = 太阳赤纬，经度 = 太阳赤经 − 格林尼治恒星时（时角为 0 的经度）。
  // 被测的是 NOAA 分数年近似式（Spencer 1971 的赤纬级数），只拿来画晨昏线。实测它的纬度误差随
  // 4 年闰年周期起伏，峰值落在 9 月分点前后，2024–2031 最大约 0.55°（分数年式把分点日期算偏了
  // 一天多）。世界地图 240 px 宽，1 px ≈ 1.5°，所以门槛取半个像素 0.75°——超过才看得出来。
  // 区间要盖满一个完整闰年周期，只挑某几年会刚好躲过峰值。
  time_t t0 = mkutc(2024, 1, 1), t1 = mkutc(2032, 1, 1);
  double wLat = 0, wLon = 0;
  for (time_t t = t0; t < t1; t += 3 * 3600 + 17 * 60) {
    double lat0, lon0; hostSubsolarPointAt(t, lat0, lon0);
    double jd = jdOf(t), ra, dec; refSunRaDec(jd, ra, dec);
    double refLon = wrap180(ra - refGmst(jd));
    wLat = fmax(wLat, fabs(lat0 / D2R - dec));
    wLon = fmax(wLon, fabs(wrap180(lon0 - refLon)));
  }
  check(wLat < 0.75, "2024–2031 日下点纬度最大误差 %.3f°", wLat);
  check(wLon < 0.75, "2024–2031 日下点经度最大误差 %.3f°", wLon);

  // 时区无关：内部用 gmtime_r，换 TZ 结果必须逐位相同（用 localtime 就会错整整几个时区）
  time_t t = mkutc(2026, 7, 4, 3, 30);
  double a1, b1, a2, b2;
  setenv("TZ", "UTC0", 1); tzset(); hostSubsolarPointAt(t, a1, b1);
  setenv("TZ", "CST-8", 1); tzset(); hostSubsolarPointAt(t, a2, b2);
  setenv("TZ", "UTC0", 1); tzset();
  check(a1 == a2 && b1 == b2, "TZ=UTC 与 TZ=CST-8 结果逐位相同");
}

static void testMoon() {
  printf("--- 5. 月相：跟 Meeus 第 49 章真朔 / 真望比 ---\n");
  // 被测代码从 2000-01-06 18:14 UTC 那次朔按平均朔望月线性外推。真实朔望跟"平均"朔望之间
  // 本来就有最多十几小时的起伏（月球轨道偏心），所以这里要回答的问题是：它在 2026 年实际差多少，
  // 跟 moon.h 注释里写的"误差几小时级别"是不是一回事。
  const double SYN = 29.530588853;
  // 先确认外推起点本身：2000 年 1 月那次朔（k = 0）
  double k0utc = unixOf(refLunationJde(0)) - 63.8;   // 2000 年 ΔT ≈ 63.8 s
  check(fabs(k0utc - 947182440.0) < 120, "外推起点 2000-01-06 18:14 UTC 跟真朔差 %.0f s", k0utc - 947182440.0);

  double worstH = 0; int nEvents = 0; int dayWrongCst = 0;
  double k = floor((2026 - 2000) * 12.3685) - 1;
  for (double kk = k; kk < k + 16; kk += 0.5) {
    double tUtc = unixOf(refLunationJde(kk)) - DELTA_T_2026;
    if (tUtc < mkutc(2026, 1, 1) || tUtc >= mkutc(2027, 1, 1)) continue;
    bool full = fmod(kk, 1.0) != 0.0;
    double p = hostMoonPhaseAt((time_t)llround(tUtc));
    // 到最近那个"目标相位"的圆周距离：朔时 p 可能是 0.99x（被测代码的平朔还没到）也可能是 0.00x，
    // 两者都叫 NEW MOON（phaseName 在 ≥0.9375 时也返回 NEW MOON），所以按圆周距离算，不按 |p−0| 算。
    double target = full ? 0.5 : 0.0, dp = fabs(p - target);
    dp = fmin(dp, 1.0 - dp);
    double errH = dp * SYN * 24.0;
    worstH = fmax(worstH, errH);
    nEvents++;
    // 页面上 "NEXT FULL/NEW" 显示的是日期。按北京时间看，被测代码预测的日期和真实日期差不差一天：
    double off = (full ? 0.5 : 0.0) - p; off -= floor(off + 0.5);   // 被测代码认为的"下一个目标相位"距此刻多少周期（有符号）
    time_t predicted = (time_t)llround(tUtc + off * SYN * 86400.0);
    time_t trueT = (time_t)llround(tUtc);
    if ((predicted + 8 * 3600) / 86400 != (trueT + 8 * 3600) / 86400) dayWrongCst++;
    check(hostPhaseName(p) == std::string(full ? "FULL MOON" : "NEW MOON") ,
          "%s %s UTC：被测相位 %.4f → %s（差 %.1f h）", full ? "望" : "朔",
          [&]{ static char b[32]; time_t tt = trueT; struct tm u; gmtime_r(&tt, &u);
               strftime(b, sizeof(b), "%m-%d %H:%M", &u); return b; }(),
          p, hostPhaseName(p), errH);
  }
  printf("  2026 年 %d 次朔望里最大偏差 %.1f h；按北京时间有 %d 次 NEXT FULL/NEW 的日期差一天\n",
         nEvents, worstH, dayWrongCst);
  check(nEvents >= 24, "2026 年覆盖到 %d 次朔望", nEvents);
  // moon.h 的注释说"误差几小时级别"，页面上 NEXT FULL / NEXT NEW 显示的是日期。按这两条说法各断言一次：
  //   · 几小时：取 < 10 h；
  //   · 日期：一次都不该错。
  // 2026 年实测两条都不满足（最大约 17 h，北京时间 9 次日期错一天）。原因有两层：
  //   1) 线性外推丢掉了真朔相对平朔的周期起伏（主项 0.4072 d·sin M'，合计可达 ±14 h）；
  //   2) 外推起点取的是一次真朔（2000-01-06 18:14），比那次平朔晚约 3.9 h，整条线整体偏了。
  // 只改起点能降到约 13.5 h，日期错误次数不变；要真正对上得在 moon.cpp 里加周期项（Meeus 第 49 章）。
  // 这两条 FAIL 是要修的问题，不是容差没调好——别为了变绿去放宽它们。
  check(worstH < 10.0, "最大偏差 %.1f h 符合注释'几小时级别'（< 10 h）", worstH);
  check(dayWrongCst == 0, "北京时间 NEXT FULL/NEW 日期全年无一出错（实际错 %d 次）", dayWrongCst);
}

static void testPhaseName() {
  printf("--- 6. phaseName 分档边界 ---\n");
  // 八档，每档 1/8 周期，以 0、1/8、2/8… 为中心：档界在 1/16 的奇数倍。期望值来自这个定义本身。
  static const char* N[8] = {"NEW MOON", "WAXING CRESCENT", "FIRST QUARTER", "WAXING GIBBOUS",
                             "FULL MOON", "WANING GIBBOUS", "LAST QUARTER", "WANING CRESCENT"};
  int ok = 0, n = 0;
  for (int i = 0; i < 8; i++) {
    double lo = (2 * i - 1) / 16.0, hi = (2 * i + 1) / 16.0;
    double samples[3] = {fmod(lo + 1e-9 + 1.0, 1.0), i / 8.0, hi - 1e-9};
    for (double p : samples) { n++; if (std::string(hostPhaseName(p)) == N[i]) ok++; else printf("    p=%.6f → %s（应为 %s）\n", p, hostPhaseName(p), N[i]); }
  }
  check(ok == n, "%d/%d 个档内采样点落在正确档位（含 0.9375 以上回卷到 NEW MOON）", ok, n);
}

int main() {
  setenv("TZ", "UTC0", 1); tzset();
  testReferenceAgainstBook();
  testSolarElev();
  testSunCross();
  testSubsolar();
  testMoon();
  testPhaseName();
  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
