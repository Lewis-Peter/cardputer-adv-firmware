#include "globals.h"
#include "bg_fetch.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <ctime>

Preferences prefs;

// 静态存储：第一次用锁可能正好在工人的 CanvasLease 里（geoGet 存位置缓存），
// 往堆上建一个永不释放的小对象会落进画布腾出的洞里把画布钉死。
static SemaphoreHandle_t prefsMutex() {
  static StaticSemaphore_t buf;
  static SemaphoreHandle_t h = xSemaphoreCreateRecursiveMutexStatic(&buf);
  return h;
}
PrefsLock::PrefsLock()  { xSemaphoreTakeRecursive(prefsMutex(), portMAX_DELAY); }
PrefsLock::~PrefsLock() { xSemaphoreGiveRecursive(prefsMutex()); }

// 统一用读写方式打开：只读打开一个还不存在的命名空间会被 Preferences 刷一行
// "nvs_open failed: NOT_FOUND" 到串口，部分调用点（如 hotspot 页每秒轮询一次）会把日志刷满。
bool loadBool(const char* ns, const char* key, bool def) {
  PrefsLock lock;
  prefs.begin(ns, false);
  bool value = prefs.getBool(key, def);
  prefs.end();
  return value;
}

void saveBool(const char* ns, const char* key, bool value) {
  PrefsLock lock;
  prefs.begin(ns, false);
  prefs.putBool(key, value);
  prefs.end();
}

uint32_t loadUInt(const char* ns, const char* key, uint32_t def) {
  PrefsLock lock;
  prefs.begin(ns, false);
  uint32_t value = prefs.getUInt(key, def);
  prefs.end();
  return value;
}

void saveUInt(const char* ns, const char* key, uint32_t value) {
  PrefsLock lock;
  prefs.begin(ns, false);
  prefs.putUInt(key, value);
  prefs.end();
}

int loadInt(const char* ns, const char* key, int def) {
  PrefsLock lock;
  prefs.begin(ns, false);
  int value = prefs.getInt(key, def);
  prefs.end();
  return value;
}

void saveInt(const char* ns, const char* key, int value) {
  PrefsLock lock;
  prefs.begin(ns, false);
  prefs.putInt(key, value);
  prefs.end();
}

double loadDouble(const char* ns, const char* key, double def) {
  PrefsLock lock;
  prefs.begin(ns, false);
  double value = prefs.getDouble(key, def);
  prefs.end();
  return value;
}

void saveDouble(const char* ns, const char* key, double value) {
  PrefsLock lock;
  prefs.begin(ns, false);
  prefs.putDouble(key, value);
  prefs.end();
}

String loadString(const char* ns, const char* key, const String& def) {
  PrefsLock lock;
  prefs.begin(ns, false);
  String value = prefs.getString(key, def);
  prefs.end();
  return value;
}

void saveString(const char* ns, const char* key, const String& value) {
  PrefsLock lock;
  prefs.begin(ns, false);
  prefs.putString(key, value);
  prefs.end();
}

uint8_t loadUChar(const char* ns, const char* key, uint8_t def) {
  PrefsLock lock;
  prefs.begin(ns, false);
  uint8_t value = prefs.getUChar(key, def);
  prefs.end();
  return value;
}

void saveUChar(const char* ns, const char* key, uint8_t value) {
  PrefsLock lock;
  prefs.begin(ns, false);
  prefs.putUChar(key, value);
  prefs.end();
}

M5Canvas cv(&M5.Display);
int SW = 0, SH = 0;
uint16_t ACCENT, CARD_BG, DIM_BORDER, ICON_DIM;

// 画布缓冲自己申请、凑到 TLSF 的整档边界，再 setBuffer 交给 cv。
// IDF 4.4 的 TLSF 申请时把请求向上取整到下一档（64K 这一段每档 2KB）再找块，保证档里任何一块都够；
// 释放出来的块却按实际大小归档。createSprite 要 64800 字节（+12 字节 heap poisoning 头尾，再加 LovyanGFX 自己的对齐余量，实测块 64816），
// 释放后落在 [63488, 65536) 档，而再申请 64800 会被取整到 >=65536 档 —— **原样大小再也申请不回来**。
// PC MODE 退出后画布恢复失败就是这个：HEAPDUMP 里那 64816 字节完整空闲，largest 却只报 63476。
// 让实际块正好是 65536（档位起点）：申请 65536-12，取整后仍在同一档，释放后也回到同一档，自洽。
// 12 是 CONFIG_HEAP_POISONING_LIGHT 的头尾开销（依赖已锁版本）；万一变了还有下面按原尺寸重试兜底。
// 这是 IDF 分配器自己的 bug：espressif/tlsf 81d37795 "Fix block allocation size"，IDF v5.4 才带上；
// 升到 v5.4+ 之后这套对齐可以删掉，直接 createSprite。
static void* cvMem = nullptr;
static const size_t CANVAS_POISON_OVERHEAD = 12;   // 8 字节头(canary+长度) + 4 字节尾 canary

static size_t canvasAllocBytes() {
  size_t need = (size_t)SW * SH * 2;
  const size_t cls = 65536 - CANVAS_POISON_OVERHEAD;
  return need < cls ? cls : need;
}

void canvasRelease() {
  if (cv.getBuffer()) {
    cv.deleteSprite();   // setBuffer 进去的缓冲库不会 free，这里只是解绑
  }
  if (cvMem) {
    heap_caps_free(cvMem);
    cvMem = nullptr;
    if (debugOn) Serial.printf("[canvas] released\n");
  }
}

void canvasRestore() {
  // 收音机后台任务尚未退出时，解码器和源缓冲仍占着连续内存，
  // 此时强行申请画布会导致分配失败或把碎片钉死，必须等任务注销后再恢复。
  if (radioIsActive()) return;
  // 后台拉取进行中：画布归工人（它 lease 里释放的），谁都别在这时候要回来——主线程这边会跟
  // 工人的 TLS 抢那 64KB，工人那边 lease 析构时恢复又可能早于它线程本地的东西被释放、把洞钉住。
  // 统一由 bgFetchService() 在删掉工人任务之后恢复（见 bg_fetch.h）。
  if (bgFetchBusy()) return;
  if (!cv.getBuffer() && SW > 0 && SH > 0) {
    // DMA：跟 createSprite 默认一致，pushSprite 走 SPI DMA 要求
    cvMem = heap_caps_malloc(canvasAllocBytes(), MALLOC_CAP_DMA);
    if (!cvMem) cvMem = heap_caps_malloc((size_t)SW * SH * 2, MALLOC_CAP_DMA);   // 块合并变大时还能要到
    if (!cvMem) {
      if (debugOn) Serial.printf("[canvas] restore FAILED\n");
      return;
    }
    cv.setColorDepth(16);
    cv.setBuffer(cvMem, SW, SH, 16);
    cv.setTextDatum(top_left);
    cv.setFont(&fonts::Font0);
    if (debugOn) Serial.printf("[canvas] restore OK\n");
  }
}

bool canvasAvailable() {
  return cv.getBuffer() != nullptr;
}

// APPS[] 按分组连续排列：drawMenu() 直接拿 GROUPS[] 里的 [first, first+count) 当一页画。
// 每组都控制在一屏(MENU_COLS×2=8)以内，所以主菜单不再需要上下滚动——以前 21 个 app 平铺
// 成 6 行、一屏只看得到 2 行，永远不知道下面还有什么。
// ⚠️ 往这里加 app 一定要同步改 GROUPS[]（下面的 static_assert 会在忘记时把编译拦下来）。
const AppDef APPS[] = {
  // --- TOOLS：本地工具，不依赖任何射频 ---
  // Clock 和 Timer 合并成 Time（时钟 / 秒表+倒计时 两页）——它们本来就是"看时间"的一件事，
  // 分成两格纯属历史。腾出来的那一格给了 Astro（见 SKY 组）。
  {"Time",      APP_TIME},     {"IMU",      APP_COMPASS},  {"Files",    APP_FILES},
  {"Spectrum",  APP_SPECTRUM}, {"Calc",     APP_CALC},     {"Converter",APP_CONV},    {"Player",   APP_PLAYER},
  {"Hash Oven", APP_HASHOVEN},
  // --- SCAN：被动找东西。全靠混杂模式/扫描，**一个都不需要真的连上网**，
  //     所以没网、没路由器的地方这四个照样能用——这条界线用户能直接感觉到。
  //     （原来它们和下面 NET 那四个挤在一个 8 格满员的 NETWORK 组里，加不进任何东西。）
  //     BLE Scan 也归这里：它跟上面四个是同一件事——找周围有什么，不需要连上谁。
  //     （原来它是 Bluetooth app 里的一个子屏，跟"当键盘用"绑在一起纯属共享一次 BLE init。）
  {"WiFi Chan", APP_WIFICHAN}, {"Sniffer",  APP_WSNIFF},   {"Wardrive", APP_WARDRIVE}, {"Drone ID", APP_RID},
  {"BLE Scan",  APP_BTSCAN},
  // --- NET：主动收发，要真连上（或自己当 AP）才有意义 ---
  {"Hotspot",   APP_HOTSPOT},  {"LAN Scan", APP_LANSCAN},  {"NetProbe", APP_NETPROBE},
  {"Router",    APP_ROUTER},   {"SSH",      APP_SSH},
  // --- SIGNAL：Wi-Fi 以外的收发通道。叫 SIGNAL 不叫 RADIO 是因为红外是光不是射频，
  //     而这四个的共同点是"隔空收发信号"（LoRa/BLE 收发、GNSS 只收、IR 只发）。
  //     Drone ID 曾经也塞在这一组，但它是靠 esp_wifi_set_promiscuous 蹭 Wi-Fi 扫的，
  //     跟这四个的收发硬件毫无关系，挪去 NETWORK 跟 Sniffer 归一类了 ---
  // --- HID：把这台设备当键盘/遥控器去敲别人。三个出口原本散在两个组里
  //     （BT Keys / BT Media 藏在 Bluetooth app 里，Ducky 在 MORE），但它们是同一件事，
  //     只是一个走 BLE、一个走 USB ---
  {"BT Keys",   APP_BTKB},     {"BT Media", APP_BTMEDIA},  {"Ducky",    APP_DUCKY},
  // --- SIG：隔空收发信号 + 定位。⚠️ 名字从 SIGNAL 缩成 SIG 是被 tab 宽度逼的，
  //     7 组时每格只有 240/7 = 34px，而 "SIGNAL" 要 36px——下面有编译期检查兜着 ---
  //     Map 从 GNSS 的第 5 个子页拆出来单独成 app：它自己就是个完整的联网应用
  //     （HTTP 瓦片状态机 + 48KB 底图缓存 + WGS-84→GCJ-02 换算），跟"看当前定位"不是一件事。
  //     拆出来最实在的好处是那 48KB 的申请从"翻页翻到就悄悄发生"变成一次明确的进入动作。
  //     摆在 GNSS 旁边是因为没锁定时你多半要先去隔壁看一眼 fix 状态。
  {"LoRa",      APP_LORA},     {"GNSS",     APP_GNSS},      {"Map",      APP_MAP},
  {"IR",        APP_IR},
  // --- SKY：抬头看天的（天文 / 天气 / 头顶的飞机 / 头顶的卫星）---
  //     Astro 是从别处收拢来的：月相和晨昏线原本是 Clock 的第 2、3 页，日照是 Weather 的
  //     第 3 页。三处都是"给时间和位置就能算"的天文页，散在两个 app 里纯属历史。
  //     ⚠️ 它跟这一组其余三个的关键区别：**完全不联网**，晨昏线连位置都不要。
  //     Quake 摆在 Typhoon 旁边：都是"天灾预警"，都靠一个免 key 的公开 JSON 源，
  //     也都用 worldmap.h 那张陆地掩码打点。区别是 JMA 只发西北太平洋，USGS 是全球的。
  {"Astro",     APP_ASTRO},    {"Weather",  APP_WEATHER},  {"Planes",   APP_ADSB},
  {"Sats",      APP_SATS},     {"Typhoon",  APP_TYPHOON},  {"Quake",    APP_QUAKE},
  // --- MORE：上面几类都不沾边的。Ducky 是 USB HID（插线敲键盘），以前挂在 RADIO 下面
  //     纯属分类错误。Router 是路由器网络监控，已归入 NET 组 ---
  //     FX 也归这里：它跟 GitHub/Router 一样是"盯一个外部数字"的个人仪表盘，
  //     不是网络工具。Settings 永远排最后。
  {"ChatGPT",   APP_CHAT},     {"GitHub",   APP_GITHUB},    {"Radio",    APP_RADIO},
  {"FX",        APP_FX},       {"OKX",      APP_OKX},       {"Reader",   APP_READER},
  {"BadApple",  APP_BADAPPLE}, {"Settings",  APP_SETTINGS},
};
const int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);

// ⚠️ 组名最多 6 个字符：tab 条是 SW/GROUP_COUNT 平分屏宽，6 组时每格 240/6 = 40px，
// 而 Font0 是 6px/字符——"SIGNAL" 已经占到 36px，只剩 2px 边距。再加组或起更长的名字之前
// 先算这笔账（ui_common.cpp 的 drawGroupTabs）。
constexpr GroupDef GROUPS[] = { {"TOOLS", 0, 8}, {"SCAN", 8, 5}, {"NET", 13, 5},
                                {"HID", 18, 3}, {"SIG", 21, 4}, {"SKY", 25, 6},
                                {"MORE", 31, 8} };
const int GROUP_COUNT = sizeof(GROUPS) / sizeof(GROUPS[0]);

const int MENU_COLS = 4;

// 护栏。原来这里只有一条"APPS 数量 == 30"的 static_assert，配一句注释说 GROUPS 的覆盖范围
// 没法在编译期算——其实能算，把 GROUPS 变成 constexpr 就行。数量那条只能提醒你"去看一眼"，
// 下面这条是真的替你把三件事都验了：
//   ① 各组首尾相接（first 必须正好接上一组的末尾）——不留缝、不重叠
//   ② 每组不超过一屏（MENU_COLS×2），否则主菜单会画到屏幕外且没有滚动
//   ③ 合起来正好盖满 APPS[]，不多不少
// 菜单重排要反复动这两张表，人肉核对迟早出错，而错法是静默的（某个 app 从菜单里消失、
// 或者两组重叠着显示同一个）。
// ⚠️ 写成递归而不是 for 循环：这套工具链是 gnu++11，C++11 的 constexpr 函数只允许
// 单条 return 语句（GCC 会报 "body of constexpr function not a return-statement"）。
constexpr bool groupsChainOk(int g, int expect) {
  return g == (int)(sizeof(GROUPS) / sizeof(GROUPS[0]))
           ? expect == (int)(sizeof(APPS) / sizeof(APPS[0]))          // ③ 正好盖满
           : (GROUPS[g].first == expect                               // ① 首尾相接
              && GROUPS[g].count >= 1
              && GROUPS[g].count <= MENU_COLS * 2                     // ② 每组不超过一屏
              && groupsChainOk(g + 1, expect + GROUPS[g].count));
}
static_assert(groupsChainOk(0, 0),
              "GROUPS[] 和 APPS[] 对不上：各组必须首尾相接盖满 APPS[]，且每组 <= MENU_COLS*2");

// 组名放不放得进 tab 条，也让编译器管。这个溢出是**静默**的：名字太长就直接画到相邻组头上，
// 屏幕上糊成一片，编译器一声不吭。7 组时每格只有 240/7 = 34px，而 "SIGNAL" 要 36px——
// 这一步就是被它逼着把 SIGNAL 缩成 SIG 的，与其写进注释靠人记，不如让它编译失败。
static const int PANEL_W = 240;   // ST7789 240×135 是焊死的；SW 要等 M5 初始化才有值，用不了
constexpr int cstrLen(const char* c) { return *c ? 1 + cstrLen(c + 1) : 0; }
constexpr int groupCount() { return (int)(sizeof(GROUPS) / sizeof(GROUPS[0])); }
constexpr bool groupNamesFit(int g) {
  return g == groupCount()
           ? true
           // Font0 是 6px/字符；再留 2px 余量，不然相邻两个组名会贴到一起
           : (cstrLen(GROUPS[g].name) * 6 + 2 <= PANEL_W / groupCount()
              && groupNamesFit(g + 1));
}
static_assert(groupNamesFit(0),
              "组名太长，tab 条放不下：每格宽 240/GROUP_COUNT，Font0 每字符 6px。缩短组名或减少组数");
int menuIndex = 0;

int menuGroupOf(int appIdx) {
  for (int g = 0; g < GROUP_COUNT; g++)
    if (appIdx < GROUPS[g].first + GROUPS[g].count) return g;
  return GROUP_COUNT - 1;
}

const SetDef SETTINGS[] = {
  {"Wi-Fi",       SET_WIFI},
  {"PC MODE",     SET_PCMODE},
  {"GNSS Config", SET_GNSS},
  {"Brightness",  SET_BRIGHT},
  {"Volume",     SET_VOL},
  {"Boot sound", SET_BOOT_SOUND},
  {"LED",        SET_LED},
  {"Theme",      SET_THEME},
  {"Screen off", SET_SLEEP},
  {"Timezone",   SET_TZ},
  {"Units",      SET_WX_UNIT},
  {"Battery",    SET_BATTERY},
  {"Debug bar",  SET_DEBUG},
  {"Format SD",  SET_FORMAT},
  {"About",      SET_ABOUT},
};
const int SET_COUNT = sizeof(SETTINGS) / sizeof(SETTINGS[0]);
int settingsIndex = 0;

bool bootSoundOn = true;

void bootSoundInit() {
  bootSoundOn = (loadUChar("sys", "boot_snd", 1) != 0);
}

void bootSoundSet(bool on) {
  bootSoundOn = on;
  saveUChar("sys", "boot_snd", on ? 1 : 0);
}

uint8_t themeMode = 0;

const char* themeName(uint8_t mode) {
  static const char* NAMES[] = { "Auto", "Green", "Amber", "Cyan", "Purple", "Coral", "Ice", "Silver" };
  return (mode < 8) ? NAMES[mode] : "Auto";
}

void themeSet(uint8_t mode) {
  themeMode = mode % 8;
  saveUChar("sys", "theme", themeMode);
}


bool debugOn = false;
void debugInit() {
  debugOn = loadBool("debug", "on", false);
}
void debugSet(bool on) {
  debugOn = on;
  saveBool("debug", "on", debugOn);
  M5.Speaker.setVolume(volVal());
  if (on) M5.Speaker.stop();
}

Screen screen = SCREEN_CLOCK;  // 开机默认停在时钟页；menuIndex 仍是 0（Time），` 退出正好落回它
bool dirty = true;
int lastSecond = -1;
int formatStep = 0;

const int SLEEP_OPTS[] = {5, 15, 30, 60, 0};   // 秒；0 = 不熄灭
const int SLEEP_OPT_COUNT = sizeof(SLEEP_OPTS) / sizeof(SLEEP_OPTS[0]);
int sleepOptIdx = 4;   // 默认从 NVS 恢复，未设置时默认不熄灭（SLEEP_OPTS 最后一项=0）
uint32_t lastActivityMs = 0;
bool screenOff = false;

void sleepInit() {
  uint8_t val = loadUChar("sys", "sleep", 4);
  if (val >= SLEEP_OPT_COUNT) val = 4;
  sleepOptIdx = val;
}

void sleepSet(int idx) {
  if (idx < 0) idx = 0;
  if (idx >= SLEEP_OPT_COUNT) idx = SLEEP_OPT_COUNT - 1;
  sleepOptIdx = idx;
  saveUChar("sys", "sleep", (uint8_t)sleepOptIdx);
}

int brightPct = 45;
int volPct = 60;

void brightInit() {
  uint8_t val = loadUChar("sys", "bright", 45);
  if (val < 5 || val > 100) val = 45;
  brightPct = val;
}

void brightSet(int pct) {
  brightPct = constrain(pct, 5, 100);
  saveUChar("sys", "bright", (uint8_t)brightPct);
  M5.Display.setBrightness(brightVal());
}

void volInit() {
  volPct = loadUChar("sys", "vol", 60);
  if (volPct > 100) volPct = 60;
}

void volSet(int pct) {
  volPct = constrain(pct, 0, 100);
  saveUChar("sys", "vol", (uint8_t)volPct);
  M5.Speaker.setVolume(volVal());
  if (volVal() == 0) M5.Speaker.stop();
}

// 主菜单高亮框的当前绘制位置（向 menuIndex 那一格逼近，见 ui_common.cpp 的 menuUpdateAnim）
float menuSelX = 0.0f, menuSelY = 0.0f;

// 时区（POSIX TZ 字符串）。中国大陆 = UTC+8、无夏令时 → "CST-8"。
const TzOpt TZ_OPTS[] = {
  {"Beijing (CST-8)", "CST-8"},
  {"Tokyo (JST-9)", "JST-9"},
  {"UTC", "UTC0"},
  {"US Pacific", "PST8PDT"},
  {"US Eastern", "EST5EDT"},
  {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
};
const int TZ_OPT_COUNT = sizeof(TZ_OPTS) / sizeof(TZ_OPTS[0]);
int tzOptIdx = 0;
const char* TZ_INFO = TZ_OPTS[0].posix;

void tzInit() {
  tzOptIdx = loadInt("tz", "idx", 0);
  if (tzOptIdx < 0 || tzOptIdx >= TZ_OPT_COUNT) tzOptIdx = 0;
  TZ_INFO = TZ_OPTS[tzOptIdx].posix;
  setenv("TZ", TZ_INFO, 1);
  tzset();
}

void tzSet(int idx) {
  if (idx < 0) idx = 0;
  if (idx >= TZ_OPT_COUNT) idx = TZ_OPT_COUNT - 1;
  tzOptIdx = idx;
  TZ_INFO = TZ_OPTS[tzOptIdx].posix;
  setenv("TZ", TZ_INFO, 1);
  tzset();
  saveInt("tz", "idx", tzOptIdx);
}

bool timeSynced = false;
bool timeFromGps = false;
