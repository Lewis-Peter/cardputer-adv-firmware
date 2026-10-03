// GNSS（Cap LoRa-1262 板载 ATGM336H GPS 模块，内置陶瓷天线）。
// UART: ESP32 TX=GPIO13 -> 模块 GPS-RX ; ESP32 RX=GPIO15 <- 模块 GPS-TX，115200 8N1
// 天线开关由 PI4IOE5V6408 IO 扩展芯片的 P0 控制，必须先拉高才能收到信号。
#pragma once
#include "globals.h"

extern bool gnssIoOk;        // IO 扩展芯片通得上 = Cap 模块存在
extern bool gnssAntennaOn;   // 有源天线供电当前状态（配置页可切）

bool gnssCapConnected();     // 节流探测 Cap 扩展模块 (0x43) 是否在位（支持热插拔）
void gnssInit();
void gnssPoll();   // 串口数据随时喂给解析器；不管当前在哪个屏幕都要调用，避免缓冲区溢出丢数据

// PC 主导模式：GNSS 串口流式输出 (GNSS ON [hz] / GNSS OFF)
void gnssStreamSet(bool on, int hz = 1);
void gnssStreamTick();
bool gnssStreamIsActive();
int  gnssStreamGetHz();
void gnssNmeaEcho(uint32_t ms);   // 串口 NMEA 命令：ms 毫秒内原样回显模块输出的每一行，0 关闭

// 给外部模块（比如 Wardrive）用的定位读数，内部转发到 gnss.cpp 里的 TinyGPSPlus 单例
bool   gnssHasFix(uint32_t maxAgeMs = 3000);   // 定位在 maxAgeMs 内更新过才算有（TinyGPS++ 的 isValid 不会自己清零）
double gnssLat();
double gnssLng();
double gnssAlt();   // 米；GNSS 高程精度差，只够区分几十米量级
int    gnssSats();  // 卫星颗数

void drawGnss();
void drawGnssDetail();
void gnssDetailKey(char k);
void drawGnssSat();
void gnssSatKey(char k);
bool gnssSatInspectActive();
void drawGnssConfig();
void gnssConfigKey(char k);
void drawGnssMap();     // GNSS 页链中的地图页（WORLD 离线掩码 / z5~z14 在线瓦片）
void gnssMapZoom(int dir);   // [ 缩小 / ] 放大
// 地图瓦片是分帧拉的（一帧最多一块），必须在 loop() 里按帧调，否则永远停在加载中。
// 之所以不在 drawGnssMap() 里同步拉：一轮最坏几十秒，期间主循环卡住、键盘全无响应。
void gnssMapEnter();         // 进地图 app 时调：登记一次"没 GPS 就问 IP 要个中心点"
void gnssMapUpdate();

// 串口诊断（handleSerialCmd 里的 GPS 指令）：打出收字节数/校验和统计/静默时长，
// 用来区分"模块没在发"和"发了但被丢/解析不了"
void gnssPrintDiag();
void drawGnssSpeed();   // 速度表
void gnssSpeedKey(char k);
void drawGnssTrip();    // 行程与诊断页
void gnssTripKey(char k);

// 坐标换算（供详情页与主机端测试使用）
void formatDms(double val, bool isLat, char* out, size_t sz);
void toMaidenhead(double lat, double lon, char* out, size_t sz);
void toUtm(double lat, double lon, char* out, size_t sz);

// GSA 语句解析与查询
void gnssProcessGSA(char* line);
bool gnssIsSatUsed(char sys, uint8_t prn);
int  gnssUsedSatCount();
float gnssGetPdop();
float gnssGetHdop();
float gnssGetVdop();

// 行程统计与跳点过滤
void gnssTripResetStats();   // 行程页 r 键：只清行程统计
void gnssTripReset();        // 连 TTFF 一起清，测试台用
void gnssTripProcessPoint(uint32_t now, bool fix, uint32_t fixAgeMs,
                          double lat, double lng, float spd,
                          bool altValid, float alt);
double gnssTripGetDistMeters();
uint32_t gnssTripGetMovingMs();
float gnssTripGetMaxSpeed();
float gnssTripGetTtffSec();
float gnssTripGetReacqSec();
void gnssTripSetFirstByteMs(uint32_t ms);

// 退出 GNSS：释放地图页那块 ~48KB 的底图缓存画布（没 PSRAM，留着会挤掉后面 app 的内存）
void gnssMapExit();

// WGS-84 -> GCJ-02（火星坐标）。国内地图服务（高德/腾讯/百度）用的都是加过偏移的
// 坐标系，境内两者差 300~600 m；境外原样返回。
//
// 地图页画自身位置时已经过了这道换算（见 latLonToTilePx）。导出来是给**别的数据源**
// 用的：Remote ID 按规范播的是 WGS-84（实测国标载荷里的"坐标系类型"字段确实是
// 0=WGS-84），要把无人机画到高德底图上，必须先过这里，否则会稳定偏出几百米——
// 而且是整片一致的偏移，看起来特别像"RID 数据不准"，极难往坐标系上想。
void wgs2gcj(double wLat, double wLon, double& gLat, double& gLon);
