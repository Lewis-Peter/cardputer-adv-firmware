#include "battlog.h"

#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <ctime>

#include "globals.h"     // loadBool/saveBool、screen、timeSynced
#include "ui_common.h"   // nowStamp / sdAppend
#include "sd_files.h"    // sdMounted

static const uint32_t LOG_EVERY_MS = 10000;   // 10 秒一行。整晚也就 8600 行、几百 KB
static bool     logOn = false;
static bool     flagLoaded = false;
static bool     inited = false;
static uint32_t lastLogMs = 0;
static String   logPath;

bool battLogEnabled() {
  if (!flagLoaded) { flagLoaded = true; logOn = loadBool("power", "log", false); }
  return logOn;
}

void battLogSet(bool on) {
  flagLoaded = true;
  logOn = on;
  saveBool("power", "log", on);
  if (!on) { logPath = ""; inited = false; }
}

void battLogTick(uint32_t now, int mv, float ema, int level, bool charging, int spread) {
  // ⚠️ 开关必须从 NVS 恢复。这个日志的全部意义就是记录"串口断开之后"发生了什么，
  //    而拔线往往伴随重启（没电关机、插回来按复位），不恢复就等于没记。
  if (!battLogEnabled()) return;

  if (!inited) {
    inited = true;
    if (!sdMounted) { Serial.println("[batt] 没有 SD 卡，日志开不了"); logOn = false; return; }
    if (!SD.exists("/battlog")) SD.mkdir("/battlog");
    // ⚠️ 文件名不能用 nowStamp()——那是给日志**行内**用的人可读格式（"2026-08-26 10:41:30"），
    //    带空格和冒号，FAT 上非法，fopen 直接失败。跟 AIS 日志一样自己拼。
    char pathBuf[48];
    struct tm ti;
    if (timeSynced && getLocalTime(&ti, 0))
      snprintf(pathBuf, sizeof(pathBuf), "/battlog/%04d%02d%02d_%02d%02d%02d.csv",
               ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec);
    else
      // ⚠️ 没对上时间时的兜底名。用开机秒数会**跨重启撞名**（每次都是 run4.csv，
      //    于是多次开机的数据全追加进同一个文件，分析时得靠 t_ms 回退来切会话
      //    ——2026-08-26 就这么踩了一次）。掺一个随机数把它们分开。
      snprintf(pathBuf, sizeof(pathBuf), "/battlog/run%lu_%04lx.csv",
               (unsigned long)(now / 1000), (unsigned long)(esp_random() & 0xFFFF));
    logPath = pathBuf;
    sdAppend(logPath.c_str(), String("# ") + nowStamp() +
             "  cardputer_adv battery log（charging 一列是猜的，本机没有充电状态脚）");
    sdAppend(logPath.c_str(), "t_ms,mv,ema,level,charging,screen,wifi,spread");
    Serial.printf("[batt] 日志文件 %s\n", logPath.c_str());
    lastLogMs = now - LOG_EVERY_MS;   // 立刻写第一行
  }

  if (now - lastLogMs < LOG_EVERY_MS) return;
  lastLogMs = now;
  char line[96];
  // spread：采样窗峰峰值。快充时 200~300mV、充满后 20~50mV，是"充满了没"的判据
  snprintf(line, sizeof(line), "%lu,%d,%.1f,%d,%d,%d,%d,%d",
           (unsigned long)now, mv, ema, level, charging ? 1 : 0,
           (int)screen, WiFi.status() == WL_CONNECTED ? 1 : 0, spread);
  sdAppend(logPath.c_str(), line);
}
