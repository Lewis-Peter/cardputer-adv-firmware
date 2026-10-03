#pragma once
#include <stdint.h>
// ---------------------------------------------------------------------------
// Cardputer ADV 键盘（TCA8418 I2C 键盘控制器）
//
// ADV 和老 Cardputer 不一样：老款是 GPIO 矩阵直扫，ADV 换成了 TCA8418 这颗
// I2C 键盘芯片（地址 0x34），挂在内部 I2C 总线（引脚 SDA=8 / SCL=9）上。
// 这条总线正好是 M5Unified 的内部 I2C（M5.In_I2C），所以我们直接复用它，
// 不再自己 Wire.begin，避免两个 I2C 主机抢同一对引脚。
//
// 键盘物理布局是 4 行 × 14 列（=56 键），映射表来自 M5Cardputer 键盘库。
// 参考：Bruce 固件 boards/m5stack-cardputer/interface.cpp
// ---------------------------------------------------------------------------

namespace kbd {

// 修饰键位掩码。位序刻意跟 HID 报表的 modifier 字节完全一致，
// BLE 键盘那边拿到 modMask() 可以原样填进报文，不用再翻译一道。
enum : uint8_t {
  MOD_CTRL  = 0x01,
  MOD_SHIFT = 0x02,
  MOD_ALT   = 0x04,
  MOD_GUI   = 0x08,   // 机器上印的是 "opt"，HID 语义是 Cmd/Win
};

// 扩展键码：标准 ASCII 和 HIDKeyboardTypes.h 的 FUNCTION_KEY(128~151) 都表达不了的键，
// 走这个私有区间。转发给主机时由 bt.cpp 翻成裸 HID usage；本机各页面一律不认，等于忽略。
enum : uint8_t {
  KX_ESC = 160,
  KX_DEL = 161,       // 前向删除（不是退格）
  KX_END = 162,       // FUNCTION_KEY 那张表里有 HOME 没有 END，只能自己补
  // 下面两个是本机 UI 用的键，不是发给主机的键。BLE 键盘页把整块键盘都让给了主机
  // （连裸 ` 和 Enter 都要能打出去），本机功能只能挂在主机用不到的组合键上。
  KX_BACK     = 163,   // Fn+`     ：返回
  KX_FN_ENTER = 164,   // Fn+Enter ：留给页面自己定义（键盘页拿它切 IME 模式）
};

// 初始化 TCA8418（配置成 7 行 × 8 列的矩阵扫描）。M5.begin() 之后调用。
void begin();

// 取一个"新按下"的按键。没有按键返回 0。
// 返回值：可打印字符原样返回（'a'、';'、'.' 等）；回车 '\n'；退格 '\b'；Tab '\t'。
// 修饰键（Fn/Shift/Ctrl/Alt/Opt）本身不产生字符（返回 0），只更新 modMask()。
//
// Fn 层（键帽上印的副功能）：
//   Fn + ; . , /        ↑ ↓ ← →      （128+ 的 FUNCTION_KEY 码）
//   Fn + Tab            Esc
//   Fn + 1..0 - =       F1..F12
//   Fn + Backspace      Delete
//   Fn + [ / ]          Home / End
//   Fn + `              KX_BACK      （本机返回键，不转发给主机）
//   Fn + Enter          KX_FN_ENTER  （本机功能键，不转发给主机）
char readKey();

// 当前生效的修饰键 = 正按住的 + 单击粘滞的。
// 粘滞：单独按一下 Ctrl 再松开（中间没按别的键）＝ 锁定，再点一下解锁。
// 这样单手也能发 Ctrl+C；正常按住不放的用法完全不受影响。
uint8_t modMask();

// 只是粘滞的那部分（UI 用来把"锁住的"和"正按着的"画成两种样子）。
uint8_t modSticky();

// 用掉粘滞位（发完一次组合键就调）。粘滞是"只管下一个键"的语义。
void consumeSticky();

// 长按连发。默认关——主菜单/列表页不需要，开了反而容易划过头；
// 只有 BLE 键盘转发那种"当真键盘用"的页面才打开。
void setAutoRepeat(bool on);

// 排空 TCA8418 硬件 FIFO 里的全部待处理事件，并重置连发状态。
// 在任何阻塞操作（doScan / connectWith 等）结束后调用，防止阻塞期间积压的
// 按键事件在下一帧被批量消费，导致意外重复触发同一操作（表现为闪烁/卡死）。
void flushEvents();

}  // namespace kbd
