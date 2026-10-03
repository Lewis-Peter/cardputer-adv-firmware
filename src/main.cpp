// Cardputer ADV — 摸鱼机 v4
// -----------------------------------------------------------------------------
// 顶层：横向卡片轮播（Clock / Settings…）。
// 设置为二级菜单（竖排列表）：Wi-Fi / Bluetooth / Brightness / Volume / About。
//
// 导航键（TCA8418）：; 上/上一个   . 下/下一个   Enter 进入/确认   ` 返回
// 渲染：双缓冲（M5Canvas），只在变化时重画+推送；轮播带滑动缓动动画。
// Wi-Fi：在设备上扫描选择并输入密码，连过的网络记进 NVS，下次自动连。
//
// 代码按功能拆成多个文件：
//   globals.*    共享状态（画布、配色、Screen/AppId/SetId 枚举、熄屏/动画状态）
//   icons.* / ui_common.*  矢量图标 / 通用绘制（居中提示、顶栏、主菜单轮播、开机动画、时钟）
//   sd_files.* / reader.*  SD 卡挂载+文件管理器 / 小说阅读器
//   wifi_net.*   Wi-Fi 扫描/连接 + NTP        wifi_chan.*  信道分析器（重叠信号曲线）
//   wsniff.*     Wi-Fi 混杂嗅探（被动）        hotspot.*    SoftAP 热点
//   gnss.* / imu.*  GNSS 定位 / IMU 指南针水平仪   spectrum.*  麦克风 FFT 频谱
//   bt.*         BLE 扫描 + HID 键盘           lora.*       LoRa(SX1262) 嗅探
//   chat.*       ChatGPT 问答
//   calc.*       计算器      ir.*  红外(占位)   led.*  板载 RGB LED   settings_ui.*  设置子屏
//   stopwatch.*  秒表/倒计时                   moon.*       月相（Clock 第3页）
//   geoloc.*     共享定位(GNSS优先/IP兜底)     weather.*    Open-Meteo 天气（六页）
//   adsb.*       ADS-B 飞机雷达(adsb.lol)      sats.*       卫星过顶(N2YO)   router.*  Clash/mihomo 监控
//   github.*     贡献热力图                    net_job.*    延后拉网络的共用小状态机
//   pages.*      多子页 app 的翻页链（翻页按键 + 底部页码点唯一的真相来源）
//   serial_cmd.* 串口调试控制台（SHOT/GOTO/STAT/HGET/... 见 HELP）
// main.cpp 只保留 setup()/loop()/render()/handleKey()/cleanupApp() 的顶层调度。
// -----------------------------------------------------------------------------

#include <M5Unified.h>
SET_LOOP_TASK_STACK_SIZE(12 * 1024);
#include <WiFi.h>
#include <esp_wifi.h>
#include <SD.h>
#include <ctime>
#include <cmath>
#include <esp_heap_caps.h>
#include "keyboard_adv.h"
#include "bctrail.h"
#include "ram_profile.h"
#include "astro.h"

#include "globals.h"
#include "tls_ca.h"
#include "list_sel.h"
#include "icons.h"
#include "ui_common.h"
#include "clock.h"
#include "sd_files.h"
#include "reader.h"
#include "wifi_net.h"
#include "wifi_chan.h"
#include "hotspot.h"
#include "gnss.h"
#include "imu.h"
#include "settings_ui.h"
#include "bt.h"
#include "chat.h"
#include "spectrum.h"
#include "calc.h"
#include "lora.h"
#include "ir.h"
#include "conv.h"
#include "player.h"
#include "badapple.h"
#include "radio.h"
#include "led.h"
#include "power_util.h"
#include "wsniff.h"
#include "netprobe.h"
#include "wardrive.h"
#include "ducky.h"
#include "lanscan.h"
#include "stopwatch.h"
#include "weather.h"
#include "moon.h"
#include "pages.h"
#include "adsb.h"
#include "typhoon.h"
#include "quake.h"
#include "fx.h"
#include "okx.h"
#include "ridapp.h"
#include "sats.h"
#include "router.h"
#include "github.h"
#include "ssh_app.h"
#include "hash_oven.h"
#include "pcmode.h"
#include "serial_cmd.h"
#include "bg_fetch.h"

// 调试状态条：开了 Settings->Debug 之后画在所有页面最上层。
// 会盖掉底部那行按键提示/页码点——这是有意的，调试时那条信息更重要，关掉就恢复。
// 内容：堆余量 / 最大连续块（判断"能不能再申请那 48KB 地图缓存"就看这个）/ WiFi 信号 /
// 上一帧到这一帧的间隔（卡不卡一眼看出来）/ 当前 Screen 序号（配 GOTO 用）。
// ---- 后台拉取期间的输入（见 bg_fetch.h）----
// 工人在跑时页面不能动（画布在它手里、页面数据它在写），所以按键先攒着，收尾后原样重放；
// 第 1 页的返回键 / BtnA 意味着"不看了"：当场请求取消，收尾后再真的离开。
static char bgKeys[8];
static int  bgKeyN = 0;
static bool bgPendingBtnA = false;
static bool bgLeaving = false;     // 已经按了离开：指示改成 "leaving"，之后的键一律不要
static Screen bgShadow = SCREEN_MENU;   // 排队的翻页键推演之后会停在哪一页（见 bgKeyWhileBusy）
static uint32_t renderStartMs = 0;   // render() 入口时刻，debug 条用来算这一帧画了多久

static void drawDebugBar() {
  // ⚠️ 数值必须节流。菜单高亮框做缓动时 loop 是 8ms 一帧（~125fps），要是每帧都重新取
  // 堆余量/绘制耗时，这几个数字就会疯狂跳动——屏幕上看就是"底栏左边有个数字在乱闪"，
  // 而且 heap_caps_get_largest_free_block() 每次都要遍历空闲链表并上锁，125 次/秒纯属浪费。
  // 4Hz 刷一次够看了；框和文字还是每帧画（画一行字很便宜），不然会被上一帧的内容盖掉。
  static uint32_t lastCalcMs = 0;
  static char sHeap[24] = "", sWifi[16] = "", sPerf[20] = "", sBatt[16] = "";
  static bool heapLow = false, slowFrame = false;
  static uint32_t peakDrawMs = 0;   // 取窗口内的最大值，不然一闪而过的卡顿根本看不见

  uint32_t drawMs = millis() - renderStartMs;
  if (drawMs > peakDrawMs) peakDrawMs = drawMs;

  if (lastCalcMs == 0 || millis() - lastCalcMs >= 250) {
    lastCalcMs = millis();
    size_t freeHeap = ESP.getFreeHeap();
    size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    // 最大连续块低于 50KB 就该警觉了：地图那块底图缓存要 48KB
    heapLow = (largest < 50 * 1024);
    // 用 b 不用 l 标"最大块"：小写 L 在这个点阵字体里跟数字 1 几乎一样，一眼会读成 183k
    snprintf(sHeap, sizeof(sHeap), "h%dk b%dk", (int)(freeHeap / 1024), (int)(largest / 1024));
    if (WiFi.status() == WL_CONNECTED) snprintf(sWifi, sizeof(sWifi), "%ddBm", (int)WiFi.RSSI());
    else                               snprintf(sWifi, sizeof(sWifi), "wifi--");
    // 量的是"这一帧画了多久"，不是"距上次重画多久"：多数页面只在 dirty 时才重画、
    // 主菜单本来就一秒一次，用间隔会一直标橙色，属于误报
    // 电压 + 电量，符号位是充电推测（+ 在充 / - 没在充）。⚠️ 这两个数在拔线之后
    // **只能从屏幕上看**（串口断了），而电量查表的标定恰恰要在拔线状态下做。
    // 没采到电压时如实留空，不显示一个编出来的数。
    {
      int bmv = powerBatteryMv(), blv = powerBatteryLevel();
      if (bmv > 0) snprintf(sBatt, sizeof(sBatt), "%c%d %d%%",
                            powerCharging() ? '+' : '-', bmv, blv);
      else         snprintf(sBatt, sizeof(sBatt), "bat?");
    }
    snprintf(sPerf, sizeof(sPerf), "%lums s%d", (unsigned long)peakDrawMs, (int)screen);
    slowFrame = (peakDrawMs > 100);
    peakDrawMs = 0;
  }

  // ⚠️ 高度必须够盖住页面自己的底部提示行，否则会留下"横线上飘着一排点"的残影。
  // 算一下：那些提示大多是 size1 + bottom 基准画在 SH-2（=133），字高 8px 于是占 125..132 行；
  // 原来这条从 126 开始填，第 125 行盖不住，剩下的就是每个字母最顶上那一排像素。
  // 11px（124..134）能盖住全部底部锚定的绘制：SH-2 的文字(125..132)、SH-1 的文字(126..133)、
  // 页码点(dotY=SH-4, r=2 -> 129..133)。全项目底部没有 size2 的文字，核对过了。
  const int h = 11, y = SH - h;
  cv.fillRect(0, y, SW, h, TFT_BLACK);
  cv.drawFastHLine(0, y, SW, DIM_BORDER);
  cv.setTextSize(1);

  cv.setTextDatum(bottom_left);
  cv.setTextColor(heapLow ? TFT_ORANGE : ICON_DIM, TFT_BLACK);
  cv.drawString(sHeap, 2, SH);

  // 中间这格塞两样：Wi-Fi 信号 + 电池。整条 240px/6px = 40 字符，左边堆信息最长 11 个、
  // 右边耗时最长 8 个，中间还剩 20 个上下，"wifi-- +3512 83%" 是 16 个，放得下。
  {
    char mid[32];
    snprintf(mid, sizeof(mid), "%s %s", sWifi, sBatt);
    cv.setTextDatum(bottom_center);
    cv.setTextColor(ICON_DIM, TFT_BLACK);
    cv.drawString(mid, SW / 2 + 4, SH);
  }

  cv.setTextDatum(bottom_right);
  cv.setTextColor(slowFrame ? TFT_ORANGE : ICON_DIM, TFT_BLACK);
  cv.drawString(sPerf, SW - 2, SH);
}

void render() {
  // 后台拉取进行中：画布在工人手里，只在屏上直接画一个会动的小指示（不碰 cv），页面停在最后一帧
  if (bgFetchBusy()) {
    bgFetchDrawIndicator(bgLeaving);
    return;
  }
  if (screen == SCREEN_SSH && sshIsTerminalActive()) {
    drawSsh();
    return;
  }
  if (screen == SCREEN_BADAPPLE && badappleIsPlaying()) {
    drawBadApple();
    return;
  }
  if (screen == SCREEN_CHAT && chatBusy()) {
    drawChatBusyDirect();
    return;
  }
  if (screen == SCREEN_PCMODE) {
    drawPcMode();
    return;
  }
  if (screen == SCREEN_RADIO_PLAY) {
    drawRadioPlay();
    return;
  }

  // 保证使用 cv 的页面画布安全挂载（自适应显存管理）
  canvasRestore();
  // 画布拿不回来（典型：Chat 请求被放弃但 TLS 握手还占着堆）：往 cv 画全是空操作，
  // 屏幕会停在上一帧。直接在屏上给个提示，等 chatUpdate() 收尾恢复画布后再正常画。
  static bool noCanvasNoticeShown = false;
  if (!canvasAvailable()) {
    if (debugOn) Serial.printf("[render] noCanvas screen=%d\n", (int)screen);
    if (!noCanvasNoticeShown) {
      M5.Display.fillScreen(TFT_BLACK);
      M5.Display.setFont(&fonts::Font0);
      M5.Display.setTextSize(1);
      M5.Display.setTextDatum(middle_center);
      M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
      M5.Display.drawString("finishing request...", SW / 2, SH / 2);
      noCanvasNoticeShown = true;
    }
    bgFetchPokeRestore();
    return;
  }
  noCanvasNoticeShown = false;

  renderStartMs = millis();
  switch (screen) {
    case SCREEN_MENU:       drawMenu(); break;
    case SCREEN_CLOCK:      drawClock(); break;
    case SCREEN_SETTINGS:   drawSettings(); break;
    case SCREEN_WIFI_SCAN:  drawWifiScan(); break;
    case SCREEN_WIFI_PW:    drawWifiPw(); break;
    case SCREEN_BT_SCAN:    drawBtScan(); break;
    case SCREEN_BT_DEVICE:  drawBtDeviceDetail(); break;
    case SCREEN_BT_KEYBOARD: drawBtKeyboard(); break;
    case SCREEN_BT_MEDIA:   drawBtMedia(); break;
    case SCREEN_BT_RADAR:   drawBtRadar(); break;
    case SCREEN_BRIGHTNESS: drawBar("Brightness", brightPct, "; up  . down"); break;
    case SCREEN_VOLUME:     drawBar("Volume", volPct, "; up  . down"); break;
    case SCREEN_SLEEP:      drawSleep(); break;
    case SCREEN_TZ:         drawTz(); break;
    case SCREEN_BATTERY:    drawBatteryDetail(); break;
    case SCREEN_FORMAT:     drawFormat(); break;
    case SCREEN_ABOUT:      drawAbout(); break;
    case SCREEN_COMPASS:    drawCompass(); break;
    case SCREEN_COMPASS_DETAIL: drawImuDetail(); break;
    case SCREEN_FILES:      drawFiles(); break;
    case SCREEN_READER:     drawReader(); break;
    case SCREEN_READER_SHELF: drawReaderShelf(); break;
    case SCREEN_GNSS:       drawGnss(); break;
    case SCREEN_GNSS_DETAIL: drawGnssDetail(); break;
    case SCREEN_GNSS_SAT:   drawGnssSat(); break;
    case SCREEN_GNSS_CONFIG: drawGnssConfig(); break;
    case SCREEN_GNSS_MAP:   drawGnssMap(); break;
    case SCREEN_GNSS_SPEED: drawGnssSpeed(); break;
    case SCREEN_GNSS_TRIP:  drawGnssTrip(); break;
    case SCREEN_CHAT:        drawChatInput(); break;
    case SCREEN_CHAT_REPLY:  drawChatReply(); break;
    case SCREEN_SPECTRUM:    drawSpectrum(); break;
    case SCREEN_WIFI_CHAN:   drawWifiChan(); break;
    case SCREEN_NETPROBE:    drawNetprobe(); break;
    case SCREEN_HASHOVEN:    drawHashOven(); break;
    case SCREEN_WIFI_CHAN_DETAIL: drawWifiChanDetail(); break;
    case SCREEN_WSNIFF:      drawWsniff(); break;
    case SCREEN_HOTSPOT:     drawHotspot(); break;
    case SCREEN_HOTSPOT_PW:  drawHotspotPw(); break;
    case SCREEN_CALC:        drawCalc(); break;
    case SCREEN_LORA:        drawLora(); break;
    case SCREEN_IR:          drawIr(); break;
    case SCREEN_CONV:        drawConv(); break;
    case SCREEN_PLAYER:      drawPlayer(); break;
    case SCREEN_BADAPPLE:    drawBadApple(); break;
    case SCREEN_RADIO:       drawRadio(); break;
    case SCREEN_RADIO_PLAY:  drawRadioPlay(); break;
    case SCREEN_RADIO_URL:   drawRadioUrl(); break;
    case SCREEN_WARDRIVE:    drawWardrive(); break;
    case SCREEN_DUCKY:       drawDucky(); break;
    case SCREEN_LANSCAN:     drawLanscan(); break;
    case SCREEN_HOTSPOT_QR:  drawHotspotQr(); break;
    case SCREEN_STOPWATCH:   drawStopwatch(); break;
    case SCREEN_MOON:        drawMoon(); break;
    case SCREEN_ASTRO_SUN:   drawAstroSun(); break;
    case SCREEN_ASTRO_TERM:  drawAstroTerm(); break;
    case SCREEN_WEATHER:      drawWeather(); break;
    case SCREEN_WEATHER_HOUR: drawWeatherHour(); break;
    case SCREEN_WEATHER_AIR:  drawWeatherAir(); break;
    case SCREEN_WEATHER_AQI:  drawWeatherAqi(); break;
    case SCREEN_WEATHER_FC:   drawWeatherFc(); break;
    case SCREEN_ADSB:        drawAdsb(); break;
    case SCREEN_RID:        drawRidApp(); break;
    case SCREEN_RID_DETAIL: drawRidAppDetail(); break;
    case SCREEN_TYPHOON:     drawTyphoon(); break;
    case SCREEN_TYPHOON_TRACK: drawTyphoonTrack(); break;
    case SCREEN_QUAKE:       drawQuake(); break;
    case SCREEN_QUAKE_MAP:   drawQuakeMap(); break;
    case SCREEN_FX:          drawFx(); break;
    case SCREEN_FX_DAYS:     drawFxDays(); break;
    case SCREEN_OKX:         drawOkx(); break;
    case SCREEN_SATS:        drawSats(); break;
    case SCREEN_ROUTER:      drawRouter(); break;
    case SCREEN_GITHUB:      drawGithub(); break;
    case SCREEN_SSH:         drawSsh(); break;
    case SCREEN_PCMODE:      break;
    // 哨兵，不是真页面。这里刻意不写 default: —— 少了 default，新加一个 Screen 却忘了
    // 配绘制函数时，-Wswitch 会当场报出来；写成 default 就把这层保护关掉了。
    case SCREEN__COUNT:      break;
  }
  if (debugOn) drawDebugBar();
  if (stopwatchIsAlarming() && screen != SCREEN_STOPWATCH) {
    cv.fillRoundRect(20, 2, SW - 40, 16, 3, TFT_RED);
    cv.setTextColor(TFT_WHITE, TFT_RED);
    cv.setTextDatum(middle_center);
    cv.setTextSize(1);
    cv.drawString("ALARM! Press any key", SW / 2, 10);
  }
  cv.pushSprite(0, 0);
}

// 统一的退出清理：某个 app 的屏幕要回主菜单时，释放它占的硬件/资源。
// 反引号(`)退出 和 BtnA 一键回菜单 两条路径都走这里，避免两处清理逻辑不同步——
// 以前正是分两处各写一份，WiFi Chan 详情页用 BtnA 退出就漏清了 WiFi。
void cleanupApp(Screen s) {
  if (isBleScreen(s)) {
    kbd::setAutoRepeat(false);
    if (s == SCREEN_DUCKY) duckyExit();
    btExit();
    return;
  }   // 关 BLE 控制器并释放
  switch (s) {
    case SCREEN_FILES:            filesExit(); break;                              // 释放文件列表
    case SCREEN_READER_SHELF:     readerShelfExit(); break;                        // 释放书架列表
    case SCREEN_READER:
      readerSaveProgress(); readerFile.close(); readerShelfExit();
      if (readerFromFiles) { filesExit(); readerFromFiles = false; }
      break;
    case SCREEN_SPECTRUM:         spectrumExit(); break;                            // 关 Mic、换回 Speaker
    case SCREEN_PLAYER:           playerExit(); break;                             // 停止 WAV 流、删任务
    case SCREEN_BADAPPLE:         badappleExit(); break;                          // 关文件、删 1bpp 帧缓冲
    case SCREEN_RADIO:
    case SCREEN_RADIO_PLAY:
    case SCREEN_RADIO_URL:        radioExit(); break;                              // 停止收音机流、释放 pipeline
    case SCREEN_LORA:             loraExit(); break;                               // 电台待机、释放 SPI
    case SCREEN_WIFI_SCAN:
    case SCREEN_WIFI_PW:          wifiScanExit(); break;                           // 释放扫描结果与输入缓存
    case SCREEN_WIFI_CHAN:
    case SCREEN_WIFI_CHAN_DETAIL: wifiChanExit(); break;                           // 释放扫描结果、清 WiFi
    // 地图拆成独立 app 之后，那 48KB 底图缓存只可能由它申请，所以只有它需要清。
    // GNSS 那五页现在一个字节都不占，不必再挂。
    case SCREEN_GNSS_MAP:         gnssMapExit(); break;
    case SCREEN_RID:
    case SCREEN_RID_DETAIL:       ridAppExit(); break;                            // 关混杂模式、还原 WiFi
    case SCREEN_WSNIFF:           wsniffExit(); break;                             // 关混杂模式、清 WiFi
    case SCREEN_WARDRIVE:         wardriveExit(); break;                          // 释放扫描结果、清 WiFi
    case SCREEN_LANSCAN:          lanscanExit(); break;                        // 停掉还在跑的ping、清 WiFi
    // Time app：第 1 页是时钟、第 2 页是秒表。从哪一页退出都要停闹铃（计时值保留，
    // 基准是 millis()，回来还能接着看）
    case SCREEN_CLOCK:
    case SCREEN_STOPWATCH:        stopwatchExit(); break;
    case SCREEN_CHAT:
    case SCREEN_CHAT_REPLY:       chatExit(); break;
    case SCREEN_IR:               irExit(); break;      // 把 RMT 通道还回去（FastLED 也要用）
    case SCREEN_ROUTER:           routerExit(); break;   // 关掉常开的 /traffic 连接                           // 在途请求跑完后丢掉结果，别污染历史
    case SCREEN_SATS:             satsExit(); break;    // 释放卫星列表
    case SCREEN_FX:
    case SCREEN_FX_DAYS:          fxExit(); break;      // 释放汇率走势缓存
    case SCREEN_HASHOVEN:         hashOvenExit(); break; // 停掉烘焙/穷举任务
    case SCREEN_NETPROBE:         netprobeExit(); break;
    case SCREEN_SSH:              sshExit(); break;
    case SCREEN_PCMODE:           pcmodeExit(); break;
    default: break;   // 其余无需释放（Hotspot 的 AP 有意保持常开）
  }
}

// =============================================================================
// 输入
// =============================================================================
static void handleKeyAction(char k);

void handleKey(char k) {
  if (k == 0) return;
  lastActivityMs = millis();
  if (stopwatchIsAlarming()) {                   // 闹钟响铃时任意键先只负责停闹钟，不触发原本的动作
    stopwatchStopAlarm();
    dirty = true;
    return;
  }
  if (screenOff) {                              // 熄屏时任意键只负责唤醒，不触发原本的按键动作
    M5.Display.setBrightness(brightVal());
    screenOff = false;
    ledApply();                               // SK6812 断电后状态丢失，立即重推一帧
    dirty = true;
    return;
  }
  handleKeyAction(k);
}

// 按键的"动作"部分（上面那两道唤醒/停闹钟的闸之后）。单独拎出来给后台拉取收尾时重放用：
// 那些键按下时屏是亮的、闹钟没响，等拉完才熄屏/响铃的话不能让它们被当成唤醒键吃掉。
static void handleKeyAction(char k) {
  // Fn+` 在 BLE 键盘页是退出键（那一页把裸 ` 让给了主机）。为了别让人学了一个手势、
  // 换一页就变死键，其余所有页面一律把它当成普通的 `——全局等价，不用各页各写一遍。
  // 同时兼容 Fn+Tab（键盘丝印为 Esc，对应 KX_ESC）。
  if (((uint8_t)k == kbd::KX_BACK || (uint8_t)k == kbd::KX_ESC) && screen != SCREEN_BT_KEYBOARD && screen != SCREEN_SSH) k = '`';

  // 多子页 app 的翻页统一在这儿处理（链定义在 pages.cpp）：
  //   . /  往后一页    ; ,  往前一页    `  折回第 1 页
  // 第 1 页的 ` 不拦，留给下面的 switch —— 各 app 退出时要做的事不一样（有的还要 cleanupApp）
  {
    int cur, cnt;
    if (pageIndex(screen, cur, cnt)) {
      // 探针弹窗开着时翻页键也不放行，免得弹窗跟着带到页外、回来时还挂着
      if (screen == SCREEN_GNSS_SAT && gnssSatInspectActive() &&
          (k == '.' || k == '/' || k == ';' || k == ',')) return;
      if (k == '.' || k == '/')      { screen = pageStep(screen, +1); dirty = true; return; }
      if (k == ';' || k == ',')      { screen = pageStep(screen, -1); dirty = true; return; }
      if (k == '`' && cur > 0) {
        if (screen == SCREEN_GNSS_SAT && gnssSatInspectActive()) {
          gnssSatKey('`');
          return;
        }
        screen = pageFirst(screen);
        dirty = true;
        return;
      }
    }
  }

  switch (screen) {
    case SCREEN_MENU:
      // 移动逻辑连同网格布局一起放在 ui_common 里（menuMove 会自己翻 dirty）：
      // 左右在组内走、走出边界切到相邻组；上下在组内两行之间循环。
      if      (k == ',') menuMove(-1, 0);   // ← 左
      else if (k == '/') menuMove(+1, 0);   // → 右
      else if (k == ';') menuMove(0, -1);   // ↑ 上
      else if (k == '.') menuMove(0, +1);   // ↓ 下
      else if (k == '\n') {
        AppId want = APPS[menuIndex].id;
        ACCENT = getGroupTheme(menuGroupOf(menuIndex)).primary;
        switch (want) {
          // Time 第 2 页是秒表；stopwatchEnter 只是翻 dirty，进 app 时调一次就够
          case APP_TIME:     screen = SCREEN_CLOCK; stopwatchEnter(); break;
          case APP_ASTRO:    screen = SCREEN_ASTRO_SUN; astroEnter(); break;
          case APP_COMPASS:  screen = SCREEN_COMPASS; break;
          case APP_FILES:    screen = SCREEN_FILES; curPath = "/"; loadDir(curPath); break;
          case APP_GNSS:     screen = SCREEN_GNSS; break;
          case APP_MAP:      screen = SCREEN_GNSS_MAP; gnssMapEnter(); break;
          case APP_LORA:     screen = SCREEN_LORA; loraEnter(); break;
          // Bluetooth 拆成三个 app：原来那个三选一的子菜单没有存在的理由了
          case APP_BTSCAN:   screen = SCREEN_BT_SCAN; btScan(); break;
          case APP_BTMEDIA:  screen = SCREEN_BT_MEDIA; btMediaStart(); break;
          case APP_BTKB:
            screen = SCREEN_BT_KEYBOARD;
            btKeyboardStart();
            btKeyboardResetEcho();
            // 当真键盘用，长按要连发；但 IME 模式下不连发（会吃掉整串候选），
            // 而 IME 开关是跨进出保留的，所以按它的状态来，不能无脑开
            kbd::setAutoRepeat(!btKeyboardImeMode());
            kbd::consumeSticky();       // 清掉进来之前在别的页面误点出来的粘滞位
            break;
          case APP_RID:      screen = SCREEN_RID; ridAppEnter(); break;
          case APP_CHAT:     screen = SCREEN_CHAT; chatReset(); break;
          case APP_SPECTRUM: screen = SCREEN_SPECTRUM; spectrumEnter(); break;
          case APP_WIFICHAN: screen = SCREEN_WIFI_CHAN; wifiChanScan(); break;
          case APP_WSNIFF:   screen = SCREEN_WSNIFF; wsniffEnter(); break;
          case APP_HOTSPOT:  screen = SCREEN_HOTSPOT; break;
          case APP_NETPROBE: screen = SCREEN_NETPROBE; netprobeEnter(); break;
          case APP_HASHOVEN: screen = SCREEN_HASHOVEN; hashOvenEnter(); break;
          case APP_CALC:     screen = SCREEN_CALC; break;
          case APP_IR:       screen = SCREEN_IR; irEnter(); break;
          case APP_CONV:     screen = SCREEN_CONV; convEnter(); break;
          case APP_PLAYER:   screen = SCREEN_PLAYER; playerEnter(); break;
          case APP_BADAPPLE: screen = SCREEN_BADAPPLE; badappleEnter(); break;
          case APP_RADIO:    screen = SCREEN_RADIO; radioEnter(); break;
          case APP_WARDRIVE: screen = SCREEN_WARDRIVE; wardriveEnter(); break;
          case APP_DUCKY:    screen = SCREEN_DUCKY; duckyEnter(); break;
          case APP_LANSCAN:  screen = SCREEN_LANSCAN; lanscanEnter(); break;
          case APP_WEATHER:  screen = SCREEN_WEATHER; weatherEnter(); break;
          case APP_ADSB:     screen = SCREEN_ADSB; adsbEnter(); break;
          case APP_TYPHOON:  screen = SCREEN_TYPHOON; typhoonEnter(); break;
          case APP_QUAKE:    screen = SCREEN_QUAKE; quakeEnter(); break;
          case APP_FX:       screen = SCREEN_FX; fxEnter(); break;
          case APP_OKX:      screen = SCREEN_OKX; okxEnter(); break;
          case APP_SATS:     screen = SCREEN_SATS; satsEnter(); break;
          case APP_ROUTER:   screen = SCREEN_ROUTER; routerEnter(); break;
          case APP_GITHUB:   screen = SCREEN_GITHUB; githubEnter(); break;
          case APP_SSH:      screen = SCREEN_SSH; sshEnter(); break;
          case APP_READER:   readerEnter(); break;
          case APP_SETTINGS: screen = SCREEN_SETTINGS; settingsIndex = 0; break;
        }
        dirty = true;
      }
      return;

    case SCREEN_SETTINGS:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else if (k == ';' || k == ',') { listMoveIndex(settingsIndex, SET_COUNT, -1); dirty = true; }
      else if (k == '.' || k == '/') { listMoveIndex(settingsIndex, SET_COUNT, 1); dirty = true; }
      else if (k == '\n') {
        switch (SETTINGS[settingsIndex].id) {
          case SET_WIFI:   doScan(); screen = SCREEN_WIFI_SCAN; break;
          case SET_GNSS:   screen = SCREEN_GNSS_CONFIG; break;
          case SET_PCMODE: pcmodeEnter(); break;
          case SET_BRIGHT: screen = SCREEN_BRIGHTNESS; break;
          case SET_LED:    ledCycleMode(); break;       // 原地切换模式（含关闭），不进子屏
          case SET_THEME: {
            uint8_t nextMode = (themeMode + 1) % 8;
            themeSet(nextMode);
            if (themeMode > 0) ACCENT = getGroupTheme(themeMode - 1).primary;
            else ACCENT = 0x07FF;
            break;
          }
          // 打开 Debug 时顺手把停滞看门狗起上，不用等重启（关掉不停，见 bctrail.cpp）
          case SET_DEBUG:  debugSet(!debugOn); if (debugOn) bcTrailStartWatchdog(); break;
          case SET_WX_UNIT: weatherUnitsSet(!weatherImperial); break; // 同上
          case SET_VOL:    screen = SCREEN_VOLUME; break;
          case SET_BOOT_SOUND:
            bootSoundSet(!bootSoundOn);
            if (bootSoundOn && volVal() > 0) M5.Speaker.tone(1568, 60);
            break;
          case SET_SLEEP:  screen = SCREEN_SLEEP; break;
          case SET_TZ:     screen = SCREEN_TZ; break;
          case SET_BATTERY: screen = SCREEN_BATTERY; break;
          case SET_FORMAT: screen = SCREEN_FORMAT; formatStep = 0; break;
          case SET_ABOUT:  screen = SCREEN_ABOUT; aboutPage = 0; aboutRamReset(); break;
        }
        dirty = true;
      }
      return;

    // 下面这些多子页 app 的翻页已经在 switch 之前处理掉了，这里只剩"第 1 页按 ` 退出"
    // Time 第 1 页：` 退出（第 2 页的 ` 已经被上面的翻页块折回来了）
    case SCREEN_CLOCK:
      if (k == '`') { cleanupApp(SCREEN_CLOCK); screen = SCREEN_MENU; dirty = true; }
      else clockKey(k);
      return;

    // Time 第 2 页：翻页键已被拦掉，其余交给秒表（Enter 起停 / m 切模式 / l 计次 /
    // r 归零 / [ ] - = 调倒计时预设）
    case SCREEN_STOPWATCH:
      stopwatchKey(k);
      return;

    // Astro 三页共用 r 重新定位；只有第 1 页的 ` 会走到这儿
    case SCREEN_ASTRO_SUN:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else            astroKey(k);
      return;
    case SCREEN_MOON: case SCREEN_ASTRO_TERM:
      astroKey(k);
      return;

    case SCREEN_COMPASS: case SCREEN_COMPASS_DETAIL:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else if (k == '\n' || k == ' ') { imuTare(); dirty = true; }
      return;

    case SCREEN_FILES:
      if (fileDelConfirm) {
        if (k == '`') { fileDelConfirm = false; dirty = true; }
        else if (k == '\n') {
          String full = joinPath(curPath, fileList[fileIdx].name);
          SD.remove(full);
          loadDir(curPath);
          dirty = true;
        }
        return;
      }
      if (k == '`') {
        if (curPath == "/") { cleanupApp(SCREEN_FILES); screen = SCREEN_MENU; }
        else {
          int slash = curPath.lastIndexOf('/');
          curPath = (slash <= 0) ? "/" : curPath.substring(0, slash);
          loadDir(curPath);
        }
        dirty = true;
      } else if (k == ';' || k == ',') {
        listMove(fileIdx, fileTop, fileCount, FILES_VIS, -1);
        dirty = true;
      } else if (k == '.' || k == '/') {
        listMove(fileIdx, fileTop, fileCount, FILES_VIS, 1);
        dirty = true;
      } else if (k == '\n') {
        if (fileCount > 0 && fileList && fileList[fileIdx].isDir) {
          curPath = joinPath(curPath, fileList[fileIdx].name);
          loadDir(curPath);
          dirty = true;
        } else if (fileCount > 0 && fileList && fileList[fileIdx].name.endsWith(".txt")) {
          readerFromFiles = true;
          readerOpen(joinPath(curPath, fileList[fileIdx].name));
          screen = SCREEN_READER;
          dirty = true;
        }
      } else if (k == '\b') {
        if (fileCount > 0 && fileList && !fileList[fileIdx].isDir) { fileDelConfirm = true; dirty = true; }
      }
      return;

    case SCREEN_READER_SHELF:
      readerShelfKey(k);
      return;

    case SCREEN_READER:
      if (k == '`') {
        bool fromFiles = readerFromFiles;
        // 退回文件列表：列表要留着（光标位置也在里面），所以先摘掉标志，
        // 免得 cleanupApp 把它当成"离开整个 Files"一起释放
        readerFromFiles = false;
        cleanupApp(SCREEN_READER);
        if (fromFiles) {
          screen = SCREEN_FILES;
        } else {
          screen = SCREEN_READER_SHELF;
          readerEnter();
        }
        dirty = true;
      }
      else if (k == '.' || k == '/') { readerNextPage(); dirty = true; }
      else if (k == ';' || k == ',') { readerPrevPage(); dirty = true; }
      else if (k == ']') { readerJump(+1); dirty = true; }   // 快进 ~5%
      else if (k == '[') { readerJump(-1); dirty = true; }   // 快退 ~5%
      else if (k == 'f' || k == 'F') { readerCycleFontSize(); dirty = true; }
      else if (k == '+' || k == '=') {
        if (readerFontSize < FONT_SIZE_COUNT - 1) readerSetFontSize(readerFontSize + 1);
        dirty = true;
      }
      else if (k == '-' || k == '_') {
        if (readerFontSize > 0) readerSetFontSize(readerFontSize - 1);
        dirty = true;
      }
      return;

    case SCREEN_GNSS:
      // 只有第 1 页会带着 ` 走到这儿（后面几页的 ` 上面已经折回第 1 页了）。
      // 地图拆走之后这几页不占任何资源，退出无需清理。
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      return;

    case SCREEN_GNSS_SPEED:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else gnssSpeedKey(k);
      return;

    case SCREEN_GNSS_DETAIL: gnssDetailKey(k); return;
    case SCREEN_GNSS_TRIP:   gnssTripKey(k);   return;

    // 这页的 ` 上面的翻页块已经拦掉了（折回 GNSS 第 1 页），这里只处理页内按键
    case SCREEN_GNSS_SAT:    gnssSatKey(k);    return;

    case SCREEN_GNSS_CONFIG:
      if (k == '`') { screen = SCREEN_SETTINGS; dirty = true; return; }
      gnssConfigKey(k); return;

    case SCREEN_GNSS_MAP:
      // 独立 app 了，` 直接回主菜单并释放那 48KB（不再是 GNSS 的子页）
      if (k == '`') { cleanupApp(SCREEN_GNSS_MAP); screen = SCREEN_MENU; dirty = true; }
      else if (k == ']') gnssMapZoom(+1);   // 放大（换档会阻塞拉一次瓦片）
      else if (k == '[') gnssMapZoom(-1);
      return;

    case SCREEN_WIFI_SCAN:
      if (k == '`') { wifiScanExit(); screen = SCREEN_SETTINGS; dirty = true; }
      else if ((k == 'r' || k == 'R') && millis() - lastWifiScanMs >= 3000) { doScan(); dirty = true; }
      else if (scanCount > 0 && (k == ';' || k == ',')) {
        listMove(wifiIdx, wifiTop, scanCount, WIFI_VIS, -1);
        dirty = true;
      }
      else if (scanCount > 0 && (k == '.' || k == '/')) {
        listMove(wifiIdx, wifiTop, scanCount, WIFI_VIS, 1);
        dirty = true;
      }
      else if (scanCount > 0 && k == '\n') {
        selSSID = WiFi.SSID(wifiIdx);
        if (WiFi.encryptionType(wifiIdx) == WIFI_AUTH_OPEN) {
          // 开放网络，直接连
          bool ok = connectWith(selSSID.c_str(), "", true);
          if (ok) saveCreds(selSSID, "");
          centerMsg(ok ? "connected!" : "failed", ok ? TFT_GREEN : TFT_RED);
          delay(1200);
          wifiScanExit();
          screen = SCREEN_SETTINGS;
        } else {
          pwInput = ""; screen = SCREEN_WIFI_PW;
        }
        dirty = true;
      }
      return;

    case SCREEN_WIFI_PW:
      if (k == '`') { screen = SCREEN_WIFI_SCAN; dirty = true; }
      else if (k == '\b') { if (pwInput.length()) pwInput.remove(pwInput.length() - 1); dirty = true; }
      else if (k == '\n') {
        bool ok = connectWith(selSSID.c_str(), pwInput.c_str(), true);
        if (ok) saveCreds(selSSID, pwInput);
        centerMsg(ok ? "connected!" : "wrong pw / failed", ok ? TFT_GREEN : TFT_RED);
        delay(1200);
        if (ok) {
          wifiScanExit();
          screen = SCREEN_SETTINGS;
        } else {
          screen = SCREEN_WIFI_SCAN;
        }
        dirty = true;
      }
      else if (k >= 0x20 && k <= 0x7e && pwInput.length() < 63) { pwInput += k; dirty = true; }
      return;

    case SCREEN_RID:
      if (k == '`') { cleanupApp(SCREEN_RID); screen = SCREEN_MENU; dirty = true; }
      else if (k == '\n') { screen = SCREEN_RID_DETAIL; dirty = true; }
      else ridAppKey(k);
      return;

    case SCREEN_RID_DETAIL:
      if (k == '`') { screen = SCREEN_RID; dirty = true; }
      else if (k == 's' || k == 'S') { ridAppKey(k); dirty = true; }
      return;

    case SCREEN_BT_SCAN:
      if (k == '`') { cleanupApp(SCREEN_BT_SCAN); screen = SCREEN_MENU; dirty = true; }
      else if (k == 'r' || k == 'R') { btScan(); dirty = true; }
      else if (btDevCount > 0 && (k == ';' || k == ',')) {
        listMove(btDevIdx, btDevTop, btDevCount, BT_SCAN_VIS, -1);
        dirty = true;
      }
      else if (btDevCount > 0 && (k == '.' || k == '/')) {
        listMove(btDevIdx, btDevTop, btDevCount, BT_SCAN_VIS, +1);
        dirty = true;
      }
      else if (btDevCount > 0 && k == '\n') { screen = SCREEN_BT_DEVICE; dirty = true; }
      return;

    case SCREEN_BT_DEVICE:
      if (k == '`') { screen = SCREEN_BT_SCAN; dirty = true; }
      else if (k == '\n') { btRadarStart(btDevIdx); screen = SCREEN_BT_RADAR; dirty = true; }
      return;

    case SCREEN_BT_KEYBOARD:
      // 这一页除了 Fn+` 之外所有键都原样转发给主机（裸 ` 也要能打出去，所以退出
      // 挪到了 Fn+`；顶上的 GO 物理键同样能退，见 loop() 里 BtnA 的特判）。
      if ((uint8_t)k == kbd::KX_BACK) {
        cleanupApp(SCREEN_BT_KEYBOARD); screen = SCREEN_MENU; dirty = true;
        return;
      }
      // Fn+Enter 切 IME 模式。开着输入法时连发退格会把整串候选一口气吃光，所以一起关掉
      if ((uint8_t)k == kbd::KX_FN_ENTER) {
        btKeyboardToggleIme();
        kbd::setAutoRepeat(!btKeyboardImeMode());
        dirty = true;
        return;
      }
      btKeyboardType(k, kbd::modMask());
      kbd::consumeSticky();
      dirty = true;
      return;

    case SCREEN_BT_MEDIA:
      if (k == '`') { cleanupApp(SCREEN_BT_MEDIA); screen = SCREEN_MENU; dirty = true; }
      else if (k == ',')  btMediaSend(BT_MEDIA_PREV);
      else if (k == '/')  btMediaSend(BT_MEDIA_NEXT);
      else if (k == ';')  btMediaSend(BT_MEDIA_VOL_UP);
      else if (k == '.')  btMediaSend(BT_MEDIA_VOL_DOWN);
      else if (k == '\n') btMediaSend(BT_MEDIA_PLAYPAUSE);
      else if (k == 'm' || k == 'M') btMediaSend(BT_MEDIA_MUTE);
      else if (k == 's' || k == 'S') btMediaSend(BT_MEDIA_STOP);
      return;

    case SCREEN_BT_RADAR:
      if (k == '`') { btRadarStop(); screen = SCREEN_BT_DEVICE; dirty = true; }
      else if (k == 'm' || k == 'M') { btRadarToggleBeep(); dirty = true; }
      // 重置曲线：停掉再按同一个目标重开，省得为这一个动作单开一个 API
      else if (k == 'r' || k == 'R') { btRadarStop(); btRadarStart(btDevIdx); dirty = true; }
      return;

    case SCREEN_CHAT:
      if (k == '`') { cleanupApp(SCREEN_CHAT); screen = SCREEN_MENU; dirty = true; }
      else if (chatBusy()) { /* 请求进行中，输入一律忽略；` 仍然可以退出 */ }
      else if (k == '\b') { chatBackspace(); dirty = true; }
      else if (k == '\n') {
        // 非阻塞：起后台任务就返回，停在本页画 "asking..."。
        // 请求完成后由 loop() 里的 chatTakeReply() 负责切到回复页。
        if (chatInput.length() && !chatBusy()) chatSend();
        dirty = true;
      }
      else if (k >= 0x20 && k <= 0x7e && chatInput.length() < 200) { chatInput += k; dirty = true; }
      return;

    case SCREEN_CHAT_REPLY:
      if (k == '`') { chatInput = ""; screen = SCREEN_CHAT; dirty = true; }
      else if (k == ';' || k == ',') { if (chatPage > 0) chatPage--; dirty = true; }
      else if (k == '.' || k == '/') { if (chatPage < chatPageCount() - 1) chatPage++; dirty = true; }
      return;

    case SCREEN_SPECTRUM:
      if (k == '`') { cleanupApp(SCREEN_SPECTRUM); screen = SCREEN_MENU; dirty = true; }
      else { spectrumKey(k); dirty = true; }
      return;

    case SCREEN_NETPROBE:
      if (netprobeKey(k)) { cleanupApp(SCREEN_NETPROBE); screen = SCREEN_MENU; }
      dirty = true;
      break;

    case SCREEN_HASHOVEN:
      if (hashOvenKey(k)) { cleanupApp(SCREEN_HASHOVEN); screen = SCREEN_MENU; dirty = true; }
      return;

    case SCREEN_WARDRIVE:
      if (k == '`') { cleanupApp(SCREEN_WARDRIVE); screen = SCREEN_MENU; dirty = true; }
      else            wardriveKey(k);
      return;

    case SCREEN_DUCKY:
      if (k == '`') { cleanupApp(SCREEN_DUCKY); screen = SCREEN_MENU; dirty = true; }
      else            duckyKey(k);
      return;

    case SCREEN_LANSCAN:
      if (k == '`') { cleanupApp(SCREEN_LANSCAN); screen = SCREEN_MENU; dirty = true; }
      else            lanscanKey(k);
      return;

    case SCREEN_WEATHER: case SCREEN_WEATHER_HOUR:
    case SCREEN_WEATHER_AIR: case SCREEN_WEATHER_AQI: case SCREEN_WEATHER_FC:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }   // 只可能是第 1 页
      else weatherKey(k);                                     // 六页共用一个 r=刷新
      return;


    case SCREEN_ADSB:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else            adsbKey(k);
      return;

    case SCREEN_TYPHOON: case SCREEN_TYPHOON_TRACK:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }   // 两页共用 ;/. 切台风、r 刷新
      else            typhoonKey(k);
      return;

    case SCREEN_QUAKE: case SCREEN_QUAKE_MAP:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }   // 两页共用 ;/. 选条目、r 刷新、m 换源
      else            quakeKey(k);
      return;

    case SCREEN_FX: case SCREEN_FX_DAYS:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }   // 两页共用 ;/. 翻条目、r 刷新、m 换窗口
      else            fxKey(k);
      return;

    case SCREEN_OKX:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else            okxKey(k);
      return;

    case SCREEN_SATS:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else            satsKey(k);
      return;

    case SCREEN_ROUTER:
      if (routerKey(k)) { cleanupApp(SCREEN_ROUTER); screen = SCREEN_MENU; dirty = true; }
      return;

    case SCREEN_GITHUB:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else            githubKey(k);
      return;

    case SCREEN_WIFI_CHAN:
      if (k == '`') { cleanupApp(SCREEN_WIFI_CHAN); screen = SCREEN_MENU; dirty = true; }
      else          { wifiChanKey(k); }
      return;

    case SCREEN_WIFI_CHAN_DETAIL:
      if (k == '`') { screen = SCREEN_WIFI_CHAN; dirty = true; }
      else if (chanCount > 0 && (k == ';' || k == ',')) { listMoveIndex(chanSel, chanCount, -1); dirty = true; }
      else if (chanCount > 0 && (k == '.' || k == '/')) { listMoveIndex(chanSel, chanCount, 1); dirty = true; }
      return;

    case SCREEN_WSNIFF:
      if (k == '`') { cleanupApp(SCREEN_WSNIFF); screen = SCREEN_MENU; dirty = true; }
      else            wsniffKey(k);
      return;

    case SCREEN_CALC:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else            calcKey(k);
      return;

    case SCREEN_CONV:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else            convKey(k);
      return;

    case SCREEN_PLAYER:
      if (k == '`') { cleanupApp(SCREEN_PLAYER); screen = SCREEN_MENU; dirty = true; }
      else            playerKey(k);
      return;

    case SCREEN_BADAPPLE:
      if (k == '`') { cleanupApp(SCREEN_BADAPPLE); screen = SCREEN_MENU; dirty = true; }
      else            badappleKey(k);
      return;

    case SCREEN_RADIO:
      if (k == '`') { cleanupApp(SCREEN_RADIO); screen = SCREEN_MENU; dirty = true; }
      else            radioKey(k);
      return;

    case SCREEN_RADIO_PLAY:
      if (k == '`') { cleanupApp(SCREEN_RADIO_PLAY); screen = SCREEN_MENU; dirty = true; }
      else            radioKey(k);
      return;

    case SCREEN_RADIO_URL:
      radioUrlKey(k);
      return;

    case SCREEN_LORA:
      if (k == '`') { cleanupApp(SCREEN_LORA); screen = SCREEN_MENU; dirty = true; }
      else            loraKey(k);
      return;

    case SCREEN_IR:
      if (k == '`') { cleanupApp(SCREEN_IR); screen = SCREEN_MENU; dirty = true; }
      else            irKey(k);
      return;

    case SCREEN_HOTSPOT:
      if (k == '`') { screen = SCREEN_MENU; dirty = true; }
      else if (k == '\n') { hotspotToggle(); dirty = true; }
      else if (k == 'p' || k == 'P') { hotspotPwInput = hotspotPassword(); screen = SCREEN_HOTSPOT_PW; dirty = true; }
      else if (k == 'q' || k == 'Q') {
        hotspotEnsureOn();   // 扫出来的码得真能连上，所以进这页顺手把 AP 打开
        screen = SCREEN_HOTSPOT_QR; dirty = true;
      }
      return;

    case SCREEN_HOTSPOT_QR:
      if (k == '`') { screen = SCREEN_HOTSPOT; dirty = true; }
      return;

    case SCREEN_HOTSPOT_PW:
      if (k == '`') { screen = SCREEN_HOTSPOT; dirty = true; }
      else if (k == '\b') { if (hotspotPwInput.length()) hotspotPwInput.remove(hotspotPwInput.length() - 1); dirty = true; }
      else if (k == '\n') {
        if (hotspotPwInput.length() >= 8) { hotspotSetPassword(hotspotPwInput); screen = SCREEN_HOTSPOT; }
        dirty = true;
      }
      else if (k >= 0x20 && k <= 0x7e && hotspotPwInput.length() < 32) { hotspotPwInput += k; dirty = true; }
      return;

    case SCREEN_BRIGHTNESS:
      if (k == '`') { screen = SCREEN_SETTINGS; dirty = true; }
      else if (k == ';') { brightSet(min(100, brightPct + 5)); dirty = true; }
      else if (k == '.') { brightSet(max(5, brightPct - 5));   dirty = true; }
      return;

    case SCREEN_VOLUME:
      if (k == '`') { screen = SCREEN_SETTINGS; dirty = true; }
      else if (k == ';' || k == '.') {
        if (k == ';') volSet(min(100, volPct + 10));
        else          volSet(max(0, volPct - 10));
        if (!debugOn && volVal() > 0) M5.Speaker.tone(1200, 60);       // 调节时"嘀"一声反馈
        dirty = true;
      }
      return;

    case SCREEN_SLEEP:
      if (k == '`') { screen = SCREEN_SETTINGS; dirty = true; }
      else if (k == ';') { sleepSet((sleepOptIdx - 1 + SLEEP_OPT_COUNT) % SLEEP_OPT_COUNT); dirty = true; }
      else if (k == '.') { sleepSet((sleepOptIdx + 1) % SLEEP_OPT_COUNT); dirty = true; }
      return;

    case SCREEN_TZ:
      if (k == '`') { screen = SCREEN_SETTINGS; dirty = true; }
      else if (k == ';') { tzSet((tzOptIdx - 1 + TZ_OPT_COUNT) % TZ_OPT_COUNT); dirty = true; }
      else if (k == '.') { tzSet((tzOptIdx + 1) % TZ_OPT_COUNT); dirty = true; }
      return;

    case SCREEN_BATTERY:
      if (k == '`') { screen = SCREEN_SETTINGS; dirty = true; }
      return;

    case SCREEN_ABOUT:
      if (k == '`') { screen = SCREEN_SETTINGS; dirty = true; }
      else if (aboutPage == 2 && (k == '[' || k == '{')) { aboutRamScrollMove(-1); dirty = true; }
      else if (aboutPage == 2 && (k == ']' || k == '}')) { aboutRamScrollMove(+1); dirty = true; }
      else if (k == ';' || k == ',') { listMoveIndex(aboutPage, ABOUT_PAGE_COUNT, -1); dirty = true; }
      else if (k == '.' || k == '/') { listMoveIndex(aboutPage, ABOUT_PAGE_COUNT, 1); dirty = true; }
      return;

    case SCREEN_FORMAT:
      if (k == '`') { screen = SCREEN_SETTINGS; dirty = true; }   // 随时可取消
      else if ((k == 'y' || k == 'Y') && sdMounted) {
        if (formatStep == 0) { formatStep = 1; dirty = true; }    // 第一次确认
        else {                                                    // 第二次确认 → 执行
          centerMsg("formatting...", TFT_YELLOW);
          bool ok = formatSD();
          kbd::flushEvents();   // 删整张卡的时间跟文件数成正比（可能几分钟），期间的按键全丢掉
          centerMsg(ok ? "done - card erased" : "format failed", ok ? TFT_GREEN : TFT_RED);
          delay(1500);
          formatStep = 0; screen = SCREEN_SETTINGS; dirty = true;
        }
      }
      return;

    case SCREEN_SSH:
      if (sshKey(k)) { cleanupApp(SCREEN_SSH); screen = SCREEN_MENU; dirty = true; }
      return;

    case SCREEN_PCMODE:
      if (k == '`') { pcmodeExit(); }
      return;

    // 同 render()：哨兵单列，不用 default，好让漏配按键处理的新页面被 -Wswitch 抓住
    case SCREEN__COUNT:
      return;
  }
}

// 开机自检（POST）：逐行探测硬件、打终端日志。放在各硬件 init 之后调用
void bootSelfTest() {
  termLogReset();
  char b[40];
  termLogLine("boot> Cardputer ADV"); termLogDraw(true); delay(140);

  snprintf(b, sizeof(b), " . soc  %s x%d @%dMHz", ESP.getChipModel(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  termLogLine(b); termLogDraw(true); delay(110);
  snprintf(b, sizeof(b), " . mem  %dMB flash %s", (int)(ESP.getFlashChipSize() / (1024 * 1024)), psramFound() ? "psram" : "no-psram");
  termLogLine(b); termLogDraw(true); delay(110);
  snprintf(b, sizeof(b), " . disp %dx%d", SW, SH);
  termLogLine(b); termLogDraw(true); delay(110);

  bool ok;
  ok = M5.In_I2C.scanID(0x34);   // TCA8418 键盘
  snprintf(b, sizeof(b), " . keys  TCA8418  %s", ok ? "ok" : "--");
  termLogLine(b, ok ? 0 : TFT_RED); termLogDraw(true); delay(110);
  ok = M5.Imu.isEnabled();
  snprintf(b, sizeof(b), " . imu   BMI270   %s", ok ? "ok" : "--");
  termLogLine(b, ok ? 0 : TFT_RED); termLogDraw(true); delay(110);
  ok = M5.In_I2C.scanID(0x18);   // ES8311 音频 codec
  snprintf(b, sizeof(b), " . audio ES8311   %s", ok ? "ok" : "--");
  termLogLine(b, ok ? 0 : TFT_RED); termLogDraw(true); delay(110);

  if (sdMounted) snprintf(b, sizeof(b), " . sd    %lluMB  ok", (unsigned long long)sdSizeMB);
  else           snprintf(b, sizeof(b), " . sd    no card");
  termLogLine(b); termLogDraw(true); delay(110);
  snprintf(b, sizeof(b), " . gnss  cap      %s", gnssIoOk ? "ok" : "absent");
  termLogLine(b); termLogDraw(true); delay(110);
  snprintf(b, sizeof(b), " . heap  %dK  temp %.0fC", (int)(ESP.getFreeHeap() / 1024), temperatureRead());
  termLogLine(b); termLogDraw(true); delay(110);

  termLogLine("ready"); termLogDraw(false); delay(600);
}

// =============================================================================
void setup() {
  // GPIO5 不接是浮空的：平时没插扩展模块偏巧浮到能用的电平，一接上 GNSS/LoRa
  // Cap 模块（占用扩展总线）就被带偏，SD 卡 SPI 直接检测不到。Bruce 固件的
  // _setup_gpio() 里也专门注了这条「Set GPIO5 HIGH for SD card compatibility」。
  pinMode(5, OUTPUT);
  digitalWrite(5, HIGH);

  // ⚠️ 这一枪必须打在 M5.begin() **之前**：它记的是"静态段都分配完了、动态的还一个没花"
  // 那个时刻的余量，也就是 README 里 94,876 -> 119,420 该拿来对比的那个数。
  // 往后挪一行，它就变成"减去 M5Unified 那一坨之后"的数，没法跨版本比了。
  ramMark("boot");

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  brightInit();                           // 从 NVS 恢复屏幕亮度偏好
  M5.Display.setBrightness(brightVal());
  SW = M5.Display.width();
  SH = M5.Display.height();
  ramMark("M5.begin");

  // 画布+配色先建好，放在联网之前——先播开机动画，别被 WiFi 连接(可能阻塞十几秒)卡在黑屏
  canvasRestore();          // 画布缓冲的申请方式有讲究（TLSF 档位），统一走 globals.cpp
  ramMark("canvas");        // 这一步就是那 64KB（240×135×2），全项目最大的单笔分配
  ACCENT     = cv.color565(0, 230, 120);
  CARD_BG    = cv.color565(10, 34, 22);
  DIM_BORDER = cv.color565(30, 55, 44);
  ICON_DIM   = cv.color565(0, 120, 66);   // 未选中图标/次级文字：暗一档的同色系绿，不是灰
  themeMode  = loadUChar("sys", "theme", 0);
  if (themeMode > 0) ACCENT = getGroupTheme(themeMode - 1).primary;
  menuSnapSelection();                    // 高亮框落到开机默认那格，别从 (0,0) 飞进来

  // Speaker 参数得在 begin() 之前设。提前到 bootAnim 前，让开机动画能播放赛博合成音效
  {
    auto scfg = M5.Speaker.config();
    scfg.sample_rate = 44100;
    scfg.dma_buf_count = 16;
    scfg.task_priority = 5;
    M5.Speaker.config(scfg);
  }
  M5.Speaker.begin();
  M5.Speaker.setVolume(volVal());
  ramMark("speaker");

  kbd::begin();                           // 提前初始化键盘：开机动画期间敲任意键可瞬间跳过
  ramMark("kbd");

  debugInit();                            // 提前读取调试开关：调试模式下开机全机静音
  volInit();                              // 恢复系统音量偏好
  bootSoundInit();                        // 恢复开机音效偏好
  sleepInit();                            // 恢复熄屏超时偏好
  M5.Speaker.setVolume(volVal());
  bootAnim();
  ramMark("bootAnim");

  Serial.begin(115200);
  // 先回放上一轮的面包屑：如果上次是卡死后手动复位的，轨迹还在 RTC 里，这是唯一
  // 能看到"卡在哪一步"的机会（见 bctrail.h / docs/ble-teardown.md）。
  bcTrailInit();
  bcTrailDumpPrevious(Serial);
  Serial.printf("[boot] board id=%d\n", (int)M5.getBoard());
  ramMark("serial");        // 到这儿串口才活；上面四枪全靠攒着才看得到（见 ram_profile.h）

  tzInit();                             // 时区：从 NVS 恢复并立即应用
  clockInit();                          // 时钟表盘风格：从 NVS 恢复
  // 停滞看门狗要付 3KB 常驻栈，所以跟着 Debug 走，不白占（理由见 bctrail.cpp）
  if (debugOn) bcTrailStartWatchdog();
  ledInit();                            // 板载 RGB LED：恢复上次开/关状态
  weatherUnitsInit();                   // 天气单位：恢复上次公制/英制选择
  ramMark("kbd+nvs");
  sdInit();                             // 挂载 TF 卡（失败也不影响其它功能）
  Serial.printf("[boot] sd mounted=%d size=%lluMB\n", sdMounted, sdSizeMB);
  ramMark("sd");            // 挂上卡才有 FAT 缓冲；没插卡时这一行的 Δ 应该接近 0
  gnssInit();                            // 打开 GPS 天线开关 + 串口（没插 Cap 模块也不影响其它功能）
  Serial.printf("[boot] gnss io=%d antenna=%d\n", gnssIoOk, gnssAntennaOn);
  ramMark("gnss");

  bootSelfTest();                        // 开机自检：屏上打硬件检测日志（动画之后、联网之前）
  ramMark("selftest");

  // 开机连网+对时：用上次记住的网络自动连，连上之后**保持常开**（不再像早期版本那样
  // 对完时就关掉 radio）——这样天气/飞机/瓦片/Router 这些联网页面进去就能用。
  // 常开会踩一个坑：记住的网不在范围内时协议栈每 ~2.4s 重试一次（NO_AP_FOUND），期间
  // WiFi.scanNetworks() 会因为 STA 正忙而失败。那个坑现在由 doScan() 自己扛（扫描前先把
  // STA 复位一次），详见 wifi_net.cpp 顶部那段策略说明。
  // 没有记住的就先不连，跑 millis 时钟，等用户在设置→Wi-Fi 里选一次（之后会被记住）。
  // 这段之前是阻塞的 connectWith()（最坏 WiFi 15s + NTP 8s ≈ 23s 卡在黑屏），改成后台
  // 状态机：这里只踢一脚开始连，真正推进靠 loop() 里的 bootWifiUpdate()，开机动画播完
  // 立刻能进菜单，联网/对时在后台悄悄跑完。
  tlsCryptoWarmup();   // 必须在任何 CanvasLease 之前：理由见 tls_ca.cpp
  bootWifiStart();
  // ⚠️ 这里只是"踢了一脚"。协议栈那 ~36KB 是后台异步吃掉的，所以真正的账要等
  // wifi_net.cpp 里连上时那一枪（"wifi up"）才看得见——别把这一行的 Δ 当成 WiFi 的开销。
  ramMark("wifi start");

  lastActivityMs = millis();
  dirty = true;
}

// 串口调试通道：给电脑那头一个不用碰实体键盘也能操纵设备的口子。
// 单字符行原样当按键喂给 handleKey（覆盖 ;./`回车 这些导航键，也能给文本输入框打字）；
// 几个常用键给了大小写不敏感的别名方便手打；NETPROBE/MENU 是跳转快捷方式。
// BTN GO（顶部 G0 按键，M5.BtnA）：熄屏时只负责唤醒；在 app 里一键回主菜单；
// 已经在主菜单里就当翻页键用——一下跳到下一组，不用横着按满一整组才换得过去。
// "动作"部分单独拎出来，理由同 handleKeyAction：收尾重放时跳过唤醒/停闹钟那两道闸
static void onBtnAAction() {
  if (screen == SCREEN_MENU) {
    menuNextGroup();
  } else {
    cleanupApp(screen);   // 释放当前 app 的资源（跟 ` 退出同一份清理逻辑，覆盖所有子屏，不会漏）
    screen = SCREEN_MENU;
    menuSnapSelection();  // 直接落到选中格，不做"从上次位置滑进来"的动画
    dirty = true;
  }
}

static void onBtnA() {
  lastActivityMs = millis();
  if (stopwatchIsAlarming()) {
    stopwatchStopAlarm();
    dirty = true;
  } else if (screenOff) {
    M5.Display.setBrightness(brightVal());
    screenOff = false;
    ledApply();                             // SK6812 断电后状态丢失，立即重推一帧
    dirty = true;
  } else {
    onBtnAAction();
  }
}

// 后台拉取进行中的按键。停闹钟、熄屏唤醒这两种当场处理（handleKey 对它们处理完就返回，
// 既不碰 cv 也不碰页面数据）；其余的攒起来收尾后重放。
static void bgKeyWhileBusy(char k) {
  if (k == 0) return;
  if (stopwatchIsAlarming() || screenOff) { handleKey(k); return; }
  lastActivityMs = millis();
  if (bgLeaving) return;
  if ((uint8_t)k == kbd::KX_BACK || (uint8_t)k == kbd::KX_ESC) k = '`';   // 同 handleKey 的归一
  // "返回是不是要离开"得按排队的键**都执行完之后**会停在哪一页来判断，不能按眼前这一页：
  // 先按 . 翻到第 2 页再按 `，本意是折回第 1 页，按眼前（第 1 页）判断就成了退出 app。
  // 所以用一个影子屏幕把排队的翻页键先推演一遍，规则照抄 handleKey 开头那段分页处理。
  if (bgKeyN == 0) bgShadow = screen;
  int cur, cnt;
  const bool paged = pageIndex(bgShadow, cur, cnt);
  if (k == '`' && (!paged || cur == 0)) {
    // 第 1 页（或没有分页）的返回 = 离开这个 app：在途请求当场取消，收尾后再真的退出。
    // 之前排的键都不要了（要走了，翻页、选中都没意义）。
    bgLeaving = true;
    bgFetchCancel();
    bgKeyN = 0;
    bgKeys[bgKeyN++] = '`';
    dirty = true;
    return;
  }
  if (bgKeyN >= (int)sizeof(bgKeys)) return;
  bgKeys[bgKeyN++] = k;
  if (paged) {
    if (k == '.' || k == '/')      bgShadow = pageStep(bgShadow, +1);
    else if (k == ';' || k == ',') bgShadow = pageStep(bgShadow, -1);
    else if (k == '`')             bgShadow = pageFirst(bgShadow);
  }
}

static void bgBtnAWhileBusy() {
  if (stopwatchIsAlarming() || screenOff) { onBtnA(); return; }   // 同上：这两支不碰页面
  lastActivityMs = millis();
  bgPendingBtnA = true;
  bgLeaving = true;
  bgFetchCancel();
  dirty = true;
}

// 收尾之后：把忙时攒下的动作按原顺序补上。BtnA 优先（它本来就是"不管在干嘛，回主菜单"）。
static void bgReplay() {
  char keys[sizeof(bgKeys)];
  const int n = bgKeyN;
  memcpy(keys, bgKeys, n);
  const bool btnA = bgPendingBtnA;
  bgKeyN = 0;
  bgPendingBtnA = false;
  bgLeaving = false;
  // 走"动作"入口，不走 handleKey/onBtnA：拉取期间屏可能熄了、闹钟可能响了，
  // 那两道闸会把排着的第一个动作当成唤醒/停闹钟吃掉（按了返回却没走）。
  // 熄屏时照样执行（离开就回到主菜单，下次亮屏看到的就是它），不顺便把屏点亮。
  if (btnA) { onBtnAAction(); return; }
  for (int i = 0; i < n; i++) handleKeyAction(keys[i]);
}

void loop() {
  bcTrailHeartbeat();   // 喂停滞看门狗：这一行不再被执行，5 秒后它就会把面包屑打出来
  M5.update();
  bootWifiUpdate();   // 推进开机后台连网/对时状态机；连完/超时后自动变 no-op
  if (bgFetchService()) bgReplay();   // 后台拉取刚结束：画布已要回来，补上忙时攒下的按键

  // 后台拉取进行中：按键先攒着、串口指令先留在接收缓冲里（见 bg_fetch.h 的线程边界）
  if (bgFetchBusy()) {
    bgKeyWhileBusy(kbd::readKey());
    if (M5.BtnA.wasClicked()) bgBtnAWhileBusy();
  } else {
    handleKey(kbd::readKey());
    serialCmdPoll();
    if (M5.BtnA.wasClicked()) onBtnA();
  }

  // 主菜单高亮框缓动：每帧往目标格拉一截，动画期间把帧率提上去（见循环末尾的 delay）
  bool menuAnim = (screen == SCREEN_MENU) && menuUpdateAnim();
  if (menuAnim) dirty = true;

  // 顶栏时间/走秒每秒刷新（Battery 页也借用这个 1 秒心跳刷新电压/电流读数）
  if (screen == SCREEN_MENU || screen == SCREEN_CLOCK || screen == SCREEN_BATTERY ||
      screen == SCREEN_HOTSPOT || screen == SCREEN_MOON || screen == SCREEN_ADSB) {
    int h, m, s; nowHM(h, m, s);
    if (s != lastSecond) { lastSecond = s; dirty = true; }
  }
  if (screen == SCREEN_CLOCK && clockNeedsFastUpdate()) dirty = true;

  // IMU：传感器数据实时刷新
  if (screen == SCREEN_COMPASS || screen == SCREEN_COMPASS_DETAIL) {
    imuSample();
    dirty = true;
  }

  // Wi-Fi 看门狗：常开+自动重连策略的兜底。下面这些页面自己拿着射频（混杂模式/AP/扫描），
  // 这时候别去抢模式；它们一退出，看门狗就把 STA 拉回来。
  {
    bool radioBusy = (screen == SCREEN_WIFI_CHAN || screen == SCREEN_WIFI_CHAN_DETAIL ||
                      screen == SCREEN_WSNIFF   || screen == SCREEN_WARDRIVE ||
                      screen == SCREEN_RID      || screen == SCREEN_RID_DETAIL ||
                      screen == SCREEN_LANSCAN  || screen == SCREEN_WIFI_SCAN ||
                      screen == SCREEN_WIFI_PW  || screen == SCREEN_HOTSPOT ||
                      screen == SCREEN_HOTSPOT_PW || screen == SCREEN_HOTSPOT_QR ||
                      chatBusy() || radioIsActive() || bgFetchBusy() || ridStreamIsActive() || btStreamIsActive() ||
                      (screen == SCREEN_SSH && sshIsTerminalActive()) ||
                      isBleScreen(screen));
    // ⚠️ BLE 还占着堆时不要去拉 STA——那 37KB 花下去也连不上，纯亏。
    // 2026-08-26 实测（用过蓝牙后退回主菜单，STAT 连读）：
    //     BLE 扫描中   wifi=down  heap=45684
    //     刚回主菜单   wifi=down  heap= 8432   ← 看门狗 init 了 Wi-Fi，吃掉 37KB
    //     停 16 秒后   wifi=down  heap= 8916   ← 连不上，也不释放，不自愈
    // 两条各自合理的规则撞在一起：btReleaseForOtherApps() 认为"停在主菜单不算去用别的
    // app"所以不放 BLE（为了蓝牙里进出零开销），而看门狗认为"主菜单不 radioBusy"所以
    // 该把 STA 拉回来。结果是钱花了、网没连上、堆掉到 8KB，得等进了别的 app 触发释放
    // 才活过来。bt.cpp 顶上那段注释预言到了一半（挂起状态下 esp_wifi_init 会失败），
    // 但没料到它是**先申请、后失败、然后一直占着**。
    // 加这一条之后菜单上剩 ~45KB 而不是 8KB，Wi-Fi 状态不变（本来就是 down），
    // 等 BLE 真被释放了看门狗自然会把网拉回来。
    wifiKeeperUpdate(!radioBusy && !btHoldsHeap());
  }

  // 蓝牙用完只是挂起（BLE 对象全留着，蓝牙里进进出出零开销）。一旦真的去用别的 app，
  // 就得把内存还给 WiFi——挂起状态下堆不够 esp_wifi_init 用。
  // 若停在主菜单但光标已经移出 HID/BLE 组，或在主菜单停留超过 30 秒，则自动彻底释放 BLE，
  // 允许 Wi-Fi 看门狗自愈连网，避免在菜单长期待机时 Wi-Fi 永久假死或后续引发内存碎片化。
  {
    static uint32_t menuBtHoldStart = 0;
    // PC MODE 里 BLE 流停掉后保持挂起（反复开关零泄漏）；要用 Wi-Fi 的命令自己先释放
    if (screen != SCREEN_MENU && !isBleScreen(screen) && screen != SCREEN_PCMODE) {
      btReleaseForOtherApps();
      menuBtHoldStart = 0;
    } else if (screen == SCREEN_MENU && btHoldsHeap() && !btIsConnected()) {
      if (menuBtHoldStart == 0) menuBtHoldStart = millis();
      bool inBleArea = (menuGroupOf(menuIndex) == 3 || APPS[menuIndex].id == APP_BTSCAN);
      if (!inBleArea || millis() - menuBtHoldStart >= 30000) {
        btReleaseForOtherApps();
        menuBtHoldStart = 0;
      }
    } else {
      menuBtHoldStart = 0;
    }
  }

  // 充电状态推测（电压趋势）+ 板载 RGB LED 电量指示；都跟屏幕无关，每帧刷
  powerUpdate();
  ledUpdate();

  // GNSS & IMU & LoRa：串口流式输出（PC 主导模式或其他模式下开启）
  gnssPoll();
  gnssStreamTick();
  imuStreamTick();
  loraSniffTick();
  if (screen == SCREEN_GNSS || screen == SCREEN_GNSS_DETAIL || screen == SCREEN_GNSS_SAT ||
      screen == SCREEN_GNSS_CONFIG || screen == SCREEN_GNSS_SPEED || screen == SCREEN_GNSS_TRIP) dirty = true;
  // 地图页陆地/海洋背景已经缓存成静态画布（见 gnss.cpp 的 mapBg），重画开销不大了，
  // 但 GPS 模块本身也就 1Hz 更新一次定位，跟前两页一样卷到每帧刷新纯属浪费。
  static uint32_t gnssMapLastMs = 0;
  if (screen == SCREEN_GNSS_MAP) {
    gnssMapUpdate();                 // 分帧拉瓦片（自己会翻 dirty）
    if (millis() - gnssMapLastMs >= 1000) { gnssMapLastMs = millis(); dirty = true; }
  }

  // BLE 键盘/媒体遥控：连接状态是从 BLE 回调异步翻的，这里每帧强制重画才能及时看到"connected"
  if (screen == SCREEN_BT_KEYBOARD || screen == SCREEN_BT_MEDIA) dirty = true;

  // 找物雷达：采样进曲线 + 驱动蜂鸣，全在主循环做（扫描回调只写最新 RSSI，见 bt.cpp）
  if (screen == SCREEN_BT_RADAR) { btRadarUpdate(); dirty = true; }

  // 音乐播放器：自动切歌、电平动效与状态维护
  if (screen == SCREEN_PLAYER) playerUpdate();

  // Bad Apple：按时间线推进播放（自己按帧号节流、读到新帧才翻 dirty，不用外面再限频）
  if (screen == SCREEN_BADAPPLE) badappleUpdate();

  // 收音机：后台任务退出检测、挂起动作执行、状态串口诊断与直推屏幕刷新
  radioUpdate();

  // 频谱分析：每帧录一段音频、做 FFT，柱状图持续动
  if (screen == SCREEN_SPECTRUM) { spectrumUpdate(); dirty = true; }

  // LoRa 嗅探：每帧扫频点 / 收包
  if (screen == SCREEN_LORA) { loraUpdate(); dirty = true; }


  // WiFi 嗅探：逐信道跳 + 每帧刷新计数/列表
  if (screen == SCREEN_RID || screen == SCREEN_RID_DETAIL) { ridAppUpdate(); dirty = true; }
  else if (ridStreamIsActive()) ridAppUpdate();
  btStreamPump();   // PC 主导模式的 BLE 流：从回调队列取报告、限频、往串口吐行
  if (screen == SCREEN_WSNIFF) { wsniffUpdate(); dirty = true; }
  if (screen == SCREEN_NETPROBE) { netprobeUpdate(); dirty = true; }
  if (screen == SCREEN_HASHOVEN) { hashOvenUpdate(); }
  if (screen == SCREEN_WIFI_CHAN || screen == SCREEN_WIFI_CHAN_DETAIL) { wifiChanUpdate(); }
  if (screen == SCREEN_WARDRIVE) { wardriveUpdate(); dirty = true; }
  if (screen == SCREEN_DUCKY)    { duckyUpdate(); dirty = true; }
  if (screen == SCREEN_LANSCAN)  { lanscanUpdate(); dirty = true; }
  if (screen == SCREEN_SSH)      { sshUpdate(); }
  if (screen == SCREEN_PCMODE)   { pcmodeUpdate(); }

  // 计算器：光标闪烁与按键回弹动画
  static uint32_t calcBlinkMs = 0;
  if (screen == SCREEN_CALC) {
    if (millis() - calcBlinkMs >= 250) { calcBlinkMs = millis(); dirty = true; }
  }

  // 秒表/倒计时：不管在哪个屏幕都要推进（倒计时到点要响铃、要把屏幕叫醒），
  // 但只有停在这一页时才需要每帧重画那行大字。
  stopwatchUpdate();
  if ((screen == SCREEN_STOPWATCH && stopwatchBusy()) || stopwatchIsAlarming()) dirty = true;

  // 这几页要"活"的数据：停在页面上时按各自的节奏自动重拉。拉取本身交给后台工人（bg_fetch.h），
  // 不再卡主循环；Router 的轮询还是同步的（每次 2~4s、局域网直连，不值得挪）。
  // 后台拉取进行中这些页面一律不推进：工人正在写它们的数据，也别再叠一个新的拉取上去
  if (!bgFetchBusy()) {
    if (screen == SCREEN_ADSB)   adsbUpdate();
    if (screen == SCREEN_TYPHOON || screen == SCREEN_TYPHOON_TRACK) typhoonUpdate();
    if (screen == SCREEN_QUAKE   || screen == SCREEN_QUAKE_MAP)     quakeUpdate();
    if (screen == SCREEN_FX      || screen == SCREEN_FX_DAYS)       fxUpdate();
    if (screen == SCREEN_OKX)    okxUpdate();
    if (screen == SCREEN_IR)     irUpdate();   // 关机扫频要按节拍推进，不能在按键里 for 完
    if (screen == SCREEN_SATS)   satsUpdate();
    // ⚠️ 熄屏时别轮询 Router。它的轮询是同步阻塞的（打 2~4 个 HTTP，实测能把主循环钉住
    // 一两秒），而熄屏时屏幕上没有任何东西需要更新——继续轮询的唯一效果就是让"按键唤醒"
    // 要等当前那一轮跑完，表现为唤醒发木。这一页也没有任何后台职责（不像秒表要响铃、
    // 收音机要出声），停掉零损失。
    // 熄屏时不只是"不轮询"，还要主动**关掉**那条常开流——mihomo 每秒都在往里推，
    // 没人读的话数据就堆在 socket 缓冲区里，醒来第一眼看到的还是一堆过期样本。
    // trafficClose() 是幂等的，关过之后每帧就是个空调用。
    if (screen == SCREEN_ROUTER) { if (screenOff) routerExit(); else routerUpdate(); }
    if (screen == SCREEN_GITHUB) githubUpdate();
    // Weather 六页共用同一份数据，停在任意一页都要推进延后拉取（六页的 draw 都会 markShown）
    switch (screen) {
      case SCREEN_WEATHER:     case SCREEN_WEATHER_HOUR:
      case SCREEN_WEATHER_AIR: case SCREEN_WEATHER_AQI:  case SCREEN_WEATHER_FC:
        weatherUpdate(); break;
      default: break;
    }
    if (screen == SCREEN_ASTRO_SUN || screen == SCREEN_MOON || screen == SCREEN_ASTRO_TERM) astroUpdate();
  }

  // Chat 的后台任务不跟着页面走——人可以按 ` 走开，回来时回复已经排好版了。
  // 切页只在还停在 Chat 页时做，免得把人从别的 app 里拽走。
  chatUpdate();
  if (chatTakeReply() && screen == SCREEN_CHAT) { screen = SCREEN_CHAT_REPLY; dirty = true; }
  if (screen == SCREEN_CHAT && chatBusy()) dirty = true;   // 让 "asking..." 的省略号动起来
  if (bgFetchBusy()) dirty = true;   // 让后台拉取的指示动起来（画的频率在 bgFetchDrawIndicator 里自己限）

  // 无操作自动熄屏
  int sleepSec = SLEEP_OPTS[sleepOptIdx];
  if (!screenOff && sleepSec > 0 && millis() - lastActivityMs > (uint32_t)sleepSec * 1000) {
    M5.Display.setBrightness(0);
    screenOff = true;
  }

  if (dirty && !screenOff) { render(); dirty = false; }
  // IMU 或 LoRa 嗅探流开着时不能按 20ms 一帧跑：挂在主循环上，快速轮询中断标志以便在收包后立刻重新武装接收。
  // 流只在插着电脑时用，功耗无所谓；其它时候照旧 20ms 省电。
  delay(menuAnim ? 8 : ((imuStreamIsActive() || loraSniffIsActive()) ? 2 : 20));
}
