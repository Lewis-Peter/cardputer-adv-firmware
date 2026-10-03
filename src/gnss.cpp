#include "gnss.h"
#include <TinyGPS++.h>
#include <ctime>
#include <sys/time.h>
#include <cstring>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <cmath>
#include <esp_heap_caps.h>
#include "ui_common.h"
#include "wifi_net.h"
#include "geoloc.h"
#include "net_job.h"
#include <SD.h>
#include "sd_files.h"
#include "worldmap.h"
#include "http_json.h"

static const int GPS_TX_PIN = 13;   // ESP32 TX -> 模块 GPS-RX
static const int GPS_RX_PIN = 15;   // ESP32 RX <- 模块 GPS-TX
static const uint8_t IOEXP_ADDR = 0x43;
static HardwareSerial GPSSerial(1);
static TinyGPSPlus gps;
// 按 NMEA 语句的"话务员前缀"区分卫星属于哪个星座（GSV 语句第 3 个字段=该星座可见卫星数）。
// ATGM336H 支持 GPS/北斗/GLONASS/Galileo/QZSS，北斗前缀新旧标准不一样，GB/BD 都接一份兜底。
static TinyGPSCustom gsvGPS(gps, "GPGSV", 3);
static TinyGPSCustom gsvGLONASS(gps, "GLGSV", 3);
static TinyGPSCustom gsvGalileo(gps, "GAGSV", 3);
static TinyGPSCustom gsvBeidouGB(gps, "GBGSV", 3);
static TinyGPSCustom gsvBeidouBD(gps, "BDGSV", 3);
static TinyGPSCustom gsvQZSS(gps, "GQGSV", 3);
static int gsvCount(TinyGPSCustom& c) { return c.isValid() ? atoi(c.value()) : 0; }

// 从 GGA 语句读取定位质量 (field 6) 和大地水准面差距 (field 11)，兼容 GNGGA 与 GPGGA
static TinyGPSCustom ggaQualityGN(gps, "GNGGA", 6);
static TinyGPSCustom ggaQualityGP(gps, "GPGGA", 6);
static TinyGPSCustom ggaGeoidGN(gps, "GNGGA", 11);
static TinyGPSCustom ggaGeoidGP(gps, "GPGGA", 11);

static int gnssFixQuality() {
  if (ggaQualityGN.isValid() && ggaQualityGN.age() < 2500) return atoi(ggaQualityGN.value());
  if (ggaQualityGP.isValid() && ggaQualityGP.age() < 2500) return atoi(ggaQualityGP.value());
  return 0;
}
static const char* gnssQualityName(int q) {
  switch (q) {
    case 1: return "GPS";
    case 2: return "DGPS";
    case 3: return "PPS";
    case 4: return "RTK";
    case 5: return "Float";
    case 6: return "DR";
    default: return "None";
  }
}
static bool gnssGeoidSeparation(double& sep) {
  // 没定位时 GGA 照发但这个字段是空的，atof("") 会得到 0
  if (ggaGeoidGN.isValid() && ggaGeoidGN.age() < 2500 && ggaGeoidGN.value()[0]) { sep = atof(ggaGeoidGN.value()); return true; }
  if (ggaGeoidGP.isValid() && ggaGeoidGP.age() < 2500 && ggaGeoidGP.value()[0]) { sep = atof(ggaGeoidGP.value()); return true; }
  return false;
}
// 两件事必须分开记：IO 扩展芯片能不能通（硬件在不在），和天线现在开着没有（用户可切）。
// 混成一个标志的话，用户在配置页关掉天线，其它页就会误报"init failed"，看着像硬件坏了。
bool gnssIoOk = false;       // I2C 通得上 = Cap 模块存在
bool gnssAntennaOn = false;  // 有源天线供电当前状态
static uint32_t gnssLastByteMs = 0;

static bool gnssSetAntenna(bool on) {
  bool ok1 = M5.In_I2C.bitOn(IOEXP_ADDR, 0x03, 0x01, 100000);   // P0 方向=输出
  bool ok2 = M5.In_I2C.bitOff(IOEXP_ADDR, 0x07, 0x01, 100000);  // P0 驱动使能
  bool ok3 = on ? M5.In_I2C.bitOn(IOEXP_ADDR, 0x05, 0x01, 100000)
                : M5.In_I2C.bitOff(IOEXP_ADDR, 0x05, 0x01, 100000);
  bool ok = ok1 && ok2 && ok3;
  gnssIoOk = ok;
  // 只有写成功才更新状态：关闭失败时天线其实还开着，记成 off 会让 UI 和硬件对不上。
  if (ok) gnssAntennaOn = on;
  return ok;
}

// ATGM336H 使用 PCAS 私有 NMEA 命令；校验和由这里统一计算，避免手写十六进制出错。
static void gnssSendPcas(const char* body) {
  uint8_t sum = 0;
  for (const char* p = body; *p; p++) sum ^= (uint8_t)*p;
  char line[96];
  snprintf(line, sizeof(line), "$%s*%02X\r\n", body, sum);
  GPSSerial.print(line);
}

// 统一发送高动态（PCAS11,6）、5Hz更新率（PCAS02,200）、精简语句集 nav+gsv（PCAS03）
static void gnssSendDefaultPcas() {
  gnssSendPcas("PCAS11,6");
  gnssSendPcas("PCAS02,200");
  gnssSendPcas("PCAS03,1,0,1,1,1,0,0,0,0,0,,,0,0");
}

// 开机 3.5s 延迟重发兜底 & 热插拔非阻塞下发配置的目标时间戳（0 表示无待发送任务）
static uint32_t gnssPendingConfigAt = 3500;

void gnssInit() {
  // Cap 模块的 IO 扩展芯片跟 Cardputer Adv 内部 I2C 是同一条总线（SCL=9 SDA=8，
  // M5.begin() 时已经初始化好），必须复用 M5.In_I2C，不能另开一个 TwoWire 抢同一条总线
  // ——那样会跟 M5Unified 自己对内部总线上其它芯片（电源管理等）的访问冲突。
  gnssSetAntenna(true);

  // ⚠️ 默认 RX 缓冲只有 256 字节，115200 波特下只装得下 22ms 的数据——而主循环一帧
  // （render + delay(20)）本来就要 30~50ms，拉地图瓦片时更是好几秒。缓冲一满后面的字节
  // 直接丢，NMEA 语句被拦腰截断，表现就是卫星时有时无、校验和错一堆。必须在 begin 之前放大。
  GPSSerial.setRxBufferSize(4096);
  GPSSerial.begin(115200, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  delay(50);
  // 配置动态导航模型 (PCAS11)：
  // 查证自中科微官方协议手册《AT3340 ProductManual》第 3.2.8 节 CAS11 (Dynamic Model):
  //   0: Portable (便携模式, 默认，动态滤波保守)
  //   1: Static (静止/授时模式, 限速 10m/s)
  //   2: Walking (步行模式, 限速 30m/s)
  //   3: Vehicle (车载模式, 最大速度 84m/s ≈ 302.4km/h, 无法满足高铁 350km/h 场景且垂直过载受限)
  //   4: Voyage (航海模式, 最大速度 25m/s)
  //   5: Aviation <1g (航空低过载模式, 最大速度 100m/s ≈ 360km/h)
  //   6: Aviation <2g (航空中过载模式, 最大速度 250m/s ≈ 900km/h, 最大垂直速度 100m/s, 通用高动态)
  //   7: Aviation <4g (航空高过载模式, 最大速度 500m/s ≈ 1800km/h)
  // 设置为 6 (Aviation <2g)，完美覆盖高铁 350km/h、无人机与民航巡航，彻底根治高速移动下卡死失步。
  gnssSendPcas("PCAS11,6");
  delay(20);
  // 默认启动 5Hz 高更新率 (200ms)；避免出厂默认 1Hz 在高速移动下位置延迟严重 (300km/h 下 1Hz 间隔达 83 米)。
  gnssSendPcas("PCAS02,200");
  delay(20);
  // 收窄 NMEA 输出集为 nav+gsv (仅保留 GGA, GSA, GSV, RMC，关闭 GLL, VTG, ZDA, ANT)，
  // 避免 5Hz 双星座全量输出打爆 115200 波特率及串口缓冲。
  gnssSendPcas("PCAS03,1,0,1,1,1,0,0,0,0,0,,,0,0");
}

bool gnssCapConnected() {
  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  if (now - lastCheck >= 1500 || lastCheck == 0) {
    lastCheck = now;
    bool ok = M5.In_I2C.scanID(IOEXP_ADDR);
    if (ok != gnssIoOk) {
      if (ok && !gnssIoOk) {
        gnssSetAntenna(true);
        // 热插拔：非阻塞调度 500ms 后下发高动态/高刷新/精简语句配置，无阻塞 delay
        uint32_t t = millis() + 500;
        gnssPendingConfigAt = (t == 0) ? 1 : t;
      }
      gnssIoOk = ok;
    }
  }
  return gnssIoOk;
}
// ---- 每颗卫星的仰角/方位角（画卫星分布图用）----
// TinyGPSPlus 本身不解析 GSV 里每颗卫星的明细，这里旁路一份原始 NMEA 行自己抠。
struct SatInfo { char sys; uint8_t prn; int16_t elev; int16_t azim; uint8_t snr; };
static const int SAT_MAX = 48;
static SatInfo satList[SAT_MAX];
static int satCount = 0;

static char gnssSysFromTalker(const char* line) {   // line 开头两个字符是话务员前缀
  if (!strncmp(line, "GP", 2)) return 'G';   // GPS
  if (!strncmp(line, "GL", 2)) return 'R';   // GLONASS
  if (!strncmp(line, "GA", 2)) return 'E';   // Galileo
  if (!strncmp(line, "GB", 2) || !strncmp(line, "BD", 2)) return 'C';   // 北斗（新/旧前缀都认）
  if (!strncmp(line, "GQ", 2)) return 'J';   // QZSS
  return '?';   // 未知前缀（比如老式接收机把混合星座都塞进 GNGSV，这里没做 PRN 反推）
}

// 卫星物理轨道分类（GEO 地球静止轨道 / IGSO 倾斜地球同步轨道 / MEO 中圆轨道 / QZO 准天顶轨道）
static const char* gnssOrbitName(char sys, uint8_t prn) {
  if (sys == 'C') {
    if ((prn >= 1 && prn <= 5) || (prn >= 59 && prn <= 61)) return "GEO";
    if ((prn >= 6 && prn <= 10) || prn == 13 || prn == 16 || (prn >= 38 && prn <= 40)) return "IGS";
    return "MEO";
  }
  if (sys == 'J') {
    if (prn == 4) return "GEO";
    return "QZO";
  }
  if (sys == 'S' || (sys == 'G' && prn >= 33 && prn <= 64)) return "GEO";
  return "MEO";
}

// 标称载波频段代号
static const char* gnssBandName(char sys, uint8_t prn) {
  if (sys == 'C') return "B1";
  if (sys == 'R') return "G1";
  if (sys == 'E') return "E1";
  return "L1";
}

// 详细频点与标称频率 (紧凑格式，适配 240x135 屏幕)
static const char* gnssFreqShortStr(char sys) {
  if (sys == 'C') return "1561.10 MHz";
  if (sys == 'R') return "1602.00 MHz";
  return "1575.42 MHz";
}

// 物理轨道类型文字描述
static const char* gnssOrbitDesc(char sys, uint8_t prn) {
  const char* orb = gnssOrbitName(sys, prn);
  if (!strcmp(orb, "GEO")) return "Geostationary";
  if (!strcmp(orb, "IGS")) return "Inclined-Geo";
  if (!strcmp(orb, "QZO")) return "Quasi-Zenith";
  return "Medium-Earth";
}

// 卫星全称与研制国
static const char* gnssSysFullName(char sys, uint8_t prn) {
  if (sys == 'C') return "BDS Beidou (China)";
  if (sys == 'R') return "GLONASS (Russia)";
  if (sys == 'E') return "Galileo (Europe)";
  if (sys == 'J') return "QZSS Michibiki (Japan)";
  if (sys == 'S' || (sys == 'G' && prn >= 33 && prn <= 64)) return "SBAS Geostationary";
  return "GPS Navstar (USA)";
}

// 标称轨道半长轴或高度 (km)
static float gnssOrbitAltitudeKm(char sys, uint8_t prn) {
  const char* orb = gnssOrbitName(sys, prn);
  if (!strcmp(orb, "GEO") || !strcmp(orb, "IGS") || !strcmp(orb, "QZO")) return 35786.0f;
  if (sys == 'C') return 21528.0f;
  if (sys == 'R') return 19100.0f;
  if (sys == 'E') return 23222.0f;
  return 20200.0f; // GPS MEO
}

// 空间斜距几何推算 (Slant Range, km) 与单程电磁波飞行时延 (ms)
static void gnssCalcRangeDelay(char sys, uint8_t prn, int16_t elev, float& rangeKm, float& delayMs) {
  float h = gnssOrbitAltitudeKm(sys, prn);
  const float Re = 6371.0f; // 地球平均半径 (km)
  float el = (elev >= 0 && elev <= 90) ? (float)elev : 45.0f;
  float rad = el * (float)M_PI / 180.0f;
  float sinEl = sinf(rad);
  // 由余弦定理推导出的斜距精确解：d = sqrt(Re^2 * sin^2(el) + 2*Re*h + h^2) - Re*sin(el)
  rangeKm = sqrtf(Re * Re * sinEl * sinEl + 2.0f * Re * h + h * h) - Re * sinEl;
  delayMs = rangeKm / 299792.458f * 1000.0f;
}

// 取下一个以逗号分隔的字段，支持连续逗号空字段
static char* nextCsvField(char*& p) {
  if (!p) return nullptr;
  char* start = p;
  char* comma = strchr(p, ',');
  if (comma) {
    *comma = '\0';
    p = comma + 1;
  } else {
    p = nullptr;
  }
  return start;
}

// 处理一条已经去掉 '$' 和 '*校验和' 的 GSV 语句，比如 "GPGSV,3,1,11,05,54,047,32,..."
static void gnssProcessGSV(char* line) {
  // ⚠️ 不能用 strtok：它把 ",," 当一个分隔符，某颗星仰角/方位角/信号为空（常见，比如 "04,,,27"）
  // 时后面字段整体错位，卫星号读成 162/165 这种不存在的值，真正在用的星就对不上了。
  char sysc = gnssSysFromTalker(line);
  char* p = line;
  nextCsvField(p);                                   // 语句名，如 "GPGSV"
  if (!nextCsvField(p)) return;                      // 总消息数
  char* fMsg = nextCsvField(p);   if (!fMsg) return; // 消息序号
  char* fTotal = nextCsvField(p); if (!fTotal) return;
  int msgNum = atoi(fMsg);
  int total = atoi(fTotal);                          // 该星座可见卫星总数
  if (msgNum < 1) return;

  if (msgNum == 1) {   // 这个星座新一轮开始上报，先清掉它上一轮的旧记录
    int w = 0;
    for (int i = 0; i < satCount; i++) {
      if (satList[i].sys != sysc && !(sysc == 'G' && satList[i].sys == 'S')) {
        satList[w++] = satList[i];
      }
    }
    satCount = w;
  }
  // 本条带几颗星按总数算，不按剩余字段数：NMEA 4.1 末尾还多一个信号编号字段（",1*66" 的 1），
  // 按字段数会多解析出半颗假星，可见数虚高
  int inMsg = total - (msgNum - 1) * 4;
  if (inMsg > 4) inMsg = 4;
  for (int k = 0; k < inMsg; k++) {
    char* pPrn  = nextCsvField(p);
    char* pElev = nextCsvField(p);
    char* pAzim = nextCsvField(p);
    char* pSnr  = nextCsvField(p);
    if (!pPrn || !*pPrn) break;
    int prn  = atoi(pPrn);
    int elev = (pElev && *pElev) ? atoi(pElev) : -1;   // -1 = 未知（天空图不画它）
    int azim = (pAzim && *pAzim) ? atoi(pAzim) : -1;
    int snr  = (pSnr && *pSnr) ? atoi(pSnr) : 0;       // 0 = 可见但未跟踪
    if (prn <= 0 || prn > 255) continue;
    char effSys = (sysc == 'G' && prn >= 33 && prn <= 64) ? 'S' : sysc;
    int found = -1;
    for (int i = 0; i < satCount; i++) if (satList[i].sys == effSys && satList[i].prn == prn) { found = i; break; }
    if (found < 0 && satCount < SAT_MAX) found = satCount++;
    if (found >= 0) satList[found] = SatInfo{ effSys, (uint8_t)prn, (int16_t)elev, (int16_t)azim, (uint8_t)snr };
  }
}

// ---- GSA 语句手工解析与各星座在用卫星管理 ----
static uint8_t gnssFixType = 1; // 1=无定位, 2=2D, 3=3D
static float gnssPdop = -1.0f;
static float gnssHdop = -1.0f;
static float gnssVdop = -1.0f;

static const int SYS_SLOT_COUNT = 5;
static const char SYS_SLOT_CODES[SYS_SLOT_COUNT] = { 'G', 'C', 'R', 'E', 'J' };

struct ConstellationUsed {
  char sys;
  uint8_t count;
  uint8_t prns[12];
  uint32_t updatedMs;   // 超过 GSA_SLOT_TTL_MS 没刷新就当这个星座已不参与定位（比如配置页关掉了北斗）
};
static const uint32_t GSA_SLOT_TTL_MS = 3000;
static bool gsaSlotFresh(const ConstellationUsed& u) {
  return u.count > 0 && millis() - u.updatedMs < GSA_SLOT_TTL_MS;
}
static ConstellationUsed gnssUsedBySys[SYS_SLOT_COUNT];


void gnssProcessGSA(char* line) {
  if (!line) return;
  if (*line == '$') line++;
  char* star = strchr(line, '*');
  if (star) *star = '\0';

  char talkerSys = gnssSysFromTalker(line);
  char* p = line;
  nextCsvField(p); // 0: 语句名，如 "GNGSA", "GPGSA", "BDGSA"
  nextCsvField(p); // 1: M/A
  char* fFix = nextCsvField(p); // 2: 1=无定位, 2=2D, 3=3D
  if (!fFix) return;
  int fix = atoi(fFix);
  if (fix >= 1 && fix <= 3) gnssFixType = (uint8_t)fix;

  uint8_t prns[12];
  uint8_t prnCount = 0;
  for (int i = 0; i < 12; i++) {
    char* fPrn = nextCsvField(p);
    if (fPrn && *fPrn) {
      int prn = atoi(fPrn);
      if (prn > 0 && prnCount < 12) {
        prns[prnCount++] = (uint8_t)prn;
      }
    }
  }

  char* fPdop = nextCsvField(p);
  char* fHdop = nextCsvField(p);
  char* fVdop = nextCsvField(p);
  if (fPdop && *fPdop) gnssPdop = atof(fPdop);
  if (fHdop && *fHdop) gnssHdop = atof(fHdop);
  if (fVdop && *fVdop) gnssVdop = atof(fVdop);

  char* fSysId = nextCsvField(p);
  char sysc = talkerSys;
  if (sysc == '?' && fSysId && *fSysId) {
    int sid = atoi(fSysId);
    if (sid == 1) sysc = 'G';
    else if (sid == 2) sysc = 'R';
    else if (sid == 3) sysc = 'E';
    else if (sid == 4) sysc = 'C';
    else if (sid == 5) sysc = 'J';
  }
  if (sysc == '?' && prnCount > 0) {
    // GNGSA 不带 systemId（NMEA 4.1 之前的格式）：拿 PRN 去 GSV 卫星列表里反查星座，
    // 否则 GPS 和北斗两条 GNGSA 会互相覆盖同一个 'G' 槽位
    for (int i = 0; i < satCount && sysc == '?'; i++) {
      for (int k = 0; k < prnCount; k++) {
        if (satList[i].prn == prns[k] && satList[i].sys != '?') { sysc = satList[i].sys; break; }
      }
    }
  }
  if (sysc == '?') {
    if (prnCount > 0 && prns[0] > 64 && prns[0] <= 96) sysc = 'R';
    else sysc = 'G';
  }

  // 收到新的一条就替换该星座自己的集合
  for (int s = 0; s < SYS_SLOT_COUNT; s++) {
    if (SYS_SLOT_CODES[s] == sysc) {
      gnssUsedBySys[s].sys = sysc;
      gnssUsedBySys[s].count = prnCount;
      memcpy(gnssUsedBySys[s].prns, prns, prnCount);
      gnssUsedBySys[s].updatedMs = millis();
      break;
    }
  }
}

bool gnssIsSatUsed(char sys, uint8_t prn) {
  if (gnssFixType <= 1) return false;
  char checkSys = (sys == 'S') ? 'G' : sys;
  for (int s = 0; s < SYS_SLOT_COUNT; s++) {
    if (gnssUsedBySys[s].sys == checkSys) {
      if (!gsaSlotFresh(gnssUsedBySys[s])) return false;
      for (int i = 0; i < gnssUsedBySys[s].count; i++) {
        if (gnssUsedBySys[s].prns[i] == prn) return true;
      }
      return false;
    }
  }
  return false;
}

int gnssUsedSatCount() {
  if (gnssFixType <= 1) return 0;
  int total = 0;
  for (int s = 0; s < SYS_SLOT_COUNT; s++)
    if (gsaSlotFresh(gnssUsedBySys[s])) total += gnssUsedBySys[s].count;
  return total;
}

float gnssGetPdop() { return gnssPdop; }
float gnssGetHdop() { return gnssHdop; }
float gnssGetVdop() { return gnssVdop; }

// ---- 坐标格式支持：DEG / DMS / Maidenhead / UTM ----
enum CoordFormat { COORD_DEG = 0, COORD_DMS, COORD_GRID, COORD_UTM };
static CoordFormat gnssCoordFmt = COORD_DEG;

void gnssDetailKey(char k) {
  if (k == 'c' || k == 'C') {
    gnssCoordFmt = (CoordFormat)((gnssCoordFmt + 1) % 4);
    dirty = true;
  }
}

void formatDms(double val, bool isLat, char* out, size_t sz) {
  char dir = isLat ? (val >= 0 ? 'N' : 'S') : (val >= 0 ? 'E' : 'W');
  double a = fabs(val);
  int d = (int)a;
  double mTotal = (a - d) * 60.0;
  int m = (int)mTotal;
  double s = (mTotal - m) * 60.0;
  // 59.96" 按 %.1f 会显示成 60.0"：先四舍五入到 0.1 秒再进位
  s = floor(s * 10.0 + 0.5) / 10.0;
  if (s >= 60.0) { s -= 60.0; m++; }
  if (m >= 60) { m -= 60; d++; }
  // Font0 没有 ° 的字形，用 d 代替
  int tenths = (int)(s * 10.0 + 0.5);   // 整数格式化：编译器推不出 s<60，%f 会报截断警告
  snprintf(out, sz, "%ud%02u'%02u.%u\"%c", (unsigned)(uint16_t)d, (unsigned)(uint8_t)m,
           (unsigned)(uint8_t)(tenths / 10), (unsigned)(uint8_t)(tenths % 10), dir);
}

void toMaidenhead(double lat, double lon, char* out, size_t sz) {
  if (lat < -90.0) lat = -90.0;
  if (lat >= 90.0) lat = 89.999999;
  if (lon < -180.0) lon = -180.0;
  if (lon >= 180.0) lon = 179.999999;

  double lonP = lon + 180.0;
  double latP = lat + 90.0;

  int fLon = (int)(lonP / 20.0);
  int fLat = (int)(latP / 10.0);
  lonP -= fLon * 20.0;
  latP -= fLat * 10.0;

  int sLon = (int)(lonP / 2.0);
  int sLat = (int)(latP / 1.0);
  lonP -= sLon * 2.0;
  latP -= sLat * 1.0;

  int ssLon = (int)(lonP * 12.0);
  int ssLat = (int)(latP * 24.0);

  if (fLon < 0) fLon = 0;
  if (fLon > 17) fLon = 17;
  if (fLat < 0) fLat = 0;
  if (fLat > 17) fLat = 17;
  if (sLon < 0) sLon = 0;
  if (sLon > 9) sLon = 9;
  if (sLat < 0) sLat = 0;
  if (sLat > 9) sLat = 9;
  if (ssLon < 0) ssLon = 0;
  if (ssLon > 23) ssLon = 23;
  if (ssLat < 0) ssLat = 0;
  if (ssLat > 23) ssLat = 23;

  snprintf(out, sz, "%c%c%d%d%c%c",
           'A' + fLon, 'A' + fLat,
           sLon, sLat,
           'a' + ssLon, 'a' + ssLat);
}

void toUtm(double lat, double lon, char* out, size_t sz) {
  if (lat < -80.0 || lat > 84.0) {
    snprintf(out, sz, "UTM Lat Out");
    return;
  }
  const double a = 6378137.0;
  const double f = 1.0 / 298.257223563;
  const double e2 = 2 * f - f * f;
  const double ePrime2 = e2 / (1.0 - e2);
  const double k0 = 0.9996;

  int zone = (int)((lon + 180.0) / 6.0) + 1;
  if (zone > 60) zone = 60;
  if (zone < 1) zone = 1;

  double lonOrigin = (double)(zone - 1) * 6.0 - 180.0 + 3.0;

  double latRad = lat * M_PI / 180.0;
  double lonRad = lon * M_PI / 180.0;
  double lonOriginRad = lonOrigin * M_PI / 180.0;

  double N = a / sqrt(1.0 - e2 * sin(latRad) * sin(latRad));
  double T = tan(latRad) * tan(latRad);
  double C = ePrime2 * cos(latRad) * cos(latRad);
  double A = cos(latRad) * (lonRad - lonOriginRad);

  double M = a * ((1.0 - e2 / 4.0 - 3.0 * e2 * e2 / 64.0 - 5.0 * e2 * e2 * e2 / 256.0) * latRad
                - (3.0 * e2 / 8.0 + 3.0 * e2 * e2 / 32.0 + 45.0 * e2 * e2 * e2 / 1024.0) * sin(2.0 * latRad)
                + (15.0 * e2 * e2 / 256.0 + 45.0 * e2 * e2 * e2 / 1024.0) * sin(4.0 * latRad)
                - (35.0 * e2 * e2 * e2 / 3072.0) * sin(6.0 * latRad));

  double easting = k0 * N * (A + (1.0 - T + C) * pow(A, 3) / 6.0
                            + (5.0 - 18.0 * T + T * T + 72.0 * C - 58.0 * ePrime2) * pow(A, 5) / 120.0)
                   + 500000.0;

  double northing = k0 * (M + N * tan(latRad) * (A * A / 2.0
                                                + (5.0 - T + 9.0 * C + 4.0 * C * C) * pow(A, 4) / 24.0
                                                + (61.0 - 58.0 * T + T * T + 600.0 * C - 330.0 * ePrime2) * pow(A, 6) / 720.0));
  char hemi = (lat >= 0) ? 'N' : 'S';
  if (lat < 0) northing += 10000000.0;

  snprintf(out, sz, "%02d%c %.0f %.0f", zone, hemi, easting, northing);
}

// ---- 行程与诊断统计 ----
static uint32_t gnssTripStartTime = 0;
static uint32_t gnssTripMovingMs = 0;
static double   gnssTripDistMeters = 0.0;
static float    gnssTripMaxSpeed = 0.0f;
static float    gnssTripMaxAlt = -9999.0f;
static float    gnssTripMinAlt = 9999.0f;

static float    gnssTtffSec = -1.0f;
static float    gnssReacqSec = -1.0f;
static uint32_t gnssFirstByteMs = 0;
static uint32_t gnssLostFixMs = 0;
static bool     gnssPrevFixState = false;

static double   gnssLastFilterLat = 0.0;
static double   gnssLastFilterLng = 0.0;
static uint32_t gnssLastFilterMs = 0;
static bool     gnssHasFilterPos = false;

static const int SNR_HIST_LEN = 120;
static uint8_t  gnssSnrHist[SNR_HIST_LEN];
static int      gnssSnrHistHead = 0;
static int      gnssSnrHistCount = 0;
static float    gnssCurTop4Snr = 0.0f;
static uint32_t gnssLastSnrSampleMs = 0;

static uint32_t gnssLastFixAt = 0;   // 上一次处理过的定位时刻（millis 时间轴）

// 行程页按 r：只清行程统计，TTFF / 重新定位耗时 / SNR 曲线保留
void gnssTripResetStats() {
  gnssTripDistMeters = 0.0;
  gnssTripMovingMs = 0;
  gnssTripStartTime = millis();
  gnssTripMaxSpeed = 0.0f;
  gnssTripMaxAlt = -9999.0f;
  gnssTripMinAlt = 9999.0f;
  gnssHasFilterPos = false;
  gnssLastFixAt = 0;
  gnssLastFilterMs = 0;
  gnssLastFilterLat = 0.0;
  gnssLastFilterLng = 0.0;
}

// 全部清零（含 TTFF）：只给主机端测试台用，固件里没有调用
void gnssTripReset() {
  gnssTripResetStats();
  gnssTtffSec = -1.0f;
  gnssReacqSec = -1.0f;
  gnssLostFixMs = 0;
  gnssPrevFixState = false;
  gnssFirstByteMs = 0;
}

void gnssTripSetFirstByteMs(uint32_t ms) {
  gnssFirstByteMs = ms;
}

void gnssTripKey(char k) {
  if (k == 'r' || k == 'R') {
    gnssTripResetStats();
    dirty = true;
  }
}

// TinyGPS++ 的 isValid() 拿到过一次定位之后就永远为 true，失锁只能靠数据新鲜度判断
static const uint32_t GNSS_FIX_STALE_MS = 3000;
static bool gnssFixFresh() {
  return gps.location.isValid() && gps.location.age() < GNSS_FIX_STALE_MS;
}
// 速度只在定位新鲜且质量像样时才可信：HDOP 很大（室内/遮挡、只剩三四颗星）的定位，
// 位置来回跳几十米，模块给出的对地速度会跟着乱跳，静止时也能报出几十 km/h
static const float GNSS_SPEED_MAX_HDOP = 8.0f;
static bool gnssSpeedTrusted() {
  if (!gnssFixFresh() || !gps.speed.isValid() || gps.speed.age() >= GNSS_FIX_STALE_MS) return false;
  if (gps.hdop.isValid() && gps.hdop.hdop() > GNSS_SPEED_MAX_HDOP) return false;
  return true;
}
// 静止多普勒噪声死区滤波：< 1.5 km/h 视为静止归零，消除桌面微弱漂移
static float gnssSpeedKmph() {
  if (!gnssSpeedTrusted()) return 0.0f;
  float spd = (float)gps.speed.kmph();
  return (spd < 1.5f) ? 0.0f : spd;
}
// 航向角只在移动时有物理几何意义（静止时基线无位移，为纯数学随机噪声）
static bool gnssCourseTrusted() {
  return gnssSpeedTrusted() && gps.course.isValid() && gps.speed.kmph() >= 2.0f;
}

void gnssTripProcessPoint(uint32_t now, bool fix, uint32_t fixAgeMs,
                          double lat, double lng, float spd,
                          bool altValid, float alt) {
  if (gnssTripStartTime == 0) gnssTripStartTime = now;
  if (gnssFirstByteMs == 0 && gnssLastByteMs > 0) gnssFirstByteMs = gnssLastByteMs;

  if (fix && gnssTtffSec < 0.0f) {
    gnssTtffSec = (now - (gnssFirstByteMs ? gnssFirstByteMs : 1)) / 1000.0f;
  }

  if (!fix && gnssPrevFixState) {
    gnssLostFixMs = now;
  } else if (fix && !gnssPrevFixState && gnssLostFixMs > 0) {
    gnssReacqSec = (now - gnssLostFixMs) / 1000.0f;
  }
  gnssPrevFixState = fix;

  // 只在收到新的一个定位点时统计一次：本函数每帧都跑（~25fps），而定位只有 5Hz，
  // 按帧累加时长会把同一段时间重复算好几遍
  uint32_t fixAt = fix ? now - fixAgeMs : 0;
  bool newFix = fix && (gnssLastFixAt == 0 || (int32_t)(fixAt - gnssLastFixAt) > 50);
  if (newFix) {
    uint32_t dtMs = gnssLastFixAt ? fixAt - gnssLastFixAt : 0;
    gnssLastFixAt = fixAt;
    // 仅在真实移动(>=3.0 km/h)且当前定位可信时更新极速，避免室内漂移脉冲污染
    if (spd >= 3.0f && spd > gnssTripMaxSpeed) gnssTripMaxSpeed = spd;

    if (spd > 3.0f && dtMs > 0 && dtMs < 5000) {
      gnssTripMovingMs += dtMs;
      if (gnssHasFilterPos) {
        // 跟上一个被接受的点比：中间被丢弃的跳点不算，所以用距那个点的时间差
        double dist = TinyGPSPlus::distanceBetween(gnssLastFilterLat, gnssLastFilterLng, lat, lng);
        float sinceFilterSec = (fixAt - gnssLastFilterMs) / 1000.0f;
        float maxAllowed = (spd / 3.6f) * sinceFilterSec * 1.5f + 5.0f;
        if (dist <= maxAllowed) {
          if (dist > 0.3) gnssTripDistMeters += dist;   // 30cm 以下当定位抖动
          gnssLastFilterLat = lat; gnssLastFilterLng = lng; gnssLastFilterMs = fixAt;
        } else if (sinceFilterSec > 3.0f) {
          // 连续跳点太久：以当前点重新起算，不计入这段距离
          gnssLastFilterLat = lat; gnssLastFilterLng = lng; gnssLastFilterMs = fixAt;
        }
      } else {
        gnssLastFilterLat = lat; gnssLastFilterLng = lng; gnssLastFilterMs = fixAt;
        gnssHasFilterPos = true;
      }
    } else {
      // 静止或刚恢复定位：锚点跟着当前位置走，免得静止时的漂移在下次移动时一次性算进里程
      gnssLastFilterLat = lat; gnssLastFilterLng = lng; gnssLastFilterMs = fixAt;
      gnssHasFilterPos = true;
    }
  }

  if (altValid) {
    if (gnssTripMaxAlt < -9000.0f) {
      gnssTripMaxAlt = alt;
      gnssTripMinAlt = alt;
    } else {
      if (alt > gnssTripMaxAlt) gnssTripMaxAlt = alt;
      if (alt < gnssTripMinAlt) gnssTripMinAlt = alt;
    }
  }
}

double gnssTripGetDistMeters() { return gnssTripDistMeters; }
uint32_t gnssTripGetMovingMs() { return gnssTripMovingMs; }
float gnssTripGetMaxSpeed()    { return gnssTripMaxSpeed; }
float gnssTripGetTtffSec()     { return gnssTtffSec; }
float gnssTripGetReacqSec()    { return gnssReacqSec; }

static void gnssTripUpdate() {
  uint32_t now = millis();
  bool fix = gnssFixFresh();
  uint32_t fixAgeMs = fix ? gps.location.age() : 0;
  double lat = gps.location.lat();
  double lng = gps.location.lng();
  float spd = gnssSpeedKmph();
  bool altValid = fix && gps.altitude.isValid();
  float alt = altValid ? (float)gps.altitude.meters() : 0.0f;

  gnssTripProcessPoint(now, fix, fixAgeMs, lat, lng, spd, altValid, alt);

  if (now - gnssLastSnrSampleMs >= 1000 || gnssLastSnrSampleMs == 0) {
    gnssLastSnrSampleMs = now;
    uint8_t top4[4] = {0, 0, 0, 0};
    for (int i = 0; i < satCount; i++) {
      uint8_t s = satList[i].snr;
      for (int j = 0; j < 4; j++) {
        if (s > top4[j]) {
          for (int k = 3; k > j; k--) top4[k] = top4[k - 1];
          top4[j] = s;
          break;
        }
      }
    }
    int vCount = 0; float sum = 0;
    for (int j = 0; j < 4; j++) if (top4[j] > 0) { sum += top4[j]; vCount++; }
    gnssCurTop4Snr = (vCount > 0) ? (sum / vCount) : 0.0f;
    gnssSnrHist[gnssSnrHistHead] = (uint8_t)roundf(gnssCurTop4Snr);
    gnssSnrHistHead = (gnssSnrHistHead + 1) % SNR_HIST_LEN;
    if (gnssSnrHistCount < SNR_HIST_LEN) gnssSnrHistCount++;
  }
}

static char gnssLineBuf[96];
static int gnssLineLen = 0;
// 串口 NMEA 命令：在这个时间点之前，把模块发来的每一行原样回显到 USB 串口（排查解析问题用）
static uint32_t gnssNmeaEchoUntil = 0;
void gnssNmeaEcho(uint32_t ms) { gnssNmeaEchoUntil = ms ? millis() + ms : 0; }
// UTC 年月日时分秒 → Unix epoch（不依赖 timegm/时区，civil 天数算法）
static time_t utcToEpoch(int y, int mo, int d, int hh, int mm, int ss) {
  y -= mo <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;
  return (time_t)days * 86400 + hh * 3600 + mm * 60 + ss;
}

// 用 GPS 的 UTC 给系统对时。第一次没对过就立刻对；之后每隔 GPS_RESYNC_MS 再校一次
// （ESP32 没 RTC，晶振会慢慢漂，GPS 常校能一直压住）。没网时这也是唯一的准时间源。
static const uint32_t GPS_RESYNC_MS = 5 * 60 * 1000;   // 5 分钟
static uint32_t lastGpsSyncMs = 0;
static void gnssTrySyncClock() {
  if (!gps.date.isValid() || !gps.time.isValid()) return;
  if (gps.date.year() < 2021) return;            // 模块上电初值/乱码防呆
  if (gps.time.age() > 1500) return;             // 只用新鲜的时间
  // 已由 GPS 对过、且还没到重校间隔 → 跳过（NTP 对的则让 GPS 接管持续校准）
  if (timeSynced && timeFromGps && (millis() - lastGpsSyncMs) < GPS_RESYNC_MS) return;

  time_t epoch = utcToEpoch(gps.date.year(), gps.date.month(), gps.date.day(),
                            gps.time.hour(), gps.time.minute(), gps.time.second());
  if (epoch < 1600000000) return;                // sanity：> 2020-09
  epoch += (gps.time.age() + 500) / 1000;        // 补上从定位到现在的延时

  setenv("TZ", TZ_INFO, 1); tzset();             // 没走过 NTP 时也得设时区，否则显示成 UTC
  struct timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  timeSynced = true; timeFromGps = true;
  lastGpsSyncMs = millis();
}

bool   gnssHasFix(uint32_t maxAgeMs) { return gps.location.isValid() && gps.location.age() < maxAgeMs; }
double gnssLat()    { return gps.location.lat(); }
double gnssLng()    { return gps.location.lng(); }
// 海拔（米，椭球面以上）。GNSS 高程精度本来就比平面差 2~3 倍，只够区分"楼上/海边地面"
// 这种几十米的量级，别拿它当楼层高度用。没定位时返回 0。
double gnssAlt()    { return gps.altitude.isValid() ? gps.altitude.meters() : 0.0; }
int    gnssSats()   { return gps.satellites.isValid() ? (int)gps.satellites.value() : 0; }

void gnssPoll() {
  // 冷启动延迟重发保险（开机 3.5s 后兜底重发一次）与热插拔非阻塞配置下发
  if (gnssPendingConfigAt != 0 && (int32_t)(millis() - gnssPendingConfigAt) >= 0) {
    gnssPendingConfigAt = 0;
    if (gnssIoOk) {
      gnssSendDefaultPcas();
    }
  }

  while (GPSSerial.available()) {
    char c = GPSSerial.read();
    gps.encode(c);
    gnssLastByteMs = millis();

    if (c == '$') {
      gnssLineLen = 0;
    } else if (c == '\r' || c == '\n') {
      if (gnssLineLen > 6) {
        gnssLineBuf[gnssLineLen] = 0;
        // 必须在下面截掉校验和、GSV/GSA 解析改写缓冲之前打印，才是模块输出的原样
        if (gnssNmeaEchoUntil && (int32_t)(millis() - gnssNmeaEchoUntil) < 0) Serial.printf("NMEA $%s\n", gnssLineBuf);
        char* star = strchr(gnssLineBuf, '*');
        if (star) *star = 0;
        if (!strncmp(gnssLineBuf + 2, "GSV", 3)) gnssProcessGSV(gnssLineBuf);
        else if (!strncmp(gnssLineBuf + 2, "GSA", 3)) gnssProcessGSA(gnssLineBuf);
      }
      gnssLineLen = 0;
    } else if (gnssLineLen < (int)sizeof(gnssLineBuf) - 1) {
      gnssLineBuf[gnssLineLen++] = c;
    }
  }
  gnssTrySyncClock();
  gnssTripUpdate();
}

void gnssPrintDiag() {
  Serial.printf("[gps] antenna=%d silent=%lums rxAvail=%d\n",
                gnssAntennaOn, (unsigned long)(millis() - gnssLastByteMs), GPSSerial.available());
  Serial.printf("[gps] chars=%lu sentences=%lu passedCk=%lu failedCk=%lu\n",
                (unsigned long)gps.charsProcessed(), (unsigned long)gps.sentencesWithFix(),
                (unsigned long)gps.passedChecksum(), (unsigned long)gps.failedChecksum());
  Serial.printf("[gps] fix=%d sats=%d(list %d) hdop=%.1f lat=%.6f lon=%.6f age=%lums\n",
                gps.location.isValid(), gps.satellites.isValid() ? gps.satellites.value() : -1,
                satCount, gps.hdop.isValid() ? gps.hdop.hdop() : -1.0,
                gps.location.lat(), gps.location.lng(), (unsigned long)gps.location.age());
}

// ---- PC 主导模式：GNSS 串口流式输出 ----
static bool gnssStreamActive = false;
static int  gnssStreamHz = 1;
static uint32_t gnssStreamNextMs = 0;
static uint32_t gnssStreamSeq = 0;

void gnssStreamSet(bool on, int hz) {
  if (on) {
    if (hz < 1) hz = 1;
    if (hz > 5) hz = 5;
    gnssStreamActive = true;
    gnssStreamHz = hz;
    gnssStreamSeq = 0;
    Serial.printf("GNSS {\"t\":\"start\",\"hz\":%d}\n", hz);
    gnssStreamNextMs = millis() + (1000 / hz);
  } else {
    gnssStreamActive = false;
    Serial.println("GNSS {\"t\":\"end\"}");
  }
}

bool gnssStreamIsActive() {
  return gnssStreamActive;
}

int gnssStreamGetHz() {
  return gnssStreamHz;
}

static void gnssStreamPrintSats(uint32_t now) {
  Serial.printf("GNSS {\"t\":\"sats\",\"ts\":%lu,\"list\":[", (unsigned long)now);
  for (int i = 0; i < satCount; i++) {
    bool used = gnssIsSatUsed(satList[i].sys, satList[i].prn);
    Serial.printf("%s{\"sys\":\"%c\",\"prn\":%u,\"snr\":%u,",
                  (i > 0 ? "," : ""), satList[i].sys, (unsigned)satList[i].prn, (unsigned)satList[i].snr);
    // 仰角/方位角在 GSV 里可能为空（解析时记为 -1），按协议输出 null
    if (satList[i].elev >= 0) Serial.printf("\"el\":%d,", (int)satList[i].elev); else Serial.print("\"el\":null,");
    if (satList[i].azim >= 0) Serial.printf("\"az\":%d,", (int)satList[i].azim); else Serial.print("\"az\":null,");
    Serial.printf("\"used\":%s}", used ? "true" : "false");
  }
  Serial.println("]}");
}

static void gnssStreamPrintFix(uint32_t now) {
  Serial.print("GNSS {\"t\":\"fix\",");
  bool fresh = gnssFixFresh();
  Serial.printf("\"ms\":%lu,\"ts\":%lu,\"valid\":%s,", (unsigned long)now, (unsigned long)now, fresh ? "true" : "false");

  // lat / lon: valid=false 时仍给最后一次的值，从未有效或无效浮点时给 null
  if (gps.location.isValid() && !std::isnan(gps.location.lat()) && !std::isinf(gps.location.lat()) &&
      !std::isnan(gps.location.lng()) && !std::isinf(gps.location.lng())) {
    Serial.printf("\"lat\":%.6f,\"lon\":%.6f,", gps.location.lat(), gps.location.lng());
  } else {
    Serial.print("\"lat\":null,\"lon\":null,");
  }

  // alt: 椭球高，米
  if (gps.altitude.isValid() && !std::isnan(gps.altitude.meters()) && !std::isinf(gps.altitude.meters())) {
    Serial.printf("\"alt\":%.1f,", gps.altitude.meters());
  } else {
    Serial.print("\"alt\":null,");
  }

  // spd / crs: km/h 与度
  if (gps.speed.isValid() && !std::isnan(gps.speed.kmph()) && !std::isinf(gps.speed.kmph())) {
    Serial.printf("\"spd\":%.1f,", gps.speed.kmph());
  } else {
    Serial.print("\"spd\":null,");
  }

  if (gps.course.isValid() && !std::isnan(gps.course.deg()) && !std::isinf(gps.course.deg())) {
    Serial.printf("\"crs\":%.1f,", gps.course.deg());
  } else {
    Serial.print("\"crs\":null,");
  }

  // spd_ok: 速度与航向是否可信 (定位新鲜且 HDOP <= 8)
  Serial.printf("\"spd_ok\":%s,", gnssSpeedTrusted() ? "true" : "false");

  // fix: GSA 定位类型 (1=无定位, 2=2D, 3=3D)
  int fixType = (gnssIoOk && (now - gnssLastByteMs < 3000)) ? (int)gnssFixType : 1;
  Serial.printf("\"fix\":%d,", fixType);

  // q: GGA 定位质量 (0=无效, 1=GPS, 2=DGPS ... 没收到或超时时给 null)
  int qVal = 0;
  bool qOk = false;
  if (ggaQualityGN.isValid() && ggaQualityGN.age() < 2500 && ggaQualityGN.value()[0]) {
    qVal = atoi(ggaQualityGN.value());
    qOk = true;
  } else if (ggaQualityGP.isValid() && ggaQualityGP.age() < 2500 && ggaQualityGP.value()[0]) {
    qVal = atoi(ggaQualityGP.value());
    qOk = true;
  }
  if (qOk) Serial.printf("\"q\":%d,", qVal);
  else Serial.print("\"q\":null,");

  // used: 参与定位的卫星数 (GSA)
  Serial.printf("\"used\":%d,", gnssUsedSatCount());

  // vis: 可见卫星数 (GSV 统计)
  int visCount = satCount ? satCount : (gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
  Serial.printf("\"vis\":%d,", visCount);

  // hdop / pdop / vdop: 精度因子 (无定位或超出有效范围时 null)
  bool dopValid = (fixType > 1);
  float hd = (gnssHdop >= 0.0f) ? gnssHdop : ((gps.hdop.isValid() && gps.hdop.age() < 3000) ? (float)gps.hdop.hdop() : -1.0f);
  if (dopValid && hd >= 0.0f && hd < 99.0f && !std::isnan(hd) && !std::isinf(hd)) Serial.printf("\"hdop\":%.1f,", hd);
  else Serial.print("\"hdop\":null,");

  if (dopValid && gnssPdop >= 0.0f && gnssPdop < 99.0f && !std::isnan(gnssPdop) && !std::isinf(gnssPdop)) Serial.printf("\"pdop\":%.1f,", gnssPdop);
  else Serial.print("\"pdop\":null,");

  if (dopValid && gnssVdop >= 0.0f && gnssVdop < 99.0f && !std::isnan(gnssVdop) && !std::isinf(gnssVdop)) Serial.printf("\"vdop\":%.1f,", gnssVdop);
  else Serial.print("\"vdop\":null,");

  // geoid: 大地水准面差距，米
  double geoidSep = 0.0;
  if (gnssGeoidSeparation(geoidSep) && !std::isnan(geoidSep) && !std::isinf(geoidSep)) {
    Serial.printf("\"geoid\":%.1f,", geoidSep);
  } else {
    Serial.print("\"geoid\":null,");
  }

  // age: 定位数据距今毫秒数
  if (gps.location.isValid()) {
    Serial.printf("\"age\":%lu,", (unsigned long)gps.location.age());
  } else {
    Serial.print("\"age\":null,");
  }

  // utc: ISO 8601 UTC 时间 (来自 RMC)
  if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2021 && gps.time.age() <= 3000) {
    Serial.printf("\"utc\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\"}\n",
                  gps.date.year(), gps.date.month(), gps.date.day(),
                  gps.time.hour(), gps.time.minute(), gps.time.second());
  } else {
    Serial.println("\"utc\":null}");
  }
}

void gnssStreamTick() {
  if (!gnssStreamActive) return;
  uint32_t now = millis();
  if ((int32_t)(now - gnssStreamNextMs) < 0) return;
  gnssStreamNextMs = now + (1000 / gnssStreamHz);

  gnssStreamPrintFix(now);
  if ((gnssStreamSeq % 2) == 0) {
    gnssStreamPrintSats(now);
  }
  gnssStreamSeq++;
}

// 各星座配色（跟星座分布图、详情页的 GPS/BDS/GLO/GAL/QZS 保持一致）
// ⚠️ 必须返回 uint16_t(RGB565)。原来写的是 uint32_t，而 LGFX 见到 uint32_t 会按 RGB888 解释：
// TFT_WHITE(0xFFFF) 被当成 0x00FFFF 画出青色、TFT_CYAN(0x07FF) 画出蓝色、TFT_YELLOW 画出青绿，
// 图例和天空图的颜色全错。16 位的 TFT_* 宏和 24 位十六进制字面量不能混在一个返回类型里。
static uint16_t gnssSysColor(char sys) {
  switch (sys) {
    case 'G': return TFT_WHITE;                      // GPS
    case 'C': return cv.color565(255, 159, 10);      // 北斗（橙）
    case 'R': return TFT_CYAN;                       // GLONASS
    case 'E': return cv.color565(177, 156, 217);     // Galileo（紫）
    case 'J': return TFT_YELLOW;                     // QZSS（黄）
    case 'S': return cv.color565(255, 215, 0);       // SBAS（金黄）
    default:  return TFT_DARKGREY;
  }
}
static const char* gnssSysName(char sys) {
  switch (sys) {
    case 'G': return "GPS"; case 'C': return "BDS"; case 'R': return "GLO";
    case 'E': return "GAL"; case 'J': return "QZS"; case 'S': return "SBA";
    default: return "?";
  }
}

// GNSS 各子页共用的顶栏：左标题、右定位状态、底下一条分隔线，固定占 y=0..12，
// 内容从 y=14 起。extra 会摆在状态左边（地图页用来显示缩放档位）。
// 返回 false = IO 扩展芯片不通（Cap 模块没插/坏了），调用方直接 return，别再往下画。
// 注意：天线被用户关掉不算失败，那种情况照常画，只在顶栏挂一个 RF OFF 标记。
// （抽出来之前这段在四个函数里各抄了一份，连"antenna switch init failed"那句都是四份。）
static const int GNSS_HDR_BOTTOM = PAGE_HDR_BOTTOM;

static bool gnssHeader(const char* title, const char* extra = nullptr) {
  cv.fillScreen(TFT_BLACK);
  const bool stale = (millis() - gnssLastByteMs > 3000);
  const char* st = stale ? "STALE" : (gnssFixFresh() ? "FIX" : "NO FIX");
  drawPageHeader(title, st, stale ? TFT_RED : (gnssFixFresh() ? ACCENT : TFT_DARKGREY));
  if (extra) {   // 摆在状态左边（地图页放缩放档位）
    cv.setTextDatum(top_right); cv.setTextSize(1);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString(extra, SW - 6 - (int)strlen(st) * 6 - 8, 3);
  }

  if (!gnssIoOk) {
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.setTextDatum(middle_center);
    cv.drawString("antenna switch (I2C 0x43) init failed", SW / 2, SH / 2);
    return false;
  }
  // 天线是用户自己关的，只做提示，不挡页面。extra 占的就是这个位置（地图页的缩放档位），
  // 那种页面让位给 extra——地图页不显示 RF OFF，配置页里那一行本来就写着 on/off。
  if (!gnssAntennaOn && !extra) {
    cv.setTextDatum(top_right); cv.setTextSize(1);
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("RF OFF", SW - 6 - (int)strlen(st) * 6 - 8, 3);
  }
  return true;
}

static int gnssVisibleSatCountForSys(char sys) {
  int c = 0;
  for (int i = 0; i < satCount; i++) {
    if (satList[i].sys == sys) c++;
  }
  if (c > 0) return c;
  if (sys == 'G') return gsvCount(gsvGPS);
  if (sys == 'C') return max(gsvCount(gsvBeidouGB), gsvCount(gsvBeidouBD));
  if (sys == 'R') return gsvCount(gsvGLONASS);
  if (sys == 'E') return gsvCount(gsvGalileo);
  if (sys == 'J') return gsvCount(gsvQZSS);
  return 0;
}

static int gnssUsedSatCountForSys(char sys) {
  if (gnssFixType <= 1) return 0;
  char checkSys = (sys == 'S') ? 'G' : sys;
  for (int s = 0; s < SYS_SLOT_COUNT; s++) {
    if (gnssUsedBySys[s].sys == checkSys && gsaSlotFresh(gnssUsedBySys[s])) {
      if (sys == 'S') {
        int sbasUsed = 0;
        for (int i = 0; i < gnssUsedBySys[s].count; i++) {
          if (gnssUsedBySys[s].prns[i] >= 33 && gnssUsedBySys[s].prns[i] <= 64) sbasUsed++;
        }
        return sbasUsed;
      } else if (sys == 'G') {
        int gpsUsed = 0;
        for (int i = 0; i < gnssUsedBySys[s].count; i++) {
          if (gnssUsedBySys[s].prns[i] < 33 || gnssUsedBySys[s].prns[i] > 64) gpsUsed++;
        }
        return gpsUsed;
      }
      return gnssUsedBySys[s].count;
    }
  }
  return 0;
}

// 主界面：卫星分布图（天空图）——圆心=正上方天顶，圆周=地平线，
// 每个点是一颗卫星，按仰角/方位角摆位置，点的大小随信号强度(SNR)变化，按星座上色
void drawGnss() {
  if (!gnssHeader("GNSS")) return;
  const bool haveFix = gnssFixFresh();
  const bool stale = (millis() - gnssLastByteMs > 3000);

  // ---- 1. 左侧天空图：圆心=天顶，圆周=地平线 ----
  const int cx = 56, cy = 74, R = 46;

  // 仰角同心圆（地平线 + 30°/60° 圈）
  cv.drawCircle(cx, cy, R, ICON_DIM);
  cv.drawCircle(cx, cy, R * 2 / 3, DIM_BORDER);
  cv.drawCircle(cx, cy, R / 3, DIM_BORDER);

  // 十字分划线（留出天顶空隙，营造准星感）
  cv.drawLine(cx, cy - R, cx, cy - 4, DIM_BORDER);
  cv.drawLine(cx, cy + 4, cx, cy + R, DIM_BORDER);
  cv.drawLine(cx - R, cy, cx - 4, cy, DIM_BORDER);
  cv.drawLine(cx + 4, cy, cx + R, cy, DIM_BORDER);
  cv.drawPixel(cx, cy, ACCENT);

  // 方位刻度标识 N/S/E/W
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  cv.setTextDatum(bottom_center); cv.drawString("N", cx, cy - R - 1);
  cv.setTextDatum(top_center);    cv.drawString("S", cx, cy + R + 2);
  cv.setTextDatum(middle_right);  cv.drawString("W", cx - R - 2, cy);
  cv.setTextDatum(middle_left);   cv.drawString("E", cx + R + 2, cy);

  // 卫星打点：位置=仰角/方位角，大小=SNR，颜色=星座（在用实心，未在用空心）
  for (int i = 0; i < satCount; i++) {
    SatInfo& s = satList[i];
    if (s.elev < 0 || s.elev > 90 || s.azim < 0) continue;   // GSV 里仰角/方位角可能为空
    float r = R * (90 - s.elev) / 90.0f;
    float a = s.azim * (float)M_PI / 180.0f;
    int x = cx + (int)(r * sinf(a)), y = cy - (int)(r * cosf(a));
    int dotR = constrain(2 + s.snr / 12, 2, 5);
    bool used = gnssIsSatUsed(s.sys, s.prn);
    uint16_t col = stale ? TFT_DARKGREY : gnssSysColor(s.sys);
    cv.drawCircle(x, y, dotR + 1, TFT_BLACK);
    if (used) {
      cv.fillCircle(x, y, dotR, col);
    } else {
      cv.fillCircle(x, y, dotR, TFT_BLACK);
      cv.drawCircle(x, y, dotR, col);
    }
  }

  // ---- 2. 右侧信息卡片区 ----
  const int rx = 114, rw = SW - rx - 6;
  const int xr = SW - 6;

  // A. 顶部各星座在用/可见数（双列紧凑，全覆盖 5 大系统与 SBAS）
  const char SYS_ORDER[] = { 'G', 'C', 'R', 'E', 'J', 'S' };
  int visibleSysCount = 0;
  for (int i = 0; i < 6; i++) {
    if (gnssVisibleSatCountForSys(SYS_ORDER[i]) > 0 || gnssUsedSatCountForSys(SYS_ORDER[i]) > 0)
      visibleSysCount++;
  }

  int ly = 14;
  if (visibleSysCount > 0) {
    int col = 0;
    for (int i = 0; i < 6; i++) {
      char sys = SYS_ORDER[i];
      int vCount = gnssVisibleSatCountForSys(sys);
      int uCount = gnssUsedSatCountForSys(sys);
      if (vCount <= 0 && uCount <= 0) continue;
      int bx = (col % 2 == 0) ? rx + 2 : rx + 62;
      cv.fillCircle(bx + 3, ly + 4, 3, gnssSysColor(sys));
      char b[16];
      snprintf(b, sizeof(b), "%s %d/%d", gnssSysName(sys), uCount, vCount);
      cv.setTextColor(uCount > 0 ? TFT_LIGHTGREY : TFT_DARKGREY, TFT_BLACK);
      cv.setTextDatum(top_left);
      cv.drawString(b, bx + 10, ly);
      if (col % 2 == 1) ly += 11;
      col++;
    }
    if (col % 2 == 1) ly += 11;
  } else {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(top_left);
    cv.drawString("scanning sky...", rx + 4, ly);
    ly += 11;
  }

  // B. 精度与星数横条
  int uSats = gnssUsedSatCount();
  int vSats = satCount ? satCount : (gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
  float hdop = gps.hdop.isValid() ? gps.hdop.hdop() : -1;
  uint16_t hcol = hdop < 0    ? TFT_DARKGREY
                : hdop < 2.0f ? ACCENT
                : hdop < 5.0f ? TFT_YELLOW : TFT_ORANGE;

  cv.drawFastHLine(rx, ly + 2, rw, DIM_BORDER);
  ly += 5;

  char b[32];
  cv.setTextDatum(top_left);
  cv.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (uSats > 0) snprintf(b, sizeof(b), "%du / %dv", uSats, vSats);
  else           snprintf(b, sizeof(b), "%d sats", vSats);
  cv.drawString(b, rx + 2, ly);

  cv.setTextDatum(top_right);
  cv.setTextColor(hcol, TFT_BLACK);
  if (hdop < 0) snprintf(b, sizeof(b), "HDOP -");
  else          snprintf(b, sizeof(b), "HDOP %.1f", hdop);
  cv.drawString(b, xr, ly);
  ly += 12;

  // C. 定位数据卡片
  int cardH = min(48, SH - ly - 8);
  cv.fillRoundRect(rx, ly, rw, cardH, 3, CARD_BG);
  cv.drawRoundRect(rx, ly, rw, cardH, 3, DIM_BORDER);

  if (haveFix) {
    // 纬度
    cv.setTextDatum(top_left);
    cv.setTextColor(ICON_DIM, CARD_BG); cv.drawString("LAT", rx + 5, ly + 3);
    cv.setTextDatum(top_right);
    cv.setTextColor(TFT_WHITE, CARD_BG);
    snprintf(b, sizeof(b), "%+.4f", gps.location.lat());
    cv.drawString(b, xr - 4, ly + 3);

    // 经度
    cv.setTextDatum(top_left);
    cv.setTextColor(ICON_DIM, CARD_BG); cv.drawString("LON", rx + 5, ly + 15);
    cv.setTextDatum(top_right);
    cv.setTextColor(TFT_WHITE, CARD_BG);
    snprintf(b, sizeof(b), "%+.4f", gps.location.lng());
    cv.drawString(b, xr - 4, ly + 15);

    // 高度与速度/时间
    cv.setTextDatum(top_left);
    cv.setTextColor(ICON_DIM, CARD_BG); cv.drawString("ALT", rx + 5, ly + 27);
    cv.setTextDatum(top_right);
    cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
    double alt = gps.altitude.isValid() ? gps.altitude.meters() : 0;
    double spd = gnssSpeedKmph();
    if (spd >= 2.0f) {
      snprintf(b, sizeof(b), "%.0fm %.0fkm/h", alt, spd);
    } else if (gps.time.isValid()) {
      snprintf(b, sizeof(b), "%.0fm %02d:%02d UTC", alt, gps.time.hour(), gps.time.minute());
    } else {
      snprintf(b, sizeof(b), "%.0fm", alt);
    }
    cv.drawString(b, xr - 4, ly + 27);
  } else if (stale) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(TFT_RED, CARD_BG);
    char nb[24];
    snprintf(nb, sizeof(nb), "NO DATA %lus", (unsigned long)((millis() - gnssLastByteMs) / 1000));
    cv.drawString(nb, rx + rw / 2, ly + 18);
    cv.setTextColor(TFT_DARKGREY, CARD_BG);
    cv.drawString("CHECK WIRING", rx + rw / 2, ly + 30);
  } else {
    cv.setTextDatum(middle_center);
    cv.setTextColor(TFT_DARKGREY, CARD_BG);
    cv.drawString("WAITING FOR", rx + rw / 2, ly + 18);
    cv.drawString("POSITION FIX", rx + rw / 2, ly + 30);
  }

  drawPageDots();
}

void drawGnssDetail() {
  if (!gnssHeader("GNSS DATA")) return;

  char b[48];
  cv.setFont(&fonts::Font0); cv.setTextSize(1);

  // 1. 左右双卡片区域 (y=15..76, cardH=62)
  const int cardW = (SW - 16) / 2; // 112px
  const int leftX = 6;
  const int rightX = leftX + cardW + 4; // 122px
  const int cardY = 15, cardH = 62;

  // 左卡片：精度与品质
  cv.fillRoundRect(leftX, cardY, cardW, cardH, 3, CARD_BG);
  cv.drawRoundRect(leftX, cardY, cardW, cardH, 3, DIM_BORDER);

  // 右卡片：运动与高程
  cv.fillRoundRect(rightX, cardY, cardW, cardH, 3, CARD_BG);
  cv.drawRoundRect(rightX, cardY, cardW, cardH, 3, DIM_BORDER);

  const bool haveFix = gnssFixFresh();

  // 左卡片条目绘制 (4行，间距 14px)
  // 行 1: FIX 定位类型与 GGA 质量
  int ly = cardY + 4;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("FIX", leftX + 5, ly);
  cv.setTextDatum(top_right);
  if (!haveFix || gnssFixType <= 1) {
    cv.setTextColor(TFT_DARKGREY, CARD_BG);
    cv.drawString("NO FIX", leftX + cardW - 5, ly);
  } else {
    int q = gnssFixQuality();
    const char* fStr = (gnssFixType == 3) ? "3D" : "2D";
    snprintf(b, sizeof(b), "%s %s", fStr, gnssQualityName(q));
    cv.setTextColor((gnssFixType == 3) ? ACCENT : TFT_YELLOW, CARD_BG);
    cv.drawString(b, leftX + cardW - 5, ly);
  }

  // 行 2: SATS 在用/可见星数
  ly += 14;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("SATS", leftX + 5, ly);
  cv.setTextDatum(top_right);
  int uSats = gnssUsedSatCount();
  int vSats = satCount ? satCount : (gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
  snprintf(b, sizeof(b), "%du / %dv", uSats, vSats);
  cv.setTextColor(TFT_WHITE, CARD_BG);
  cv.drawString(b, leftX + cardW - 5, ly);

  // 行 3: DOP 精度因子 (PDOP, HDOP, VDOP)
  ly += 14;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("DOP", leftX + 5, ly);
  cv.setTextDatum(top_right);
  float hVal = (gnssHdop >= 0) ? gnssHdop : (gps.hdop.isValid() ? (float)gps.hdop.hdop() : -1.0f);
  if (gnssPdop >= 0 || hVal >= 0) {
    // 紧凑格式：两位数的 DOP 不带小数，否则会压到左边的标签上
    auto dopFmt = [](float v) { return v >= 10.0f ? "%.0f" : "%.1f"; };
    if (gnssPdop >= 0 && gnssVdop >= 0) {
      char f[24];
      snprintf(f, sizeof(f), "%s/%s/%s", dopFmt(gnssPdop), dopFmt(hVal), dopFmt(gnssVdop));
      snprintf(b, sizeof(b), f, gnssPdop > 99 ? 99.0f : gnssPdop, hVal > 99 ? 99.0f : hVal, gnssVdop > 99 ? 99.0f : gnssVdop);
    }
    else if (hVal >= 0) snprintf(b, sizeof(b), "HDOP %.1f", hVal);
    else snprintf(b, sizeof(b), "n/a");
  } else {
    snprintf(b, sizeof(b), "n/a");
  }
  uint16_t dopCol = (hVal < 0) ? TFT_DARKGREY : (hVal < 2.0f ? ACCENT : (hVal < 5.0f ? TFT_YELLOW : TFT_ORANGE));
  cv.setTextColor(dopCol, CARD_BG);
  cv.drawString(b, leftX + cardW - 5, ly);

  // 行 4: AGE 数据龄期
  ly += 14;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("AGE", leftX + 5, ly);
  cv.setTextDatum(top_right);
  uint32_t age = gps.location.age();
  if (haveFix && age != (uint32_t)-1) snprintf(b, sizeof(b), "%.1fs", age / 1000.0f);
  else snprintf(b, sizeof(b), "n/a");
  cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
  cv.drawString(b, leftX + cardW - 5, ly);

  // 右卡片条目绘制 (4行，间距 14px)
  // 行 1: SPD 速度
  int ry = cardY + 4;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("SPD", rightX + 5, ry);
  cv.setTextDatum(top_right);
  if (gnssSpeedTrusted()) snprintf(b, sizeof(b), "%.1f km/h", gnssSpeedKmph());
  else snprintf(b, sizeof(b), "n/a");
  cv.setTextColor(TFT_WHITE, CARD_BG);
  cv.drawString(b, rightX + cardW - 5, ry);

  // 行 2: CRS 航向
  ry += 14;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("CRS", rightX + 5, ry);
  cv.setTextDatum(top_right);
  if (gnssCourseTrusted()) snprintf(b, sizeof(b), "%.0f deg", gps.course.deg());
  else snprintf(b, sizeof(b), "---");
  cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
  cv.drawString(b, rightX + cardW - 5, ry);

  // 行 3: ALT 海拔高程
  ry += 14;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("ALT", rightX + 5, ry);
  cv.setTextDatum(top_right);
  if (gps.altitude.isValid()) snprintf(b, sizeof(b), "%.0fm", gps.altitude.meters());
  else snprintf(b, sizeof(b), "n/a");
  cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
  cv.drawString(b, rightX + cardW - 5, ry);

  // 行 4: GEOID 大地水准面差距
  ry += 14;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("GEOID", rightX + 5, ry);
  cv.setTextDatum(top_right);
  double sepVal = 0.0;
  if (gnssGeoidSeparation(sepVal)) snprintf(b, sizeof(b), "%+.1fm", sepVal);
  else snprintf(b, sizeof(b), "n/a");
  cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
  cv.drawString(b, rightX + cardW - 5, ry);

  // 2. 坐标全宽卡片 (y=79..118, 宽 228, 高 40)
  const int cpX = 6, cpY = 79, cpW = SW - 12, cpH = 40;
  cv.fillRoundRect(cpX, cpY, cpW, cpH, 3, CARD_BG);
  cv.drawRoundRect(cpX, cpY, cpW, cpH, 3, DIM_BORDER);

  cv.setTextDatum(top_left);
  cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("COORD", cpX + 6, cpY + 4);

  const char* fmtLabels[4] = { "DEG", "DMS", "GRID", "UTM" };
  cv.setTextDatum(top_right);
  cv.setTextColor(ACCENT, CARD_BG);
  snprintf(b, sizeof(b), "[%s]", fmtLabels[gnssCoordFmt]);
  cv.drawString(b, cpX + cpW - 6, cpY + 4);

  if (haveFix) {
    double lat = gps.location.lat();
    double lng = gps.location.lng();
    if (gnssCoordFmt == COORD_DEG) {
      char latBuf[24], lonBuf[24];
      snprintf(latBuf, sizeof(latBuf), "LAT %+.5f", lat);
      snprintf(lonBuf, sizeof(lonBuf), "LON %+.5f", lng);
      cv.setTextColor(TFT_WHITE, CARD_BG);
      cv.setTextDatum(top_left);
      cv.drawString(latBuf, cpX + 6, cpY + 16);
      cv.drawString(lonBuf, cpX + 116, cpY + 16);
    } else if (gnssCoordFmt == COORD_DMS) {
      char latDms[24], lonDms[24];
      formatDms(lat, true, latDms, sizeof(latDms));
      formatDms(lng, false, lonDms, sizeof(lonDms));
      cv.setTextColor(TFT_WHITE, CARD_BG);
      cv.setTextDatum(top_left);
      cv.drawString(latDms, cpX + 6, cpY + 16);
      cv.drawString(lonDms, cpX + 116, cpY + 16);
    } else if (gnssCoordFmt == COORD_GRID) {
      char gridBuf[16];
      toMaidenhead(lat, lng, gridBuf, sizeof(gridBuf));
      cv.setTextColor(TFT_WHITE, CARD_BG);
      cv.setTextDatum(top_left);
      snprintf(b, sizeof(b), "LOCATOR: %s", gridBuf);
      cv.drawString(b, cpX + 6, cpY + 16);
      snprintf(b, sizeof(b), "(WGS84 %+.4f, %+.4f)", lat, lng);
      cv.setTextColor(ICON_DIM, CARD_BG);
      cv.drawString(b, cpX + 6, cpY + 28);
    } else if (gnssCoordFmt == COORD_UTM) {
      char utmBuf[32];
      toUtm(lat, lng, utmBuf, sizeof(utmBuf));
      cv.setTextColor(TFT_WHITE, CARD_BG);
      cv.setTextDatum(top_left);
      snprintf(b, sizeof(b), "UTM: %s", utmBuf);
      cv.drawString(b, cpX + 6, cpY + 16);
      cv.setTextColor(ICON_DIM, CARD_BG);
      cv.drawString("DATUM: WGS-84", cpX + 6, cpY + 28);
    }
  } else if (millis() - gnssLastByteMs > 3000) {
    cv.setTextDatum(middle_center);
    cv.setTextColor(TFT_RED, CARD_BG);
    cv.drawString("NO DATA (CHECK WIRING)", cpX + cpW / 2, cpY + 22);
  } else {
    cv.setTextDatum(middle_center);
    cv.setTextColor(TFT_DARKGREY, CARD_BG);
    cv.drawString("WAITING FOR POSITION FIX", cpX + cpW / 2, cpY + 22);
  }

  // 3. 底部提示与页码点 (y=122..134)
  cv.setTextDatum(bottom_left);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("c: fmt", 6, SH - 2);

  cv.setTextDatum(bottom_right);
  if (gps.time.isValid()) {
    snprintf(b, sizeof(b), "UTC %02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
    cv.setTextColor(ICON_DIM, TFT_BLACK);
  } else {
    snprintf(b, sizeof(b), "UTC --:--:--");
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  }
  cv.drawString(b, SW - 6, SH - 2);

  drawPageDots();
}

// 卫星信号页：默认天空图（与 Sats 统一为横轴方位、纵轴仰角的抬头天际图），
// 按 'm'（或 t / v）在"天空图 / 信号表格"之间切换；天空图下 Enter 也会切到表格。
// 表格页按 SNR 从高到低列出当前 GSV 快照，支持物理轨道/频段解析与单星深度探针弹窗。
static bool satSkyMode = true;   // 默认天空图
static int satDetailTop = 0;
static int satDetailSel = 0;
static bool satInspectOpen = false;

bool gnssSatInspectActive() { return satInspectOpen; }

void gnssSatKey(char k) {
  if (satInspectOpen) {
    if (k == '\n' || k == '\r' || k == ' ' || k == '`' || k == '\b' || (uint8_t)k == 27) {
      satInspectOpen = false;
      dirty = true;
    }
    return;
  }
  // m / t / v：在天空图与表格模式之间切换；天空图下 Enter 也进表格（表格里 Enter 是探针弹窗）
  if (k == 'm' || k == 'M' || k == 't' || k == 'T' || k == 'v' || k == 'V' ||
      (satSkyMode && (k == '\n' || k == '\r'))) {
    satSkyMode = !satSkyMode;
    dirty = true;
    return;
  }
  if (satSkyMode) return;
  // 以下只在表格模式生效
  if (k == '\n' || k == '\r' || k == ' ') {
    if (satCount > 0) {
      satInspectOpen = true;
      dirty = true;
    }
    return;
  }
  if (k == '[' || k == 'k' || k == 'K' || k == 'w' || k == 'W') {
    if (satDetailSel > 0) satDetailSel--;
    if (satDetailSel < satDetailTop) satDetailTop = satDetailSel;
    dirty = true;
  } else if (k == ']' || k == 'j' || k == 'J' || k == 's' || k == 'S') {
    if (satDetailSel < satCount - 1) satDetailSel++;
    if (satDetailSel >= satDetailTop + 6) satDetailTop = satDetailSel - 5;
    dirty = true;
  }
}

// 天空图空状态：冷启动或未收到 GSV 语句时友好提示，避免绘制空坐标系误导用户
static void drawGnssSatEmpty() {
  const bool stale = (millis() - gnssLastByteMs > 3000);
  const int cy = 60;

  cv.fillRoundRect(14, cy - 24, SW - 28, 54, 4, cv.color565(10, 16, 22));
  cv.drawRoundRect(14, cy - 24, SW - 28, 54, 4, DIM_BORDER);

  cv.setTextDatum(top_center);
  cv.setTextSize(1);
  cv.setFont(&fonts::Font0);

  if (!gnssAntennaOn) {
    cv.setTextColor(TFT_YELLOW, cv.color565(10, 16, 22));
    cv.drawString("RF ANTENNA POWER OFF", SW / 2, cy - 16);
    cv.setTextColor(TFT_LIGHTGREY, cv.color565(10, 16, 22));
    cv.drawString("Enable antenna power in CONFIG page", SW / 2, cy);
    cv.setTextColor(TFT_DARKGREY, cv.color565(10, 16, 22));
    cv.drawString("Module cannot track without RF feed", SW / 2, cy + 14);
  } else if (stale) {
    cv.setTextColor(TFT_RED, cv.color565(10, 16, 22));
    cv.drawString("NO GNSS DATA STREAM", SW / 2, cy - 16);
    cv.setTextColor(TFT_LIGHTGREY, cv.color565(10, 16, 22));
    cv.drawString("GPS module serial silent (>3s)", SW / 2, cy);
    cv.setTextColor(TFT_DARKGREY, cv.color565(10, 16, 22));
    cv.drawString("Check Cap LoRa-1262 socket / pins", SW / 2, cy + 14);
  } else {
    cv.setTextColor(ACCENT, cv.color565(10, 16, 22));
    cv.drawString("SCANNING SKY FOR SATELLITES", SW / 2, cy - 16);
    cv.setTextColor(TFT_LIGHTGREY, cv.color565(10, 16, 22));
    cv.drawString("Waiting for GSV ephemeris data...", SW / 2, cy);
    cv.setTextColor(TFT_DARKGREY, cv.color565(10, 16, 22));
    cv.drawString("Cold start takes 30~60s outdoors", SW / 2, cy + 14);
  }

  // 底部提示行：左侧切换键、中间页码点、右侧卫星数 (0u / 0v)
  cv.setTextDatum(bottom_left); cv.setTextColor(DIM_BORDER, TFT_BLACK);
  cv.drawString("m table", 6, SH - 2);

  cv.setTextDatum(bottom_right); cv.setTextColor(DIM_BORDER, TFT_BLACK);
  cv.drawString("0u / 0v", SW - 6, SH - 2);

  drawPageDots();
}

// 抬头天际图渲染（GNSS 卫星分布）
static void drawGnssSatSky() {
  const int ytop = 13, yhor = 88, maxEl = 90;
  drawSkyBg(ytop, yhor, maxEl);

  const bool stale = (millis() - gnssLastByteMs > 3000);

  // 收集并换算在天际坐标系内的有效卫星
  struct SatLabelItem {
    int idx;
    int x, y, r;
    int prio;
    bool used;
  };
  SatLabelItem items[SAT_MAX];
  int itemCount = 0;

  for (int i = 0; i < satCount; i++) {
    const SatInfo& s = satList[i];
    if (s.elev < 0 || s.azim < 0) continue;
    int x, y;
    if (!skyCoord(s.azim, s.elev, ytop, yhor, maxEl, x, y)) continue;
    bool used = gnssIsSatUsed(s.sys, s.prn);
    int r = (s.snr == 0) ? 2 : ((s.snr >= 32) ? 4 : ((s.snr >= 20) ? 3 : 2));
    // 参与定位权重最高 (+1000)，其次按 SNR 排序，再次仰角
    int prio = (used ? 1000 : 0) + s.snr * 10 + s.elev;
    items[itemCount++] = SatLabelItem{ i, x, y, r, prio, used };
  }

  // 1. 绘制卫星圆点：颜色=星座、大小=SNR、无信号=空心、在用星=外圈绿辉光环
  for (int i = 0; i < itemCount; i++) {
    const SatInfo& s = satList[items[i].idx];
    const int x = items[i].x, y = items[i].y, r = items[i].r;
    const bool used = items[i].used;
    uint16_t col = stale ? TFT_DARKGREY : gnssSysColor(s.sys);

    // 横轴是环形的（0 与 360 度都是正北 N），正北贴边点必须绕着画，见 drawWrapped
    drawWrapped(x, SW, r + (used ? 2 : 0) + 1, [&](int xx) {
      if (s.snr == 0) {
        // 无 SNR 信号：画空心圆
        cv.drawCircle(xx, y, r, stale ? TFT_DARKGREY : cv.color565(90, 95, 100));
        if (used) cv.drawCircle(xx, y, r + 2, ACCENT);
      } else {
        // 有信号：画实心星座色圆点
        cv.fillCircle(xx, y, r, col);
        if (used) {
          // 参与定位的高亮：外圈加一层鲜艳的 ACCENT 辉光环
          cv.drawCircle(xx, y, r + 2, ACCENT);
        }
      }
    });
  }

  // 2. 标注 PRN（优先级排序 + 4 方位避让与防文字重叠）
  for (int i = 1; i < itemCount; i++) {
    SatLabelItem v = items[i];
    int j = i;
    while (j > 0 && items[j - 1].prio < v.prio) {
      items[j] = items[j - 1];
      j--;
    }
    items[j] = v;
  }

  struct RectBox { int16_t x1, y1, x2, y2; };
  RectBox boxes[SAT_MAX * 2];
  int boxCount = 0;

  // 将所有卫星圆心避让区域注册为障碍物，避免字盖在圆点上
  for (int i = 0; i < itemCount && boxCount < SAT_MAX * 2; i++) {
    int reach = items[i].r + (items[i].used ? 2 : 0) + 1;
    boxes[boxCount++] = RectBox{
      (int16_t)(items[i].x - reach), (int16_t)(items[i].y - reach),
      (int16_t)(items[i].x + reach), (int16_t)(items[i].y + reach)
    };
  }

  cv.setFont(&fonts::Font0);
  cv.setTextSize(1);
  cv.setTextDatum(top_left);

  for (int i = 0; i < itemCount; i++) {
    const SatInfo& s = satList[items[i].idx];
    char tag[8];
    snprintf(tag, sizeof(tag), "%c%02u", s.sys, (unsigned)s.prn);
    // 按实际字符数算宽度：QZSS/SBAS 的 PRN 是三位数（J193），写死 3 个字符会让标签
    // 比登记的避让框宽出一截，盖到旁边的星和它的标签上
    const int tw = (int)strlen(tag) * 6;
    const int th = 8;
    const int reach = items[i].r + (items[i].used ? 2 : 0);

    // 候选方位：右、左、上、下
    int candX[4] = {
      items[i].x + reach + 2,
      items[i].x - reach - 2 - tw,
      items[i].x - tw / 2,
      items[i].x - tw / 2
    };
    int candY[4] = {
      items[i].y - th / 2,
      items[i].y - th / 2,
      items[i].y - reach - 2 - th,
      items[i].y + reach + 3
    };

    int bestCand = -1;
    for (int c = 0; c < 4; c++) {
      int bx1 = candX[c], by1 = candY[c];
      int bx2 = bx1 + tw, by2 = by1 + th;

      // 屏幕边界限制：不能贴破左右边界，也不能盖住顶栏或地平线刻度字
      if (bx1 < 2 || bx2 > SW - 2) continue;
      if (by1 < ytop + 2 || by2 > yhor - 1) continue;

      // 碰撞检测：不能与已有圆点或已绘制文字重叠
      bool collide = false;
      for (int b = 0; b < boxCount; b++) {
        if (bx1 < boxes[b].x2 && bx2 > boxes[b].x1 &&
            by1 < boxes[b].y2 && by2 > boxes[b].y1) {
          collide = true;
          break;
        }
      }
      if (!collide) {
        bestCand = c;
        break;
      }
    }

    if (bestCand >= 0) {
      int lx = candX[bestCand], ly = candY[bestCand];
      if (boxCount < SAT_MAX * 2) {
        boxes[boxCount++] = RectBox{
          (int16_t)(lx - 1), (int16_t)(ly - 1),
          (int16_t)(lx + tw + 1), (int16_t)(ly + th + 1)
        };
      }
      uint16_t textCol = items[i].used ? TFT_WHITE
                       : (s.snr == 0 ? TFT_DARKGREY : gnssSysColor(s.sys));
      // 绘制微型暗色底块，保证掠过仰角虚线时文字依然清晰
      cv.fillRect(lx - 1, ly - 1, tw + 2, th + 2, cv.color565(12, 18, 22));
      cv.setTextColor(textCol, cv.color565(12, 18, 22));
      cv.drawString(tag, lx, ly);
    }
  }

  // 3. 底部 HUD 卡片：y=99..124 (高 25px)
  cv.fillRoundRect(4, 99, SW - 8, 25, 2, cv.color565(10, 16, 20));
  cv.drawRoundRect(4, 99, SW - 8, 25, 2, DIM_BORDER);

  // 第一行 (y=101): 星座数量与最高 SNR
  const char SYS_ORDER[] = { 'G', 'C', 'R', 'E', 'J' };
  int sysCount[5] = {0};
  uint8_t maxSnr = 0;
  char topSat[8] = "";
  for (int i = 0; i < satCount; i++) {
    const SatInfo& s = satList[i];
    for (int k = 0; k < 5; k++) {
      if (s.sys == SYS_ORDER[k]) { sysCount[k]++; break; }
    }
    if (s.snr > maxSnr) {
      maxSnr = s.snr;
      snprintf(topSat, sizeof(topSat), "%c%02u", s.sys, (unsigned)s.prn);
    }
  }

  // 右侧的最高信号卫星先算好宽度，左侧星座计数只准用剩下的地方：四个星座各两位数时
  // （"GPS 12 BDS 10 GLO 8 GAL 6"）会一路画进右边的 "G14 50dB" 里去，放不下的星座就不画了
  char topBuf[20] = "";
  if (maxSnr > 0) snprintf(topBuf, sizeof(topBuf), "%s %udB", topSat, (unsigned)maxSnr);
  const int sysLimit = SW - 8 - (int)strlen(topBuf) * 6 - 4;

  int kx = 8;
  cv.setTextDatum(top_left); cv.setTextSize(1);
  for (int k = 0; k < 4; k++) {
    if (sysCount[k] <= 0) continue;
    char b[16];
    snprintf(b, sizeof(b), "%s %d", gnssSysName(SYS_ORDER[k]), sysCount[k]);
    if (kx + 9 + (int)strlen(b) * 6 > sysLimit) break;
    cv.fillCircle(kx + 3, 105, 3, gnssSysColor(SYS_ORDER[k]));
    cv.setTextColor(TFT_LIGHTGREY, cv.color565(10, 16, 20));
    cv.drawString(b, kx + 9, 101);
    kx += (int)strlen(b) * 6 + 14;
  }

  // 右侧标最高信号卫星与强度
  if (maxSnr > 0) {
    cv.setTextDatum(top_right);
    cv.setTextColor((maxSnr >= 30) ? ACCENT : ((maxSnr >= 18) ? TFT_YELLOW : TFT_RED), cv.color565(10, 16, 20));
    cv.drawString(topBuf, SW - 8, 101);
  }

  // 第二行 (y=112): 图例（定位环、跟踪点、无信号、点大小）
  cv.setTextDatum(middle_left);
  // 定位星图例
  cv.fillCircle(11, 118, 2, TFT_WHITE);
  cv.drawCircle(11, 118, 4, ACCENT);
  cv.setTextColor(TFT_LIGHTGREY, cv.color565(10, 16, 20));
  cv.drawString("Fix", 18, 118);

  // 仅跟踪、未参与定位：用中性灰点。不能拿某个星座的颜色当图例（原来用北斗橙），
  // 图上橙色的意思是"北斗"，参与定位的北斗星也是橙色的，两层含义会混在一起
  cv.fillCircle(48, 118, 2, TFT_LIGHTGREY);
  cv.drawString("Track", 54, 118);

  // 无信号空心
  cv.drawCircle(95, 118, 2, cv.color565(120, 130, 135));
  cv.setTextColor(TFT_DARKGREY, cv.color565(10, 16, 20));
  cv.drawString("No-SNR", 101, 118);

  // 大小说明
  cv.setTextDatum(middle_right);
  cv.setTextColor(ICON_DIM, cv.color565(10, 16, 20));
  cv.drawString("Size~SNR", SW - 8, 118);

  // 底部提示行：m 键切换为表格、右侧卫星数 (在用/可见)、中间页码点
  int usedSats = gnssUsedSatCount();
  cv.setTextDatum(bottom_left); cv.setTextColor(DIM_BORDER, TFT_BLACK);
  cv.drawString("m table", 6, SH - 2);

  char satBuf[24];
  snprintf(satBuf, sizeof(satBuf), "%du / %dv", usedSats, satCount);
  cv.setTextDatum(bottom_right); cv.setTextColor(DIM_BORDER, TFT_BLACK);
  cv.drawString(satBuf, SW - 6, SH - 2);

  drawPageDots();
}

// 原始卫星信号详情表格（按 SNR 从高到低排序，支持 '['/']' 滚动浏览）
static void drawGnssSatTable() {
  int order[SAT_MAX];
  for (int i = 0; i < satCount; i++) order[i] = i;
  for (int i = 1; i < satCount; i++) {
    int v = order[i], j = i;
    while (j > 0 && satList[order[j - 1]].snr < satList[v].snr) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = v;
  }

  const int rows = 6;
  // 卫星数会随 GSV 变少：光标跟着收，别停在不存在的行上
  if (satDetailSel > max(0, satCount - 1)) satDetailSel = max(0, satCount - 1);
  if (satCount == 0) satInspectOpen = false;
  if (satDetailTop > max(0, satCount - rows)) satDetailTop = max(0, satCount - rows);
  const int first = satDetailTop;
  const int last = min(satCount, first + rows);

  const int COL_SYS  = 7;   // GPS, BDS, GLO, GAL, QZS, SBA
  const int COL_PRN  = 27;  // 02, 27, 43
  const int COL_ORB  = 48;  // MEO, GEO, IGS, QZO
  const int COL_SNR  = 69;  // 39, 27
  const int COL_EL   = 90;  // 45, 67
  const int COL_AZ   = 106; // 052, 303
  const int COL_BAR  = 130; // 8格信号条
  const int COL_BAND = 182; // L1, B1, G1, E1
  const int COL_STA  = 204; // FIX / VIS

  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  cv.setTextDatum(top_left);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("SYS", COL_SYS, 17);
  cv.drawString("PRN", COL_PRN, 17);
  cv.drawString("ORB", COL_ORB, 17);
  cv.drawString("SNR", COL_SNR, 17);
  cv.drawString("EL",  COL_EL,  17);
  cv.drawString("AZ",  COL_AZ,  17);
  cv.drawString("SIGNAL", COL_BAR, 17);
  cv.drawString("BND", COL_BAND, 17);
  cv.drawString("STA", COL_STA, 17);
  cv.drawFastHLine(6, 27, SW - 12, DIM_BORDER);

  char b[32];
  const uint16_t offSlotCol = cv.color565(12, 30, 20); // 信号格未点亮底槽

  for (int row = first; row < last; row++) {
    const SatInfo& s = satList[order[row]];
    int y = 30 + (row - first) * 14;

    // 选中光标高亮框与斑马条纹
    if (row == satDetailSel && !satInspectOpen) {
      cv.fillRect(5, y - 1, SW - 13, 13, cv.color565(12, 40, 28));
      cv.drawRoundRect(4, y - 1, SW - 11, 13, 2, ACCENT);
    } else if ((row - first) % 2 == 1) {
      cv.fillRect(5, y - 1, SW - 13, 13, cv.color565(6, 20, 14));
    }

    bool used = gnssIsSatUsed(s.sys, s.prn);
    int bar = constrain((int)s.snr / 5, 0, 8);

    // 在用卫星画实心绿点指示，仅可见卫星画空心暗灰点
    if (used) cv.fillCircle(COL_SYS - 4, y + 4, 2, ACCENT);
    else      cv.drawCircle(COL_SYS - 4, y + 4, 2, cv.color565(40, 50, 50));

    cv.setTextColor(used ? gnssSysColor(s.sys) : cv.color565(120, 130, 135), TFT_BLACK);
    cv.drawString(gnssSysName(s.sys), COL_SYS, y);

    snprintf(b, sizeof(b), "%02u", (unsigned)s.prn);
    cv.setTextColor(used ? TFT_WHITE : TFT_DARKGREY, TFT_BLACK); cv.drawString(b, COL_PRN + 1, y);

    cv.setTextColor(cv.color565(140, 180, 160), TFT_BLACK);
    cv.drawString(gnssOrbitName(s.sys, s.prn), COL_ORB, y);

    uint16_t snrCol = (s.snr >= 30) ? ACCENT : ((s.snr >= 18) ? TFT_YELLOW : TFT_RED);
    cv.setTextColor(used ? snrCol : cv.color565(100, 110, 115), TFT_BLACK);
    snprintf(b, sizeof(b), "%2u", (unsigned)s.snr); cv.drawString(b, COL_SNR + 3, y);

    cv.setTextColor(used ? TFT_LIGHTGREY : TFT_DARKGREY, TFT_BLACK);
    if (s.elev >= 0) snprintf(b, sizeof(b), "%2d", (int)s.elev); else snprintf(b, sizeof(b), "--");
    cv.drawString(b, COL_EL, y);
    if (s.azim >= 0) snprintf(b, sizeof(b), "%3d", (int)s.azim); else snprintf(b, sizeof(b), " --");
    cv.drawString(b, COL_AZ, y);

    // 8 格信号强度条
    uint16_t litCol = used ? gnssSysColor(s.sys) : cv.color565(55, 70, 65);
    for (int k = 0; k < 8; k++) {
      uint16_t c = (k < bar) ? litCol : offSlotCol;
      cv.fillRect(COL_BAR + k * 6, y + 2, 4, 7, c);
    }

    // 频段与状态指示
    cv.setTextColor(used ? ACCENT : cv.color565(80, 100, 90), TFT_BLACK);
    cv.drawString(gnssBandName(s.sys, s.prn), COL_BAND + 2, y);

    cv.setTextColor(used ? ACCENT : cv.color565(80, 85, 90), TFT_BLACK);
    cv.drawString(used ? "FIX" : "VIS", COL_STA, y);
  }

  // 垂直细微滚动条指示
  drawScrollBar(SW - 7, 30, 80, satDetailTop, satCount, rows, ACCENT, DIM_BORDER);

  if (satCount == 0) {
    cv.setTextDatum(middle_center); cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.drawString("waiting for GSV data", SW / 2, 66);
  }

  // 底部提示行：左右分离
  int usedSats = gnssUsedSatCount();
  cv.setTextDatum(bottom_left); cv.setTextColor(DIM_BORDER, TFT_BLACK);
  cv.drawString("m:sky Ent:insp", 6, SH - 2);

  char satBuf[24];
  snprintf(satBuf, sizeof(satBuf), "%du / %dv", usedSats, satCount);
  cv.setTextDatum(bottom_right); cv.setTextColor(DIM_BORDER, TFT_BLACK);
  cv.drawString(satBuf, SW - 6, SH - 2);

  // 单星遥测详情探针弹窗 (Modal)
  if (satInspectOpen && satCount > 0) {
    int selIdx = (satDetailSel >= 0 && satDetailSel < satCount) ? order[satDetailSel] : order[0];
    const SatInfo& s = satList[selIdx];
    bool used = gnssIsSatUsed(s.sys, s.prn);
    float rangeKm = 0.0f, delayMs = 0.0f;
    gnssCalcRangeDelay(s.sys, s.prn, s.elev, rangeKm, delayMs);
    const char* orbName = gnssOrbitName(s.sys, s.prn);
    const char* fullName = gnssSysFullName(s.sys, s.prn);
    int az = (s.azim >= 0) ? (int)s.azim : 0;
    int cIdx = (int)((az + 22.5f) / 45.0f) % 8;
    if (cIdx < 0) cIdx += 8;
    const char* cardDir = CARD8[cIdx];

    const int mw = 226, mh = 106;
    const int mx = (SW - mw) / 2; // 7
    const int my = 14;

    // 半透明/深色卡片底
    cv.fillRoundRect(mx, my, mw, mh, 4, cv.color565(4, 14, 10));
    cv.drawRoundRect(mx, my, mw, mh, 4, used ? ACCENT : cv.color565(20, 80, 50));

    // 模态顶栏
    cv.fillRoundRect(mx + 1, my + 1, mw - 2, 17, 3, cv.color565(10, 32, 22));
    cv.fillCircle(mx + 8, my + 9, 3, gnssSysColor(s.sys));

    char titleBuf[48];
    snprintf(titleBuf, sizeof(titleBuf), "%s PRN %02u", gnssSysName(s.sys), (unsigned)s.prn);
    cv.setTextDatum(top_left);
    cv.setTextColor(gnssSysColor(s.sys), cv.color565(10, 32, 22));
    cv.drawString(titleBuf, mx + 16, my + 5);

    cv.setTextDatum(top_right);
    if (used) {
      cv.setTextColor(ACCENT, cv.color565(10, 32, 22));
      cv.drawString("[3D FIX]  [x]", mx + mw - 6, my + 5);
    } else {
      cv.setTextColor(TFT_DARKGREY, cv.color565(10, 32, 22));
      cv.drawString("[VIS]  [x]", mx + mw - 6, my + 5);
    }

    int ty = my + 22;

    // 第1行：归属星座与研制国
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, cv.color565(4, 14, 10));
    cv.drawString("SYSTEM", mx + 6, ty);
    cv.setTextColor(TFT_WHITE, cv.color565(4, 14, 10));
    cv.drawString(fullName, mx + 48, ty);
    ty += 14;

    // 第2行：物理轨道与类型解析
    char orbBuf[48];
    snprintf(orbBuf, sizeof(orbBuf), "%s %s (~%.0fkm)", orbName, gnssOrbitDesc(s.sys, s.prn), gnssOrbitAltitudeKm(s.sys, s.prn));
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, cv.color565(4, 14, 10));
    cv.drawString("ORBIT", mx + 6, ty);
    cv.setTextColor(TFT_LIGHTGREY, cv.color565(4, 14, 10));
    cv.drawString(orbBuf, mx + 48, ty);
    ty += 14;

    // 第3行：空间仰角与天球方位
    char posBuf[36];
    if (s.elev >= 0 && s.azim >= 0) {
      snprintf(posBuf, sizeof(posBuf), "EL %2d deg / AZ %03d (%s)", (int)s.elev, (int)s.azim, cardDir);
    } else if (s.elev >= 0) {
      snprintf(posBuf, sizeof(posBuf), "EL %2d deg / AZ --", (int)s.elev);
    } else {
      snprintf(posBuf, sizeof(posBuf), "EL -- / AZ -- (acquiring)");
    }
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, cv.color565(4, 14, 10));
    cv.drawString("SKY", mx + 6, ty);
    cv.setTextColor(TFT_LIGHTGREY, cv.color565(4, 14, 10));
    cv.drawString(posBuf, mx + 48, ty);
    ty += 14;

    // 第4行：电磁波空间斜距与单向时延
    char dlyBuf[36];
    snprintf(dlyBuf, sizeof(dlyBuf), "%.0f km (tof ~%.1f ms)", rangeKm, delayMs);
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, cv.color565(4, 14, 10));
    cv.drawString("RANGE", mx + 6, ty);
    cv.setTextColor(TFT_WHITE, cv.color565(4, 14, 10));
    cv.drawString(dlyBuf, mx + 48, ty);
    ty += 14;

    // 第5行：信噪比与载波频点
    char snrBuf[48];
    snprintf(snrBuf, sizeof(snrBuf), "%u dB-Hz  [%s] %s", (unsigned)s.snr, gnssBandName(s.sys, s.prn), gnssFreqShortStr(s.sys));
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, cv.color565(4, 14, 10));
    cv.drawString("SIGNAL", mx + 6, ty);
    uint16_t sc = (s.snr >= 30) ? ACCENT : ((s.snr >= 18) ? TFT_YELLOW : TFT_RED);
    cv.setTextColor(sc, cv.color565(4, 14, 10));
    cv.drawString(snrBuf, mx + 48, ty);
  }

  drawPageDots();
}

void drawGnssSat() {
  if (satSkyMode) {
    if (!gnssHeader("SAT SKY")) return;
    if (satCount == 0) {
      drawGnssSatEmpty();
      return;
    }
    drawGnssSatSky();
  } else {
    if (!gnssHeader("SAT TABLE")) return;
    drawGnssSatTable();
  }
}


// ---- ATGM336H 配置页 ----
static const char* CFG_RATE_NAME[] = { "1 Hz", "2 Hz", "5 Hz" };
static const char* CFG_RATE_MS[]   = { "1000", "500", "200" };
static const char* CFG_SYS_NAME[]  = { "GPS+BDS", "GPS only", "BDS only", "GPS+GLO" };
static const char* CFG_SYS_MODE[]  = { "3", "1", "2", "5" };
static const char* CFG_DYN_NAME[]  = { "Aero <2g", "Aero <1g", "Vehicle", "Portable" };
static const char* CFG_DYN_MODE[]  = { "6", "5", "3", "0" };
static const char* CFG_NMEA_NAME[] = { "full", "nav+gsv" };
static int cfgSel = 0, cfgRate = 2, cfgSys = 0, cfgDyn = 0, cfgNmea = 1;
static uint32_t cfgMsgUntil = 0;
static char cfgMsg[24] = "";

static void gnssConfigMessage(const char* msg) {
  strncpy(cfgMsg, msg, sizeof(cfgMsg) - 1); cfgMsg[sizeof(cfgMsg) - 1] = 0;
  cfgMsgUntil = millis() + 1800;
  dirty = true;
}

static void gnssApplyConfigItem(int item) {
  gnssPendingConfigAt = 0; // 用户手动干预配置，取消未执行的开机默认配置重试
  char body[80];
  switch (item) {
    case 0:
      snprintf(body, sizeof(body), "PCAS02,%s", CFG_RATE_MS[cfgRate]);
      gnssSendPcas(body); gnssConfigMessage("rate applied"); break;
    case 1:
      snprintf(body, sizeof(body), "PCAS04,%s", CFG_SYS_MODE[cfgSys]);
      gnssSendPcas(body); gnssConfigMessage("system applied"); break;
    case 2:
      snprintf(body, sizeof(body), "PCAS11,%s", CFG_DYN_MODE[cfgDyn]);
      gnssSendPcas(body); gnssConfigMessage("dynamic applied"); break;
    case 3:
      if (cfgNmea == 0) gnssSendPcas("PCAS03,1,1,1,1,1,1,1,1,0,0,,,0,0");
      else              gnssSendPcas("PCAS03,1,0,1,1,1,0,0,0,0,0,,,0,0");
      gnssConfigMessage("NMEA applied"); break;
    case 4:
      if (gnssSetAntenna(!gnssAntennaOn)) gnssConfigMessage(gnssAntennaOn ? "RF antenna on" : "RF antenna off");
      else gnssConfigMessage("antenna I2C failed");
      break;
  }
}

void gnssConfigKey(char k) {
  const int itemCount = 5;
  if (k == '[') { cfgSel = (cfgSel - 1 + itemCount) % itemCount; dirty = true; return; }
  if (k == ']') { cfgSel = (cfgSel + 1) % itemCount; dirty = true; return; }
  if (k == '-' || k == '=') {
    int d = (k == '=') ? 1 : -1;
    if (cfgSel == 0) cfgRate = (cfgRate + d + 3) % 3;
    else if (cfgSel == 1) cfgSys = (cfgSys + d + 4) % 4;
    else if (cfgSel == 2) cfgDyn = (cfgDyn + d + 4) % 4;
    else if (cfgSel == 3) cfgNmea = (cfgNmea + d + 2) % 2;
    else gnssApplyConfigItem(4);
    dirty = true; return;
  }
  if (k == '\n') { gnssApplyConfigItem(cfgSel); return; }
  if (k == 's' || k == 'S') { gnssPendingConfigAt = 0; gnssSendPcas("PCAS00"); gnssConfigMessage("saved to GNSS"); return; }
}

void drawGnssConfig() {
  if (!gnssHeader("GNSS CONFIG")) return;
  const char* values[] = { CFG_RATE_NAME[cfgRate], CFG_SYS_NAME[cfgSys], CFG_DYN_NAME[cfgDyn], CFG_NMEA_NAME[cfgNmea], gnssAntennaOn ? "ON" : "OFF" };
  const char* names[] = { "Rate", "System", "Dynamic", "NMEA", "RF antenna" };
  cv.setFont(&fonts::Font0); cv.setTextSize(1);

  for (int i = 0; i < 5; i++) {
    int y = 16 + i * 18;
    bool sel = (i == cfgSel);
    if (sel) {
      cv.fillRoundRect(5, y - 1, SW - 10, 17, 3, CARD_BG);
      cv.drawRoundRect(5, y - 1, SW - 10, 17, 3, DIM_BORDER);
    }
    cv.setTextDatum(middle_left);
    cv.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? CARD_BG : TFT_BLACK);
    cv.drawString(names[i], 12, y + 8);

    // RF antenna 开关绘制微型胶囊指示
    if (i == 4) {
      int swX = SW - 32, swY = y + 3, swW = 20, swH = 10;
      if (gnssAntennaOn) {
        cv.fillRoundRect(swX, swY, swW, swH, 5, ACCENT);
        cv.fillCircle(swX + swW - 5, swY + 5, 4, TFT_BLACK);
      } else {
        cv.fillRoundRect(swX, swY, swW, swH, 5, DIM_BORDER);
        cv.fillCircle(swX + 5, swY + 5, 4, TFT_DARKGREY);
      }
    } else {
      cv.setTextDatum(middle_right);
      cv.setTextColor(sel ? ACCENT : ICON_DIM, sel ? CARD_BG : TFT_BLACK);
      cv.drawString(values[i], SW - 12, y + 8);
    }
  }

  // 临时反馈消息优先显示在中间空余带；无消息时显示按键提示，完全避开底部页码点
  if ((int32_t)(cfgMsgUntil - millis()) > 0) {
    cv.setTextDatum(top_center); cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString(cfgMsg, SW / 2, 116);
  } else {
    cv.setTextDatum(top_center); cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("[-/+] edit   [Enter] apply   [S] save", SW / 2, 116);
  }

  drawPageDots();
}

// 第3页：地图。五档缩放——
//   WORLD  离线世界图（Natural Earth 110m 陆地掩码，worldmap.h，跟时钟晨昏线页共用）
//   z5/z8/z11/z14  在线瓦片（Carto dark_all，免 key），分别大致对应 国家/市域/街区/街道
//
// 为什么高倍要走在线瓦片：那份离线掩码整个地球才 240×109 像素，一个像素约 167km，
// 放大出来纯粹是马赛克方块，连自己在哪个省都看不出来。要"更精细"就只能换数据源。
// 没网（或瓦片拉失败）时自动退回掩码裁剪，角标标 offline，缩放照样能用，只是糊。
//
// ⚠️ 这块缓存是 240x101x2 ≈ 48KB，而这块板子没有 PSRAM、主画布 cv 本身又已经占了 64KB，
// 所以两件事必须做到：
//   1) createSprite() 的返回值一定要判——申请失败时 _img 是空指针，往里 drawPixel 会直接
//      写空地址炸掉。失败就退回"每帧直接画到 cv 上"的老路（慢一点但能用），下次进页面再试。
//   2) 离开 GNSS 要把它还回去（gnssMapExit()，挂在 main.cpp 的 cleanupApp 上），
//      不然逛一次地图页就永久少 48KB，后面开 WiFi/BLE 的 app 可能就申请不到内存了。
//
// 试过把它改成静态数组好给 PNG 解码器腾连续内存——没用：.bss 跟堆是同一块 DRAM，
// 搬过去只是让这 48KB 变成永久占用（连不看地图时也占着），堆池同步缩小 48KB，
// 实测 largestBlock 反而从 24KB 掉到 16KB。已回退。
// 实测（板子上 STAT 读的）：冷启动 largestBlock 有 94KB，48KB 随便申请。但**只要发生过
// 一次 HTTPS 请求**，largestBlock 就永久掉到 41~47KB——总空闲量几乎没变（100KB→98KB），
// 是被打散了。48480 这个需求正好卡在门槛上方，于是"逛过 Sats/Typhoon 之后地图缩放就失灵"。
// 所以这里分两档退：16bpp 拿不到就退 8bpp（24KB，实测那点碎片下也进得去），缩放照样能用，
// 只是卫星影像被量化成 RGB332、会有色带。两档都拿不到才真的没有缩放。
static M5Canvas mapBg(&cv);
static bool mapBgReady = false;
static bool mapBgTried = false;   // 这次进页面已经试过申请了（失败就别每帧再去 malloc 48KB）
static bool mapBgLowColor = false; // 当前这块是 8bpp 降级缓存（角标要标出来）

// ---- 缩放档位 ----
// 0 = 离线世界图；其余是 slippy map 的 zoom 级别。
static const int ZOOMS[] = {0, 5, 8, 11, 14, 16};
static const int ZOOM_COUNT = sizeof(ZOOMS) / sizeof(ZOOMS[0]);
static int zoomIdx = 0;

// mapBg 里现在装的是什么，用来判断要不要重画/重拉
enum BgKind { BG_NONE, BG_WORLD, BG_TILES, BG_MASK_ZOOM };
static BgKind bgKind = BG_NONE;
static int    bgZ = -1;
static double bgOriginX = 0, bgOriginY = 0;   // 瓦片视图：窗口左上角的世界像素坐标
static String mapErr = "";                    // 非空 = 这次没拉到瓦片，退回了离线底图
static bool   jobFromSd = false;              // 当前瓦片是否来源于 SD 卡离线瓦片包

// 尾迹：定位点每隔一段时间记一个历史坐标，越新的画得越亮，能看出移动方向/大致路径，
// 不用太密——15秒一个点，20个点覆盖5分钟，够看出"往哪走"而不会糊成一团。
// 存的是经纬度而不是屏幕坐标：屏幕坐标一缩放/平移就全废了，经纬度是绝对的。
struct TrailPt { float lat, lon; };
static const int TRAIL_MAX = 20;
static TrailPt trail[TRAIL_MAX];
static int trailCount = 0;
static uint32_t lastTrailMs = 0;
static const uint32_t TRAIL_INTERVAL_MS = 15000;

static void trailRecord(double lat, double lon) {
  uint32_t now = millis();
  if (trailCount > 0 && now - lastTrailMs < TRAIL_INTERVAL_MS) return;
  lastTrailMs = now;
  if (trailCount >= TRAIL_MAX) {
    memmove(&trail[0], &trail[1], sizeof(TrailPt) * (TRAIL_MAX - 1));
    trailCount = TRAIL_MAX - 1;
  }
  trail[trailCount].lat = (float)lat;
  trail[trailCount].lon = (float)lon;
  trailCount++;
}

// ---- Web Mercator（slippy map）投影 ----
// 单位是"世界像素"：该 zoom 下整个地球正好 256*2^z 像素见方。瓦片编号 = 世界像素 / 256。
static const double MERC_LAT_LIMIT = 85.05112878;   // 墨卡托在极点发散，标准做法是截到这个纬度

static void latLonToWorldPx(double lat, double lon, int z, double& px, double& py) {
  if (lat >  MERC_LAT_LIMIT) lat =  MERC_LAT_LIMIT;
  if (lat < -MERC_LAT_LIMIT) lat = -MERC_LAT_LIMIT;
  double n = 256.0 * (double)(1 << z);
  px = (lon + 180.0) / 360.0 * n;
  double s = sin(lat * M_PI / 180.0);
  py = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * n;
}

static void worldPxToLatLon(double px, double py, int z, double& lat, double& lon) {
  double n = 256.0 * (double)(1 << z);
  lon = px / n * 360.0 - 180.0;
  double t = M_PI * (1 - 2 * py / n);
  lat = atan(sinh(t)) * 180.0 / M_PI;
}

// ---- WGS-84 -> GCJ-02 ----
// 国内的地图服务（高德/腾讯/百度）用的都是加过偏移的坐标系，而 GPS 给的是 WGS-84，
// 两者差 300~600m。在 z14（10m/px）上就是几十像素，定位点会明显飘到隔壁街去，必须换算。
// 算法是公开的那套（Krasovsky 椭球 + 经验多项式），只在国境内生效，境外原样返回。
static const double GCJ_A  = 6378245.0;              // Krasovsky 1940 长半轴
static const double GCJ_EE = 0.00669342162296594323; // 第一偏心率平方

static bool outOfChina(double lat, double lon) {
  return !(lon > 73.66 && lon < 135.05 && lat > 3.86 && lat < 53.55);
}
static double gcjTransLat(double x, double y) {
  double r = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * sqrt(fabs(x));
  r += (20.0 * sin(6.0 * x * M_PI) + 20.0 * sin(2.0 * x * M_PI)) * 2.0 / 3.0;
  r += (20.0 * sin(y * M_PI) + 40.0 * sin(y / 3.0 * M_PI)) * 2.0 / 3.0;
  r += (160.0 * sin(y / 12.0 * M_PI) + 320.0 * sin(y * M_PI / 30.0)) * 2.0 / 3.0;
  return r;
}
static double gcjTransLon(double x, double y) {
  double r = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * sqrt(fabs(x));
  r += (20.0 * sin(6.0 * x * M_PI) + 20.0 * sin(2.0 * x * M_PI)) * 2.0 / 3.0;
  r += (20.0 * sin(x * M_PI) + 40.0 * sin(x / 3.0 * M_PI)) * 2.0 / 3.0;
  r += (150.0 * sin(x / 12.0 * M_PI) + 300.0 * sin(x / 30.0 * M_PI)) * 2.0 / 3.0;
  return r;
}
void wgs2gcj(double wLat, double wLon, double& gLat, double& gLon) {
  if (outOfChina(wLat, wLon)) { gLat = wLat; gLon = wLon; return; }
  double dLat = gcjTransLat(wLon - 105.0, wLat - 35.0);
  double dLon = gcjTransLon(wLon - 105.0, wLat - 35.0);
  double radLat = wLat / 180.0 * M_PI;
  double magic = sin(radLat);
  magic = 1 - GCJ_EE * magic * magic;
  double sqrtMagic = sqrt(magic);
  dLat = (dLat * 180.0) / ((GCJ_A * (1 - GCJ_EE)) / (magic * sqrtMagic) * M_PI);
  dLon = (dLon * 180.0) / (GCJ_A / sqrtMagic * cos(radLat) * M_PI);
  gLat = wLat + dLat;
  gLon = wLon + dLon;
}

// 自动检测 SD 卡是否存在 OSM / WGS-84 标牌文件（存在则按国际标准 WGS-84 投影，不加偏移）
// ⚠️ 只在进地图后第一次用到时查一遍，之后吃缓存，gnssMapExit() 再作废。原来是每 4 秒重查，
// 而 latLonToTilePx() 是在 drawGnssMap() 里调的——等于每 4 秒在绘制路径里同步打 6 次 SD，
// 慢卡上一次 stat 就是几十毫秒，键盘和 GNSS 串口都得陪着等。
static int  osmCoordCached = -1;
// 本次看地图期间 SD 已判定"不能用"（卡报 ready 但连根目录都 stat 不到——多半是被拔了）。
// 置位后地图页不再碰卡，免得每块瓦片都白吃 4~5 次失败的 stat。gnssMapExit() 清掉。
static bool mapSdBad = false;
static bool mapSdOk() { return sdReady() && !mapSdBad; }

static bool isOsmCoord() {
  if (osmCoordCached < 0) {
    // ⚠️ 只缓存"确定"的结论：卡没就绪、或者探测时卡不响应，都不落缓存，下次再问。
    // 不然一次没插好/没挂上就把整个会话锁死在错误的坐标系上。
    if (!mapSdOk()) return false;
    static const char* const MARKS[] = {"/map/wgs84", "/map/osm", "/tiles/wgs84",
                                        "/tiles/osm", "/map/wgs84.txt", "/map/osm.txt"};
    for (const char* m : MARKS)
      if (SD.exists(m)) { osmCoordCached = 1; return true; }
    if (!SD.exists("/")) { mapSdBad = true; return false; }   // 根目录都没有 = 卡坏/拔了，不算"没有标牌"
    osmCoordCached = 0;
  }
  return osmCoordCached == 1;
}

// GPS 给的 WGS-84 经纬度 -> 瓦片世界像素（若为标准 OSM/WGS84 瓦片则直投，高德/国内源过一道 GCJ-02）
static void latLonToTilePx(double lat, double lon, int z, double& px, double& py) {
  if (isOsmCoord()) {
    latLonToWorldPx(lat, lon, z, px, py);
  } else {
    double gLat, gLon;
    wgs2gcj(lat, lon, gLat, gLon);
    latLonToWorldPx(gLat, gLon, z, px, py);
  }
}

// 米/像素（赤道 156543.03 m/px @ z0，按纬度收窄）
static double metersPerPixel(double lat, int z) {
  return 156543.033928 * cos(lat * M_PI / 180.0) / (double)(1 << z);
}

// ---- 离线底图 ----
// 陆地/海洋 + 经纬网格线画一遍（世界视图的背景，往后不再变）。经线每60°一条、纬线每30°一条
// (赤道单独更亮那条，不算在这批里，免得重复画两次)。
// dst/yOff 是为了两条路都能复用同一份画法：正常走缓存画布(dst=mapBg, yOff=0)，
// 缓存申请不到时直接画到主画布上(dst=cv, yOff=mapTop)。
static void renderMapBackground(M5Canvas& dst, int yOff, int mapH) {
  const uint16_t seaCol  = cv.color565(18, 30, 48);
  const uint16_t landCol = cv.color565(40, 75, 50);

  // 查表法加速世界地图渲染：SW=240 与 WORLD_MASK_W 刚好 1:1，避免 24k 次浮点运算
  for (int y = 0; y < mapH; y++) {
    int row = (int)(y * WORLD_MASK_H / mapH);
    if (row >= WORLD_MASK_H) row = WORLD_MASK_H - 1;
    for (int x = 0; x < SW; x++) {
      dst.drawPixel(x, yOff + y, worldIsLand(x, row) ? landCol : seaCol);
    }
  }
  const uint16_t gridCol = cv.color565(32, 50, 68);
  for (int lon = -120; lon <= 120; lon += 60) {
    int gx = (int)((lon + 180.0) / 360.0 * SW);
    dst.drawFastVLine(gx, yOff, mapH, gridCol);
  }
  for (int lat = -60; lat <= 60; lat += 30) {
    if (lat == 0) continue;
    int gy = (int)((90.0 - lat) / 180.0 * mapH);
    dst.drawFastHLine(0, yOff + gy, SW, gridCol);
  }
  dst.drawFastHLine(0, yOff + mapH / 2, SW, cv.color565(55, 80, 105));   // 赤道参考线
}

// 没网时的降级底图：把那份 110m 掩码按当前窗口裁出来。像素早就不够用了（放大后一格
// 掩码要摊成一大片），所以只能看个海陆轮廓——角标会标 offline，别让人以为是真地图。
static void renderMaskWindow(M5Canvas& dst, int yOff, int mapH, double originX, double originY, int z) {
  const uint16_t seaCol  = cv.color565(18, 32, 46);
  const uint16_t landCol = cv.color565(40, 55, 34);
  // 墨卡托下纬度只跟行有关（atan/sinh 每行算一次），经度对列是线性的——
  // lon = lon0 + x·dLon，逐像素一次乘加就够，不用额外申请行缓冲（原来那块 1.9KB 的 malloc
  // 在碎片化的堆上会失败，失败了只能退回一张跟定位点对不上的世界图）。
  const double n = 256.0 * (double)(1 << z);
  const double lon0 = originX / n * 360.0 - 180.0;
  const double dLon = 360.0 / n;
  for (int y = 0; y < mapH; y++) {
    double lat, lonDummy;
    worldPxToLatLon(originX, originY + y, z, lat, lonDummy);
    for (int x = 0; x < SW; x++) {
      double lon = lon0 + x * dLon;
      if (lon >= 180.0) lon -= 360.0;        // 窗口跨过日界线：掩码只认 ±180，绕回来再查，
      else if (lon < -180.0) lon += 360.0;   // 否则那一截会被夹到边缘列、画成一条拉长的竖带
      dst.drawPixel(x, yOff + y, worldIsLandAtLatLon(lat, lon) ? landCol : seaCol);
    }
  }
}

// ---- 在线瓦片 ----
// 瓦片源：高德**卫星影像**（webst0X，style=6，免 key，返回 JPEG）。
//
// 为什么不用更好看的路网图：路网图（webrd/wprd）只提供 PNG，而 PNG 的 deflate 要一块
// 32KB 连续内存装 LZ77 窗口（格式规定，压不下来）。实机实测：主画布 64KB + 地图缓存 48KB +
// WiFi 协议栈之后，free heap 只剩 ~42KB 且 largestBlock 只有 24KB，pngle 的 32KB 申请必然失败，
// drawPng 直接返回 false（日志里就是 png=0）。换 8bpp 缓存、改静态数组都试过，都不成。
// JPEG 解码器(tjpgd)是按 MCU 块流式解的，工作区只要几 KB，在 16KB 的空洞里都跑得开。
// 顺带一提卫星影像在 z14 的信息量其实比路网图大，只是没有路名。
//
// 原本用的是 Carto dark_all，但实测国内 TCP 443 直接超时
// （PROBE a.basemaps.cartocdn.com:443 -> BLOCKED，tile.openstreetmap.org 同样），
// TLS 握手连不上，一块也拉不下来。高德 webrd0X 实测 8ms 就通。
// ⚠️ 高德用 GCJ-02 坐标，所以所有经纬度进瓦片坐标之前都要过 latLonToTilePx()。
// ⚠️ 高德的图是浅色的，跟这套深色 UI 不搭，解码完会整体做一次灰度反相（见 tintMapDark）。
// 个人设备自用；要发布得按高德的条款署名。
// 窗口 240x101 最多跨 2x2 = 4 块瓦片，逐块拉、逐块解到 mapBg 上。
// PNG 解码用 M5GFX 自带的（drawPng 直接吃内存里的字节），不用另外引库。

// ⚠️ 拉瓦片绝对不能放在 drawGnssMap() 里同步做。render() 是在 loop() 里调的，
// 一次拉图最坏要几十秒（连 Wi-Fi 15s + 每块瓦片 connect/read 超时），期间主循环停在里面，
// 键盘根本轮询不到——表现就是"整机卡死，连返回键都按不动"。
// 所以拆成一个小状态机：每帧最多推进一步（连一次网 / 拉一块瓦片），拉完一块就 dirty 一次，
// 于是瓦片是一块一块浮现出来的，中间键盘照常响应，还能画加载进度。
enum MapJob { JOB_IDLE, JOB_WIFI, JOB_TILE, JOB_DONE, JOB_FALLBACK };
static MapJob jobState = JOB_IDLE;
static int    jobZ = 0, jobMapH = 0;
static int    jobTx0 = 0, jobTx1 = 0, jobTy0 = 0, jobTy1 = 0;
static int    jobIdx = 0, jobTotal = 0, jobOk = 0;
static bool   jobShown = false;   // 进度条已经推上屏了吗（见 gnssMapUpdate 开头的解释）
// 在线瓦片的临时失败（堆太碎被跳过、超时、HTTP 错）记进这个位图，整轮拉完后隔一阵补拉，
// 最多补 MAP_RETRY_MAX 轮。SD 上本来就没有的块不进来——重试也不会凭空长出来。
// 窗口 240x101 最多跨 2x2 块，32 位绰绰有余。
static uint32_t jobRetryMask = 0;
static int      jobRetryRound = 0;
static uint32_t jobRetryAt = 0;
static uint32_t jobRetryFailed = 0;   // 本轮补拉里仍然失败的，下一轮再试
static const int      MAP_RETRY_MAX = 2;
static const uint32_t MAP_RETRY_BACKOFF_MS = 3000;   // 第 n 轮等 n*3s，给 LwIP 的 TIME_WAIT 一点时间还内存

// 从 SD 卡高速读取离线传统地图瓦片 (支持 /map/{z}/{x}/{y}.jpg, /tiles/{z}/{x}/{y}.jpg, /map/{z}/{x}_{y}.jpg, .jpeg)
static bool loadTileFromSd(int z, int tx, int ty, int dx, int dy) {
  if (!mapSdOk()) return false;

  // 候选路径按顺序试，命中一个就停（原来是一串 if(!exists) 改 path，命中后还会把后面的 exists 再跑一遍）
  static const char* const FMTS[] = {
    "/map/%d/%d/%d.jpg",     // 标准 slippy map 目录
    "/tiles/%d/%d/%d.jpg",
    "/map/%d/%d_%d.jpg",     // 扁平化
    "/map/%d/%d/%d.jpeg",
  };
  char path[64];
  bool found = false;
  for (const char* fmt : FMTS) {
    snprintf(path, sizeof(path), fmt, z, tx, ty);
    if (SD.exists(path)) { found = true; break; }
  }
  if (!found) {
    // 全没命中：可能只是没这块，也可能卡被拔了。stat 一下根目录分辨，拔了就本次会话不再碰卡
    if (!SD.exists("/")) { mapSdBad = true; Serial.println("[map] SD not responding, skip SD for this session"); }
    return false;
  }

  File f = SD.open(path, FILE_READ);
  if (!f) { mapSdBad = true; Serial.printf("[map] SD open failed: %s\n", path); return false; }
  bool ok = mapBg.drawJpg((Stream*)&f, dx, dy);
  f.close();
  if (ok && debugOn) {
    Serial.printf("[map] SD tile loaded: %s\n", path);
  }
  return ok;
}

static bool fetchOneTile(int z, int tx, int ty, int dx, int dy) {
  // 每建一条 TCP 连接、解一块 JPEG 都要点堆。实测连拉几轮之后 largestBlock 会掉到 9KB
  // （关掉的连接在 LwIP 里还要 TIME_WAIT 一阵才把内存还回来），太紧的时候宁可这块不画，
  // 也别硬着头皮去撞一次 malloc 失败。流式解码需要的连续内存不多，阈值可以放低。
  size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (big < 6 * 1024) {
    Serial.printf("[map] skip tile %d/%d/%d, largestBlock=%u\n", z, tx, ty, (unsigned)big);
    return false;
  }

  char url[128];
  // 子域按轮次选，不按瓦片选：同一轮里几块图打同一台，省掉重复 DNS，
  // 连接也更容易被 keep-alive 复用（每建一条 TCP，LwIP 的 PCB 加 TIME_WAIT 都要吃几 KB）
  const int sub = (z + jobIdx / 8) % 4 + 1;
  snprintf(url, sizeof(url),
           "http://webst0%d.is.autonavi.com/appmaptile?style=6&x=%d&y=%d&z=%d",
           sub, tx, ty, z);

  // ⚠️ 明文 HTTP 是必须的，不是偷懒：走 https 时 mbedTLS 握手要 40~50KB 连续堆，而这会儿
  // 地图缓存(48KB) + 主画布(64KB) 都占着，实测握手直接失败（start_ssl_client: -1，
  // 而同一台机器 PROBE 443 端口 8ms 就通，所以不是网络问题是内存问题）。
  // 瓦片是公开地图数据、请求里不带任何凭据，为它扛一层 TLS 不值这 40KB。
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(5000);   // 超时都压得比较短：一块瓦片卡住最多也就顿这么久
  http.setTimeout(8000);
  if (!http.begin(client, url)) {
    Serial.printf("[map] tile %d/%d/%d begin failed\n", z, tx, ty);   // 这条以前是静默失败的
    return false;
  }
  http.setUserAgent(HTTP_UA);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[map] tile %d/%d/%d http %d\n", z, tx, ty, code);
    http.end();
    client.stop();
    return false;
  }

  // ⚠️ 直接从 HTTP 流里解码，不要先 malloc 整块压缩数据。
  // 踩过的坑：原来是 malloc(Content-Length) 再 drawJpg(buf,...)，而 z8/z11 的瓦片有 26~30KB，
  // 那时 largestBlock 只剩 11KB → malloc 失败，而且那条分支还没打日志，
  // 于是"两块瓦片只画出一块"，屏幕上就是半张图。
  // tjpgd 是按 MCU 块流式解的，配 StreamWrapper 每次只要几百字节的读缓冲，
  // 连续内存需求从 30KB 直接掉到几 KB。
  const int len = http.getSize();
  bool ok = mapBg.drawJpg(http.getStreamPtr(), dx, dy);
  http.end();
  client.stop(); // 显式释放 TCP 连接资源，避免套接字残留占用内部堆
  if (debugOn)
    Serial.printf("[map] tile %d/%d/%d len=%d jpg=%d heap=%u largest=%u\n",
                  z, tx, ty, len, ok, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  return ok;
}

// 卫星影像或浅色传统地图降亮/去饱和，让底图退到背景里去，定位点、尾迹、比例尺更鲜明。
// 若采样判定已经是暗黑风格地图（暗色模式路网），则跳过压暗，保持原汁原味的高对比。
// 采样判定整幅要不要压暗，结论存进 jobTintOn，补拉的瓦片按同一结论单独处理那一块
static bool jobTintOn = false;
static void tintRect(int x0, int y0, int x1, int y1);
static void tintMapDark() {
  const int h = mapBg.height(), w = mapBg.width();
  // 采样 16 个散列点的平均亮度
  int sampleLum = 0;
  for (int i = 0; i < 16; i++) {
    int sx = (i % 4) * (w / 4) + w / 8;
    int sy = (i / 4) * (h / 4) + h / 8;
    uint16_t c = mapBg.readPixel(sx, sy);
    int r = ((c >> 11) & 0x1f) << 3, g = ((c >> 5) & 0x3f) << 2, b = (c & 0x1f) << 3;
    sampleLum += (r * 77 + g * 151 + b * 28) >> 8;
  }
  sampleLum /= 16;
  jobTintOn = (sampleLum >= 65); // 原图已是深色/暗黑主题，无需二次压暗
  if (jobTintOn) tintRect(0, 0, w, h);
}

static void tintRect(int x0, int y0, int x1, int y1) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > mapBg.width())  x1 = mapBg.width();
  if (y1 > mapBg.height()) y1 = mapBg.height();

  if (mapBgLowColor) {
    // 8bpp (RGB332) 模式：蓝通道只有 2 位，过度脱色会导致断阶，采用 3/4 平滑微压暗
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        uint16_t c = mapBg.readPixel(x, y);
        int r = (((c >> 11) & 0x1f) << 3) * 3 / 4;
        int g = (((c >> 5) & 0x3f) << 2) * 3 / 4;
        int b = ((c & 0x1f) << 3) * 3 / 4;
        mapBg.drawPixel(x, y, mapBg.color565(r, g, b));
      }
    }
  } else {
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        uint16_t c = mapBg.readPixel(x, y);
        int r = ((c >> 11) & 0x1f) << 3, g = ((c >> 5) & 0x3f) << 2, b = (c & 0x1f) << 3;
        int gray = (r * 77 + g * 151 + b * 28) >> 8;
        r = (r + gray * 2) / 3 * 5 / 8;     // 往灰里拉 2/3，再整体压到 5/8 亮度
        g = (g + gray * 2) / 3 * 5 / 8;
        b = (b + gray * 2) / 3 * 5 / 8;
        mapBg.drawPixel(x, y, mapBg.color565(r, g, b));
      }
    }
  }
}

// 开一轮新的拉取（换档、走出屏幕、或第一次进这一档时调）
static void startTileJob(double lat, double lon, int z, int mapH) {
  double cx, cy;
  latLonToTilePx(lat, lon, z, cx, cy);
  bgOriginX = cx - SW / 2.0;
  bgOriginY = cy - mapH / 2.0;
  bgZ = z;
  jobZ = z;
  jobMapH = mapH;

  jobTx0 = (int)floor(bgOriginX / 256.0); jobTx1 = (int)floor((bgOriginX + SW - 1) / 256.0);
  jobTy0 = (int)floor(bgOriginY / 256.0); jobTy1 = (int)floor((bgOriginY + mapH - 1) / 256.0);
  jobTotal = (jobTx1 - jobTx0 + 1) * (jobTy1 - jobTy0 + 1);
  jobIdx = 0;
  jobOk = 0;
  jobShown = false;
  mapErr = "";
  jobFromSd = false;
  jobRetryMask = 0;
  jobRetryRound = 0;
  jobRetryFailed = 0;

  if (debugOn) Serial.printf("[map] job z=%d tiles=%d origin=%.0f,%.0f\n", z, jobTotal, bgOriginX, bgOriginY);
  mapBg.fillSprite(cv.color565(18, 20, 26));   // 还没拉到的地方留个底色，别是花的
  bgKind = BG_TILES;                            // 先按"瓦片视图"算坐标，拉不到再退回掩码
  // 若 SD 卡可用，直接进入 JOB_TILE 执行高速本地瓦片读取；否则才进入 JOB_WIFI 确认联网
  jobState = sdReady() ? JOB_TILE : JOB_WIFI;
  dirty = true;
}

// 退回离线掩码：整幅重画一次，标记成 offline
static void tileJobFallback(const char* why) {
  Serial.printf("[map] fallback: %s\n", why);
  mapErr = why;
  renderMaskWindow(mapBg, 0, jobMapH, bgOriginX, bgOriginY, jobZ);
  bgKind = BG_MASK_ZOOM;
  jobState = JOB_FALLBACK;
  dirty = true;
}

// 第 idx 块瓦片的编号与在 mapBg 上的落点；超出墨卡托南北边界的返回 false
static bool jobTileAt(int idx, int& wrapped, int& ty, int& dx, int& dy) {
  const int cols = jobTx1 - jobTx0 + 1;
  ty = jobTy0 + idx / cols;
  const int tx = jobTx0 + idx % cols;
  const int world = 1 << jobZ;
  if (ty < 0 || ty >= world) return false;
  wrapped = ((tx % world) + world) % world;
  dx = (int)lround(tx * 256.0 - bgOriginX);
  dy = (int)lround(ty * 256.0 - bgOriginY);
  return true;
}

// ⚠️ 卡上是 WGS-84/OSM 瓦片包时不拿高德（GCJ-02）补缺：两套坐标差几百米，
// 拼在一起接缝处道路会错开，还不如留占位框
static bool onlineFallbackAllowed() {
  return WiFi.status() == WL_CONNECTED && !isOsmCoord();
}

// 补拉一块之前失败的在线瓦片。成功就只对这一块做同样的压暗；一轮补完还有失败的，退避后再来
static void retryOneTile() {
  int idx = 0;
  while (idx < 32 && !(jobRetryMask & (1u << idx))) idx++;
  jobRetryMask &= ~(1u << idx);
  int wrapped, ty, dx, dy;
  if (onlineFallbackAllowed() && jobTileAt(idx, wrapped, ty, dx, dy)) {
    if (fetchOneTile(jobZ, wrapped, ty, dx, dy)) {
      if (jobTintOn) tintRect(dx, dy, dx + 256, dy + 256);
      dirty = true;
    } else {
      jobRetryFailed |= (1u << idx);
    }
  }
  gnssPoll();
  if (jobRetryMask == 0) {            // 这一轮补完
    jobRetryRound++;
    if (jobRetryFailed && jobRetryRound < MAP_RETRY_MAX) {
      jobRetryMask = jobRetryFailed;
      jobRetryAt = millis() + MAP_RETRY_BACKOFF_MS * (jobRetryRound + 1);
    }
    jobRetryFailed = 0;
  }
}

// 每帧推进一步。挂在 main.cpp 的 loop() 上（只在地图页调）。
// 没 GPS 时的兜底中心点。⚠️ 必须走 DeferredFetch：geoGet() 在没有 GNSS fix 时会发一次
// IP 定位的 HTTP 请求，而 draw() 是在 loop() 里调的——在绘制路径里同步拉网络会把主循环
// 整个卡住，键盘都轮询不到（README "别在按键回调/绘制路径里同步拉网络" 那条）。
static DeferredFetch posJob;
static double ipLat = 0, ipLon = 0;
static bool   haveIpPos = false;

void gnssMapEnter() { posJob.request(); }

void gnssMapUpdate() {
  // 有卫星定位就不必问 IP；没有才去取一次（geoGet 内部会缓存 IP 结果，不会每次都打接口）
  if (posJob.due() && !gps.location.isValid()) {
    GeoFix f;
    if (geoGet(f)) { ipLat = f.lat; ipLon = f.lon; haveIpPos = true; dirty = true; }
    return;   // geoGet 可能刚在这里阻塞了好几秒 HTTP，这一轮别再接着解瓦片，先回 loop 扫键盘
  }

  // 整轮已拉完、有在线瓦片临时失败：到点了就补一块（每次调用最多一块，跟正常拉取一样）
  if (jobState == JOB_DONE && jobRetryMask && millis() >= jobRetryAt) {
    retryOneTile();
    return;
  }

  if (jobState != JOB_WIFI && jobState != JOB_TILE) return;
  // 等"connecting wifi"/"loading map"那一帧真推上屏了再干活——SD 路径也一样，
  // 不然第一帧还没画出提示，主循环就已经卡在读卡上了，看着像按了没反应
  if (!jobShown) return;

  if (jobState == JOB_WIFI) {
    // wifiEnsureConnected() 现在是纯查询、不阻塞（掉线时不再自己发起 15s 的连接），
    // 所以这一步是瞬时的：没网就立刻退回离线掩码，不会再让整机僵十几秒。
    // 这个状态保留着是为了让"connecting wifi"那一帧有地方落，顺带隔开 WiFi 判断和拉瓦片两件事。
    bool ok = wifiEnsureConnected();
    gnssPoll();   // 顺手消化一下串口攒的 NMEA（下面拉瓦片每块最长 8 秒，中间不能没人管）
    if (!ok) { tileJobFallback("no wifi"); return; }
    jobState = JOB_TILE;
    dirty = true;
    return;
  }

  // 处于 JOB_TILE 状态：优先从 SD 卡读取，SD 没有且有网络才尝试在线补充。
  // ⚠️ 每次调用最多处理一块，SD 也不例外。原来 SD 命中后直接 continue，一帧里把 2x2 四块
  // 全解完（每块还要先试 4 个路径的 SD.exists），慢卡/碎片化的卡上一块就能到几百毫秒，
  // 整轮下来主循环停一两秒：键盘轮询不到，GNSS 串口缓冲也会溢出丢句子。
  // 一块一块来，瓦片照样逐块浮现，块与块之间 loop() 能回去扫键盘、收 NMEA。
  if (jobIdx < jobTotal) {
    const int idx = jobIdx++;
    int wrapped, ty, dx, dy;
    if (jobTileAt(idx, wrapped, ty, dx, dy)) {
      bool ok = false, online = false;
      if (loadTileFromSd(jobZ, wrapped, ty, dx, dy)) {          // 1. 优先 SD 离线瓦片
        ok = true;
        jobFromSd = true;
      } else if (onlineFallbackAllowed()) {                     // 2. SD 没有：在线补
        online = true;
        ok = fetchOneTile(jobZ, wrapped, ty, dx, dy);
      }
      if (ok) jobOk++;
      else {
        if (online && idx < 32) jobRetryMask |= (1u << idx);
        // 缺块占位：描一圈暗框，让人看得出"这块没有"，而不是以为地图就长这样
        mapBg.drawRect(dx, dy, 256, 256, cv.color565(40, 44, 54));
      }
    }
  }
  gnssPoll();   // 顺手消化一下串口攒的 NMEA

  if (jobIdx >= jobTotal) {
    if (jobOk == 0) {
      tileJobFallback(WiFi.status() == WL_CONNECTED ? "tiles failed" : (sdReady() ? "no SD tile" : "no wifi / SD"));
    } else {
      tintMapDark();
      jobState = JOB_DONE;
      jobRetryAt = millis() + MAP_RETRY_BACKOFF_MS;
    }
  }
  dirty = true;
}

// 离开 GNSS（回主菜单/被 BtnA 一键退出）时把地图缓存和尾迹还回去：缓存那 48KB 留着白占内存，
// 尾迹也只对"这一次看地图"有意义。下次进来会重新申请、重新记。
void gnssMapExit() {
  posJob = DeferredFetch();   // 待办作废，免得下次进来立刻又拉一次

  if (mapBgReady) { mapBg.deleteSprite(); mapBgReady = false; }
  mapBgTried = false;
  mapBgLowColor = false;
  trailCount = 0;
  bgKind = BG_NONE;
  mapErr = "";
  jobFromSd = false;
  osmCoordCached = -1;   // 下次进来重新认一次卡上的坐标系标牌（可能换过卡）
  mapSdBad = false;
  jobRetryMask = 0;
  jobState = JOB_IDLE;   // 在跑的拉取任务作废——sprite 都释放了，回调进去就是往空指针上画
  zoomIdx = 0;           // 回 WORLD 档：不然下次进来立刻又开拉，看着像"一进去就卡死"
}

// [ 缩小 / ] 放大（main.cpp 的 SCREEN_GNSS_MAP 按键分支转发过来）
void gnssMapZoom(int dir) {
  int ni = zoomIdx + dir;
  if (ni < 0) ni = 0;
  if (ni >= ZOOM_COUNT) ni = ZOOM_COUNT - 1;
  if (ni == zoomIdx) return;
  zoomIdx = ni;
  bgKind = BG_NONE;    // 换档就作废底图缓存，下一帧重拉/重画
  jobState = JOB_IDLE; // 在跑的那轮是旧档的，接着拉完只是白拉
  dirty = true;
}

// 比例尺：挑一个画出来接近 60px 的整数距离
static void drawScaleBar(int x, int y, double mpp) {
  static const double NICE[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000,
                                20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000};
  double best = NICE[0];
  int bestPx = 0;
  for (double d : NICE) {
    int px = (int)(d / mpp);
    if (px <= 75 && px > bestPx) { bestPx = px; best = d; }
  }
  if (bestPx < 8) return;   // 这个量级下画不出有意义的比例尺，干脆不画

  char b[16];
  if (best >= 1000) snprintf(b, sizeof(b), "%gkm", best / 1000.0);
  else              snprintf(b, sizeof(b), "%gm", best);

  int tw = cv.textWidth(b);
  int boxW = bestPx + tw + 10;
  int boxH = 13;
  int boxY = y - 9;
  // 暗底微胶囊容器，使比例尺在各种背景下清晰独立
  cv.fillRoundRect(x - 3, boxY, boxW, boxH, 2, cv.color565(12, 16, 22));
  cv.drawRoundRect(x - 3, boxY, boxW, boxH, 2, cv.color565(45, 55, 65));

  cv.drawFastHLine(x, y - 2, bestPx, TFT_WHITE);
  cv.drawFastVLine(x, y - 5, 4, TFT_WHITE);
  cv.drawFastVLine(x + bestPx - 1, y - 5, 4, TFT_WHITE);

  cv.setTextDatum(middle_left); cv.setTextSize(1);
  cv.setTextColor(TFT_WHITE, cv.color565(12, 16, 22));
  cv.drawString(b, x + bestPx + 4, y - 2);
}

void drawGnssMap() {
  posJob.markShown();   // ⚠️ 必须第一行，见 net_job.h（这一页有"没数据就提前 return"的分支）
  // 缩放档位塞进顶栏（原来是画在右上角 y=6，跟新顶栏的状态位打架）
  char zb[24];
  {
    int zz = ZOOMS[zoomIdx];
    // ⚠️ 底图缓存没申请到时 z 会被强制回 0（见下面那行 const int z），这里要是照着 zoomIdx
    // 显示，就会出现"标着 z8、画的却是世界图、按 [ ] 只有标签在动"——正是之前那个
    // "缩放失效"看起来像坏了的原因。没缓存就直说没缩放。
    if (mapBgTried && !mapBgReady) snprintf(zb, sizeof(zb), "no zoom: low mem");
    else if (zz == 0)              snprintf(zb, sizeof(zb), "WORLD");
    // 中心来自 IP 定位时标一个 "ip"：地图摆得对不对、准不准，用户有权知道是谁给的坐标
    else snprintf(zb, sizeof(zb), "z%d%s%s%s%s", zz,
                  jobFromSd ? " SD" : "",
                  bgKind == BG_MASK_ZOOM ? " offline" : "", mapBgLowColor ? " 8bit" : "",
                  gps.location.isValid() ? "" : " ip");
  }
  if (!gnssHeader("GNSS map", zb)) return;

  const int mapTop = 22, mapBot = SH - 12;
  const int mapH = mapBot - mapTop;

  if (!mapBgReady && !mapBgTried) {
    mapBgTried = true;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    // 16bpp 需要 240 * mapH * 2 ≈ 49KB。在没有 PSRAM 的 ESP32-S3 上，如果强行分配 49KB，
    // 剩余连续块会骤降到 2.2KB，导致 drawJpg 的 3.9KB 工作区和 HTTP TCP 缓冲必定失败 (tiles failed)。
    // 只有当最大连续块 >= 75KB 时才分配 16bpp，否则一律使用 8bpp（只需 24.7KB，净省 25KB 连续堆）
    bool try16 = (largest >= 75 * 1024);
    if (try16) {
      mapBg.setColorDepth(16);
      if (mapBg.createSprite(SW, mapH)) {
        mapBgReady = true; mapBgLowColor = false;
      }
    }
    if (!mapBgReady) {
      mapBg.setColorDepth(8);
      if (mapBg.createSprite(SW, mapH)) {
        mapBgReady = true; mapBgLowColor = true;
      }
    }
  }

  // ⚠️ 刻意分成两个概念，别合并：
  //   haveGps    真有卫星定位。**只有它**才配画尾迹和"你在这"的标记——IP 定位是个城市级
  //              的点（误差几公里），画成当前位置是撒谎，连成尾迹更是。
  //   haveCenter 只是"知道该把地图摆在哪"。这件事 IP 定位就够用了，于是没锁定/在室内
  //              也能看地图，而不是只能盯着一张世界图。
  const bool haveGps = gps.location.isValid();
  double lat = 0, lon = 0;
  bool haveCenter = false;
  if (haveGps)        { lat = gps.location.lat(); lon = gps.location.lng(); haveCenter = true; }
  else if (haveIpPos) { lat = ipLat; lon = ipLon;                            haveCenter = true; }
  const int z = (haveCenter && zoomIdx > 0 && mapBgReady) ? ZOOMS[zoomIdx] : 0;

  // ---- 底图 ----
  if (!mapBgReady) {
    renderMapBackground(cv, mapTop, mapH);       // 缓存申请不到：老路子，每帧现算现画世界图
    bgKind = BG_NONE;
  } else if (z == 0) {
    if (bgKind != BG_WORLD) { renderMapBackground(mapBg, 0, mapH); bgKind = BG_WORLD; bgZ = 0; mapErr = ""; }
    mapBg.pushSprite(0, mapTop);
  } else {
    // 走远了（超过约四分之一屏）或换了档才重拉，不然 GPS 每秒抖一下就要重来一轮网络请求。
    // 拉取本身交给 gnssMapUpdate() 分帧做，这里只负责"该不该开一轮"和把现状推上屏。
    if (jobState == JOB_IDLE || jobState == JOB_DONE || jobState == JOB_FALLBACK) {
      double cx, cy;
      latLonToTilePx(lat, lon, z, cx, cy);
      bool stale = (bgKind == BG_NONE) || (bgZ != z) ||
                   fabs(cx - (bgOriginX + SW / 2.0)) > SW / 4.0 ||
                   fabs(cy - (bgOriginY + mapH / 2.0)) > mapH / 4.0;
      // 已经退回离线掩码了就别再自动重试，不然每次 GPS 飘一下又是一轮失败的网络请求；
      // 想重试按 [ ] 换档（换档一定会重开一轮）
      if (stale && !(bgKind == BG_MASK_ZOOM && bgZ == z)) startTileJob(lat, lon, z, mapH);
    }
    mapBg.pushSprite(0, mapTop);
  }

  // ---- 叠加：尾迹 + 当前位置 ----
  if (haveGps) {
    trailRecord(lat, lon);

    // 把一个经纬度投到当前视图的屏幕坐标；返回 false = 落在窗口外
    auto project = [&](double la, double lo, int& sx, int& sy) -> bool {
      if (z == 0) {
        sx = (int)((lo + 180.0) / 360.0 * SW);
        sy = mapTop + (int)((90.0 - la) / 180.0 * mapH);
      } else {
        double wx, wy;
        latLonToTilePx(la, lo, z, wx, wy);
        sx = (int)lround(wx - bgOriginX);
        sy = mapTop + (int)lround(wy - bgOriginY);
      }
      return sx >= 0 && sx < SW && sy >= mapTop && sy < mapBot;
    };

    int px = -1, py = -1;
    for (int i = 0; i < trailCount; i++) {
      int tx, ty;
      if (!project(trail[i].lat, trail[i].lon, tx, ty)) { px = -1; continue; }
      uint8_t v = 60 + (uint8_t)(160 * i / (trailCount > 1 ? trailCount - 1 : 1));   // 越新越亮
      uint16_t col = cv.color565(v, v / 2, 0);
      // 放大之后相邻两点隔得远，只点一个点看不出是条路径，连起来
      if (z > 0 && px >= 0) cv.drawLine(px, py, tx, ty, col);
      cv.fillCircle(tx, ty, 1, col);
      px = tx; py = ty;
    }

    int mx, my;
    project(lat, lon, mx, my);
    if (mx < 0) mx = 0; else if (mx >= SW) mx = SW - 1;
    if (my < mapTop) my = mapTop; else if (my >= mapBot) my = mapBot - 1;

    // 定位光标：如果有速度且航向有效，画指向前进方向的航向箭头；否则画战术瞄准光标与红心
    if (gnssCourseTrusted() && gps.speed.kmph() > 2.0f) {
      float rad = (float)(gps.course.deg() * M_PI / 180.0f);
      float dx = sinf(rad), dy = -cosf(rad);
      float px = -dy, py = dx;
      int tipX = mx + (int)(dx * 8), tipY = my + (int)(dy * 8);
      int leftX = mx - (int)(dx * 5) + (int)(px * 5), leftY = my - (int)(dy * 5) + (int)(py * 5);
      int rightX = mx - (int)(dx * 5) - (int)(px * 5), rightY = my - (int)(dy * 5) - (int)(py * 5);
      cv.fillTriangle(tipX, tipY, leftX, leftY, rightX, rightY, TFT_RED);
      cv.drawTriangle(tipX, tipY, leftX, leftY, rightX, rightY, TFT_WHITE);
      cv.fillCircle(mx, my, 2, TFT_WHITE);
    } else {
      cv.drawCircle(mx, my, 8, cv.color565(140, 30, 30));
      cv.drawFastHLine(mx - 8, my, 3, TFT_WHITE);
      cv.drawFastHLine(mx + 6, my, 3, TFT_WHITE);
      cv.drawFastVLine(mx, my - 8, 3, TFT_WHITE);
      cv.drawFastVLine(mx, my + 6, 3, TFT_WHITE);
      cv.fillCircle(mx, my, 3, TFT_RED);
      cv.drawCircle(mx, my, 4, TFT_WHITE);
    }
  } else {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(middle_center); cv.setTextSize(1);
    cv.drawString("waiting for fix", SW / 2, mapTop + mapH / 2);
  }

  // ---- 加载进度：一块瓦片一块瓦片地涨，让人知道是在干活不是死了 ----
  if (jobState == JOB_WIFI || jobState == JOB_TILE) {
    const int bw = 132, bh = 8, bx = (SW - bw) / 2, by = mapBot - 26;
    int pct = (jobState == JOB_WIFI || jobTotal <= 0) ? 0 : jobIdx * 100 / jobTotal;
    cv.fillRoundRect(bx - 4, by - 15, bw + 8, bh + 21, 4, TFT_BLACK);
    cv.drawRoundRect(bx - 4, by - 15, bw + 8, bh + 21, 4, DIM_BORDER);
    cv.setTextDatum(top_center); cv.setTextSize(1);
    cv.setTextColor(ACCENT, TFT_BLACK);
    char lb[32];
    if (jobState == JOB_WIFI) snprintf(lb, sizeof(lb), "connecting wifi...");
    else                      snprintf(lb, sizeof(lb), "loading map  %d%%  (%d/%d)", pct, jobIdx, jobTotal);
    cv.drawString(lb, SW / 2, by - 12);
    cv.drawRoundRect(bx, by, bw, bh, 2, DIM_BORDER);
    int fw = (bw - 2) * pct / 100;
    if (fw > 0) cv.fillRoundRect(bx + 1, by + 1, fw, bh - 2, 2, ACCENT);
    jobShown = true;
  }

  // ---- 角标：拉图失败的原因 + 比例尺 ----
  // 档位已经在顶栏里了；这里只在退回离线底图时补一句原因，摆在顶栏和地图之间那条空隙
  if (mapErr.length() && bgKind == BG_MASK_ZOOM) {
    cv.setTextDatum(top_right); cv.setTextSize(1);
    cv.setTextColor(TFT_ORANGE, TFT_BLACK);
    cv.drawString(mapErr, SW - 6, GNSS_HDR_BOTTOM + 2);
  }
  if (z > 0 && haveCenter) drawScaleBar(6, mapBot - 6, metersPerPixel(lat, z));

  if (millis() - gnssLastByteMs > 3000) {
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.setTextDatum(bottom_center); cv.setTextSize(1);
    char nb[24];
    snprintf(nb, sizeof(nb), "no GPS data %lus", (unsigned long)((millis() - gnssLastByteMs) / 1000));
    cv.drawString(nb, SW / 2, SH - 2);
  } else {
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
    cv.setTextDatum(bottom_left); cv.setTextSize(1);
    cv.drawString("[ ] zoom", 4, SH - 1);
    drawPageDots();

    // 右下角显示当前经纬度坐标（GPS 定位为青色，IP 粗定位为暗黄色）
    if (haveCenter) {
      char posBuf[24];
      snprintf(posBuf, sizeof(posBuf), "%.2f,%.2f%s", lat, lon, haveGps ? "" : " ip");
      cv.setTextDatum(bottom_right); cv.setTextSize(1);
      cv.setTextColor(haveGps ? ACCENT : cv.color565(210, 170, 60), TFT_BLACK);
      cv.drawString(posBuf, SW - 4, SH - 1);
    }
  }
}

// 第4页：速度表。支持两大模式：
// 1. 220° 运动战斗机机械表盘：外圈刻度环、红区(Redline)警示、动态荧光速度弧、极速记忆幽灵刻度(Peak Marker)、
//    刀锋高亮实心指针、复合金属轴心、低速0.1精度数码读数、双侧HUD遥测翼展。
// 2. 航空数字化 HUD 抬头显示：顶部360°滚动罗盘标尺带(Lubber Line)、超大号数字速度、线性加减速动能条(G-Force)、
//    底部四联遥测卡片。
// 支持 Space/m 键一键切换表盘模式，u 键切换速度单位(km/h, mph, kt, m/s)，r 键清零极速与里程。

static bool speedHudMode = false;
static uint8_t speedUnitIdx = 0; // 0=km/h, 1=mph, 2=kt, 3=m/s
static const char* SPEED_UNITS[] = { "km/h", "mph", "kt", "m/s" };
static const float SPEED_FACTORS[] = { 1.0f, 0.621371f, 0.539957f, 1.0f / 3.6f };

// 实时加减速与动能推算 (低通滤波 g 值)
static float speedPrevKmph = 0.0f;
static uint32_t speedPrevMs = 0;
static float speedSmoothG = 0.0f;

void gnssSpeedKey(char k) {
  if (k == 'r' || k == 'R') {
    gnssTripResetStats();
    dirty = true;
  } else if (k == ' ' || k == 'm' || k == 'M') {
    speedHudMode = !speedHudMode;
    dirty = true;
  } else if (k == 'u' || k == 'U') {
    speedUnitIdx = (speedUnitIdx + 1) % 4;
    dirty = true;
  }
}

void drawGnssSpeed() {
  if (!gnssHeader("GNSS SPEED")) return;

  float speedKmh = gnssSpeedKmph();
  float maxSpdKmh = gnssTripGetMaxSpeed();

  // 平滑推算加速度 G 值 (Δv / Δt)
  uint32_t nowMs = millis();
  if (speedPrevMs > 0 && nowMs > speedPrevMs) {
    float dt = (nowMs - speedPrevMs) / 1000.0f;
    if (dt >= 0.2f) {
      float dv = (speedKmh - speedPrevKmph) / 3.6f; // m/s
      float rawG = (dv / dt) / 9.80665f;
      speedSmoothG = speedSmoothG * 0.6f + rawG * 0.4f;
      speedPrevKmph = speedKmh;
      speedPrevMs = nowMs;
    }
  } else {
    speedPrevMs = nowMs;
    speedPrevKmph = speedKmh;
  }

  // 多单位换算
  float unitFactor = SPEED_FACTORS[speedUnitIdx];
  float userSpeed = speedKmh * unitFactor;
  float userMaxSpd = maxSpdKmh * unitFactor;

  // 自适应动态量程档位 (根据当前单位换算)
  static float currentScaleKmh = 120.0f;
  if (speedKmh > 360.0f) {
    currentScaleKmh = 1000.0f;
  } else if (speedKmh > 100.0f) {
    if (currentScaleKmh < 360.0f) currentScaleKmh = 360.0f;
    else if (currentScaleKmh > 360.0f && speedKmh < 300.0f) currentScaleKmh = 360.0f;
  } else if (speedKmh < 50.0f) {
    currentScaleKmh = 120.0f;
  } else if (currentScaleKmh > 360.0f) {
    currentScaleKmh = 360.0f;
  }

  float userScale = 120.0f * unitFactor;
  if (currentScaleKmh >= 1000.0f) {
    if (speedUnitIdx == 1) userScale = 600.0f;
    else if (speedUnitIdx == 2) userScale = 540.0f;
    else if (speedUnitIdx == 3) userScale = 280.0f;
    else userScale = 1000.0f;
  } else if (currentScaleKmh >= 360.0f) {
    if (speedUnitIdx == 1) userScale = 220.0f;
    else if (speedUnitIdx == 2) userScale = 190.0f;
    else if (speedUnitIdx == 3) userScale = 100.0f;
    else userScale = 360.0f;
  } else {
    if (speedUnitIdx == 1) userScale = 75.0f;
    else if (speedUnitIdx == 2) userScale = 65.0f;
    else if (speedUnitIdx == 3) userScale = 35.0f;
    else userScale = 120.0f;
  }
  char midLabel[16], maxLabel[16];
  snprintf(midLabel, sizeof(midLabel), "%.0f", userScale * 0.5f);
  snprintf(maxLabel, sizeof(maxLabel), "%.0f", userScale);

  // 格式化当前速度：低速(<10)带一位小数，高速显示大整数
  char spdBuf[16];
  if (userSpeed < 9.95f) {
    snprintf(spdBuf, sizeof(spdBuf), "%.1f", userSpeed);
  } else {
    snprintf(spdBuf, sizeof(spdBuf), "%.0f", userSpeed);
  }

  char b[32];

  if (!speedHudMode) {
    // =========================================================================
    // MODE A: 220° 运动战斗机机械表盘
    // =========================================================================
    const int cx = 120, cy = 68, R = 44;

    // 1. 220° 刻度盘底轨
    for (int deg = -20; deg <= 200; deg += 4) {
      float rad = deg * (float)M_PI / 180.0f;
      int x = cx + (int)(cosf(rad) * R);
      int y = cy - (int)(sinf(rad) * R);
      cv.drawPixel(x, y, cv.color565(18, 36, 28));
    }

    // 2. 红区 (Redline Zone: 80%~100% 极速外圈警戒线)
    for (int deg = -20; deg <= 24; deg += 3) {
      float rad = deg * (float)M_PI / 180.0f;
      int x = cx + (int)(cosf(rad) * (R + 2));
      int y = cy - (int)(sinf(rad) * (R + 2));
      cv.drawPixel(x, y, TFT_RED);
    }

    // 3. 刻度线 (10 大段 + 10 小段)
    for (int i = 0; i <= 10; i++) {
      float rRatio = i / 10.0f;
      float rad = (200.0f - rRatio * 220.0f) * (float)M_PI / 180.0f;
      float c = cosf(rad), s = sinf(rad);
      bool redZone = (rRatio >= 0.8f);
      int r1 = R - 6, r2 = R - 1;
      cv.drawLine(cx + (int)(c * r1), cy - (int)(s * r1),
                  cx + (int)(c * r2), cy - (int)(s * r2),
                  redZone ? TFT_RED : cv.color565(180, 200, 195));

      if (i < 10) {
        float mRatio = (i + 0.5f) / 10.0f;
        float mRad = (200.0f - mRatio * 220.0f) * (float)M_PI / 180.0f;
        float mc = cosf(mRad), ms = sinf(mRad);
        bool mRed = (mRatio >= 0.8f);
        cv.drawLine(cx + (int)(mc * (R - 3)), cy - (int)(ms * (R - 3)),
                    cx + (int)(mc * (R - 1)), cy - (int)(ms * (R - 1)),
                    mRed ? TFT_RED : DIM_BORDER);
      }
    }

    // 刻度数值标签：0 (左下), 中点 (顶部), 最大值 (右下)
    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.setTextDatum(middle_right);  cv.drawString("0", 73, 83);
    cv.setTextDatum(bottom_center); cv.drawString(midLabel, cx, cy - R - 1);
    cv.setTextDatum(middle_left);   cv.drawString(maxLabel, 167, 83);

    // 4. 动态发光速度弧 (Glowing Speed Arc)
    float curRatio = constrain(userSpeed / userScale, 0.0f, 1.0f);
    if (curRatio > 0.005f) {
      float endDeg = 200.0f - curRatio * 220.0f;
      for (float deg = 200.0f; deg >= endDeg; deg -= 1.5f) {
        float rad = deg * (float)M_PI / 180.0f;
        float ratio = (200.0f - deg) / 220.0f;
        uint16_t col = (ratio < 0.6f) ? ACCENT : ((ratio < 0.8f) ? TFT_YELLOW : TFT_RED);
        float c = cosf(rad), s = sinf(rad);
        for (int dr = 1; dr <= 3; dr++) {
          cv.drawPixel(cx + (int)(c * (R - dr)), cy - (int)(s * (R - dr)), col);
        }
      }
    }

    // 5. 极速记忆幽灵刻度 (Peak Marker)
    if (userMaxSpd > 0.5f) {
      float pRatio = constrain(userMaxSpd / userScale, 0.0f, 1.0f);
      float pRad = (200.0f - pRatio * 220.0f) * (float)M_PI / 180.0f;
      float pc = cosf(pRad), ps = sinf(pRad);
      cv.drawLine(cx + (int)(pc * (R + 1)), cy - (int)(ps * (R + 1)),
                  cx + (int)(pc * (R + 5)), cy - (int)(ps * (R + 5)),
                  cv.color565(255, 140, 0));
    }

    // 6. 刀锋指针 (Tapered Blade Needle)
    float nRad = (200.0f - curRatio * 220.0f) * (float)M_PI / 180.0f;
    float dirX = cosf(nRad), dirY = -sinf(nRad);
    float perpX = -dirY, perpY = dirX;
    int tipX = cx + (int)(dirX * (R - 8)), tipY = cy + (int)(dirY * (R - 8));
    int bx1 = cx + (int)(dirX * 6 + perpX * 2.5f), by1 = cy + (int)(dirY * 6 + perpY * 2.5f);
    int bx2 = cx + (int)(dirX * 6 - perpX * 2.5f), by2 = cy + (int)(dirY * 6 - perpY * 2.5f);
    cv.fillTriangle(tipX, tipY, bx1, by1, bx2, by2, 0xF9E7); // 荧光橙红
    cv.drawLine(cx + (int)(dirX * 6), cy + (int)(dirY * 6), tipX, tipY, TFT_WHITE); // 脊线反光

    // 7. 金属复合轴心 (Multi-Ring Hub)
    cv.fillCircle(cx, cy, 7, cv.color565(20, 32, 28));
    cv.drawCircle(cx, cy, 7, DIM_BORDER);
    cv.fillCircle(cx, cy, 5, cv.color565(10, 16, 14));
    cv.fillCircle(cx, cy, 2, TFT_RED);
    cv.drawPixel(cx, cy, TFT_WHITE);

    // 8. 核心数字读数与单位
    cv.setFont(&fonts::Font4);
    cv.setTextDatum(middle_center);
    cv.setTextColor(ACCENT, TFT_BLACK);
    cv.drawString(spdBuf, cx, cy + 24);

    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextDatum(top_center);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString(SPEED_UNITS[speedUnitIdx], cx, cy + 37);

    // 9. 左右 HUD 侧卡片
    const int cardW = 54, cardH = 94;
    const int leftX = 4, rightX = SW - cardW - 4, cardY = 18;

    // 左卡：ALT / MAX / TRIP
    cv.fillRoundRect(leftX, cardY, cardW, cardH, 3, CARD_BG);
    cv.drawRoundRect(leftX, cardY, cardW, cardH, 3, DIM_BORDER);

    // ALT
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("ALT", leftX + 4, cardY + 4);
    cv.setTextDatum(top_right);
    if (gps.altitude.isValid()) snprintf(b, sizeof(b), "%.0fm", gps.altitude.meters());
    else snprintf(b, sizeof(b), "--");
    cv.setTextColor(TFT_WHITE, CARD_BG);
    cv.drawString(b, leftX + cardW - 4, cardY + 14);

    cv.drawFastHLine(leftX + 3, cardY + 28, cardW - 6, DIM_BORDER);

    // MAX
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("MAX", leftX + 4, cardY + 33);
    cv.setTextDatum(top_right);
    snprintf(b, sizeof(b), (userMaxSpd < 9.95f) ? "%.1f" : "%.0f", userMaxSpd);
    cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
    cv.drawString(b, leftX + cardW - 4, cardY + 43);
    cv.setTextDatum(top_right);
    cv.setTextColor(DIM_BORDER, CARD_BG);
    cv.drawString(SPEED_UNITS[speedUnitIdx], leftX + cardW - 4, cardY + 53);

    cv.drawFastHLine(leftX + 3, cardY + 63, cardW - 6, DIM_BORDER);

    // TRIP
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("TRIP", leftX + 4, cardY + 68);
    cv.setTextDatum(top_right);
    if (gnssTripDistMeters >= 1000.0) snprintf(b, sizeof(b), "%.1fk", gnssTripDistMeters / 1000.0);
    else snprintf(b, sizeof(b), "%.0fm", gnssTripDistMeters);
    cv.setTextColor(ACCENT, CARD_BG);
    cv.drawString(b, leftX + cardW - 4, cardY + 77);
    cv.setTextDatum(top_right);
    cv.setTextColor(DIM_BORDER, CARD_BG);
    cv.drawString("DIST", leftX + cardW - 4, cardY + 87);

    // 右卡：CRS / SATS / HDOP
    cv.fillRoundRect(rightX, cardY, cardW, cardH, 3, CARD_BG);
    cv.drawRoundRect(rightX, cardY, cardW, cardH, 3, DIM_BORDER);

    // CRS
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("CRS", rightX + 4, cardY + 4);
    cv.setTextDatum(top_right);
    if (gnssCourseTrusted()) {
      float deg = (float)gps.course.deg();
      int cIdx = (int)((deg + 22.5f) / 45.0f) % 8;
      if (cIdx < 0) cIdx += 8;
      snprintf(b, sizeof(b), "%s %.0f", CARD8[cIdx], deg);
      cv.setTextColor(TFT_WHITE, CARD_BG);
    } else {
      snprintf(b, sizeof(b), "---");
      cv.setTextColor(TFT_DARKGREY, CARD_BG);
    }
    cv.drawString(b, rightX + cardW - 4, cardY + 14);

    cv.drawFastHLine(rightX + 3, cardY + 28, cardW - 6, DIM_BORDER);

    // SATS
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("SATS", rightX + 4, cardY + 33);
    cv.setTextDatum(top_right);
    int uSats = gnssUsedSatCount();
    int vSats = satCount ? satCount : (gps.satellites.isValid() ? (int)gps.satellites.value() : 0);
    snprintf(b, sizeof(b), "%du/%dv", uSats, vSats);
    cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
    cv.drawString(b, rightX + cardW - 4, cardY + 43);

    cv.drawFastHLine(rightX + 3, cardY + 63, cardW - 6, DIM_BORDER);

    // HDOP
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("HDOP", rightX + 4, cardY + 68);
    cv.setTextDatum(top_right);
    float hdop = (gnssHdop >= 0) ? gnssHdop : (gps.hdop.isValid() ? (float)gps.hdop.hdop() : -1.0f);
    if (hdop < 0) snprintf(b, sizeof(b), "--");
    else snprintf(b, sizeof(b), "%.1f", hdop);
    uint16_t hcol = hdop < 0 ? TFT_DARKGREY : (hdop < 2.0f ? ACCENT : (hdop < 5.0f ? TFT_YELLOW : TFT_ORANGE));
    cv.setTextColor(hcol, CARD_BG);
    cv.drawString(b, rightX + cardW - 4, cardY + 77);
    cv.setTextDatum(top_right);
    cv.setTextColor(hcol, CARD_BG);
    cv.drawString(hdop < 0 ? "--" : (hdop < 2.0f ? "GOOD" : (hdop < 5.0f ? "FAIR" : "POOR")), rightX + cardW - 4, cardY + 87);
  } else {
    // =========================================================================
    // MODE B: 航空数字化 HUD 抬头显示
    // =========================================================================
    const int cx = 120;

    // 1. 顶部水平罗盘标尺带 (Compass Tape Ribbon)
    const int tapeX = 8, tapeY = 18, tapeW = 224, tapeH = 20;
    cv.fillRoundRect(tapeX, tapeY, tapeW, tapeH, 3, CARD_BG);
    cv.drawRoundRect(tapeX, tapeY, tapeW, tapeH, 3, DIM_BORDER);

    float curHdg = gnssCourseTrusted() ? (float)gps.course.deg() : 0.0f;
    bool moving = gnssCourseTrusted();

    for (int deg = 0; deg < 360; deg += 15) {
      float diff = (float)deg - curHdg;
      while (diff < -180.0f) diff += 360.0f;
      while (diff > 180.0f)  diff -= 360.0f;

      if (diff >= -60.0f && diff <= 60.0f) {
        int tx = cx + (int)(diff * 1.7f);
        if (tx >= tapeX + 4 && tx <= tapeX + tapeW - 4) {
          bool major = (deg % 30 == 0);
          int ty1 = tapeY + 1, ty2 = tapeY + (major ? 6 : 4);
          cv.drawLine(tx, ty1, tx, ty2, major ? TFT_WHITE : DIM_BORDER);

          if (major && (tx < cx - 18 || tx > cx + 18)) {
            cv.setFont(&fonts::Font0); cv.setTextSize(1);
            cv.setTextDatum(bottom_center);
            cv.setTextColor(ICON_DIM, CARD_BG);
            const char* cName = (deg == 0) ? "N" : ((deg == 90) ? "E" : ((deg == 180) ? "S" : ((deg == 270) ? "W" : "")));
            if (cName[0]) {
              cv.setTextColor(ACCENT, CARD_BG);
              cv.drawString(cName, tx, tapeY + tapeH - 2);
            } else {
              char db[8]; snprintf(db, sizeof(db), "%03d", deg);
              cv.drawString(db, tx, tapeY + tapeH - 2);
            }
          }
        }
      }
    }

    // 中心航向瞄准窗
    cv.fillRoundRect(cx - 18, tapeY + 1, 36, tapeH - 2, 2, cv.color565(8, 28, 20));
    cv.drawRoundRect(cx - 18, tapeY + 1, 36, tapeH - 2, 2, ACCENT);
    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextDatum(middle_center);
    if (moving) {
      snprintf(b, sizeof(b), "%03.0f", curHdg);
      cv.setTextColor(ACCENT, cv.color565(8, 28, 20));
    } else {
      snprintf(b, sizeof(b), "---");
      cv.setTextColor(TFT_DARKGREY, cv.color565(8, 28, 20));
    }
    cv.drawString(b, cx, tapeY + tapeH / 2);

    // 2. 超大号数显速度
    cv.setFont(&fonts::Font7);
    cv.setTextSize(0.72, 0.78);
    cv.setTextDatum(middle_center);
    cv.setTextColor(ACCENT, TFT_BLACK);
    cv.drawString(spdBuf, cx - 16, 56);

    // 速度右侧信息
    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextDatum(top_left);
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.drawString(SPEED_UNITS[speedUnitIdx], cx + 24, 46);

    snprintf(b, sizeof(b), "TOP %.0f", userMaxSpd);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString(b, cx + 24, 58);

    // 3. 线性加速度/制动条 (Accel / Decel Bar)
    const int barX = 28, barY = 74, barW = 184, barH = 8;
    cv.drawRoundRect(barX, barY, barW, barH, 2, DIM_BORDER);
    cv.fillRoundRect(barX + 1, barY + 1, barW - 2, barH - 2, 2, cv.color565(4, 14, 10));
    cv.drawFastVLine(cx, barY, barH, TFT_WHITE); // 0g 基准线

    cv.setFont(&fonts::Font0); cv.setTextSize(1);
    cv.setTextDatum(middle_right); cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("DEC", barX - 4, barY + barH / 2);
    cv.setTextDatum(middle_left); cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString("ACC", barX + barW + 4, barY + barH / 2);

    float gClamp = constrain(speedSmoothG, -0.5f, 0.5f);
    int barLen = (int)(fabsf(gClamp) / 0.5f * (barW / 2 - 4));
    if (gClamp > 0.02f) {
      cv.fillRect(cx + 1, barY + 2, barLen, barH - 4, ACCENT);
    } else if (gClamp < -0.02f) {
      cv.fillRect(cx - barLen, barY + 2, barLen, barH - 4, TFT_RED);
    }

    // 4. 底部 4 联遥测胶囊卡片
    const int capY = 88, capH = 30, capW = 54;
    const int xs[4] = { 4, 63, 122, 181 };

    for (int i = 0; i < 4; i++) {
      cv.fillRoundRect(xs[i], capY, capW, capH, 3, CARD_BG);
      cv.drawRoundRect(xs[i], capY, capW, capH, 3, DIM_BORDER);
    }

    // 卡 1: ALT
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("ALT", xs[0] + 4, capY + 3);
    cv.setTextDatum(bottom_right);
    if (gps.altitude.isValid()) snprintf(b, sizeof(b), "%.0fm", gps.altitude.meters()); else snprintf(b, sizeof(b), "--");
    cv.setTextColor(TFT_WHITE, CARD_BG);
    cv.drawString(b, xs[0] + capW - 4, capY + capH - 3);

    // 卡 2: MAX
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("MAX", xs[1] + 4, capY + 3);
    cv.setTextDatum(bottom_right);
    snprintf(b, sizeof(b), (userMaxSpd < 9.95f) ? "%.1f" : "%.0f", userMaxSpd);
    cv.setTextColor(TFT_YELLOW, CARD_BG);
    cv.drawString(b, xs[1] + capW - 4, capY + capH - 3);

    // 卡 3: TRIP
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("TRIP", xs[2] + 4, capY + 3);
    cv.setTextDatum(bottom_right);
    if (gnssTripDistMeters >= 1000.0) snprintf(b, sizeof(b), "%.1fk", gnssTripDistMeters / 1000.0);
    else snprintf(b, sizeof(b), "%.0fm", gnssTripDistMeters);
    cv.setTextColor(ACCENT, CARD_BG);
    cv.drawString(b, xs[2] + capW - 4, capY + capH - 3);

    // 卡 4: SATS & HDOP
    cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("SAT", xs[3] + 4, capY + 3);
    int uSats = gnssUsedSatCount();
    snprintf(b, sizeof(b), "%du", uSats);
    cv.setTextDatum(top_right); cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
    cv.drawString(b, xs[3] + capW - 4, capY + 3);

    cv.setTextDatum(bottom_left); cv.setTextColor(ICON_DIM, CARD_BG);
    cv.drawString("HDOP", xs[3] + 4, capY + capH - 3);
    float hdop = (gnssHdop >= 0) ? gnssHdop : (gps.hdop.isValid() ? (float)gps.hdop.hdop() : -1.0f);
    uint16_t hcol = hdop < 0 ? TFT_DARKGREY : (hdop < 2.0f ? ACCENT : (hdop < 5.0f ? TFT_YELLOW : TFT_ORANGE));
    cv.setTextDatum(bottom_right); cv.setTextColor(hcol, CARD_BG);
    if (hdop >= 0) snprintf(b, sizeof(b), "%.1f", hdop); else snprintf(b, sizeof(b), "--");
    cv.drawString(b, xs[3] + capW - 4, capY + capH - 3);
  }

  // 6. 底部提示行与页码点
  cv.setFont(&fonts::Font0); cv.setTextSize(1);
  cv.setTextDatum(bottom_left);
  cv.setTextColor(DIM_BORDER, TFT_BLACK);
  cv.drawString(speedHudMode ? "Spc:Dial u/r" : "Spc:HUD u/r", 6, SH - 2);

  cv.setTextDatum(bottom_right);
  if (gps.time.isValid()) {
    snprintf(b, sizeof(b), "UTC %02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
    cv.setTextColor(ICON_DIM, TFT_BLACK);
  } else {
    snprintf(b, sizeof(b), "UTC --:--:--");
    cv.setTextColor(TFT_DARKGREY, TFT_BLACK);
  }
  cv.drawString(b, SW - 6, SH - 2);

  drawPageDots();
}

// 行程与诊断页：TTFF、重定位耗时、里程（跳点过滤）、时长、均速/极速、极值海拔、SNR 波形诊断
void drawGnssTrip() {
  if (!gnssHeader("GNSS TRIP")) return;

  char b[48];
  cv.setFont(&fonts::Font0); cv.setTextSize(1);

  // 1. 上半部分双卡片 (y=15..72, cardH=58)
  const int cardW = (SW - 16) / 2; // 112px
  const int leftX = 6;
  const int rightX = leftX + cardW + 4; // 122px
  const int cardY = 15, cardH = 58;

  cv.fillRoundRect(leftX, cardY, cardW, cardH, 3, CARD_BG);
  cv.drawRoundRect(leftX, cardY, cardW, cardH, 3, DIM_BORDER);

  cv.fillRoundRect(rightX, cardY, cardW, cardH, 3, CARD_BG);
  cv.drawRoundRect(rightX, cardY, cardW, cardH, 3, DIM_BORDER);

  // 左卡片：行程统计
  // 行 1: 里程 DIST
  int ly = cardY + 3;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("DIST", leftX + 5, ly);
  cv.setTextDatum(top_right);
  if (gnssTripDistMeters >= 1000.0) {
    snprintf(b, sizeof(b), "%.2f km", gnssTripDistMeters / 1000.0);
  } else {
    snprintf(b, sizeof(b), "%.0f m", gnssTripDistMeters);
  }
  cv.setTextColor(ACCENT, CARD_BG);
  cv.drawString(b, leftX + cardW - 5, ly);

  // 行 2: 时长 TIME (总时长 + 移动时长)
  ly += 13;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("TIME", leftX + 5, ly);
  cv.setTextDatum(top_right);
  uint32_t totalSec = (gnssTripStartTime > 0) ? ((millis() - gnssTripStartTime) / 1000) : 0;
  uint32_t movSec = gnssTripMovingMs / 1000;
  if (totalSec < 3600) {
    snprintf(b, sizeof(b), "%02u:%02u/%02u:%02u",
             (unsigned)(totalSec / 60), (unsigned)(totalSec % 60),
             (unsigned)(movSec / 60), (unsigned)(movSec % 60));
  } else {
    snprintf(b, sizeof(b), "%uh%02u/%uh",
             (unsigned)(totalSec / 3600), (unsigned)((totalSec % 3600) / 60),
             (unsigned)(movSec / 3600));
  }
  cv.setTextColor(TFT_WHITE, CARD_BG);
  cv.drawString(b, leftX + cardW - 5, ly);

  // 行 3: 速度 SPD (平均移动速度 + 最高速度)
  ly += 13;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("SPD", leftX + 5, ly);
  cv.setTextDatum(top_right);
  float avgMovSpd = (movSec > 0) ? (float)(gnssTripDistMeters / (double)movSec * 3.6) : 0.0f;
  snprintf(b, sizeof(b), "avg%.0f/max%.0f", avgMovSpd, gnssTripMaxSpeed);
  cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
  cv.drawString(b, leftX + cardW - 5, ly);

  // 行 4: 海拔 ALT (极值范围)
  ly += 13;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("ALT", leftX + 5, ly);
  cv.setTextDatum(top_right);
  if (gnssTripMaxAlt > -9000.0f) {
    snprintf(b, sizeof(b), "%.0f~%.0fm", gnssTripMinAlt, gnssTripMaxAlt);
  } else {
    snprintf(b, sizeof(b), "--");
  }
  cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
  cv.drawString(b, leftX + cardW - 5, ly);

  // 右卡片：定位耗时与状态
  // 行 1: TTFF 首次定位耗时
  int ry = cardY + 3;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("TTFF", rightX + 5, ry);
  cv.setTextDatum(top_right);
  if (gnssTtffSec >= 0) snprintf(b, sizeof(b), "%.1fs", gnssTtffSec);
  else snprintf(b, sizeof(b), "waiting...");
  cv.setTextColor((gnssTtffSec >= 0) ? ACCENT : TFT_YELLOW, CARD_BG);
  cv.drawString(b, rightX + cardW - 5, ry);

  // 行 2: REACQ 重定位耗时
  ry += 13;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("REACQ", rightX + 5, ry);
  cv.setTextDatum(top_right);
  if (gnssReacqSec >= 0) snprintf(b, sizeof(b), "%.1fs", gnssReacqSec);
  else snprintf(b, sizeof(b), "--");
  cv.setTextColor(TFT_WHITE, CARD_BG);
  cv.drawString(b, rightX + cardW - 5, ry);

  // 行 3: 在用/可见卫星
  ry += 13;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("SATS", rightX + 5, ry);
  cv.setTextDatum(top_right);
  int uSats = gnssUsedSatCount();
  snprintf(b, sizeof(b), "%du / %dv", uSats, satCount);
  cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
  cv.drawString(b, rightX + cardW - 5, ry);

  // 行 4: 运动状态
  ry += 13;
  cv.setTextDatum(top_left); cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("STAT", rightX + 5, ry);
  cv.setTextDatum(top_right);
  bool moving = gnssSpeedKmph() > 3.0f;
  cv.setTextColor(moving ? ACCENT : TFT_DARKGREY, CARD_BG);
  cv.drawString(moving ? "MOVING" : "IDLE", rightX + cardW - 5, ry);

  // 2. 下半部分：信号诊断与波形曲线卡片 (y=75..118, 高度 44)
  const int dgX = 6, dgY = 75, dgW = SW - 12, dgH = 44;
  cv.fillRoundRect(dgX, dgY, dgW, dgH, 3, CARD_BG);
  cv.drawRoundRect(dgX, dgY, dgW, dgH, 3, DIM_BORDER);

  // 左侧数值统计
  cv.setTextDatum(top_left);
  cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("TOP4 SNR", dgX + 6, dgY + 4);

  cv.setTextDatum(top_left);
  snprintf(b, sizeof(b), "%.1f", gnssCurTop4Snr);
  uint16_t snrCol = (gnssCurTop4Snr >= 32.0f) ? ACCENT : ((gnssCurTop4Snr >= 20.0f) ? TFT_YELLOW : TFT_RED);
  cv.setTextColor(snrCol, CARD_BG);
  cv.drawString(b, dgX + 6, dgY + 16);

  cv.setTextColor(ICON_DIM, CARD_BG);
  cv.drawString("dB", dgX + 34, dgY + 16);

  cv.setTextDatum(top_left);
  snprintf(b, sizeof(b), "USED %d", uSats);
  cv.setTextColor(TFT_LIGHTGREY, CARD_BG);
  cv.drawString(b, dgX + 6, dgY + 29);

  // 右侧 2 分钟 SNR 小曲线 (宽 122, 高 34)
  const int plotX = dgX + 88, plotY = dgY + 5, plotW = dgW - 92, plotH = 34;
  cv.drawRect(plotX, plotY, plotW, plotH, DIM_BORDER);
  // 中线刻度 (25 dB)
  cv.drawFastHLine(plotX + 1, plotY + plotH / 2, plotW - 2, cv.color565(25, 45, 35));

  // 刻度标
  cv.setTextDatum(top_right); cv.setTextColor(DIM_BORDER, CARD_BG);
  cv.drawString("50", plotX - 2, plotY);
  cv.setTextDatum(bottom_right);
  cv.drawString("0", plotX - 2, plotY + plotH);

  // 绘制最近 120 秒波形
  int pts = gnssSnrHistCount;
  if (pts > 0) {
    int startIdx = (gnssSnrHistHead - pts + SNR_HIST_LEN) % SNR_HIST_LEN;
    int prevX = -1, prevY = -1;
    for (int i = 0; i < pts; i++) {
      int idx = (startIdx + i) % SNR_HIST_LEN;
      uint8_t val = gnssSnrHist[idx];
      int px = plotX + 1 + (plotW - 2 - pts) + i;
      int py = plotY + plotH - 2 - (int)(val * (plotH - 4) / 50.0f);
      if (py < plotY + 1) py = plotY + 1;
      if (py > plotY + plotH - 2) py = plotY + plotH - 2;

      uint16_t c = (val >= 32) ? ACCENT : ((val >= 20) ? TFT_YELLOW : TFT_RED);
      if (prevX >= 0) {
        cv.drawLine(prevX, prevY, px, py, c);
      } else {
        cv.drawPixel(px, py, c);
      }
      prevX = px; prevY = py;
    }
    if (prevX >= 0) cv.fillCircle(prevX, prevY, 2, ACCENT);
  }

  // 3. 底部提示
  cv.setTextDatum(bottom_left);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("r: reset", 6, SH - 2);

  cv.setTextDatum(bottom_right);
  cv.setTextColor(ICON_DIM, TFT_BLACK);
  cv.drawString("2m SNR hist", SW - 6, SH - 2);

  drawPageDots();
}
