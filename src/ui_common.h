// 通用 UI 辅助：居中消息、文本截断、时间读数、顶部状态栏、主菜单轮播、时钟屏。
#pragma once
#include "globals.h"
#include <cmath>

void centerMsg(const char* msg, uint16_t color);
String trunc(const String& s, int maxChars);
String truncPx(const String& s, int maxPx);
String fmtBytes(uint64_t bytes);
void drawScrollBar(int x, int y, int h, int top, int total, int visible, uint16_t col = ACCENT, uint16_t trackCol = 0);
void drawMiniVuMeter(int x, int y, uint8_t level, bool active, uint16_t col = ACCENT, uint16_t dimCol = 0x2104);
bool nowHM(int& h, int& m, int& s);
void drawTopBar();
void drawMenu();
// 主菜单导航与高亮框缓动（布局细节都在 ui_common.cpp，main.cpp 只管转发方向键）
void menuMove(int dx, int dy);   // 方向键：组内移动，左右走出边界就切到相邻组
void menuSnapSelection();        // 高亮框立刻吸附到当前 menuIndex（开机/换组/一键回菜单）
void menuNextGroup();            // 直接翻到下一组（顶部 BTN GO 在主菜单里的快捷键）
bool menuUpdateAnim();           // 每帧推进一次缓动，返回 true 表示还在动

// 7 大分组的专属科技感主题配色定义
struct GroupTheme {
  uint16_t primary;    // 类别主色 (选框外圈、Tab高亮、当前图标、导览条、子页主色)
  uint16_t cardBg;     // 选中卡片底色微光 (暗色调)
  uint16_t innerRing;  // 双层选框内圈层次过渡
  uint16_t pillBg;     // Tab 胶囊背景
  uint16_t pillBorder; // Tab 胶囊边框
};

const GroupTheme& getGroupTheme(int groupIdx);

void drawClock();
void bootAnim();   // 开机动画（约 1.8s）
void drawBootFrame(uint32_t t_ms);

// 终端风格滚动日志（开机自检、WiFi 握手共用）。col=0 时自动配色（标题行高亮/明细行灰）
void termLogReset();
void termLogLine(const char* s, uint16_t col = 0);
void termLogDraw(bool cursor);
// 底部居中页码点（当前页亮绿，其余暗灰）。不用传参——当前是第几页/共几页
// 自己从 pages.h 那张翻页链表里查 screen 得到；不在任何链里的页面调用它什么也不画。
// 底部页码点。⚠️ **它占着 SH-4 那一行、而且是水平居中的**（半径 2 => 129..133）。
// 所以任何分页屏上的**居中**底部文字都不能再画在 SH-2/SH-1——会跟点叠在屏幕正中央。
// 这类文字请画到 SH-12（8px 高，占 115..123，正好在点上方、也在 Debug 状态条上方）。
// 左/右对齐的底部文字不受影响：点只占中间那十几个像素。
// 已知按这条规矩排的：stopwatch、quake 地图页。
// ⚠️ gnss.cpp 有几处居中的 SH-2 文字（350 / 1157 / 1217 行）看着是同一类冲突，
//    但那几页没法在 uisim 里渲染，没敢盲改——插着板子的时候顺手看一眼。
void drawPageDots();
// 动态分页或不属于 pages.cpp 链的页面也复用同一套指示器样式。
void drawPageDots(int current, int count);

// 统一顶栏：左标题（小号，不跟内容抢地方）+ 右侧可选状态 + 一条分隔线。
// 固定占 y=0..PAGE_HDR_BOTTOM，页面内容一律从 14 起画。不负责清屏，调用方自己 fillScreen。
// right 传 nullptr 就只画标题；rightCol 传 0 用默认的暗色。
static const int PAGE_HDR_BOTTOM = 12;
void drawPageHeader(const char* title, const char* right = nullptr, uint16_t rightCol = 0);

// 需要联网的页面共用的空状态（原来 netprobe/dnsfuzz/lanscan 各写了一份，
// 坐标和措辞都不一样）。调用方判断完 WiFi.status() 再调，画完直接 return。
void drawNeedWifi(int y = 26);

// SD 留痕日志共用：nowStamp() 有对时用真实时间戳，没对时降级成"t+秒数"；sdAppend() 追加一行，
// 未挂载 SD 时静默跳过。netprobe/dnsfuzz 等巡检类 app 共用同一份，避免各自拷贝一份 SD 库版本坑。
String nowStamp();
void sdAppend(const char* path, const String& line);

// 目标地址编辑框：一行标题 + 正在输入的内容(带光标) + 底部按键提示。
// NetProbe(vpn-node 目标) 和 DnsFuzz(dnsmasq 目标) 两处原本各抄了一份一模一样的 8 行，
// 跟 sdAppend/nowStamp 一样合并到这里。
void drawFieldEditor(const char* label, const String& buf);

// 「抬头天际」背景与坐标映射：渐变天空 + 仰角虚线网格 + 地平线 + N/E/S/W 方位刻度。
// ADS-B 飞机雷达、卫星过顶与 GNSS 卫星页共用同一套坐标系（X=方位角 0~360，Y=仰角 0~maxEl）。
void drawSkyBg(int ytop, int yhor, int maxEl);
bool skyCoord(float az, float el, int ytop, int yhor, int maxEl, int& x, int& y);
extern const char* CARD8[8];              // N/NE/E/... 八方位缩写
static inline const char* cardOf(float deg) { return CARD8[((int)(deg + 22.5f) / 45) % 8]; }

// 环形横轴上画标记：横轴的首尾是**同一个位置**——天际图两端都是 N（N E S W N），
// 世界地图两端都是国际日期变更线。所以贴着接缝的目标必然有一半在屏幕外。
//
// ⚠️ 别用"往里夹几像素"糊过去（这个项目里原来有三份各自的夹法：sats.cpp 夹 4、
// adsb.cpp 夹 8、quake.cpp 早期夹 0..cols-1）。夹是把位置说谎，而且夹多少永远追不上
// 装饰的实际半径——卫星页高仰角那个 r+2 的辉光圈就还是被切掉，看着像画错了。
// 正确做法是把越界的那一半**补到另一条边**上去：位置一个像素都没挪，多出来的部分
// 自然被 sprite 裁掉。
//
// period = 转一圈对应多少像素；reach = 这个标记从中心往外伸多远（含描边/光圈）。
// draw 只管"在给定的 x 画一次"，画几次、画在哪由这里决定。
template <typename F>
inline void drawWrapped(int x, int period, int reach, F draw) {
  for (int k = -1; k <= 1; k++) {
    int xx = x + k * period;
    if (xx + reach < 0 || xx - reach >= SW) continue;
    draw(xx);
  }
}

// 大圆距离(km)。ADS-B 页在 <150km 的尺度上用的是等距圆柱近似（省一堆三角函数），
// 但跨洋航线和台风路径动辄几千公里，那套近似会错得离谱，这两处必须用真的。
static inline float gcKm(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0f, D = 3.14159265f / 180.0f;
  float dlat = (lat2 - lat1) * D, dlon = (lon2 - lon1) * D;
  float a = sinf(dlat / 2) * sinf(dlat / 2) +
            cosf(lat1 * D) * cosf(lat2 * D) * sinf(dlon / 2) * sinf(dlon / 2);
  if (a < 0) a = 0;
  if (a > 1) a = 1;
  return R * 2 * atan2f(sqrtf(a), sqrtf(1 - a));
}
