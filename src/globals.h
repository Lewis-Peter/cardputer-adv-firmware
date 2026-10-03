// 全局共享状态：画布、配色、屏幕枚举、顶层菜单/设置项定义、熄屏与动画状态。
// 各功能模块（wifi/gnss/imu/files/reader/settings_ui/ui_common/icons）都依赖这里的定义。
#pragma once
#include <M5Unified.h>
#include <Preferences.h>

extern Preferences prefs;   // NVS：wifi 凭据、阅读器进度等小配置共用一个句柄
// 这个句柄 begin()/end() 之间是独占的，而后台拉取的工人（geoGet 读写位置缓存）跟主线程（电量存盘）
// 可能同时用它：一边的 end() 会关掉另一边刚打开的命名空间，put 落进别人的命名空间里。
// 下面的 load*/save* 自带这把锁；别处直接 prefs.begin() 的代码只要可能在工人上跑，就自己套一个 PrefsLock。
// 递归锁：锁着的时候再调 load*/save* 不会把自己锁死。
struct PrefsLock {
  PrefsLock();
  ~PrefsLock();
  PrefsLock(const PrefsLock&) = delete;
  PrefsLock& operator=(const PrefsLock&) = delete;
};

bool loadBool(const char* ns, const char* key, bool def);
void saveBool(const char* ns, const char* key, bool value);
uint32_t loadUInt(const char* ns, const char* key, uint32_t def);
void saveUInt(const char* ns, const char* key, uint32_t value);
int loadInt(const char* ns, const char* key, int def);
void saveInt(const char* ns, const char* key, int value);
double loadDouble(const char* ns, const char* key, double def);
void saveDouble(const char* ns, const char* key, double value);
String loadString(const char* ns, const char* key, const String& def);
void saveString(const char* ns, const char* key, const String& value);
uint8_t loadUChar(const char* ns, const char* key, uint8_t def);
void saveUChar(const char* ns, const char* key, uint8_t value);

// ---- 画布 ----
extern M5Canvas cv;
extern int SW, SH;
// ACCENT=主色亮绿 / CARD_BG=选中卡片底 / DIM_BORDER=分隔线 / ICON_DIM=未选中图标和次级文字
// （ICON_DIM 是暗一档的同色系绿，不是灰——整屏统一在一个色调里，比"一堆灰的+一个绿的"整洁）
extern uint16_t ACCENT, CARD_BG, DIM_BORDER, ICON_DIM;

// ---- 顶层 app（分组网格启动器）----
enum AppId { APP_TIME, APP_ASTRO, APP_COMPASS, APP_FILES, APP_GNSS, APP_LORA, APP_BTSCAN, APP_BTKB, APP_BTMEDIA, APP_MAP, APP_RID, APP_CHAT, APP_SPECTRUM, APP_WIFICHAN, APP_WSNIFF, APP_HOTSPOT, APP_NETPROBE, APP_HASHOVEN, APP_CALC, APP_IR, APP_CONV, APP_PLAYER, APP_BADAPPLE, APP_RADIO, APP_WARDRIVE, APP_DUCKY, APP_LANSCAN, APP_WEATHER, APP_ADSB, APP_SATS, APP_ROUTER, APP_GITHUB, APP_TYPHOON, APP_QUAKE, APP_FX, APP_OKX, APP_READER, APP_SSH, APP_SETTINGS };
struct AppDef { const char* name; AppId id; };
extern const AppDef APPS[];
extern const int APP_COUNT;
extern const int MENU_COLS;   // 主菜单网格列数
extern int menuIndex;         // 当前选中的 app：APPS[] 的全局下标，不是组内下标

// app 分组：APPS[] 按组连续排列，每组正好塞得进一屏(MENU_COLS×2)，所以主菜单不用滚动。
// 左右键在组内走、走出边界就切到相邻组；顶部 tab 条显示共有哪几组、当前在哪一组。
struct GroupDef { const char* name; int first; int count; };
extern const GroupDef GROUPS[];
extern const int GROUP_COUNT;
int menuGroupOf(int appIdx);   // appIdx 属于哪一组

// ---- 设置项（二级列表）----
enum SetId { SET_WIFI, SET_PCMODE, SET_GNSS, SET_BRIGHT, SET_VOL, SET_BOOT_SOUND, SET_LED, SET_THEME, SET_SLEEP, SET_TZ, SET_WX_UNIT, SET_BATTERY, SET_DEBUG, SET_FORMAT, SET_ABOUT };
struct SetDef { const char* name; SetId id; };
extern const SetDef SETTINGS[];
extern const int SET_COUNT;
extern int settingsIndex;

// ---- 开机音效偏好 (true=开启, false=静音) ----
extern bool bootSoundOn;
void bootSoundInit();
void bootSoundSet(bool on);

// ---- UI 主题偏好 (0=Auto Reactive, 1..7=固定品类主色) ----
extern uint8_t themeMode;
void themeSet(uint8_t mode);
const char* themeName(uint8_t mode);


// ---- 屏幕 ----
enum Screen {
  SCREEN_MENU, SCREEN_CLOCK, SCREEN_SETTINGS,
  SCREEN_WIFI_SCAN, SCREEN_WIFI_PW, SCREEN_BT_SCAN, SCREEN_BT_DEVICE, SCREEN_BT_KEYBOARD,
  SCREEN_BT_MEDIA, SCREEN_BT_RADAR,
  SCREEN_BRIGHTNESS, SCREEN_VOLUME, SCREEN_SLEEP, SCREEN_TZ, SCREEN_BATTERY, SCREEN_FORMAT, SCREEN_ABOUT,
  SCREEN_COMPASS, SCREEN_COMPASS_DETAIL, SCREEN_FILES, SCREEN_READER, SCREEN_READER_SHELF, SCREEN_GNSS, SCREEN_GNSS_DETAIL, SCREEN_GNSS_SAT, SCREEN_GNSS_CONFIG, SCREEN_GNSS_MAP, SCREEN_GNSS_SPEED, SCREEN_GNSS_TRIP,
  SCREEN_CHAT, SCREEN_CHAT_REPLY, SCREEN_SPECTRUM, SCREEN_WIFI_CHAN, SCREEN_HOTSPOT, SCREEN_HOTSPOT_PW, SCREEN_HOTSPOT_QR,
  SCREEN_CALC, SCREEN_LORA, SCREEN_IR, SCREEN_WIFI_CHAN_DETAIL, SCREEN_WSNIFF,
  SCREEN_CONV, SCREEN_PLAYER, SCREEN_BADAPPLE, SCREEN_RADIO, SCREEN_RADIO_PLAY, SCREEN_RADIO_URL,
  SCREEN_NETPROBE, SCREEN_HASHOVEN, SCREEN_WARDRIVE, SCREEN_DUCKY, SCREEN_LANSCAN,
  SCREEN_STOPWATCH,
  // Astro：三页都是纯算的（给时间和位置就出结果，一个字节网络都不要），
  // 所以刻意跟 Weather 那一串联网页面分开。晨昏线连位置都不需要。
  SCREEN_ASTRO_SUN, SCREEN_MOON, SCREEN_ASTRO_TERM,
  SCREEN_WEATHER, SCREEN_WEATHER_HOUR, SCREEN_WEATHER_AIR,
  SCREEN_WEATHER_AQI, SCREEN_WEATHER_FC,
  SCREEN_ADSB, SCREEN_SATS, SCREEN_ROUTER, SCREEN_GITHUB,
  SCREEN_TYPHOON, SCREEN_TYPHOON_TRACK,
  // 地震：列表 + 世界地图打点。跟 Typhoon 分工见 quake.h（JMA 只管西北太平洋，USGS 是全球）
  SCREEN_QUAKE, SCREEN_QUAKE_MAP,
  // 汇率：现价+走势 / 逐日收盘。红涨绿跌，见 fx.cpp 的 upDownColor
  SCREEN_FX, SCREEN_FX_DAYS,
  SCREEN_OKX,                // OKX 的 USDT->CNY 换算率，单页
  SCREEN_RID, SCREEN_RID_DETAIL,
  SCREEN_SSH,
  SCREEN_PCMODE,             // PC 主导模式（释放全屏画布 cv，极简状态屏直推硬件显示）
  SCREEN__COUNT              // 哨兵：永远排在最后。串口 GOTO 的上界用它，
                             // 免得像以前那样写死成"当时的最后一个"，一加新页面就悄悄够不到
};
extern Screen screen;

// 「这一屏正占着 BLE」。**改上面那个枚举之前先看这里。**
//
// 这个判断原来在 main.cpp 里被写了三遍、还是三种写法：radioBusy 显式列举六个屏、
// onBtScreen 用枚举区间 `s >= SCREEN_BT && s <= SCREEN_BT_RADAR`、cleanupApp 再来一串
// case。区间那种写法**静默依赖 BT 那几个屏在枚举里连续**——往中间插一个屏、或者把某个
// BT 屏挪走，释放逻辑就会悄悄坏掉：要么 BLE 内存搁浅到重启，要么在错误的时机走
// BLEDevice::deinit()，而 deinit 是全项目唯一已知会永久阻塞整机的地方
// （见 docs/ble-teardown.md）。编译器一声不吭。
//
// 所以收成这一个函数，并且**刻意用显式列举而不是区间**：这样枚举怎么排都不影响正确性，
// 脆弱性是被消除而不是被注释掉。加了新的 BLE 页面，只要往这张表里补一行。
//
// Ducky 也算：它是靠 BLE HID 敲主机的，同样占着协议栈。
static inline bool isBleScreen(Screen s) {
  return s == SCREEN_BT_SCAN    || s == SCREEN_BT_DEVICE ||
         s == SCREEN_BT_KEYBOARD || s == SCREEN_BT_MEDIA  || s == SCREEN_BT_RADAR ||
         s == SCREEN_DUCKY;
}

// 退出某个 app 时该做的释放，实现在 main.cpp。各页的 ` 按键会调它；串口的 MENU 也要调，
// 否则从一个还占着内存的 app 里跳走会把那块内存搁浅到重启（地图那 48KB 最明显）。
void cleanupApp(Screen s);
void bootSelfTest();

// ---- 全局画布生命周期管理（按需释放/恢复 64.8KB 显存）----
void canvasRelease();
void canvasRestore();
bool canvasAvailable();

// 在一个作用域内把全屏画布（64.8KB）让给临时的大块分配（典型：TLS 握手要两块 ~16.7KB 缓冲），
// 出作用域自动要回来。**必须声明在那些临时对象之前**：C++ 逆序析构，保证它们先释放、画布后恢复。
// 规矩：作用域里只准有"用完就放"的分配（WiFiClientSecure 要在 lease 之前建好并 tlsUseCaBundle——
// 它每次都会重新 calloc 一块永不释放的证书索引）；要留下来的结果（数组、错误字符串）得在进来之前备好，
// 否则它会落进画布腾出来的那个洞里把它钉住，画布就再也要不回来了（见 globals.cpp 的 TLSF 说明）。
// 只在进来时画布确实在的情况下借/还：SSH、PC MODE、Chat 这些会自己释放画布，
// 它们没收回之前如果有人在这期间用了 lease，析构时不能替它们把画布要回来。
struct CanvasLease {
  CanvasLease() : held_(canvasAvailable()) { if (held_) canvasRelease(); }
  ~CanvasLease() { if (held_) canvasRestore(); }
  CanvasLease(const CanvasLease&) = delete;
  CanvasLease& operator=(const CanvasLease&) = delete;
 private:
  bool held_;
};
bool radioIsActive();

extern bool dirty;
extern int lastSecond;
extern int formatStep;        // 格式化二次确认步骤（0=首次警告，1=最终确认）

// ---- 无操作自动熄屏 ----
extern const int SLEEP_OPTS[];
extern const int SLEEP_OPT_COUNT;
extern int sleepOptIdx;
extern uint32_t lastActivityMs;
extern bool screenOff;
void sleepInit();
void sleepSet(int idx);

// ---- 调试开关（设置里可切，存 NVS）----
// 开了之后：① 屏幕底部常驻一条状态条（堆余量/最大连续块/WiFi/帧耗时），
// 不用插串口线就能看出"是不是内存不够了"；② 放开那些平时太吵的串口日志（比如每块地图瓦片一行）；
// ③ 全机静音（避免调试时开机音效或蜂鸣器频繁打扰）。
extern bool debugOn;
void debugInit();            // setup() 里调一次，从 NVS 恢复
void debugSet(bool on);      // 设置里切换，顺手存 NVS

// ---- 数值 ----
extern int brightPct;
extern int volPct;
void brightInit();
void brightSet(int pct);
void volInit();
void volSet(int pct);
static inline uint8_t brightVal() { return (uint8_t)(brightPct * 255 / 100); }
static inline uint8_t volVal()    {
  // 调试模式下（设置开启 Debug bar 或串口执行 DEBUG ON）自动全机静音
  if (debugOn) return 0;
  return (uint8_t)(volPct * 255 / 100);
}

// ---- 主菜单高亮框缓动 ----
// 高亮框不瞬间跳格，每帧往目标格拉一截。这几个量由 ui_common.cpp 的 menuSnapSelection()/
// menuUpdateAnim() 维护，drawMenu() 按它们画框。
// （原来这里是一套横向轮播的滑动动画 SLOT_W/ANIM_MS/animOffset/startSlide——菜单早就改成
//  网格了，那套东西没有任何地方调用过，只剩 loop() 每帧空跑它的状态机，一并删掉了。）
extern float menuSelX, menuSelY;

// ---- 时区 / 对时状态 ----
extern const char* TZ_INFO;
struct TzOpt { const char* label; const char* posix; };
extern const TzOpt TZ_OPTS[];
extern const int TZ_OPT_COUNT;
extern int tzOptIdx;
void tzInit();                         // setup() 里调一次，从 NVS 恢复
void tzSet(int idx);                    // 设置里切换，顺手存 NVS 并立即应用
extern bool timeSynced;
extern bool timeFromGps;   // 时间是 GPS 对的还是 NTP 对的（设置里区分显示）
