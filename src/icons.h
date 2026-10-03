// 矢量图标：所有 icoXxx() 原语 + 按 AppId/SetId 分发的 drawAppIcon/drawSetIcon，
// 以及状态栏用的电量/信号图标。全部画在共享画布 cv 上（见 globals.h）。
#pragma once
#include "globals.h"

void icoClock(int cx, int cy, int r, uint16_t col);
void icoMoon(int cx, int cy, int r, uint16_t col);
void icoGlobe(int cx, int cy, int r, uint16_t col);
void icoKeyboard(int cx, int cy, int r, uint16_t col);
void icoRemote(int cx, int cy, int r, uint16_t col);
void icoSun(int cx, int cy, int r, uint16_t col);
void icoInfo(int cx, int cy, int r, uint16_t col);
void icoSliders(int cx, int cy, int r, uint16_t col);
void icoWifi(int cx, int cy, int r, uint16_t col);
void icoBt(int cx, int cy, int r, uint16_t col);
void icoSpeaker(int cx, int cy, int r, uint16_t col);
void icoCompass(int cx, int cy, int r, uint16_t col);
void icoFolder(int cx, int cy, int r, uint16_t col);
void icoGnss(int cx, int cy, int r, uint16_t col);
void icoTrash(int cx, int cy, int r, uint16_t col);
void icoChat(int cx, int cy, int r, uint16_t col);
void icoSpectrum(int cx, int cy, int r, uint16_t col);
void icoPower(int cx, int cy, int r, uint16_t col);
void icoBattery(int cx, int cy, int r, uint16_t col);
void icoHotspot(int cx, int cy, int r, uint16_t col);
void icoCalc(int cx, int cy, int r, uint16_t col);
void icoLora(int cx, int cy, int r, uint16_t col);
void icoDrone(int cx, int cy, int r, uint16_t col);
void icoIr(int cx, int cy, int r, uint16_t col);
void icoLed(int cx, int cy, int r, uint16_t col);
void icoWsniff(int cx, int cy, int r, uint16_t col);
void icoRuler(int cx, int cy, int r, uint16_t col);
void icoProbe(int cx, int cy, int r, uint16_t col);
void icoBug(int cx, int cy, int r, uint16_t col);
void icoWardrive(int cx, int cy, int r, uint16_t col);
void icoDucky(int cx, int cy, int r, uint16_t col);
void icoLanscan(int cx, int cy, int r, uint16_t col);
void icoTimer(int cx, int cy, int r, uint16_t col);
void icoCloudSun(int cx, int cy, int r, uint16_t col);
void icoPlane(int cx, int cy, int r, uint16_t col);
void icoSat(int cx, int cy, int r, uint16_t col);
void icoTyphoon(int cx, int cy, int r, uint16_t col);
void icoRouter(int cx, int cy, int r, uint16_t col);
void icoTerminal(int cx, int cy, int r, uint16_t col);
void icoChanCurves(int cx, int cy, int r, uint16_t col);
void icoGithub(int cx, int cy, int r, uint16_t col);
void icoNote(int cx, int cy, int r, uint16_t col);
void icoRadio(int cx, int cy, int r, uint16_t col);
void icoApple(int cx, int cy, int r, uint16_t col);
void icoThermo(int cx, int cy, int r, uint16_t col);
void icoQuake(int cx, int cy, int r, uint16_t col);
void icoExchange(int cx, int cy, int r, uint16_t col);
void icoOkx(int cx, int cy, int r, uint16_t col);
void icoPalette(int cx, int cy, int r, uint16_t col);
void icoTimezone(int cx, int cy, int r, uint16_t col);
void icoBook(int cx, int cy, int r, uint16_t col);
void icoFlame(int cx, int cy, int r, uint16_t col);
void icoPc(int cx, int cy, int r, uint16_t col);

void drawAppIcon(int cx, int cy, int r, uint16_t col, AppId id);
void drawSetIcon(int cx, int cy, int r, uint16_t col, SetId id);

void drawBattery(int x, int y);
void drawSignal(int x, int y, int rssi, uint16_t on);
void drawWifiSignal(int cx, int by, int rssi, uint16_t on);
void drawLock(int cx, int cy, uint16_t col);
